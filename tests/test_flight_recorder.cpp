// End-to-end verification of ScarlettEdge's 4-Part Flight-Recorder Diagnostic Suite:
//   Part 1: Lock-free 4096-slot MPMC ring buffer + cross-DLL deduplicated miss registries
//           (StubThunk caller RVA, COM QueryInterface IIDs, COM vtable slots, G:\ VFS misses).
//   Part 2: Vectored Exception Handler (VEH) capturing real hardware EXCEPTION_ACCESS_VIOLATION
//           with multi-frame x64 .pdata unwinding while ignoring benign C++ exceptions.
//   Part 3: GPU DRED report generation + breadcrumbs + automatic failing Xbox PSO shader dump.
//   Part 4: Background progress-heartbeat hang watchdog thread sampling + flight_summary.json
//           + `scarlettx diag --flight-recorder <dir>` CLI integration.

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/gdk/xgameruntime_abi.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/input/gameinput_abi.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/loader/stub_thunk.h"
#include "sx_check.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#endif

extern "C" int32_t WINAPI DStorageGetFactory(const void* riid, void** out);

namespace {

std::string ReadWholeFile(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

#if defined(_WIN32)
// Pure POD functions (no C++ objects with destructors) for deterministic SEH hardware AV test
// across multiple x64 stack frames (.pdata unwind verification).
__declspec(noinline) uint32_t TriggerHardwareReadLeaf(const volatile uint32_t* bad_va) {
    return *bad_va + 1u;
}

__declspec(noinline) uint32_t TriggerHardwareReadMiddle(uintptr_t raw_addr) {
    const auto* ptr = reinterpret_cast<const volatile uint32_t*>(raw_addr);
    return TriggerHardwareReadLeaf(ptr) ^ 0x5Au;
}

__declspec(noinline) int TriggerCaughtHardwareAccessViolation(uintptr_t fault_va) {
    int caught = 0;
    __try {
        volatile uint32_t sink = TriggerHardwareReadMiddle(fault_va);
        (void)sink;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        caught = 1;
    }
    return caught;
}

bool RunProcessBlocking(const std::filesystem::path& exe_path, const std::string& args_tail) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + exe_path.string() + "\" " + args_tail;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    const DWORD wait_rc = WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait_rc == WAIT_OBJECT_0 && exit_code == 0;
}
#endif

}  // namespace

