#include "scarlettx/core/flight_recorder.h"

#include "../loader/guest_modules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#  include <tlhelp32.h>
#endif

namespace scarlettx {

std::atomic<uint8_t> FlightRecorder::s_enabled_state_{0};

const char* FlightEventCategoryName(FlightEventCategory cat) noexcept {
    switch (cat) {
        case FlightEventCategory::Lifecycle: return "Lifecycle";
        case FlightEventCategory::HleCall: return "HleCall";
        case FlightEventCategory::StubHit: return "StubHit";
        case FlightEventCategory::ComQiMiss: return "ComQiMiss";
        case FlightEventCategory::VtableSlotMiss: return "VtableSlotMiss";
        case FlightEventCategory::WinRtBroker: return "WinRtBroker";
        case FlightEventCategory::VfsAccess: return "VfsAccess";
        case FlightEventCategory::VfsMiss: return "VfsMiss";
        case FlightEventCategory::GpuBreadcrumb: return "GpuBreadcrumb";
        case FlightEventCategory::ShaderOrPsoFail: return "ShaderOrPsoFail";
        case FlightEventCategory::GpuDeviceRemoved: return "GpuDeviceRemoved";
        case FlightEventCategory::GuestFault: return "GuestFault";
        case FlightEventCategory::WatchdogHang: return "WatchdogHang";
        default: return "Unknown";
    }
}

namespace {

class ScopedSpinTryLock {
public:
    explicit ScopedSpinTryLock(std::atomic_flag& flag, int max_spins = 64) noexcept : flag_(flag) {
        for (int i = 0; i < max_spins; ++i) {
            if (!flag_.test_and_set(std::memory_order_acquire)) {
                locked_ = true;
                return;
            }
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
            _mm_pause();
#endif
        }
    }
    ~ScopedSpinTryLock() noexcept {
        if (locked_) {
            flag_.clear(std::memory_order_release);
        }
    }
    bool owns_lock() const noexcept { return locked_; }

private:
    std::atomic_flag& flag_;
    bool locked_ = false;
};

void CopyBoundedStr(char* dst, size_t dst_cap, const char* src) noexcept {
    if (!dst || dst_cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < dst_cap && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

std::string WideToUtf8Safe(const wchar_t* wide) {
    if (!wide || !*wide) return {};
#if defined(_WIN32)
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
#else
    std::string out;
    for (size_t i = 0; wide[i] != L'\0'; ++i) {
        out.push_back(wide[i] < 128 ? static_cast<char>(wide[i]) : '?');
    }
    return out;
#endif
}

void FormatGuidBytes(const void* guid_ptr, char (&out)[40]) noexcept {
    if (!guid_ptr) {
        CopyBoundedStr(out, sizeof(out), "{00000000-0000-0000-0000-000000000000}");
        return;
    }
    struct RawGuid {
        uint32_t d1;
        uint16_t d2;
        uint16_t d3;
        uint8_t d4[8];
    };
    RawGuid g{};
    std::memcpy(&g, guid_ptr, sizeof(g));
    std::snprintf(out, sizeof(out),
                  "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  g.d1, g.d2, g.d3,
                  g.d4[0], g.d4[1], g.d4[2], g.d4[3],
                  g.d4[4], g.d4[5], g.d4[6], g.d4[7]);
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8] = {};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string HexU64(uint64_t v) {
    char buf[24] = {};
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string HexU32(uint32_t v) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

uint64_t Fnv1a64(const void* data, size_t len) noexcept {
    uint64_t h = 14695981039346656037ull;
    const auto* p = static_cast<const uint8_t*>(data);
    if (!p) return h;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string SanitizeFilenameToken(const char* raw) {
    if (!raw || !*raw) return "unknown";
    std::string out;
    for (size_t i = 0; raw[i] != '\0' && out.size() < 32; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

uint32_t CurrentTid() noexcept {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentThreadId());
#else
    return 0;
#endif
}

uint32_t CurrentPid() noexcept {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

#if defined(_WIN32)

bool IsFatalHardwareOrFailFastException(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:          // 0xC0000005
        case EXCEPTION_IN_PAGE_ERROR:             // 0xC0000006
        case EXCEPTION_ILLEGAL_INSTRUCTION:       // 0xC000001D
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:  // 0xC0000025
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:     // 0xC000008C
        case EXCEPTION_INT_DIVIDE_BY_ZERO:        // 0xC0000094
        case EXCEPTION_INT_OVERFLOW:              // 0xC0000095
        case EXCEPTION_PRIV_INSTRUCTION:          // 0xC0000096
        case EXCEPTION_STACK_OVERFLOW:            // 0xC00000FD
        case 0xC0000374u:                         // STATUS_HEAP_CORRUPTION
        case 0xC0000409u:                         // STATUS_STACK_BUFFER_OVERRUN (__fastfail)
        case EXCEPTION_DATATYPE_MISALIGNMENT:     // 0x80000002
            return true;
        case EXCEPTION_BREAKPOINT:                // 0x80000003 (guest __debugbreak when no debugger)
            return !IsDebuggerPresent();
        default:
            return false;
    }
}

const char* ExceptionCodeName(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case 0xC0000374u: return "STATUS_HEAP_CORRUPTION";
        case 0xC0000409u: return "STATUS_STACK_BUFFER_OVERRUN";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
        default: return "UNKNOWN_EXCEPTION";
    }
}

bool IsCommittedReadableAddress(uint64_t va, size_t bytes) noexcept {
    if (va < 0x10000ull || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(va), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const uint64_t region_end =
        reinterpret_cast<uint64_t>(mbi.BaseAddress) + static_cast<uint64_t>(mbi.RegionSize);
    return va + bytes <= region_end;
}

bool IsExecutableCodeAddress(uint64_t va) noexcept {
    if (va < 0x10000ull) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(va), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) return false;
    constexpr DWORD kExecMask =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExecMask) != 0;
}

// Pure POD SEH helper (no C++ objects with destructors allowed in __try scope).
size_t UnwindX64FramesPod(const CONTEXT* initial_ctx,
                          uint64_t* out_rips,
                          uint64_t* out_rsps,
                          size_t max_frames) noexcept {
    if (!initial_ctx || !out_rips || !out_rsps || max_frames == 0) return 0;
    size_t count = 0;
    CONTEXT ctx = *initial_ctx;
    __try {
        for (size_t i = 0; i < max_frames; ++i) {
            const uint64_t cur_rip = ctx.Rip;
            const uint64_t cur_rsp = ctx.Rsp;
            if (i > 0 && cur_rip == 0) {
                break;
            }
            out_rips[count] = cur_rip;
            out_rsps[count] = cur_rsp;
            ++count;

            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(cur_rip, &image_base, nullptr);
            if (rf != nullptr) {
                PVOID handler_data = nullptr;
                DWORD64 establisher_frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                                 image_base,
                                 cur_rip,
                                 rf,
                                 &ctx,
                                 &handler_data,
                                 &establisher_frame,
                                 nullptr);
                if (ctx.Rip == 0 || (ctx.Rip == cur_rip && ctx.Rsp == cur_rsp)) {
                    break;
                }
            } else {
                // Leaf function, StubThunk trampoline, or wild jump to 0/unmapped VA:
                // if RIP sits at StubThunk's `add rsp, imm8` (`48 83 C4 imm8`) epilogue,
                // advance past the trampoline's stack frame before reading the return address.
                uint64_t rsp_for_ret = cur_rsp;
                if (IsCommittedReadableAddress(cur_rip, 4)) {
                    uint8_t insn[4] = {};
                    std::memcpy(insn, reinterpret_cast<const void*>(cur_rip), sizeof(insn));
                    if (insn[0] == 0x48u && insn[1] == 0x83u && insn[2] == 0xC4u) {
                        rsp_for_ret += static_cast<uint64_t>(insn[3]);
                    }
                }
                if (!IsCommittedReadableAddress(rsp_for_ret, sizeof(uint64_t))) {
                    break;
                }
                uint64_t ret_addr = 0;
                std::memcpy(&ret_addr, reinterpret_cast<const void*>(rsp_for_ret), sizeof(uint64_t));
                if (ret_addr == 0 || ret_addr == cur_rip) {
                    break;
                }
                ctx.Rip = ret_addr;
                ctx.Rsp = rsp_for_ret + 8ull;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Stop unwinding cleanly if guest stack memory is corrupted.
    }
    return count;
}

bool IsBenignFirstChanceGuardPageAv(const EXCEPTION_RECORD* er) noexcept {
    if (!er) return false;
    if ((er->ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0) {
        return false;
    }
    const DWORD code = er->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION) {
        return true;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) {
        return false;
    }
    const ULONG_PTR av_op = er->ExceptionInformation[0];
    const uint64_t target_va = static_cast<uint64_t>(er->ExceptionInformation[1]);
    if (av_op == 8 || target_va < 0x10000ull) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(target_va), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & PAGE_GUARD) != 0) {
        return true;
    }
    // Guest VirtualAlloc (MEM_PRIVATE) guard pages (PAGE_NOACCESS) and GC write-watch
    // pages (WRITE to PAGE_READONLY) raise benign first-chance AVs handled by guest
    // SEH/VEH; unhandled faults are still captured at FilterGuestEntryException.
    if (mbi.Type == MEM_PRIVATE) {
        if ((mbi.Protect & PAGE_NOACCESS) != 0) {
            return true;
        }
        if (av_op == 1 && (mbi.Protect & PAGE_READONLY) != 0) {
            return true;
        }
    }
    return false;
}

std::atomic<uint32_t> g_active_crash_tid{0};

struct ScopedActiveCrashTidGuard {
    bool active = false;
    explicit ScopedActiveCrashTidGuard(uint32_t tid) noexcept {
        const uint32_t non_zero_tid = (tid != 0u) ? tid : 1u;
        uint32_t expected = 0u;
        active = g_active_crash_tid.compare_exchange_strong(
            expected, non_zero_tid, std::memory_order_acq_rel);
    }
    ~ScopedActiveCrashTidGuard() noexcept {
        if (active) {
            g_active_crash_tid.store(0u, std::memory_order_release);
        }
    }
};

LONG WINAPI FlightVectoredExceptionHandler(PEXCEPTION_POINTERS ep) noexcept {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!FlightRecorder::IsEnabledNoProbe()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsFatalHardwareOrFailFastException(code) ||
        IsBenignFirstChanceGuardPageAv(ep->ExceptionRecord)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ScopedActiveCrashTidGuard crash_tid_guard(CurrentTid());
    if (!crash_tid_guard.active) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    FlightRecorder::Instance().CaptureCrashReport(ep, "VectoredExceptionHandler", nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

HMODULE GetSystemD3D12Module() noexcept {
    static HMODULE mod = []() noexcept -> HMODULE {
        wchar_t sys_dir[MAX_PATH] = {};
        if (GetSystemDirectoryW(sys_dir, MAX_PATH) == 0) return nullptr;
        std::wstring p(sys_dir);
        p += L"\\d3d12.dll";
        return LoadLibraryW(p.c_str());
    }();
    return mod;
}

#endif  // _WIN32

}  // namespace

FlightRecorder::FlightRecorder() {
    start_qpc_ = NowQpc();
    last_heartbeat_qpc_.store(start_qpc_, std::memory_order_relaxed);
    for (auto& slot : committed_seq_) {
        slot.store(0, std::memory_order_relaxed);
    }
    for (auto& c : category_counts_) {
        c.store(0, std::memory_order_relaxed);
    }
#if defined(_WIN32)
    if (HMODULE exe_mod = GetModuleHandleW(nullptr)) {
        host_exe_base_ = reinterpret_cast<uint64_t>(exe_mod);
        char exe_path[MAX_PATH + 1] = {};
        const DWORD n = GetModuleFileNameA(exe_mod, exe_path, MAX_PATH);
        if (n > 0) {
            const char* base = exe_path;
            for (DWORD i = 0; i < n; ++i) {
                if (exe_path[i] == '\\' || exe_path[i] == '/') {
                    base = exe_path + i + 1;
                }
            }
            CopyBoundedStr(host_exe_name_, sizeof(host_exe_name_), base);
        }
    }
#endif
}

FlightRecorder::~FlightRecorder() {
    Disable();
}

FlightRecorder& FlightRecorder::Instance() noexcept {
    static FlightRecorder instance;
    return instance;
}

int64_t FlightRecorder::QpcFreq() noexcept {
#if defined(_WIN32)
    static const int64_t freq = []() noexcept -> int64_t {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1000000ll;
    }();
    return freq;
#else
    return 1000000ll;
#endif
}

int64_t FlightRecorder::NowQpc() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
#else
    return 0;
#endif
}

uint64_t FlightRecorder::ElapsedUs() const noexcept {
    const int64_t now = NowQpc();
    const int64_t delta = (now > start_qpc_) ? (now - start_qpc_) : 0;
    return static_cast<uint64_t>((delta * 1000000ll) / QpcFreq());
}

bool FlightRecorder::IsEnabled() noexcept {
    const uint8_t s = s_enabled_state_.load(std::memory_order_relaxed);
    if (s == 2u) return true;
    if (s == 1u) return false;
    Instance().ProbeEnvOnce();
    return s_enabled_state_.load(std::memory_order_relaxed) == 2u;
}

bool FlightRecorder::IsEnabledNoProbe() noexcept {
    return s_enabled_state_.load(std::memory_order_acquire) == 2u;
}

void FlightRecorder::ProbeEnvOnce() noexcept {
    bool expected = false;
    if (!env_probing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const char* env_on = std::getenv("SCARLETTX_FLIGHT_RECORDER");
    if (!env_on || env_on[0] == '\0' || env_on[0] == '0') {
        uint8_t zero = 0;
        s_enabled_state_.compare_exchange_strong(zero, 1u, std::memory_order_release);
        env_probing_.store(false, std::memory_order_release);
        return;
    }

    FlightRecorderOptions opts;
    if (const char* dir = std::getenv("SCARLETTX_FLIGHT_RECORDER_DIR"); dir && *dir) {
        opts.output_dir = dir;
    } else {
        opts.output_dir = "flight_logs";
    }
    if (const char* to_ms = std::getenv("SCARLETTX_HANG_TIMEOUT_MS"); to_ms && *to_ms) {
        opts.hang_timeout_ms = static_cast<uint32_t>(std::strtoul(to_ms, nullptr, 10));
    }
    opts.reset_history = false;
    try {
        Enable(opts);
    } catch (...) {
        s_enabled_state_.store(1u, std::memory_order_release);
    }
    env_probing_.store(false, std::memory_order_release);
}

void FlightRecorder::ResetSession() noexcept {
    write_seq_.store(0, std::memory_order_relaxed);
    for (auto& slot : committed_seq_) {
        slot.store(0, std::memory_order_relaxed);
    }
    for (auto& c : category_counts_) {
        c.store(0, std::memory_order_relaxed);
    }
    start_qpc_ = NowQpc();
    heartbeat_seq_.store(0, std::memory_order_relaxed);
    last_heartbeat_qpc_.store(start_qpc_, std::memory_order_relaxed);
    last_hung_heartbeat_seq_.store(~0ull, std::memory_order_relaxed);
    {
        ScopedSpinTryLock lk(phase_lock_, 1024);
        CopyBoundedStr(last_phase_, sizeof(last_phase_), "init");
    }
    {
        ScopedSpinTryLock lk(miss_lock_, 1024);
        stub_miss_count_ = 0;
        com_qi_miss_count_ = 0;
        vtable_miss_count_ = 0;
        vfs_miss_count_ = 0;
    }
    {
        ScopedSpinTryLock lk(gpu_bc_lock_, 1024);
        total_gpu_breadcrumbs_ = 0;
    }
    crash_seq_.store(0, std::memory_order_relaxed);
    hang_seq_.store(0, std::memory_order_relaxed);
    dred_seq_.store(0, std::memory_order_relaxed);
    shader_seq_.store(0, std::memory_order_relaxed);
    last_crash_sig_.store(0, std::memory_order_relaxed);
    crash_artifact_count_.store(0, std::memory_order_relaxed);
    for (auto& p : crash_artifact_paths_) {
        p[0] = '\0';
    }
    {
        std::lock_guard<std::mutex> lk(artifacts_mu_);
        hang_artifacts_.clear();
        dred_artifacts_.clear();
        shader_artifacts_.clear();
        dumped_shader_hash_count_ = 0;
    }
    frame_time_count_.store(0, std::memory_order_relaxed);
    frame_stutter_count_.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < kFrameTimeRingCap; ++i) {
        frame_times_us_[i].store(0, std::memory_order_relaxed);
    }
}

void FlightRecorder::Enable(const FlightRecorderOptions& opts) {
    StopWatchdog();
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    if (opts.reset_history) {
        ResetSession();
    }
    opts_ = opts;
    if (opts_.output_dir.empty()) {
        opts_.output_dir = "flight_logs";
    }
    std::error_code ec;
    const std::filesystem::path abs_out = std::filesystem::absolute(opts_.output_dir, ec);
    opts_.output_dir = abs_out.string();
    std::filesystem::create_directories(abs_out, ec);
    {
        ScopedSpinTryLock dlk(output_dir_lock_, 4096);
        CopyBoundedStr(output_dir_utf8_, sizeof(output_dir_utf8_), opts_.output_dir.c_str());
#if defined(_WIN32)
        const std::wstring wdir = abs_out.wstring();
        const size_t n = std::min<size_t>(wdir.size(), (sizeof(output_dir_wide_) / sizeof(wchar_t)) - 1u);
        if (n > 0) {
            std::memcpy(output_dir_wide_, wdir.data(), n * sizeof(wchar_t));
        }
        output_dir_wide_[n] = L'\0';
#endif
    }

    dred_enabled_flag_.store(opts_.enable_dred, std::memory_order_relaxed);
    shader_dump_flag_.store(opts_.dump_failing_shaders, std::memory_order_relaxed);
    last_heartbeat_qpc_.store(NowQpc(), std::memory_order_relaxed);

#if defined(_WIN32)
    if (opts_.install_veh && !veh_handle_) {
        // First=1 ensures hardware faults inside guest images are captured with exact RIP/RSP
        // before C++ or OS unwind mutates registers, while benign software exceptions are filtered.
        veh_handle_ = AddVectoredExceptionHandler(1, &FlightVectoredExceptionHandler);
    }
#endif

    s_enabled_state_.store(2u, std::memory_order_release);
    RecordEvent(FlightEventCategory::Lifecycle, "flight_recorder",
                ("Enabled -> " + opts_.output_dir).c_str(), 0, 0, opts_.hang_timeout_ms, 0);

    if (opts_.hang_timeout_ms > 0) {
        StartWatchdogLocked(opts_.hang_timeout_ms);
    }
}

void FlightRecorder::Disable() noexcept {
    s_enabled_state_.store(1u, std::memory_order_release);
    StopWatchdog();
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
#if defined(_WIN32)
    if (veh_handle_) {
        RemoveVectoredExceptionHandler(veh_handle_);
        veh_handle_ = nullptr;
    }
#endif
}

std::string FlightRecorder::OutputDirectory() const {
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    return opts_.output_dir;
}

void FlightRecorder::StartWatchdogLocked(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> wlk(watchdog_mu_);
    if (watchdog_thread_.joinable()) {
        return;
    }
    watchdog_stop_ = false;
    watchdog_thread_ = std::thread(&FlightRecorder::WatchdogLoop, this, timeout_ms);
}

void FlightRecorder::StopWatchdog() noexcept {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> wlk(watchdog_mu_);
        watchdog_stop_ = true;
        watchdog_cv_.notify_all();
        if (watchdog_thread_.joinable()) {
            to_join = std::move(watchdog_thread_);
        }
    }
    if (to_join.joinable()) {
        if (to_join.get_id() == std::this_thread::get_id()) {
            to_join.detach();
        } else {
            to_join.join();
        }
    }
}

void FlightRecorder::WatchdogLoop(uint32_t timeout_ms) {
    const uint32_t poll_ms = std::clamp<uint32_t>(timeout_ms / 4u, 10u, 250u);
    std::unique_lock<std::mutex> wlk(watchdog_mu_);
    while (!watchdog_stop_) {
        watchdog_cv_.wait_for(wlk, std::chrono::milliseconds(poll_ms));
        if (watchdog_stop_ || s_enabled_state_.load(std::memory_order_relaxed) != 2u) {
            break;
        }
        const int64_t last_qpc = last_heartbeat_qpc_.load(std::memory_order_relaxed);
        const int64_t now_qpc = NowQpc();
        const uint64_t elapsed_ms = (now_qpc > last_qpc)
            ? static_cast<uint64_t>(((now_qpc - last_qpc) * 1000ll) / QpcFreq())
            : 0ull;
        if (elapsed_ms >= timeout_ms) {
            const uint64_t hb = heartbeat_seq_.load(std::memory_order_relaxed);
            uint64_t prev_hung = last_hung_heartbeat_seq_.load(std::memory_order_relaxed);
            if (prev_hung != hb && hang_seq_.load(std::memory_order_relaxed) < kMaxHangSnapshots) {
                last_hung_heartbeat_seq_.store(hb, std::memory_order_relaxed);
                wlk.unlock();
                CaptureHangSnapshot("WatchdogStallTimeout", nullptr);
                wlk.lock();
            }
        }
    }
}

void FlightRecorder::RecordEvent(FlightEventCategory category,
                                 const char* subsystem,
                                 const char* detail,
                                 int32_t status_code,
                                 uint64_t caller_rip,
                                 uint64_t arg0,
                                 uint64_t arg1) noexcept {
    if (!IsEnabled()) return;

    const size_t cat_idx = static_cast<size_t>(category);
    if (cat_idx < static_cast<size_t>(FlightEventCategory::Count)) {
        category_counts_[cat_idx].fetch_add(1, std::memory_order_relaxed);
    }

    const uint64_t seq = write_seq_.fetch_add(1, std::memory_order_relaxed) + 1ull;
    const size_t idx = static_cast<size_t>((seq - 1ull) & (kRingCapacity - 1u));

    committed_seq_[idx].store(0ull, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acq_rel);

    FlightEvent& ev = ring_[idx];
    ev.seq = seq;
    ev.timestamp_us = ElapsedUs();
    ev.thread_id = CurrentTid();
    ev.category = category;
    ev.status_code = status_code;
    ev.caller_rip = caller_rip;
    ev.arg0 = arg0;
    ev.arg1 = arg1;
    CopyBoundedStr(ev.subsystem, sizeof(ev.subsystem), subsystem);
    CopyBoundedStr(ev.detail, sizeof(ev.detail), detail);

    committed_seq_[idx].store(seq, std::memory_order_release);
}

void FlightRecorder::RecordStubHit(const char* dll_name,
                                   const char* symbol_name,
                                   bool by_ordinal,
                                   uint16_t ordinal,
                                   const uint64_t* args4,
                                   uint64_t return_value,
                                   uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;

    char key[96] = {};
    if (by_ordinal) {
        std::snprintf(key, sizeof(key), "%s!#%u", dll_name ? dll_name : "?", static_cast<unsigned>(ordinal));
    } else {
        std::snprintf(key, sizeof(key), "%s!%s", dll_name ? dll_name : "?", symbol_name ? symbol_name : "?");
    }

    const uint64_t a0 = args4 ? args4[0] : 0ull;
    const uint64_t a1 = args4 ? args4[1] : 0ull;
    RecordEvent(FlightEventCategory::StubHit, "stub_thunk", key,
                static_cast<int32_t>(return_value), caller_rip, a0, a1);

    ScopedSpinTryLock lk(miss_lock_);
    if (!lk.owns_lock()) return;
    for (uint32_t i = 0; i < stub_miss_count_; ++i) {
        if (std::strcmp(stub_misses_[i].module_and_symbol, key) == 0) {
            stub_misses_[i].hit_count += 1ull;
            if (args4) {
                for (int k = 0; k < 4; ++k) stub_misses_[i].last_args[k] = args4[k];
            }
            return;
        }
    }
    if (stub_miss_count_ < kMaxUniqueMisses) {
        auto& dst = stub_misses_[stub_miss_count_++];
        CopyBoundedStr(dst.module_and_symbol, sizeof(dst.module_and_symbol), key);
        dst.hit_count = 1ull;
        dst.first_caller_rip = caller_rip;
        dst.return_value = return_value;
        if (args4) {
            for (int k = 0; k < 4; ++k) dst.last_args[k] = args4[k];
        } else {
            std::memset(dst.last_args, 0, sizeof(dst.last_args));
        }
    }
}

void FlightRecorder::RecordComQiMiss(const char* interface_tag,
                                     const void* iid_16bytes,
                                     uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;

    char iid_str[40] = {};
    FormatGuidBytes(iid_16bytes, iid_str);

    char detail[64] = {};
    std::snprintf(detail, sizeof(detail), "%s:%s", interface_tag ? interface_tag : "COM", iid_str);
    RecordEvent(FlightEventCategory::ComQiMiss,
                interface_tag ? interface_tag : "com_qi",
                iid_str,
                static_cast<int32_t>(0x80004002u),
                caller_rip);

    ScopedSpinTryLock lk(miss_lock_);
    if (!lk.owns_lock()) return;
    const char* tag = interface_tag ? interface_tag : "COM";
    for (uint32_t i = 0; i < com_qi_miss_count_; ++i) {
        if (std::strcmp(com_qi_misses_[i].iid_str, iid_str) == 0 &&
            std::strcmp(com_qi_misses_[i].interface_tag, tag) == 0) {
            com_qi_misses_[i].hit_count += 1ull;
            return;
        }
    }
    if (com_qi_miss_count_ < kMaxUniqueMisses) {
        auto& dst = com_qi_misses_[com_qi_miss_count_++];
        CopyBoundedStr(dst.interface_tag, sizeof(dst.interface_tag), tag);
        CopyBoundedStr(dst.iid_str, sizeof(dst.iid_str), iid_str);
        dst.hit_count = 1ull;
        dst.first_caller_rip = caller_rip;
    }
}

void FlightRecorder::RecordVtableSlotMiss(const char* interface_tag,
                                          int32_t slot,
                                          uint64_t arg0,
                                          uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;

    char detail[64] = {};
    std::snprintf(detail, sizeof(detail), "%s::vtable[%d] (+0x%X)",
                  interface_tag ? interface_tag : "COM", slot, static_cast<unsigned>(slot * 8));
    RecordEvent(FlightEventCategory::VtableSlotMiss,
                interface_tag ? interface_tag : "com_vtbl",
                detail,
                static_cast<int32_t>(0x80004001u),
                caller_rip,
                arg0,
                static_cast<uint64_t>(slot));

    ScopedSpinTryLock lk(miss_lock_);
    if (!lk.owns_lock()) return;
    const char* tag = interface_tag ? interface_tag : "COM";
    for (uint32_t i = 0; i < vtable_miss_count_; ++i) {
        if (vtable_misses_[i].slot == slot &&
            std::strcmp(vtable_misses_[i].interface_tag, tag) == 0) {
            vtable_misses_[i].hit_count += 1ull;
            vtable_misses_[i].last_arg0 = arg0;
            return;
        }
    }
    if (vtable_miss_count_ < kMaxUniqueMisses) {
        auto& dst = vtable_misses_[vtable_miss_count_++];
        CopyBoundedStr(dst.interface_tag, sizeof(dst.interface_tag), tag);
        dst.slot = slot;
        dst.hit_count = 1ull;
        dst.first_caller_rip = caller_rip;
        dst.last_arg0 = arg0;
    }
}

void FlightRecorder::RecordWinRtActivation(const char* api_name,
                                           const wchar_t* runtime_class_name,
                                           int32_t hr,
                                           bool handled_by_hle,
                                           uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;
    char cls_utf8[64] = {};
    if (runtime_class_name && *runtime_class_name) {
        size_t i = 0;
        while (i + 1 < sizeof(cls_utf8) && runtime_class_name[i] != L'\0') {
            const wchar_t wc = runtime_class_name[i];
            cls_utf8[i] = (wc >= 0x20 && wc < 0x7F) ? static_cast<char>(wc) : '?';
            ++i;
        }
        cls_utf8[i] = '\0';
    } else {
        CopyBoundedStr(cls_utf8, sizeof(cls_utf8), "<null>");
    }
    RecordEvent(FlightEventCategory::WinRtBroker,
                api_name ? api_name : "WinRT",
                cls_utf8,
                hr,
                caller_rip,
                handled_by_hle ? 1ull : 0ull,
                0ull);
}

void FlightRecorder::RecordVfsAccess(const char* virtual_path,
                                     bool resolved_ok,
                                     int32_t win32_or_hresult_err,
                                     uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;

    const char* path = (virtual_path && *virtual_path) ? virtual_path : "<empty>";
    const size_t len = std::strlen(path);
    const char* tail = (len >= 60u) ? (path + (len - 59u)) : path;

    if (resolved_ok) {
        RecordEvent(FlightEventCategory::VfsAccess, "vfs", tail, 0, caller_rip);
        return;
    }

    RecordEvent(FlightEventCategory::VfsMiss, "vfs", tail, win32_or_hresult_err, caller_rip);

    ScopedSpinTryLock lk(miss_lock_);
    if (!lk.owns_lock()) return;
    for (uint32_t i = 0; i < vfs_miss_count_; ++i) {
        if (std::strcmp(vfs_misses_[i].virtual_path, path) == 0) {
            vfs_misses_[i].hit_count += 1ull;
            vfs_misses_[i].error_code = win32_or_hresult_err;
            return;
        }
    }
    if (vfs_miss_count_ < kMaxUniqueMisses) {
        auto& dst = vfs_misses_[vfs_miss_count_++];
        CopyBoundedStr(dst.virtual_path, sizeof(dst.virtual_path), path);
        dst.error_code = win32_or_hresult_err;
        dst.hit_count = 1ull;
        dst.first_caller_rip = caller_rip;
    }
}

void FlightRecorder::RecordGpuBreadcrumb(uint64_t cmd_list_id,
                                         uint64_t marker_value,
                                         const char* tag) noexcept {
    if (!IsEnabled()) return;
    BeatProgress("GpuBreadcrumb");
    RecordEvent(FlightEventCategory::GpuBreadcrumb, "d3d12_xs",
                tag ? tag : "marker", 0, 0, cmd_list_id, marker_value);

    ScopedSpinTryLock lk(gpu_bc_lock_);
    if (!lk.owns_lock()) return;
    FlightGpuBreadcrumb& slot = recent_gpu_breadcrumbs_[static_cast<size_t>(total_gpu_breadcrumbs_ % 64ull)];
    slot.command_list_id = cmd_list_id;
    slot.marker_value = marker_value;
    CopyBoundedStr(slot.tag, sizeof(slot.tag), tag ? tag : "");
    total_gpu_breadcrumbs_ += 1ull;
}

void FlightRecorder::BeatProgress(const char* phase) noexcept {
    if (!IsEnabled()) return;
    last_heartbeat_qpc_.store(NowQpc(), std::memory_order_relaxed);
    heartbeat_seq_.fetch_add(1ull, std::memory_order_relaxed);
    if (phase && *phase) {
        ScopedSpinTryLock lk(phase_lock_, 8);
        if (lk.owns_lock()) {
            CopyBoundedStr(last_phase_, sizeof(last_phase_), phase);
        }
    }
}

namespace {

#if defined(_WIN32)
bool TryExtractPeExportModuleNameLockFree(uint64_t module_base, char (&out_name)[64]) noexcept {
    out_name[0] = '\0';
    if (!IsCommittedReadableAddress(module_base, sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, reinterpret_cast<const void*>(module_base), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) {
        return false;
    }
    const uint64_t nt_va = module_base + static_cast<uint64_t>(dos.e_lfanew);
    if (!IsCommittedReadableAddress(nt_va, sizeof(IMAGE_NT_HEADERS64))) {
        return false;
    }
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, reinterpret_cast<const void*>(nt_va), sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
        return false;
    }
    const auto& exp_dd = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exp_dd.VirtualAddress == 0 || exp_dd.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        return false;
    }
    const uint64_t exp_va = module_base + exp_dd.VirtualAddress;
    if (!IsCommittedReadableAddress(exp_va, sizeof(IMAGE_EXPORT_DIRECTORY))) {
        return false;
    }
    IMAGE_EXPORT_DIRECTORY exp{};
    std::memcpy(&exp, reinterpret_cast<const void*>(exp_va), sizeof(exp));
    if (exp.Name == 0 || exp.Name >= nt.OptionalHeader.SizeOfImage) {
        return false;
    }
    const uint64_t name_va = module_base + exp.Name;
    if (!IsCommittedReadableAddress(name_va, 1)) {
        return false;
    }
    size_t i = 0;
    while (i + 1 < sizeof(out_name) && IsCommittedReadableAddress(name_va + i, 1)) {
        char ch = '\0';
        std::memcpy(&ch, reinterpret_cast<const void*>(name_va + i), 1);
        if (ch == '\0') break;
        if (static_cast<unsigned char>(ch) < 0x20u || static_cast<unsigned char>(ch) >= 0x7Fu) {
            out_name[0] = '\0';
            return false;
        }
        out_name[i++] = ch;
    }
    out_name[i] = '\0';
    return i > 0;
}
#endif

}  // namespace

FlightResolvedAddress FlightRecorder::ResolveCodeAddressNoLoaderLock(uint64_t va) noexcept {
    FlightResolvedAddress out{};
    out.raw_va = va;
    if (va < 0x10000ull) {
        return out;
    }

    guest::GuestModuleAddrInfo ginfo{};
    if (guest::TryResolveGuestAddressLockFree(va, &ginfo)) {
        out.resolved = true;
        out.is_guest = true;
        out.is_guest_exe = ginfo.is_exe;
        out.module_base = ginfo.base;
        out.rva = ginfo.rva;
        out.ghidra_va = ginfo.ghidra_va;
        CopyBoundedStr(out.module_name, sizeof(out.module_name), ginfo.name);
        return out;
    }

#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(va), &mbi, sizeof(mbi)) != 0 &&
        mbi.State == MEM_COMMIT && mbi.AllocationBase != nullptr) {
        const uint64_t alloc_base = reinterpret_cast<uint64_t>(mbi.AllocationBase);
        const auto& self = Instance();
        if (alloc_base == self.host_exe_base_ && va >= alloc_base &&
            (va - alloc_base) <= 0x7FFFFFFFull) {
            out.resolved = true;
            out.is_guest = false;
            out.module_base = alloc_base;
            out.rva = static_cast<uint32_t>(va - alloc_base);
            CopyBoundedStr(out.module_name, sizeof(out.module_name), self.host_exe_name_);
            return out;
        }
        if (mbi.Type == MEM_IMAGE && va >= alloc_base && (va - alloc_base) <= 0x7FFFFFFFull) {
            out.resolved = true;
            out.is_guest = false;
            out.module_base = alloc_base;
            out.rva = static_cast<uint32_t>(va - alloc_base);
            if (!TryExtractPeExportModuleNameLockFree(alloc_base, out.module_name)) {
                CopyBoundedStr(out.module_name, sizeof(out.module_name), "host_module");
            }
            return out;
        }
    }
#endif
    return out;
}

FlightResolvedAddress FlightRecorder::ResolveCodeAddress(uint64_t va) noexcept {
    FlightResolvedAddress out = ResolveCodeAddressNoLoaderLock(va);
#if defined(_WIN32)
    if (out.resolved && !out.is_guest && std::strcmp(out.module_name, "host_module") == 0) {
        char full_path[MAX_PATH + 1] = {};
        const DWORD n = GetModuleFileNameA(
            reinterpret_cast<HMODULE>(out.module_base), full_path, MAX_PATH);
        if (n > 0) {
            const char* slash = full_path;
            for (DWORD i = 0; i < n; ++i) {
                if (full_path[i] == '\\' || full_path[i] == '/') {
                    slash = full_path + i + 1;
                }
            }
            CopyBoundedStr(out.module_name, sizeof(out.module_name), slash);
        }
    }
#endif
    return out;
}

std::string FlightRecorder::FormatResolvedAddress(const FlightResolvedAddress& addr) {
    if (addr.raw_va == 0) return "0x0";
    char buf[192] = {};
    if (!addr.resolved) {
        std::snprintf(buf, sizeof(buf), "0x%llX (unmapped)", static_cast<unsigned long long>(addr.raw_va));
        return buf;
    }
    if (addr.is_guest) {
        std::snprintf(buf, sizeof(buf), "%s+0x%X [guest Ghidra=0x%llX, raw=0x%llX]",
                      addr.module_name,
                      addr.rva,
                      static_cast<unsigned long long>(addr.ghidra_va),
                      static_cast<unsigned long long>(addr.raw_va));
    } else {
        std::snprintf(buf, sizeof(buf), "%s+0x%X [0x%llX]",
                      addr.module_name,
                      addr.rva,
                      static_cast<unsigned long long>(addr.raw_va));
    }
    return buf;
}

std::vector<FlightEvent> FlightRecorder::SnapshotRecentEvents(size_t max_events) const {
    const uint64_t head = write_seq_.load(std::memory_order_acquire);
    if (head == 0 || max_events == 0) return {};

    const size_t window = std::min<size_t>(max_events, kRingCapacity);
    const uint64_t start_seq = (head > window) ? (head - window + 1ull) : 1ull;

    std::vector<FlightEvent> out;
    out.reserve(static_cast<size_t>(head - start_seq + 1ull));
    for (uint64_t s = start_seq; s <= head; ++s) {
        const size_t idx = static_cast<size_t>((s - 1ull) & (kRingCapacity - 1u));
        const uint64_t c1 = committed_seq_[idx].load(std::memory_order_acquire);
        if (c1 != s) continue;
        FlightEvent copy = ring_[idx];
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t c2 = committed_seq_[idx].load(std::memory_order_relaxed);
        if (c2 == s && copy.seq == s) {
            out.push_back(copy);
        }
    }
    return out;
}

void FlightRecorder::RecordFrameTimeUs(uint64_t frame_us) noexcept {
    if (!IsEnabled()) return;
    const uint64_t idx = frame_time_count_.fetch_add(1, std::memory_order_relaxed);
    frame_times_us_[idx % kFrameTimeRingCap].store(frame_us, std::memory_order_relaxed);
    if (frame_us > 33333ull) {
        frame_stutter_count_.fetch_add(1, std::memory_order_relaxed);
    }
    BeatProgress("PresentFrame");
}

FlightFramePacingSummary FlightRecorder::GetFramePacingSummary() const noexcept {
    FlightFramePacingSummary out;
    const uint64_t total = frame_time_count_.load(std::memory_order_relaxed);
    out.frames_recorded = total;
    out.stutter_count = frame_stutter_count_.load(std::memory_order_relaxed);
    if (total == 0) {
        return out;
    }
    const size_t n = static_cast<size_t>((std::min)(total, static_cast<uint64_t>(kFrameTimeRingCap)));
    std::vector<uint64_t> samples;
    samples.reserve(n);
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t v = frame_times_us_[i].load(std::memory_order_relaxed);
        samples.push_back(v);
        sum += v;
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) -> double {
        const size_t pos = static_cast<size_t>((std::min)(
            static_cast<double>(samples.size() - 1),
            std::floor(p * static_cast<double>(samples.size() - 1))));
        return static_cast<double>(samples[pos]) / 1000.0;
    };
    out.avg_ms = (static_cast<double>(sum) / static_cast<double>(n)) / 1000.0;
    out.p50_ms = pct(0.50);
    out.p95_ms = pct(0.95);
    out.p99_ms = pct(0.99);
    out.max_ms = static_cast<double>(samples.back()) / 1000.0;
    return out;
}

FlightRecorderStats FlightRecorder::GetStats() const noexcept {
    FlightRecorderStats st{};
    st.total_events = write_seq_.load(std::memory_order_relaxed);
    st.heartbeats = heartbeat_seq_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < static_cast<size_t>(FlightEventCategory::Count); ++i) {
        st.category_counts[i] = category_counts_[i].load(std::memory_order_relaxed);
    }
    {
        ScopedSpinTryLock lk(miss_lock_, 256);
        if (lk.owns_lock()) {
            st.unique_stub_misses = stub_miss_count_;
            st.unique_com_qi_misses = com_qi_miss_count_;
            st.unique_vtable_misses = vtable_miss_count_;
            st.unique_vfs_misses = vfs_miss_count_;
        }
    }
    st.crash_reports_written = crash_artifact_count_.load(std::memory_order_acquire);
    {
        std::unique_lock<std::mutex> alk(artifacts_mu_, std::try_to_lock);
        if (alk.owns_lock()) {
            st.hang_snapshots_written = static_cast<uint32_t>(hang_artifacts_.size());
            st.dred_reports_written = static_cast<uint32_t>(dred_artifacts_.size());
            st.shader_dumps_written = static_cast<uint32_t>(shader_artifacts_.size());
        } else {
            st.hang_snapshots_written = hang_seq_.load(std::memory_order_relaxed);
            st.dred_reports_written = dred_seq_.load(std::memory_order_relaxed);
            st.shader_dumps_written = shader_seq_.load(std::memory_order_relaxed);
        }
    }
    return st;
}