int main() {
#if !defined(_WIN32)
    std::cout << "[SKIP] test_flight_recorder requires Windows x64\n";
    return 0;
#else
    using namespace scarlettx;

    const std::filesystem::path bin_dir = SCARLETTX_BINARY_DIR;
    const std::filesystem::path out_dir = bin_dir / "test_flight_recorder_out_tmp";
    const std::filesystem::path cli_dir = bin_dir / "test_flight_recorder_cli_tmp";
    std::error_code ec;
    std::filesystem::remove_all(out_dir, ec);
    std::filesystem::remove_all(cli_dir, ec);

    // 0. Disabled fast-path must be completely inert (zero events, zero files).
    FlightRecorder::Instance().Disable();
    FlightRecorder::Instance().ResetSession();
    SX_CHECK(!FlightRecorder::IsEnabled());
    FlightRecorder::Instance().RecordEvent(FlightEventCategory::HleCall, "test", "ignored_when_off");
    SX_CHECK(FlightRecorder::Instance().GetStats().total_events == 0);
    SX_CHECK(!std::filesystem::exists(out_dir));

    // -----------------------------------------------------------------------
    // Part 1: Lock-free MPMC ring buffer + cross-DLL deduplicated miss tables
    // -----------------------------------------------------------------------
    FlightRecorderOptions opts;
    opts.output_dir = out_dir.string();
    opts.hang_timeout_ms = 0;  // Enable watchdog explicitly in Part 4
    opts.install_veh = true;
    opts.enable_dred = true;
    opts.dump_failing_shaders = true;
    opts.reset_history = true;
    FlightRecorder::Instance().Enable(opts);
    SX_CHECK(FlightRecorder::IsEnabled());

    // Multi-threaded lock-free stress across 4 concurrent writers
    {
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([t]() {
                for (int i = 0; i < 300; ++i) {
                    FlightRecorder::Instance().RecordEvent(
                        FlightEventCategory::HleCall, "mt_stress", "parallel_event",
                        0, SCARLETTX_CALLER_RIP(), static_cast<uint64_t>(t), static_cast<uint64_t>(i));
                }
            });
        }
        for (auto& th : writers) th.join();
    }
    SX_CHECK(FlightRecorder::Instance().GetStats().total_events >= 1201);

    // Real x86-64 StubThunk trampoline caller-RVA & argument capture + deduplication
    {
        StubThunkTable stubs(16);
        SX_CHECK(stubs.Initialize().ok());
        void* thunk_fn = nullptr;
        uint32_t stub_id = 0;
        SX_CHECK(stubs.AllocateStub("xbox_future.dll", "XFutureMissingApi", false, 0,
                                    0x80004001ull, thunk_fn, &stub_id).ok());
        SX_CHECK(thunk_fn != nullptr);
        using ThunkCall = uint64_t(*)(uint64_t, uint64_t, uint64_t, uint64_t);
        auto fn = reinterpret_cast<ThunkCall>(thunk_fn);
        for (int i = 0; i < 3; ++i) {
            const uint64_t r = fn(0x1111ull + static_cast<uint64_t>(i), 0x2222ull, 0x3333ull, 0x4444ull);
            SX_CHECK(r == 0x80004001ull);
        }
        const auto recs = stubs.SnapshotRecords();
        SX_CHECK(recs.size() == 1 && recs[0].hit_count == 3);
        SX_CHECK(recs[0].last_caller_rip != 0);
        const auto caller_res = FlightRecorder::ResolveCodeAddress(recs[0].last_caller_rip);
        SX_CHECK(caller_res.resolved && caller_res.rva > 0);
        SX_CHECK(FlightRecorder::Instance().GetStats().unique_stub_misses == 1);
    }

    // Cross-DLL misses across xgameruntime.dll, GameInput.dll, and xcrdapi.dll
    {
        struct RawGuid {
            uint32_t d1;
            uint16_t d2;
            uint16_t d3;
            uint8_t d4[8];
        };
        const RawGuid unknown_api{0xDEADBEEFu, 0xCAFEu, 0xBABEu, {1, 2, 3, 4, 5, 6, 7, 8}};
        void* out_api = nullptr;
        SX_CHECK(QueryApiImpl(&unknown_api, &unknown_api, &out_api) == static_cast<XGameResult>(0x80004002u));
        SX_CHECK(QueryApiImpl(&unknown_api, &unknown_api, &out_api) == static_cast<XGameResult>(0x80004002u));

        uint64_t gi_raw = 0;
        SX_CHECK(GameInputCreate(&gi_raw) == 0 && gi_raw != 0);
        void* gi = reinterpret_cast<void*>(gi_raw);
        using QiFn = intptr_t(__stdcall*)(void*, const void*, void**, uintptr_t);
        auto* gi_vtbl = *reinterpret_cast<void***>(gi);
        void* bad_qi_out = nullptr;
        SX_CHECK(static_cast<uint32_t>(reinterpret_cast<QiFn>(gi_vtbl[0])(gi, &unknown_api, &bad_qi_out, 0)) ==
                 0x80004002u);
        using GiRelFn = intptr_t(__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t);
        reinterpret_cast<GiRelFn>(gi_vtbl[2])(gi, 0, 0, 0);

        using RelFn = uint32_t(__stdcall*)(void*);

        const RawGuid factory_iid{0x6F26EA0Cu, 0xC3CDu, 0x48BCu, {0xB1, 0x99, 0x70, 0x0D, 0x52, 0x97, 0x53, 0xF5}};
        void* ds_factory = nullptr;
        SX_CHECK(DStorageGetFactory(&factory_iid, &ds_factory) == 0 && ds_factory != nullptr);
        auto* ds_vtbl = *reinterpret_cast<void***>(ds_factory);
        using OpenFileFn = int32_t(WINAPI*)(void*, const wchar_t*, const void*, void**);
        void* ds_file = nullptr;
        const int32_t open_hr = reinterpret_cast<OpenFileFn>(ds_vtbl[4])(
            ds_factory, L"G:\\nonexistent_folder\\missing_world_chunk.pak", &factory_iid, &ds_file);
        SX_CHECK(open_hr < 0 && ds_file == nullptr);
        reinterpret_cast<RelFn>(ds_vtbl[2])(ds_factory);

        const auto st = FlightRecorder::Instance().GetStats();
        SX_CHECK(st.unique_com_qi_misses >= 2);
        SX_CHECK(st.unique_vfs_misses >= 1);
    }

    // Lock-free seqlock guest PE64 Ghidra RVA resolution on a relocated guest executable
    {
        const auto hello_exe = bin_dir / "packages" / "hello_scarlett" / "hello_scarlett.exe";
        SX_CHECK(std::filesystem::exists(hello_exe));
        MappedPeImage guest_img;
        HleSymbolRegistry hle_reg;
        PeMapOptions map_opts;
        map_opts.force_relocate = true;
        map_opts.auto_stub_missing = true;
        map_opts.run_tls_callbacks = false;
        map_opts.shim_directory = bin_dir.string();
        SX_CHECK(guest_img.MapFile(hello_exe.string(), {}, hle_reg, map_opts).ok());

        const uint64_t entry_va = reinterpret_cast<uint64_t>(guest_img.EntryPointAddress());
        const auto resolved = FlightRecorder::ResolveCodeAddress(entry_va);
        SX_CHECK(resolved.resolved);
        SX_CHECK(resolved.is_guest && resolved.is_guest_exe);
        SX_CHECK(resolved.ghidra_va == 0x140000000ull + resolved.rva);
        guest_img.Unmap();

        const auto after_unmap = FlightRecorder::ResolveCodeAddress(entry_va);
        SX_CHECK(!after_unmap.is_guest);
    }

    // -----------------------------------------------------------------------
    // Part 2: VEH Crash Handler + .pdata Stack Unwinder + C++ Throw Filtering
    // -----------------------------------------------------------------------
    {
        // Benign MSVC C++ exceptions (0xE06D7363) MUST NOT trigger crash dumps!
        bool cpp_caught = false;
        try {
            throw std::runtime_error("benign_cpp_exception_test");
        } catch (const std::exception&) {
            cpp_caught = true;
        }
        SX_CHECK(cpp_caught);
        SX_CHECK(FlightRecorder::Instance().GetStats().crash_reports_written == 0);

        // Real hardware EXCEPTION_ACCESS_VIOLATION (0xC0000005) reading 0xDEAD
        const int hw_caught = TriggerCaughtHardwareAccessViolation(0x000000000000DEADull);
        SX_CHECK(hw_caught == 1);
        SX_CHECK(FlightRecorder::Instance().GetStats().crash_reports_written == 1);

        const auto crash_json_path =
            out_dir / ("crash_report_" + std::to_string(GetCurrentProcessId()) + "_1.json");
        const auto crash_txt_path =
            out_dir / ("crash_report_" + std::to_string(GetCurrentProcessId()) + "_1.txt");
        SX_CHECK(std::filesystem::exists(crash_json_path));
        SX_CHECK(std::filesystem::exists(crash_txt_path));

        const std::string crash_json = ReadWholeFile(crash_json_path);
        SX_CHECK(crash_json.find("\"exception_code\": \"0xC0000005\"") != std::string::npos);
        SX_CHECK(crash_json.find("\"exception_name\": \"EXCEPTION_ACCESS_VIOLATION\"") != std::string::npos);
        SX_CHECK(crash_json.find("\"access_type\": \"READ\"") != std::string::npos);
        SX_CHECK(crash_json.find("\"fault_target_va\": \"0xDEAD\"") != std::string::npos);
        SX_CHECK(crash_json.find("test_flight_recorder.exe+0x") != std::string::npos);
        SX_CHECK(crash_json.find("\"index\": 1") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Part 3: GPU DRED + Breadcrumbs + Failing Xbox PSO Shader Binary Dumper
    // -----------------------------------------------------------------------
    {
        FlightRecorder::Instance().RecordGpuBreadcrumb(7, 1001, "FrameBegin");
        FlightRecorder::Instance().RecordGpuBreadcrumb(7, 1002, "ShadowAtlasPass");

        _putenv_s("SCARLETTX_REAL_TITLE", "1");
        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 0x20004u;
        params.PreferWarp = 1;
        const GUID iid_dev = __uuidof(ID3D12Device);
        void* xbox_dev = nullptr;
        SX_CHECK(D3D12XboxCreateDevice(nullptr, &params, &iid_dev, &xbox_dev) == 0 && xbox_dev != nullptr);

        auto* vtbl = *reinterpret_cast<void***>(xbox_dev);

        // Unimplemented vtable slot 120 -> E_NOTIMPL + VtableSlotMiss
        using SlotFn = intptr_t(__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t);
        const intptr_t slot_hr = reinterpret_cast<SlotFn>(vtbl[120])(xbox_dev, 0xCAFEull, 0, 0);
        SX_CHECK(static_cast<uint32_t>(slot_hr) == 0x80004001u);
        SX_CHECK(FlightRecorder::Instance().GetStats().unique_vtable_misses >= 1);

        // Corrupt 64-byte Xbox compute shader -> CreateComputePipelineState fails & dumps raw binary
        std::vector<uint8_t> bad_cs(64, 0xAAu);
        bad_cs[0] = 'D';
        bad_cs[1] = 'X';
        bad_cs[2] = 'B';
        bad_cs[3] = 'C';
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpso_desc{};
        cpso_desc.CS.pShaderBytecode = bad_cs.data();
        cpso_desc.CS.BytecodeLength = bad_cs.size();
        void* out_pso = nullptr;
        using CreateCpsoFn = intptr_t(__stdcall*)(void*, const void*, const void*, void**);
        const intptr_t pso_hr = reinterpret_cast<CreateCpsoFn>(vtbl[11])(
            xbox_dev, &cpso_desc, &iid_dev, &out_pso);
        SX_CHECK(pso_hr < 0 && out_pso == nullptr);
        SX_CHECK(FlightRecorder::Instance().GetStats().shader_dumps_written >= 1);

        // Verify raw shader bytes on disk match `bad_cs` byte-for-byte
        bool found_raw_shader = false;
        bool found_meta_json = false;
        for (const auto& entry : std::filesystem::directory_iterator(out_dir / "shaders", ec)) {
            const std::string fname = entry.path().filename().string();
            if (fname.find("_raw.bin") != std::string::npos) {
                const std::string raw_on_disk = ReadWholeFile(entry.path());
                SX_CHECK(raw_on_disk.size() == bad_cs.size());
                SX_CHECK(std::memcmp(raw_on_disk.data(), bad_cs.data(), bad_cs.size()) == 0);
                found_raw_shader = true;
            } else if (fname.find("_meta.json") != std::string::npos) {
                const std::string meta = ReadWholeFile(entry.path());
                SX_CHECK(meta.find("\"stage_or_api\": \"ComputePSO_CS\"") != std::string::npos);
                found_meta_json = true;
            }
        }
        SX_CHECK(found_raw_shader && found_meta_json);

        // DRED report generation with breadcrumbs
        const std::string dred_path = FlightRecorder::Instance().RecordGpuDeviceRemoved(
            nullptr, static_cast<int32_t>(0x887A0006u), "SyntheticTdrCheck");
        SX_CHECK(!dred_path.empty() && std::filesystem::exists(dred_path));
        const std::string dred_json = ReadWholeFile(dred_path);
        SX_CHECK(dred_json.find("0x887A0006") != std::string::npos);
        SX_CHECK(dred_json.find("ShadowAtlasPass") != std::string::npos);

        using DevRelFn = intptr_t(__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t);
        reinterpret_cast<DevRelFn>(vtbl[2])(xbox_dev, 0, 0, 0);
        _putenv_s("SCARLETTX_REAL_TITLE", "");
    }

    // -----------------------------------------------------------------------
    // Part 4: Hang/Deadlock Watchdog + Summary Report + CLI Integration
    // -----------------------------------------------------------------------
    {
        std::atomic<bool> release_worker{false};
        std::thread stalled_worker([&]() {
            while (!release_worker.load(std::memory_order_acquire)) {
                Sleep(5);
            }
        });

        FlightRecorderOptions hang_opts = opts;
        hang_opts.hang_timeout_ms = 50;
        hang_opts.reset_history = false;
        FlightRecorder::Instance().Enable(hang_opts);

        // Stall longer than 50 ms without calling BeatProgress
        Sleep(180);

        release_worker.store(true, std::memory_order_release);
        stalled_worker.join();

        SX_CHECK(FlightRecorder::Instance().GetStats().hang_snapshots_written >= 1);
        const auto hang_json_path =
            out_dir / ("hang_snapshot_" + std::to_string(GetCurrentProcessId()) + "_1.json");
        SX_CHECK(std::filesystem::exists(hang_json_path));
        const std::string hang_json = ReadWholeFile(hang_json_path);
        SX_CHECK(hang_json.find("\"report_type\": \"hang_snapshot\"") != std::string::npos);
        SX_CHECK(hang_json.find("\"threads\": [") != std::string::npos);

        std::string sum_json_path, sum_txt_path;
        SX_CHECK(FlightRecorder::Instance().WriteSummaryReport(&sum_json_path, &sum_txt_path));
        FlightRecorder::Instance().Disable();

        SX_CHECK(std::filesystem::exists(sum_json_path));
        SX_CHECK(std::filesystem::exists(sum_txt_path));
        const std::string sum_json = ReadWholeFile(sum_json_path);
        SX_CHECK(sum_json.find("\"xbox_future.dll!XFutureMissingApi\"") != std::string::npos);
        SX_CHECK(sum_json.find("\"XGR::QueryApiImpl\"") != std::string::npos);
        SX_CHECK(sum_json.find("\"XboxID3D12Device\"") != std::string::npos);
        SX_CHECK(sum_json.find("missing_world_chunk.pak") != std::string::npos);
        SX_CHECK(sum_json.find("crash_report_") != std::string::npos);
        SX_CHECK(sum_json.find("hang_snapshot_") != std::string::npos);
        SX_CHECK(sum_json.find("gpu_dred_report_") != std::string::npos);
        SX_CHECK(sum_json.find("shader_fail_") != std::string::npos);
    }

    // CLI integration check: `scarlettx diag --flight-recorder <cli_dir>`
    {
        const auto scarlettx_exe = bin_dir / "scarlettx.exe";
        SX_CHECK(std::filesystem::exists(scarlettx_exe));
        SX_CHECK(RunProcessBlocking(scarlettx_exe, "diag --flight-recorder \"" + cli_dir.string() + "\""));
        SX_CHECK(std::filesystem::exists(cli_dir / "flight_summary.json"));
        SX_CHECK(std::filesystem::exists(cli_dir / "flight_summary.txt"));
    }

    std::filesystem::remove_all(out_dir, ec);
    std::filesystem::remove_all(cli_dir, ec);

    std::cout << "[PASS] test_flight_recorder (4-part ring buffer, VEH stack-walk RVA, DRED/shader dump & hang watchdog)\n";
    return 0;
#endif
}