namespace {

#if defined(_WIN32)
// Pure POD buffered Win32 HANDLE writer: zero heap allocations, zero CRT FILE* locks.
struct PodWin32Writer {
    HANDLE handle = INVALID_HANDLE_VALUE;
    size_t len = 0;
    char buf[4096] = {};

    void Open(const wchar_t* path) noexcept {
        len = 0;
        handle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    void Flush() noexcept {
        if (handle != INVALID_HANDLE_VALUE && len > 0) {
            DWORD written = 0;
            (void)WriteFile(handle, buf, static_cast<DWORD>(len), &written, nullptr);
            len = 0;
        }
    }

    void Close() noexcept {
        Flush();
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }

    void PutChar(char c) noexcept {
        if (handle == INVALID_HANDLE_VALUE) return;
        buf[len++] = c;
        if (len == sizeof(buf)) Flush();
    }

    void PutBytes(const char* s, size_t n) noexcept {
        if (handle == INVALID_HANDLE_VALUE || !s || n == 0) return;
        for (size_t i = 0; i < n; ++i) PutChar(s[i]);
    }

    void PutStr(const char* s) noexcept {
        if (!s) return;
        while (*s) PutChar(*s++);
    }

    void PutU64(uint64_t v) noexcept {
        char tmp[24] = {};
        size_t n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + (v % 10ull));
            v /= 10ull;
        } while (v != 0 && n < sizeof(tmp));
        while (n > 0) PutChar(tmp[--n]);
    }

    void PutHexU64(uint64_t v) noexcept {
        static constexpr char kHex[] = "0123456789ABCDEF";
        PutStr("0x");
        if (v == 0) {
            PutChar('0');
            return;
        }
        char tmp[16] = {};
        size_t n = 0;
        while (v != 0 && n < sizeof(tmp)) {
            tmp[n++] = kHex[v & 0xFu];
            v >>= 4u;
        }
        while (n > 0) PutChar(tmp[--n]);
    }

    void PutHexU32(uint32_t v) noexcept {
        static constexpr char kHex[] = "0123456789ABCDEF";
        PutStr("0x");
        for (int shift = 28; shift >= 0; shift -= 4) {
            PutChar(kHex[(v >> shift) & 0xFu]);
        }
    }

    void PutJsonEscaped(const char* s) noexcept {
        if (!s) return;
        static constexpr char kHexLower[] = "0123456789abcdef";
        while (*s) {
            const unsigned char c = static_cast<unsigned char>(*s++);
            switch (c) {
                case '"': PutStr("\\\""); break;
                case '\\': PutStr("\\\\"); break;
                case '\b': PutStr("\\b"); break;
                case '\f': PutStr("\\f"); break;
                case '\n': PutStr("\\n"); break;
                case '\r': PutStr("\\r"); break;
                case '\t': PutStr("\\t"); break;
                default:
                    if (c < 0x20u) {
                        PutStr("\\u00");
                        PutChar(kHexLower[(c >> 4u) & 0xFu]);
                        PutChar(kHexLower[c & 0xFu]);
                    } else {
                        PutChar(static_cast<char>(c));
                    }
                    break;
            }
        }
    }
};

void FormatResolvedAddressPod(const FlightResolvedAddress& addr, char (&buf)[192]) noexcept {
    if (addr.raw_va == 0) {
        CopyBoundedStr(buf, sizeof(buf), "0x0");
        return;
    }
    PodWin32Writer dummy;
    // Format without CRT locale/heap using bounded manual formatting
    size_t pos = 0;
    auto push_ch = [&](char ch) {
        if (pos + 1 < sizeof(buf)) buf[pos++] = ch;
    };
    auto push_str = [&](const char* s) {
        if (!s) return;
        while (*s && pos + 1 < sizeof(buf)) buf[pos++] = *s++;
    };
    auto push_hex64 = [&](uint64_t v) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        push_str("0x");
        if (v == 0) {
            push_ch('0');
            return;
        }
        char tmp[16] = {};
        size_t n = 0;
        while (v != 0 && n < sizeof(tmp)) {
            tmp[n++] = kHex[v & 0xFu];
            v >>= 4u;
        }
        while (n > 0) push_ch(tmp[--n]);
    };
    (void)dummy;
    if (!addr.resolved) {
        push_hex64(addr.raw_va);
        push_str(" (unmapped)");
    } else if (addr.is_guest) {
        push_str(addr.module_name);
        push_ch('+');
        push_hex64(addr.rva);
        push_str(" [guest Ghidra=");
        push_hex64(addr.ghidra_va);
        push_str(", raw=");
        push_hex64(addr.raw_va);
        push_ch(']');
    } else {
        push_str(addr.module_name);
        push_ch('+');
        push_hex64(addr.rva);
        push_str(" [");
        push_hex64(addr.raw_va);
        push_ch(']');
    }
    buf[pos] = '\0';
}

struct CrashReportStaticScratch {
    static constexpr size_t kMaxFrames = 32;
    static constexpr size_t kMaxEvents = 64;
    static constexpr size_t kMaxStubs = 64;
    static constexpr size_t kMaxBcs = 64;

    uint64_t frame_rips[kMaxFrames];
    uint64_t frame_rsps[kMaxFrames];
    FlightEvent recent_events[kMaxEvents];
    FlightStubMissRecord stubs[kMaxStubs];
    FlightGpuBreadcrumb gpu_bcs[kMaxBcs];
    char dir_utf8[512];
    wchar_t dir_wide[260];
    wchar_t json_path_w[560];
    wchar_t txt_path_w[560];
    char json_path_utf8[560];
    PodWin32Writer writer;
};

std::atomic_flag g_crash_scratch_lock = ATOMIC_FLAG_INIT;
CrashReportStaticScratch g_crash_scratch{};

void AppendAsciiToWide(wchar_t* dst, size_t cap, size_t& pos, const char* s) noexcept {
    if (!dst || cap == 0 || !s) return;
    while (*s && pos + 1 < cap) {
        dst[pos++] = static_cast<wchar_t>(static_cast<unsigned char>(*s++));
    }
    dst[pos] = L'\0';
}

void AppendU32ToWide(wchar_t* dst, size_t cap, size_t& pos, uint32_t v) noexcept {
    char tmp[16] = {};
    size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && n < sizeof(tmp));
    while (n > 0 && pos + 1 < cap) {
        dst[pos++] = static_cast<wchar_t>(tmp[--n]);
    }
    dst[pos] = L'\0';
}

void AppendAsciiToChar(char* dst, size_t cap, size_t& pos, const char* s) noexcept {
    if (!dst || cap == 0 || !s) return;
    while (*s && pos + 1 < cap) {
        dst[pos++] = *s++;
    }
    dst[pos] = '\0';
}

void AppendU32ToChar(char* dst, size_t cap, size_t& pos, uint32_t v) noexcept {
    char tmp[16] = {};
    size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && n < sizeof(tmp));
    while (n > 0 && pos + 1 < cap) {
        dst[pos++] = tmp[--n];
    }
    dst[pos] = '\0';
}
#endif

}  // namespace

bool FlightRecorder::CaptureCrashReport(const void* win32_exception_pointers,
                                        const char* reason,
                                        std::string* out_json_path) noexcept {
#if defined(_WIN32)
    if (!win32_exception_pointers) return false;
    const auto* ep = static_cast<const EXCEPTION_POINTERS*>(win32_exception_pointers);
    if (!ep->ExceptionRecord || !ep->ContextRecord) return false;

    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const uint64_t fault_rip = ep->ContextRecord->Rip;
    const uint64_t sig = (static_cast<uint64_t>(code) << 32u) ^ (fault_rip * 1099511628211ull);
    const uint64_t prev_sig = last_crash_sig_.exchange(sig, std::memory_order_acq_rel);
    if (prev_sig == sig && out_json_path == nullptr) {
        // Avoid duplicate report when both first-chance VEH and outer SEH filter see the exact same fault.
        return false;
    }

    const uint32_t seq = crash_seq_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (seq > kMaxCrashReports) {
        crash_seq_.fetch_sub(1u, std::memory_order_relaxed);
        return false;
    }

    // Zero heap allocations, zero std::mutex locks, zero PEB Loader Lock calls,
    // and minimal stack footprint (safe inside STATUS_HEAP_CORRUPTION & STACK_OVERFLOW).
    ScopedSpinTryLock scratch_lk(g_crash_scratch_lock, 4096);
    if (!scratch_lk.owns_lock()) {
        crash_seq_.fetch_sub(1u, std::memory_order_relaxed);
        return false;
    }
    auto& sc = g_crash_scratch;

    {
        ScopedSpinTryLock dlk(output_dir_lock_, 64);
        if (dlk.owns_lock()) {
            CopyBoundedStr(sc.dir_utf8, sizeof(sc.dir_utf8), output_dir_utf8_);
            std::memcpy(sc.dir_wide, output_dir_wide_, sizeof(sc.dir_wide));
        } else {
            CopyBoundedStr(sc.dir_utf8, sizeof(sc.dir_utf8), "flight_logs");
            std::memcpy(sc.dir_wide, L"flight_logs", 12 * sizeof(wchar_t));
        }
    }
    CreateDirectoryW(sc.dir_wide, nullptr);

    const uint32_t pid = CurrentPid();
    const uint32_t tid = CurrentTid();
    const FlightResolvedAddress resolved_rip = ResolveCodeAddressNoLoaderLock(fault_rip);
    char resolved_rip_str[192] = {};
    FormatResolvedAddressPod(resolved_rip, resolved_rip_str);

    const char* av_kind = "n/a";
    uint64_t av_target_va = 0;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
        av_kind = (op == 0) ? "READ" : (op == 1 ? "WRITE" : (op == 8 ? "EXECUTE_DEP" : "OTHER"));
        av_target_va = static_cast<uint64_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    }

    if (IsEnabledNoProbe()) {
        RecordEvent(FlightEventCategory::GuestFault,
                    "crash_handler",
                    ExceptionCodeName(code),
                    static_cast<int32_t>(code),
                    fault_rip,
                    av_target_va,
                    seq);
    }

    const size_t frame_count = UnwindX64FramesPod(
        ep->ContextRecord, sc.frame_rips, sc.frame_rsps, CrashReportStaticScratch::kMaxFrames);

    size_t event_count = 0;
    {
        const uint64_t head = write_seq_.load(std::memory_order_acquire);
        if (head > 0) {
            const uint64_t start_seq =
                (head > CrashReportStaticScratch::kMaxEvents)
                    ? (head - CrashReportStaticScratch::kMaxEvents + 1ull)
                    : 1ull;
            for (uint64_t s = start_seq; s <= head && event_count < CrashReportStaticScratch::kMaxEvents; ++s) {
                const size_t idx = static_cast<size_t>((s - 1ull) & (kRingCapacity - 1u));
                const uint64_t c1 = committed_seq_[idx].load(std::memory_order_acquire);
                if (c1 != s) continue;
                FlightEvent copy = ring_[idx];
                std::atomic_thread_fence(std::memory_order_acquire);
                const uint64_t c2 = committed_seq_[idx].load(std::memory_order_relaxed);
                if (c2 == s && copy.seq == s) {
                    sc.recent_events[event_count++] = copy;
                }
            }
        }
    }

    size_t stub_count = 0;
    {
        ScopedSpinTryLock mlk(miss_lock_, 64);
        if (mlk.owns_lock()) {
            stub_count = std::min<size_t>(stub_miss_count_, CrashReportStaticScratch::kMaxStubs);
            for (size_t i = 0; i < stub_count; ++i) {
                sc.stubs[i] = stub_misses_[i];
            }
        }
    }

    size_t bc_count = 0;
    {
        ScopedSpinTryLock blk(gpu_bc_lock_, 64);
        if (blk.owns_lock() && total_gpu_breadcrumbs_ > 0) {
            bc_count = static_cast<size_t>(
                std::min<uint64_t>(total_gpu_breadcrumbs_, CrashReportStaticScratch::kMaxBcs));
            for (size_t i = 0; i < bc_count; ++i) {
                const uint64_t bidx = (total_gpu_breadcrumbs_ > 64ull)
                    ? (total_gpu_breadcrumbs_ - bc_count + i)
                    : i;
                sc.gpu_bcs[i] = recent_gpu_breadcrumbs_[static_cast<size_t>(bidx % 64ull)];
            }
        }
    }

    char phase_copy[48] = "init";
    {
        ScopedSpinTryLock plk(phase_lock_, 32);
        if (plk.owns_lock()) {
            CopyBoundedStr(phase_copy, sizeof(phase_copy), last_phase_);
        }
    }

    // Build wide & UTF-8 file paths without heap allocation
    size_t wpos = 0;
    while (sc.dir_wide[wpos] != L'\0' && wpos + 1 < 560) {
        sc.json_path_w[wpos] = sc.dir_wide[wpos];
        sc.txt_path_w[wpos] = sc.dir_wide[wpos];
        ++wpos;
    }
    if (wpos > 0 && sc.json_path_w[wpos - 1] != L'\\' && sc.json_path_w[wpos - 1] != L'/') {
        sc.json_path_w[wpos] = L'\\';
        sc.txt_path_w[wpos] = L'\\';
        ++wpos;
    }
    size_t jw = wpos;
    AppendAsciiToWide(sc.json_path_w, 560, jw, "crash_report_");
    AppendU32ToWide(sc.json_path_w, 560, jw, pid);
    AppendAsciiToWide(sc.json_path_w, 560, jw, "_");
    AppendU32ToWide(sc.json_path_w, 560, jw, seq);
    AppendAsciiToWide(sc.json_path_w, 560, jw, ".json");

    size_t tw = wpos;
    AppendAsciiToWide(sc.txt_path_w, 560, tw, "crash_report_");
    AppendU32ToWide(sc.txt_path_w, 560, tw, pid);
    AppendAsciiToWide(sc.txt_path_w, 560, tw, "_");
    AppendU32ToWide(sc.txt_path_w, 560, tw, seq);
    AppendAsciiToWide(sc.txt_path_w, 560, tw, ".txt");

    size_t cpos = 0;
    AppendAsciiToChar(sc.json_path_utf8, sizeof(sc.json_path_utf8), cpos, sc.dir_utf8);
    if (cpos > 0 && sc.json_path_utf8[cpos - 1] != '\\' && sc.json_path_utf8[cpos - 1] != '/') {
        AppendAsciiToChar(sc.json_path_utf8, sizeof(sc.json_path_utf8), cpos, "\\");
    }
    AppendAsciiToChar(sc.json_path_utf8, sizeof(sc.json_path_utf8), cpos, "crash_report_");
    AppendU32ToChar(sc.json_path_utf8, sizeof(sc.json_path_utf8), cpos, pid);
    AppendAsciiToChar(sc.json_path_utf8, sizeof(sc.json_path_utf8), cpos, "_");
    AppendU32ToChar(sc.json_path_utf8, sizeof(sc.json_path_utf8), cpos, seq);
    AppendAsciiToChar(sc.json_path_utf8, sizeof(sc.json_path_utf8), cpos, ".json");

    const CONTEXT& c = *ep->ContextRecord;
    bool wrote_json = false;
    sc.writer.Open(sc.json_path_w);
    if (sc.writer.handle != INVALID_HANDLE_VALUE) {
        auto& w = sc.writer;
        w.PutStr("{\n  \"report_type\": \"crash_report\",\n  \"pid\": ");
        w.PutU64(pid);
        w.PutStr(",\n  \"tid\": ");
        w.PutU64(tid);
        w.PutStr(",\n  \"sequence\": ");
        w.PutU64(seq);
        w.PutStr(",\n  \"reason\": \"");
        w.PutJsonEscaped(reason ? reason : "Exception");
        w.PutStr("\",\n  \"last_phase\": \"");
        w.PutJsonEscaped(phase_copy);
        w.PutStr("\",\n  \"exception_code\": \"");
        w.PutHexU32(code);
        w.PutStr("\",\n  \"exception_name\": \"");
        w.PutStr(ExceptionCodeName(code));
        w.PutStr("\",\n  \"access_type\": \"");
        w.PutStr(av_kind);
        w.PutStr("\",\n  \"fault_target_va\": \"");
        w.PutHexU64(av_target_va);
        w.PutStr("\",\n  \"fault_rip\": \"");
        w.PutHexU64(fault_rip);
        w.PutStr("\",\n  \"fault_symbol\": \"");
        w.PutJsonEscaped(resolved_rip_str);
        w.PutStr("\",\n  \"fault_module\": \"");
        w.PutJsonEscaped(resolved_rip.module_name);
        w.PutStr("\",\n  \"fault_rva\": \"");
        w.PutHexU32(resolved_rip.rva);
        w.PutStr("\",\n  \"fault_ghidra_va\": \"");
        w.PutHexU64(resolved_rip.ghidra_va);
        w.PutStr("\",\n  \"is_guest_code\": ");
        w.PutStr(resolved_rip.is_guest ? "true" : "false");
        w.PutStr(",\n  \"registers\": {\n    \"rax\": \"");
        w.PutHexU64(c.Rax); w.PutStr("\", \"rbx\": \""); w.PutHexU64(c.Rbx);
        w.PutStr("\",\n    \"rcx\": \"");
        w.PutHexU64(c.Rcx); w.PutStr("\", \"rdx\": \""); w.PutHexU64(c.Rdx);
        w.PutStr("\",\n    \"rsi\": \"");
        w.PutHexU64(c.Rsi); w.PutStr("\", \"rdi\": \""); w.PutHexU64(c.Rdi);
        w.PutStr("\",\n    \"rbp\": \"");
        w.PutHexU64(c.Rbp); w.PutStr("\", \"rsp\": \""); w.PutHexU64(c.Rsp);
        w.PutStr("\",\n    \"r8\": \"");
        w.PutHexU64(c.R8); w.PutStr("\", \"r9\": \""); w.PutHexU64(c.R9);
        w.PutStr("\",\n    \"r10\": \"");
        w.PutHexU64(c.R10); w.PutStr("\", \"r11\": \""); w.PutHexU64(c.R11);
        w.PutStr("\",\n    \"r12\": \"");
        w.PutHexU64(c.R12); w.PutStr("\", \"r13\": \""); w.PutHexU64(c.R13);
        w.PutStr("\",\n    \"r14\": \"");
        w.PutHexU64(c.R14); w.PutStr("\", \"r15\": \""); w.PutHexU64(c.R15);
        w.PutStr("\",\n    \"rip\": \"");
        w.PutHexU64(c.Rip); w.PutStr("\", \"eflags\": \""); w.PutHexU32(c.EFlags);
        w.PutStr("\"\n  },\n  \"stack_frames\": [\n");
        char fmt_buf[192] = {};
        for (size_t i = 0; i < frame_count; ++i) {
            const auto fa = ResolveCodeAddressNoLoaderLock(sc.frame_rips[i]);
            FormatResolvedAddressPod(fa, fmt_buf);
            w.PutStr("    {\"index\": "); w.PutU64(i);
            w.PutStr(", \"rip\": \""); w.PutHexU64(sc.frame_rips[i]);
            w.PutStr("\", \"rsp\": \""); w.PutHexU64(sc.frame_rsps[i]);
            w.PutStr("\", \"module\": \""); w.PutJsonEscaped(fa.module_name);
            w.PutStr("\", \"rva\": \""); w.PutHexU32(fa.rva);
            w.PutStr("\", \"ghidra_va\": \""); w.PutHexU64(fa.ghidra_va);
            w.PutStr("\", \"is_guest\": "); w.PutStr(fa.is_guest ? "true" : "false");
            w.PutStr(", \"formatted\": \""); w.PutJsonEscaped(fmt_buf);
            w.PutStr(i + 1 < frame_count ? "\"},\n" : "\"}\n");
        }
        w.PutStr("  ],\n  \"gpu_breadcrumbs\": [\n");
        for (size_t i = 0; i < bc_count; ++i) {
            w.PutStr("    {\"cmd_list\": "); w.PutU64(sc.gpu_bcs[i].command_list_id);
            w.PutStr(", \"marker\": "); w.PutU64(sc.gpu_bcs[i].marker_value);
            w.PutStr(", \"tag\": \""); w.PutJsonEscaped(sc.gpu_bcs[i].tag);
            w.PutStr(i + 1 < bc_count ? "\"},\n" : "\"}\n");
        }
        w.PutStr("  ],\n  \"stub_misses\": [\n");
        for (size_t i = 0; i < stub_count; ++i) {
            const auto ca = ResolveCodeAddressNoLoaderLock(sc.stubs[i].first_caller_rip);
            FormatResolvedAddressPod(ca, fmt_buf);
            w.PutStr("    {\"symbol\": \""); w.PutJsonEscaped(sc.stubs[i].module_and_symbol);
            w.PutStr("\", \"hits\": "); w.PutU64(sc.stubs[i].hit_count);
            w.PutStr(", \"caller\": \""); w.PutJsonEscaped(fmt_buf);
            w.PutStr(i + 1 < stub_count ? "\"},\n" : "\"}\n");
        }
        w.PutStr("  ],\n  \"recent_events\": [\n");
        for (size_t i = 0; i < event_count; ++i) {
            const auto& ev = sc.recent_events[i];
            const auto ca = ResolveCodeAddressNoLoaderLock(ev.caller_rip);
            FormatResolvedAddressPod(ca, fmt_buf);
            w.PutStr("    {\"seq\": "); w.PutU64(ev.seq);
            w.PutStr(", \"t_us\": "); w.PutU64(ev.timestamp_us);
            w.PutStr(", \"tid\": "); w.PutU64(ev.thread_id);
            w.PutStr(", \"cat\": \""); w.PutStr(FlightEventCategoryName(ev.category));
            w.PutStr("\", \"sub\": \""); w.PutJsonEscaped(ev.subsystem);
            w.PutStr("\", \"detail\": \""); w.PutJsonEscaped(ev.detail);
            w.PutStr("\", \"status\": \""); w.PutHexU32(static_cast<uint32_t>(ev.status_code));
            w.PutStr("\", \"caller\": \""); w.PutJsonEscaped(fmt_buf);
            w.PutStr(i + 1 < event_count ? "\"},\n" : "\"}\n");
        }
        w.PutStr("  ]\n}\n");
        wrote_json = true;
    }
    sc.writer.Close();

    sc.writer.Open(sc.txt_path_w);
    if (sc.writer.handle != INVALID_HANDLE_VALUE) {
        auto& w = sc.writer;
        w.PutStr("=== ScarlettEdge Flight-Recorder Crash Report ===\nPID / TID:      ");
        w.PutU64(pid); w.PutStr(" / "); w.PutU64(tid);
        w.PutStr("\nReason:         "); w.PutStr(reason ? reason : "Exception");
        w.PutStr(" (phase="); w.PutStr(phase_copy);
        w.PutStr(")\nException:      "); w.PutHexU32(code);
        w.PutStr(" ("); w.PutStr(ExceptionCodeName(code));
        w.PutStr(")\nAccess Fault:   "); w.PutStr(av_kind);
        w.PutStr(" @ "); w.PutHexU64(av_target_va);
        w.PutStr("\nFaulting RIP:   "); w.PutStr(resolved_rip_str);
        w.PutStr("\nRegisters:      RAX="); w.PutHexU64(c.Rax);
        w.PutStr(" RBX="); w.PutHexU64(c.Rbx);
        w.PutStr(" RCX="); w.PutHexU64(c.Rcx);
        w.PutStr(" RDX="); w.PutHexU64(c.Rdx);
        w.PutStr("\n                RSI="); w.PutHexU64(c.Rsi);
        w.PutStr(" RDI="); w.PutHexU64(c.Rdi);
        w.PutStr(" RBP="); w.PutHexU64(c.Rbp);
        w.PutStr(" RSP="); w.PutHexU64(c.Rsp);
        w.PutStr("\n                R8="); w.PutHexU64(c.R8);
        w.PutStr(" R9="); w.PutHexU64(c.R9);
        w.PutStr(" R10="); w.PutHexU64(c.R10);
        w.PutStr(" R11="); w.PutHexU64(c.R11);
        w.PutStr("\n\nStack Trace ("); w.PutU64(frame_count); w.PutStr(" frames):\n");
        char fmt_buf[192] = {};
        for (size_t i = 0; i < frame_count; ++i) {
            FormatResolvedAddressPod(ResolveCodeAddressNoLoaderLock(sc.frame_rips[i]), fmt_buf);
            w.PutStr("  ["); w.PutU64(i); w.PutStr("] ");
            w.PutStr(fmt_buf);
            w.PutStr(" (RSP="); w.PutHexU64(sc.frame_rsps[i]); w.PutStr(")\n");
        }
    }
    sc.writer.Close();

    if (!wrote_json) {
        crash_seq_.fetch_sub(1u, std::memory_order_relaxed);
        return false;
    }

    const uint32_t art_idx = seq - 1u;
    if (art_idx < kMaxCrashReports) {
        CopyBoundedStr(crash_artifact_paths_[art_idx], sizeof(crash_artifact_paths_[art_idx]), sc.json_path_utf8);
        uint32_t cur_cnt = crash_artifact_count_.load(std::memory_order_relaxed);
        while (cur_cnt < seq &&
               !crash_artifact_count_.compare_exchange_weak(cur_cnt, seq, std::memory_order_release)) {
        }
    }
    if (out_json_path) {
        *out_json_path = sc.json_path_utf8;
    }
    if (HANDLE err_h = GetStdHandle(STD_ERROR_HANDLE);
        err_h != nullptr && err_h != INVALID_HANDLE_VALUE) {
        char msg[640] = {};
        size_t mp = 0;
        AppendAsciiToChar(msg, sizeof(msg), mp, "[ScarlettX][FlightRecorder] Wrote post-mortem crash dump: ");
        AppendAsciiToChar(msg, sizeof(msg), mp, sc.json_path_utf8);
        AppendAsciiToChar(msg, sizeof(msg), mp, "\n");
        DWORD wr = 0;
        (void)WriteFile(err_h, msg, static_cast<DWORD>(mp), &wr, nullptr);
    }
    return true;
#else
    (void)win32_exception_pointers;
    (void)reason;
    (void)out_json_path;
    return false;
#endif
}

void FlightRecorder::EnableD3D12DredIfConfigured() noexcept {
#if defined(_WIN32) && defined(__ID3D12DeviceRemovedExtendedDataSettings_INTERFACE_DEFINED__)
    const bool env_dred = []() noexcept {
        const char* v = std::getenv("SCARLETTX_DRED");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (!env_dred && (!IsEnabled() || !dred_enabled_flag_.load(std::memory_order_relaxed))) {
        return;
    }
    bool expected = false;
    if (!dred_applied_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    HMODULE d3d12_mod = GetSystemD3D12Module();
    if (!d3d12_mod) return;
    using GetDbgFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto get_dbg = reinterpret_cast<GetDbgFn>(
        reinterpret_cast<void*>(GetProcAddress(d3d12_mod, "D3D12GetDebugInterface")));
    if (!get_dbg) return;

    ID3D12DeviceRemovedExtendedDataSettings* dred_settings = nullptr;
    if (SUCCEEDED(get_dbg(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                          reinterpret_cast<void**>(&dred_settings))) &&
        dred_settings) {
        dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->Release();
        RecordEvent(FlightEventCategory::Lifecycle, "d3d12_dred",
                    "Enabled DRED AutoBreadcrumbs + PageFault", 0);
    }
#endif
}

std::string FlightRecorder::RecordGpuDeviceRemoved(void* native_id3d12_device,
                                                   int32_t triggering_hr,
                                                   const char* context_site,
                                                   const FlightGpuBreadcrumb* breadcrumbs,
                                                   size_t breadcrumb_count) noexcept {
    if (!IsEnabled()) return {};

    const uint32_t seq = dred_seq_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (seq > kMaxDredReports) {
        dred_seq_.fetch_sub(1u, std::memory_order_relaxed);
        return {};
    }

    try {
        int32_t removed_reason = triggering_hr;
        uint64_t page_fault_va = 0;
        bool has_dred_breadcrumbs = false;
        bool has_dred_page_fault = false;
        std::vector<std::string> dred_nodes;
        std::vector<std::string> fault_allocs;

#if defined(_WIN32)
        if (native_id3d12_device) {
            auto* dev = static_cast<ID3D12Device*>(native_id3d12_device);
            const HRESULT dev_reason = dev->GetDeviceRemovedReason();
            if (dev_reason != S_OK) {
                removed_reason = static_cast<int32_t>(dev_reason);
            }
#  if defined(__ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__)
            ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1),
                                              reinterpret_cast<void**>(&dred1))) &&
                dred1) {
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc_out{};
                if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc_out)) && bc_out.pHeadAutoBreadcrumbNode) {
                    has_dred_breadcrumbs = true;
                    const D3D12_AUTO_BREADCRUMB_NODE1* node = bc_out.pHeadAutoBreadcrumbNode;
                    for (size_t n = 0; node && n < 16; ++n, node = node->pNext) {
                        const std::string cl_name = node->pCommandListDebugNameW
                            ? WideToUtf8Safe(node->pCommandListDebugNameW)
                            : (node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "<unnamed_cl>");
                        const std::string cq_name = node->pCommandQueueDebugNameW
                            ? WideToUtf8Safe(node->pCommandQueueDebugNameW)
                            : (node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "<unnamed_q>");
                        const uint32_t completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
                        uint32_t last_op = 0u;
                        if (node->pCommandHistory && node->BreadcrumbCount > 0) {
                            const uint32_t idx = std::min<uint32_t>(completed, node->BreadcrumbCount - 1u);
                            last_op = static_cast<uint32_t>(node->pCommandHistory[idx]);
                        }
                        std::ostringstream ns;
                        ns << "{\"cmd_list\":\"" << JsonEscape(cl_name)
                           << "\",\"queue\":\"" << JsonEscape(cq_name)
                           << "\",\"completed\":" << completed
                           << ",\"total\":" << node->BreadcrumbCount
                           << ",\"last_op\":" << last_op << "}";
                        dred_nodes.push_back(ns.str());
                    }
                }

                D3D12_DRED_PAGE_FAULT_OUTPUT1 pf_out{};
                if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&pf_out))) {
                    has_dred_page_fault = true;
                    page_fault_va = pf_out.PageFaultVA;
                    const D3D12_DRED_ALLOCATION_NODE1* alloc = pf_out.pHeadExistingAllocationNode;
                    for (size_t n = 0; alloc && n < 16; ++n, alloc = alloc->pNext) {
                        const std::string name = alloc->ObjectNameW
                            ? WideToUtf8Safe(alloc->ObjectNameW)
                            : (alloc->ObjectNameA ? alloc->ObjectNameA : "<unnamed>");
                        std::ostringstream as;
                        as << "{\"kind\":\"existing\",\"type\":" << static_cast<int>(alloc->AllocationType)
                           << ",\"name\":\"" << JsonEscape(name) << "\"}";
                        fault_allocs.push_back(as.str());
                    }
                    const D3D12_DRED_ALLOCATION_NODE1* freed = pf_out.pHeadRecentFreedAllocationNode;
                    for (size_t n = 0; freed && n < 16; ++n, freed = freed->pNext) {
                        const std::string name = freed->ObjectNameW
                            ? WideToUtf8Safe(freed->ObjectNameW)
                            : (freed->ObjectNameA ? freed->ObjectNameA : "<unnamed>");
                        std::ostringstream as;
                        as << "{\"kind\":\"freed\",\"type\":" << static_cast<int>(freed->AllocationType)
                           << ",\"name\":\"" << JsonEscape(name) << "\"}";
                        fault_allocs.push_back(as.str());
                    }
                }
                dred1->Release();
            }
#  endif
        }
#else
        (void)native_id3d12_device;
#endif

        RecordEvent(FlightEventCategory::GpuDeviceRemoved,
                    "d3d12_dred",
                    context_site ? context_site : "DeviceRemoved",
                    removed_reason,
                    0,
                    page_fault_va,
                    seq);

        std::vector<FlightGpuBreadcrumb> merged_bcs;
        if (breadcrumbs && breadcrumb_count > 0) {
            merged_bcs.assign(breadcrumbs, breadcrumbs + breadcrumb_count);
        } else {
            ScopedSpinTryLock blk(gpu_bc_lock_, 64);
            if (blk.owns_lock() && total_gpu_breadcrumbs_ > 0) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(total_gpu_breadcrumbs_, 64ull));
                for (size_t i = 0; i < n; ++i) {
                    const uint64_t bidx = (total_gpu_breadcrumbs_ > 64ull) ? (total_gpu_breadcrumbs_ - n + i) : i;
                    merged_bcs.push_back(recent_gpu_breadcrumbs_[static_cast<size_t>(bidx % 64ull)]);
                }
            }
        }

        const std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        const std::string json_path = (std::filesystem::path(out_dir) /
            ("gpu_dred_report_" + std::to_string(CurrentPid()) + "_" + std::to_string(seq) + ".json")).string();

        {
            std::ofstream jf(json_path, std::ios::binary);
            if (jf) {
                jf << "{\n"
                   << "  \"report_type\": \"gpu_dred_report\",\n"
                   << "  \"pid\": " << CurrentPid() << ",\n"
                   << "  \"sequence\": " << seq << ",\n"
                   << "  \"context_site\": \"" << JsonEscape(context_site ? context_site : "unknown") << "\",\n"
                   << "  \"triggering_hr\": \"" << HexU32(static_cast<uint32_t>(triggering_hr)) << "\",\n"
                   << "  \"device_removed_reason\": \"" << HexU32(static_cast<uint32_t>(removed_reason)) << "\",\n"
                   << "  \"has_dred_auto_breadcrumbs\": " << (has_dred_breadcrumbs ? "true" : "false") << ",\n"
                   << "  \"has_dred_page_fault\": " << (has_dred_page_fault ? "true" : "false") << ",\n"
                   << "  \"page_fault_va\": \"" << HexU64(page_fault_va) << "\",\n"
                   << "  \"dred_auto_breadcrumb_nodes\": [\n";
                for (size_t i = 0; i < dred_nodes.size(); ++i) {
                    jf << "    " << dred_nodes[i] << (i + 1 < dred_nodes.size() ? "," : "") << "\n";
                }
                jf << "  ],\n  \"dred_page_fault_allocations\": [\n";
                for (size_t i = 0; i < fault_allocs.size(); ++i) {
                    jf << "    " << fault_allocs[i] << (i + 1 < fault_allocs.size() ? "," : "") << "\n";
                }
                jf << "  ],\n  \"scarlettx_breadcrumbs\": [\n";
                for (size_t i = 0; i < merged_bcs.size(); ++i) {
                    jf << "    {\"cmd_list\": " << merged_bcs[i].command_list_id
                       << ", \"marker\": " << merged_bcs[i].marker_value
                       << ", \"tag\": \"" << JsonEscape(merged_bcs[i].tag) << "\"}"
                       << (i + 1 < merged_bcs.size() ? "," : "") << "\n";
                }
                jf << "  ]\n}\n";
            }
        }
        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            dred_artifacts_.push_back(json_path);
        }
        return json_path;
    } catch (...) {
        return {};
    }
}

std::string FlightRecorder::DumpFailingShaderArtifact(const char* stage_or_api,
                                                      int32_t hr_or_status,
                                                      const void* raw_bytecode,
                                                      size_t raw_size,
                                                      const void* translated_bytecode,
                                                      size_t translated_size,
                                                      uint64_t caller_rip,
                                                      const char* extra_note) noexcept {
    if (!IsEnabled() || !shader_dump_flag_.load(std::memory_order_relaxed)) {
        return {};
    }

    const uint64_t blob_hash = (raw_bytecode && raw_size > 0)
        ? Fnv1a64(raw_bytecode, raw_size)
        : ((translated_bytecode && translated_size > 0) ? Fnv1a64(translated_bytecode, translated_size)
                                                        : Fnv1a64(stage_or_api ? stage_or_api : "pso",
                                                                  stage_or_api ? std::strlen(stage_or_api) : 3u));

    RecordEvent(FlightEventCategory::ShaderOrPsoFail,
                "shader_dump",
                stage_or_api ? stage_or_api : "PSO",
                hr_or_status,
                caller_rip,
                raw_size,
                blob_hash);

    try {
        std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
        const std::filesystem::path shaders_dir = std::filesystem::path(out_dir) / "shaders";
        uint32_t idx = 0;
        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            for (uint32_t i = 0; i < dumped_shader_hash_count_; ++i) {
                if (dumped_shader_hashes_[i] == blob_hash) {
                    return {};  // Already dumped this exact shader hash
                }
            }
            if (dumped_shader_hash_count_ >= kMaxShaderDumps) {
                return {};
            }
            dumped_shader_hashes_[dumped_shader_hash_count_++] = blob_hash;
            idx = shader_seq_.fetch_add(1u, std::memory_order_relaxed) + 1u;
        }

        std::error_code ec;
        std::filesystem::create_directories(shaders_dir, ec);

        char hash_hex[20] = {};
        std::snprintf(hash_hex, sizeof(hash_hex), "%016llx", static_cast<unsigned long long>(blob_hash));
        const std::string stem = "shader_fail_" + std::to_string(idx) + "_" +
                                 SanitizeFilenameToken(stage_or_api) + "_" + hash_hex;

        std::string raw_file;
        if (raw_bytecode && raw_size > 0) {
            raw_file = (shaders_dir / (stem + "_raw.bin")).string();
            std::ofstream rf(raw_file, std::ios::binary);
            if (rf) {
                rf.write(static_cast<const char*>(raw_bytecode), static_cast<std::streamsize>(raw_size));
            }
        }

        std::string tr_file;
        if (translated_bytecode && translated_size > 0) {
            tr_file = (shaders_dir / (stem + "_translated.dxil")).string();
            std::ofstream tf(tr_file, std::ios::binary);
            if (tf) {
                tf.write(static_cast<const char*>(translated_bytecode),
                         static_cast<std::streamsize>(translated_size));
            }
        }

        char magic_str[16] = "none";
        if (raw_bytecode && raw_size >= 4) {
            const auto* b = static_cast<const uint8_t*>(raw_bytecode);
            std::snprintf(magic_str, sizeof(magic_str), "%02X %02X %02X %02X", b[0], b[1], b[2], b[3]);
        }

        const auto caller_resolved = ResolveCodeAddress(caller_rip);
        const std::string meta_path = (shaders_dir / (stem + "_meta.json")).string();
        {
            std::ofstream mf(meta_path, std::ios::binary);
            if (mf) {
                mf << "{\n"
                   << "  \"artifact_type\": \"failing_shader_or_pso\",\n"
                   << "  \"index\": " << idx << ",\n"
                   << "  \"stage_or_api\": \"" << JsonEscape(stage_or_api ? stage_or_api : "PSO") << "\",\n"
                   << "  \"hresult\": \"" << HexU32(static_cast<uint32_t>(hr_or_status)) << "\",\n"
                   << "  \"hash_fnv1a64\": \"0x" << hash_hex << "\",\n"
                   << "  \"raw_magic_hex\": \"" << magic_str << "\",\n"
                   << "  \"raw_size\": " << raw_size << ",\n"
                   << "  \"translated_size\": " << translated_size << ",\n"
                   << "  \"raw_path\": \"" << JsonEscape(raw_file) << "\",\n"
                   << "  \"translated_path\": \"" << JsonEscape(tr_file) << "\",\n"
                   << "  \"caller\": \"" << JsonEscape(FormatResolvedAddress(caller_resolved)) << "\",\n"
                   << "  \"note\": \"" << JsonEscape(extra_note ? extra_note : "") << "\"\n"
                   << "}\n";
            }
        }
        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            shader_artifacts_.push_back(meta_path);
        }
        return meta_path;
    } catch (...) {
        return {};
    }
}

bool FlightRecorder::CaptureHangSnapshot(const char* reason,
                                         std::string* out_json_path) noexcept {
#if defined(_WIN32)
    const uint32_t seq = hang_seq_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (seq > kMaxHangSnapshots) {
        hang_seq_.fetch_sub(1u, std::memory_order_relaxed);
        return false;
    }

    struct RawThreadSample {
        uint32_t tid = 0;
        uint64_t rip = 0;
        uint64_t rsp = 0;
        uint64_t rbp = 0;
        uint32_t stack_word_count = 0;
        uint64_t stack_words[16] = {};
    };

    constexpr size_t kMaxSampledThreads = 64;
    RawThreadSample samples[kMaxSampledThreads]{};
    size_t sample_count = 0;

    const DWORD pid = GetCurrentProcessId();
    const DWORD self_tid = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok && sample_count < kMaxSampledThreads;
             ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self_tid) {
                continue;
            }
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;

            // CRITICAL: Zero heap allocations, zero locks, and zero module/symbol lookups while suspended!
            if (SuspendThread(th) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(th, &ctx)) {
                    RawThreadSample& dst = samples[sample_count++];
                    dst.tid = te.th32ThreadID;
                    dst.rip = ctx.Rip;
                    dst.rsp = ctx.Rsp;
                    dst.rbp = ctx.Rbp;
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (ctx.Rsp >= 0x10000ull &&
                        VirtualQuery(reinterpret_cast<LPCVOID>(ctx.Rsp), &mbi, sizeof(mbi)) != 0 &&
                        mbi.State == MEM_COMMIT &&
                        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) {
                        const uint64_t region_end =
                            reinterpret_cast<uint64_t>(mbi.BaseAddress) + static_cast<uint64_t>(mbi.RegionSize);
                        for (uint32_t w = 0; w < 16u; ++w) {
                            const uint64_t addr = ctx.Rsp + static_cast<uint64_t>(w) * 8ull;
                            if (addr + 8ull > region_end) break;
                            std::memcpy(&dst.stack_words[w], reinterpret_cast<const void*>(addr), sizeof(uint64_t));
                            dst.stack_word_count = w + 1u;
                        }
                    }
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
    }

    try {
        char phase_copy[48] = {};
        {
            ScopedSpinTryLock plk(phase_lock_, 32);
            if (plk.owns_lock()) {
                CopyBoundedStr(phase_copy, sizeof(phase_copy), last_phase_);
            }
        }

        const int64_t last_qpc = last_heartbeat_qpc_.load(std::memory_order_relaxed);
        const uint64_t stall_ms = static_cast<uint64_t>(
            std::max<int64_t>(0, NowQpc() - last_qpc) * 1000ll / QpcFreq());

        RecordEvent(FlightEventCategory::WatchdogHang,
                    "watchdog",
                    reason ? reason : "HangTimeout",
                    0,
                    sample_count > 0 ? samples[0].rip : 0ull,
                    stall_ms,
                    sample_count);

        const std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        const std::string base = "hang_snapshot_" + std::to_string(pid) + "_" + std::to_string(seq);
        const std::string json_path = (std::filesystem::path(out_dir) / (base + ".json")).string();
        const std::string txt_path = (std::filesystem::path(out_dir) / (base + ".txt")).string();

        const auto recent_events = SnapshotRecentEvents(64);

        {
            std::ofstream jf(json_path, std::ios::binary);
            if (jf) {
                jf << "{\n"
                   << "  \"report_type\": \"hang_snapshot\",\n"
                   << "  \"pid\": " << pid << ",\n"
                   << "  \"sequence\": " << seq << ",\n"
                   << "  \"reason\": \"" << JsonEscape(reason ? reason : "WatchdogStallTimeout") << "\",\n"
                   << "  \"last_phase\": \"" << JsonEscape(phase_copy) << "\",\n"
                   << "  \"stall_duration_ms\": " << stall_ms << ",\n"
                   << "  \"heartbeat_seq\": " << heartbeat_seq_.load(std::memory_order_relaxed) << ",\n"
                   << "  \"sampled_thread_count\": " << sample_count << ",\n"
                   << "  \"threads\": [\n";
                for (size_t i = 0; i < sample_count; ++i) {
                    const auto& ts = samples[i];
                    const auto rip_res = ResolveCodeAddressNoLoaderLock(ts.rip);
                    jf << "    {\n"
                       << "      \"tid\": " << ts.tid << ",\n"
                       << "      \"rip\": \"" << HexU64(ts.rip) << "\",\n"
                       << "      \"rsp\": \"" << HexU64(ts.rsp) << "\",\n"
                       << "      \"symbol\": \"" << JsonEscape(FormatResolvedAddress(rip_res)) << "\",\n"
                       << "      \"is_guest\": " << (rip_res.is_guest ? "true" : "false") << ",\n"
                       << "      \"stack_return_candidates\": [";
                    bool first_cand = true;
                    for (uint32_t w = 0; w < ts.stack_word_count; ++w) {
                        const uint64_t cand = ts.stack_words[w];
                        if (!IsExecutableCodeAddress(cand)) continue;
                        const auto ca = ResolveCodeAddressNoLoaderLock(cand);
                        if (!ca.resolved) continue;
                        if (!first_cand) jf << ", ";
                        first_cand = false;
                        jf << "\"" << JsonEscape(FormatResolvedAddress(ca)) << "\"";
                    }
                    jf << "]\n    }" << (i + 1 < sample_count ? "," : "") << "\n";
                }
                jf << "  ],\n  \"recent_events\": [\n";
                for (size_t i = 0; i < recent_events.size(); ++i) {
                    const auto& ev = recent_events[i];
                    jf << "    {\"seq\": " << ev.seq
                       << ", \"t_us\": " << ev.timestamp_us
                       << ", \"tid\": " << ev.thread_id
                       << ", \"cat\": \"" << FlightEventCategoryName(ev.category) << "\""
                       << ", \"sub\": \"" << JsonEscape(ev.subsystem) << "\""
                       << ", \"detail\": \"" << JsonEscape(ev.detail) << "\"}"
                       << (i + 1 < recent_events.size() ? "," : "") << "\n";
                }
                jf << "  ]\n}\n";
            }
        }

        {
            std::ofstream tf(txt_path);
            if (tf) {
                tf << "=== ScarlettEdge Flight-Recorder Hang Snapshot ===\n"
                   << "PID:          " << pid << "\n"
                   << "Reason:       " << (reason ? reason : "WatchdogStallTimeout") << "\n"
                   << "Stalled For:  " << stall_ms << " ms (last_phase=" << phase_copy << ")\n"
                   << "Threads (" << sample_count << "):\n";
                for (size_t i = 0; i < sample_count; ++i) {
                    const auto& ts = samples[i];
                    tf << "  - TID " << ts.tid << ": RIP=" << FormatResolvedAddress(ResolveCodeAddressNoLoaderLock(ts.rip))
                       << " RSP=" << HexU64(ts.rsp) << "\n";
                }
            }
        }

        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            hang_artifacts_.push_back(json_path);
        }
        if (out_json_path) *out_json_path = json_path;
        return true;
    } catch (...) {
        return false;
    }
#else
    (void)reason;
    (void)out_json_path;
    return false;
#endif
}

bool FlightRecorder::WriteSummaryReport(std::string* out_json_path,
                                        std::string* out_txt_path) {
    const std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    const std::string json_path = (std::filesystem::path(out_dir) / "flight_summary.json").string();
    const std::string txt_path = (std::filesystem::path(out_dir) / "flight_summary.txt").string();

    const FlightRecorderStats st = GetStats();
    const auto recent_events = SnapshotRecentEvents(256);

    std::vector<FlightStubMissRecord> stubs;
    std::vector<FlightComQiMissRecord> qi_misses;
    std::vector<FlightVtableMissRecord> vtbl_misses;
    std::vector<FlightVfsMissRecord> vfs_misses;
    {
        ScopedSpinTryLock mlk(miss_lock_, 512);
        if (mlk.owns_lock()) {
            stubs.assign(stub_misses_, stub_misses_ + stub_miss_count_);
            qi_misses.assign(com_qi_misses_, com_qi_misses_ + com_qi_miss_count_);
            vtbl_misses.assign(vtable_misses_, vtable_misses_ + vtable_miss_count_);
            vfs_misses.assign(vfs_misses_, vfs_misses_ + vfs_miss_count_);
        }
    }
    std::sort(stubs.begin(), stubs.end(), [](const auto& a, const auto& b) { return a.hit_count > b.hit_count; });
    std::sort(qi_misses.begin(), qi_misses.end(), [](const auto& a, const auto& b) { return a.hit_count > b.hit_count; });
    std::sort(vtbl_misses.begin(), vtbl_misses.end(), [](const auto& a, const auto& b) { return a.hit_count > b.hit_count; });
    std::sort(vfs_misses.begin(), vfs_misses.end(), [](const auto& a, const auto& b) { return a.hit_count > b.hit_count; });

    std::vector<std::string> crashes, hangs, dreds, shaders;
    const uint32_t crash_cnt = std::min<uint32_t>(
        crash_artifact_count_.load(std::memory_order_acquire), kMaxCrashReports);
    for (uint32_t i = 0; i < crash_cnt; ++i) {
        if (crash_artifact_paths_[i][0] != '\0') {
            crashes.emplace_back(crash_artifact_paths_[i]);
        }
    }
    {
        std::lock_guard<std::mutex> alk(artifacts_mu_);
        hangs = hang_artifacts_;
        dreds = dred_artifacts_;
        shaders = shader_artifacts_;
    }

    char phase_copy[48] = {};
    {
        ScopedSpinTryLock plk(phase_lock_, 128);
        if (plk.owns_lock()) {
            CopyBoundedStr(phase_copy, sizeof(phase_copy), last_phase_);
        }
    }

    const char* title_id = std::getenv("SCARLETTX_TITLE_ID");
    const char* pkg_root = std::getenv("SCARLETTX_PACKAGE_ROOT");
    const uint64_t uptime_ms = ElapsedUs() / 1000ull;

    bool ok_json = false;
    {
        std::ofstream jf(json_path, std::ios::binary);
        if (jf) {
            jf << "{\n"
               << "  \"report_type\": \"flight_summary\",\n"
               << "  \"pid\": " << CurrentPid() << ",\n"
               << "  \"title_id\": \"" << JsonEscape(title_id ? title_id : "") << "\",\n"
               << "  \"package_root\": \"" << JsonEscape(pkg_root ? pkg_root : "") << "\",\n"
               << "  \"uptime_ms\": " << uptime_ms << ",\n"
               << "  \"last_phase\": \"" << JsonEscape(phase_copy) << "\",\n"
               << "  \"total_events\": " << st.total_events << ",\n"
               << "  \"heartbeats\": " << st.heartbeats << ",\n";
            const auto fp = GetFramePacingSummary();
            jf << "  \"frame_pacing\": {\n"
               << "    \"frames\": " << fp.frames_recorded << ",\n"
               << "    \"stutters_gt_33ms\": " << fp.stutter_count << ",\n"
               << std::fixed << std::setprecision(2)
               << "    \"avg_ms\": " << fp.avg_ms << ",\n"
               << "    \"p50_ms\": " << fp.p50_ms << ",\n"
               << "    \"p95_ms\": " << fp.p95_ms << ",\n"
               << "    \"p99_ms\": " << fp.p99_ms << ",\n"
               << "    \"max_ms\": " << fp.max_ms << "\n"
               << std::defaultfloat
               << "  },\n"
               << "  \"category_counts\": {\n";
            for (size_t i = 0; i < static_cast<size_t>(FlightEventCategory::Count); ++i) {
                jf << "    \"" << FlightEventCategoryName(static_cast<FlightEventCategory>(i)) << "\": "
                   << st.category_counts[i]
                   << (i + 1 < static_cast<size_t>(FlightEventCategory::Count) ? "," : "") << "\n";
            }
            jf << "  },\n  \"missing_imports\": [\n";
            for (size_t i = 0; i < stubs.size(); ++i) {
                const auto ca = ResolveCodeAddress(stubs[i].first_caller_rip);
                jf << "    {\"symbol\": \"" << JsonEscape(stubs[i].module_and_symbol) << "\""
                   << ", \"hits\": " << stubs[i].hit_count
                   << ", \"return_value\": \"" << HexU64(stubs[i].return_value) << "\""
                   << ", \"last_rcx\": \"" << HexU64(stubs[i].last_args[0]) << "\""
                   << ", \"last_rdx\": \"" << HexU64(stubs[i].last_args[1]) << "\""
                   << ", \"first_caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < stubs.size() ? "," : "") << "\n";
            }
            jf << "  ],\n  \"com_qi_misses\": [\n";
            for (size_t i = 0; i < qi_misses.size(); ++i) {
                const auto ca = ResolveCodeAddress(qi_misses[i].first_caller_rip);
                jf << "    {\"interface\": \"" << JsonEscape(qi_misses[i].interface_tag) << "\""
                   << ", \"iid\": \"" << JsonEscape(qi_misses[i].iid_str) << "\""
                   << ", \"hits\": " << qi_misses[i].hit_count
                   << ", \"first_caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < qi_misses.size() ? "," : "") << "\n";
            }
            jf << "  ],\n  \"vtable_slot_misses\": [\n";
            for (size_t i = 0; i < vtbl_misses.size(); ++i) {
                const auto ca = ResolveCodeAddress(vtbl_misses[i].first_caller_rip);
                jf << "    {\"interface\": \"" << JsonEscape(vtbl_misses[i].interface_tag) << "\""
                   << ", \"slot\": " << vtbl_misses[i].slot
                   << ", \"offset\": \"+0x" << std::hex << std::uppercase << (vtbl_misses[i].slot * 8) << std::dec << "\""
                   << ", \"hits\": " << vtbl_misses[i].hit_count
                   << ", \"first_caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < vtbl_misses.size() ? "," : "") << "\n";
            }
            jf << "  ],\n  \"vfs_misses\": [\n";
            for (size_t i = 0; i < vfs_misses.size(); ++i) {
                const auto ca = ResolveCodeAddress(vfs_misses[i].first_caller_rip);
                jf << "    {\"path\": \"" << JsonEscape(vfs_misses[i].virtual_path) << "\""
                   << ", \"error\": \"" << HexU32(static_cast<uint32_t>(vfs_misses[i].error_code)) << "\""
                   << ", \"hits\": " << vfs_misses[i].hit_count
                   << ", \"first_caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < vfs_misses.size() ? "," : "") << "\n";
            }
            auto write_str_vec = [&](const char* key, const std::vector<std::string>& vec, bool comma) {
                jf << "  \"" << key << "\": [";
                for (size_t i = 0; i < vec.size(); ++i) {
                    if (i > 0) jf << ", ";
                    jf << "\"" << JsonEscape(vec[i]) << "\"";
                }
                jf << "]" << (comma ? "," : "") << "\n";
            };
            jf << "  ],\n";
            write_str_vec("crash_reports", crashes, true);
            write_str_vec("hang_snapshots", hangs, true);
            write_str_vec("gpu_dred_reports", dreds, true);
            write_str_vec("shader_fail_artifacts", shaders, true);
            jf << "  \"recent_events\": [\n";
            for (size_t i = 0; i < recent_events.size(); ++i) {
                const auto& ev = recent_events[i];
                const auto ca = ResolveCodeAddress(ev.caller_rip);
                jf << "    {\"seq\": " << ev.seq
                   << ", \"t_us\": " << ev.timestamp_us
                   << ", \"tid\": " << ev.thread_id
                   << ", \"cat\": \"" << FlightEventCategoryName(ev.category) << "\""
                   << ", \"sub\": \"" << JsonEscape(ev.subsystem) << "\""
                   << ", \"detail\": \"" << JsonEscape(ev.detail) << "\""
                   << ", \"status\": \"" << HexU32(static_cast<uint32_t>(ev.status_code)) << "\""
                   << ", \"caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < recent_events.size() ? "," : "") << "\n";
            }
            jf << "  ]\n}\n";
            ok_json = jf.good();
        }
    }

    {
        std::ofstream tf(txt_path);
        if (tf) {
            tf << "=== ScarlettEdge Flight-Recorder Diagnostic Summary ===\n"
               << "PID:                 " << CurrentPid() << "\n"
               << "Title ID:            " << (title_id && *title_id ? title_id : "<none>") << "\n"
               << "Uptime:              " << uptime_ms << " ms (heartbeats=" << st.heartbeats
               << ", last_phase=" << phase_copy << ")\n"
               << "Total Ring Events:   " << st.total_events << "\n"
               << "Unique Stub Misses:  " << stubs.size() << "\n"
               << "Unique COM QI Miss:  " << qi_misses.size() << "\n"
               << "Unique Vtable Miss:  " << vtbl_misses.size() << "\n"
               << "Unique VFS Misses:   " << vfs_misses.size() << "\n"
               << "Crash Reports:       " << crashes.size() << "\n"
               << "Hang Snapshots:      " << hangs.size() << "\n"
               << "GPU DRED Reports:    " << dreds.size() << "\n"
               << "Shader Fail Dumps:   " << shaders.size() << "\n";
            if (!stubs.empty()) {
                tf << "\nTop Missing Imports (StubThunk):\n";
                for (const auto& s : stubs) {
                    tf << "  - " << s.module_and_symbol << " (hits=" << s.hit_count
                       << ", caller=" << FormatResolvedAddress(ResolveCodeAddress(s.first_caller_rip)) << ")\n";
                }
            }
            if (!qi_misses.empty()) {
                tf << "\nUnhandled COM QueryInterface IIDs:\n";
                for (const auto& q : qi_misses) {
                    tf << "  - " << q.interface_tag << " " << q.iid_str << " (hits=" << q.hit_count << ")\n";
                }
            }
            if (!vtbl_misses.empty()) {
                tf << "\nUnimplemented COM Vtable Slots:\n";
                for (const auto& v : vtbl_misses) {
                    tf << "  - " << v.interface_tag << "::vtable[" << v.slot << "] (hits=" << v.hit_count << ")\n";
                }
            }
            if (!vfs_misses.empty()) {
                tf << "\nMissing G:\\ / PackageVfs Assets:\n";
                for (const auto& m : vfs_misses) {
                    tf << "  - " << m.virtual_path << " (hits=" << m.hit_count
                       << ", err=" << HexU32(static_cast<uint32_t>(m.error_code)) << ")\n";
                }
            }
        }
    }

    if (out_json_path) *out_json_path = json_path;
    if (out_txt_path) *out_txt_path = txt_path;
    return ok_json;
}

}  // namespace scarlettx
