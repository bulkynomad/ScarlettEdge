#include "scarlettx/runtime/emulator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_set>
#include <vector>

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/runtime/virtual_drive.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

std::atomic<uint64_t> g_temp_seq{0};
std::atomic<bool> g_async_shutdown_requested{false};
std::atomic<uint32_t> g_async_shutdown_ctrl_type{0};

uint64_t CurrentProcessIdPortable() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string NormalizeEmbeddedRelPath(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && (raw[i] == '/' || raw[i] == '\\')) {
            ++i;
        }
        const size_t start = i;
        while (i < raw.size() && raw[i] != '/' && raw[i] != '\\') {
            ++i;
        }
        if (i == start) {
            break;
        }
        const std::string part = raw.substr(start, i - start);
        if (part == ".") {
            continue;
        }
        if (!out.empty()) {
            out.push_back('/');
        }
        out += ToLowerAscii(part);
    }
    return out;
}

bool IsXvdContainerFile(const std::string& path_str) {
    std::error_code ec;
    const std::filesystem::path p(path_str);
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    const std::string ext = ToLowerAscii(p.extension().string());
    return ext == ".xvc" || ext == ".xvd" || ext == ".msixvc";
}

std::string ResolveEffectiveVolumeKey(const std::string& configured_key) {
    if (!configured_key.empty()) {
        return configured_key;
    }
    if (const char* env_key = std::getenv("SCARLETTX_VOLUME_KEY"); env_key && *env_key) {
        return env_key;
    }
    return {};
}

Status OpenXvdWithVolumeKey(const std::string& container_path,
                            const std::string& configured_key,
                            XvdContainer& out_xvd) {
    Status st = out_xvd.OpenFile(container_path);
    if (!st.ok()) {
        return st;
    }
    const std::string effective_key = ResolveEffectiveVolumeKey(configured_key);
    if (!effective_key.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(effective_key, ec)) {
            st = out_xvd.LoadKeyringDirectory(effective_key);
        } else if (std::filesystem::exists(effective_key, ec)) {
            st = out_xvd.LoadVolumeKeyFile(effective_key);
        } else {
            st = out_xvd.SetVolumeKeyHex(effective_key);
        }
        if (!st.ok()) {
            return st;
        }
    }
    if (out_xvd.Header().is_encrypted && !out_xvd.HasVolumeKey()) {
        return Status::Error(
            StatusCode::NotImplemented,
            "Encrypted XVC/XVD container requires --volume-key <hex|file> or --keyring <dir>: " +
                container_path);
    }
    return Status::Ok();
}

Status InspectOpenXvdContainer(const XvdContainer& xvd,
                               const std::string& container_path,
                               const DllRedirectionTable& redirects,
                               PackageInspectionReport& out_report,
                               std::vector<uint8_t>* out_exe_bytes,
                               std::vector<std::string>* out_dlc_stems) {
    out_report = PackageInspectionReport{};
    if (out_exe_bytes) out_exe_bytes->clear();
    if (out_dlc_stems) out_dlc_stems->clear();

    std::vector<XvdEmbeddedFile> files;
    Status st = xvd.ListEmbeddedFiles(files);
    if (!st.ok()) {
        return st;
    }

    const XvdEmbeddedFile* cfg_entry = nullptr;
    std::unordered_set<std::string> seen_dlcs;
    for (const auto& entry : files) {
        const std::string norm = NormalizeEmbeddedRelPath(entry.relative_path);
        if (!entry.is_directory && norm == "microsoftgame.config") {
            cfg_entry = &entry;
        }
        if (out_dlc_stems && norm.starts_with("dlc/") && norm.size() > 4u) {
            const size_t slash = norm.find('/', 4u);
            const std::string stem_raw = [&]() -> std::string {
                // Preserve original casing from entry.relative_path
                size_t p = 0;
                while (p < entry.relative_path.size() &&
                       (entry.relative_path[p] == '/' || entry.relative_path[p] == '\\')) {
                    ++p;
                }
                const size_t s1 = entry.relative_path.find_first_of("/\\", p);
                if (s1 == std::string::npos) return {};
                size_t s2 = s1 + 1;
                while (s2 < entry.relative_path.size() &&
                       (entry.relative_path[s2] == '/' || entry.relative_path[s2] == '\\')) {
                    ++s2;
                }
                const size_t s3 = entry.relative_path.find_first_of("/\\", s2);
                return s3 == std::string::npos ? entry.relative_path.substr(s2)
                                               : entry.relative_path.substr(s2, s3 - s2);
            }();
            const std::string lower_stem =
                slash == std::string::npos ? norm.substr(4u) : norm.substr(4u, slash - 4u);
            if (!lower_stem.empty() && lower_stem != "." && lower_stem != ".." &&
                seen_dlcs.insert(lower_stem).second) {
                out_dlc_stems->push_back(stem_raw.empty() ? lower_stem : stem_raw);
            }
        }
    }
    if (!cfg_entry) {
        return Status::Error(StatusCode::NotFound,
                             "MicrosoftGame.Config not found in XVC/XVD container: " +
                                 container_path);
    }
    if (cfg_entry->size_bytes > 16ull * 1024ull * 1024ull) {
        return Status::Error(StatusCode::ParseError,
                             "MicrosoftGame.Config exceeds 16 MiB sanity limit");
    }

    std::vector<uint8_t> cfg_bytes;
    st = xvd.ReadEmbeddedFile(cfg_entry->relative_path, cfg_bytes);
    if (!st.ok()) {
        st = xvd.ReadUserData(cfg_entry->user_data_offset, cfg_entry->size_bytes, cfg_bytes);
        if (!st.ok()) return st;
    }
    const std::string xml_str(reinterpret_cast<const char*>(cfg_bytes.data()), cfg_bytes.size());
    st = ParseGameConfigXml(xml_str, out_report.config);
    if (!st.ok()) {
        return st;
    }
    out_report.config.source_path = container_path + ":" + cfg_entry->relative_path;

    const GameExecutableEntry* exe = out_report.config.PrimaryExecutable();
    if (!exe) {
        return Status::Error(StatusCode::ParseError,
                             "No primary executable in MicrosoftGame.Config");
    }

    const std::string wanted_exe = NormalizeEmbeddedRelPath(exe->name);
    const XvdEmbeddedFile* exe_entry = nullptr;
    for (const auto& entry : files) {
        if (!entry.is_directory && NormalizeEmbeddedRelPath(entry.relative_path) == wanted_exe) {
            exe_entry = &entry;
            break;
        }
    }
    if (!exe_entry) {
        return Status::Error(StatusCode::NotFound,
                             "Package executable missing in XVC/XVD container: " + exe->name);
    }

    std::vector<uint8_t> local_exe_bytes;
    std::vector<uint8_t>& exe_buf = out_exe_bytes ? *out_exe_bytes : local_exe_bytes;
    st = xvd.ReadEmbeddedFile(exe_entry->relative_path, exe_buf);
    if (!st.ok()) {
        st = xvd.ReadUserData(exe_entry->user_data_offset, exe_entry->size_bytes, exe_buf);
        if (!st.ok()) return st;
    }

    st = InspectPeBytes(exe_buf, exe_entry->relative_path, redirects, out_report.main_pe);
    if (!st.ok()) {
        return st;
    }
    out_report.executable_path = exe_entry->relative_path;
    out_report.main_pe.file_path = out_report.executable_path;
    return Status::Ok();
}

std::string FindGameConfigInDir(const std::string& package_dir) {
    const std::filesystem::path dir(package_dir);
    std::error_code ec;
    for (const char* candidate : {"MicrosoftGame.Config", "MicrosoftGame.config"}) {
        const auto p = dir / candidate;
        if (std::filesystem::exists(p, ec)) {
            return p.string();
        }
    }
    return {};
}

}  // namespace

void RequestAsyncShutdown(uint32_t ctrl_type) noexcept {
    g_async_shutdown_ctrl_type.store(ctrl_type, std::memory_order_relaxed);
    g_async_shutdown_requested.store(true, std::memory_order_release);
}

bool IsAsyncShutdownRequested() noexcept {
    return g_async_shutdown_requested.load(std::memory_order_acquire);
}

uint32_t GetAsyncShutdownCtrlType() noexcept {
    return g_async_shutdown_ctrl_type.load(std::memory_order_relaxed);
}

void ClearAsyncShutdownRequest() noexcept {
    g_async_shutdown_requested.store(false, std::memory_order_release);
    g_async_shutdown_ctrl_type.store(0, std::memory_order_relaxed);
}

Emulator::Emulator(EmulatorConfig config) : config_(std::move(config)) {}

Status Emulator::InspectPackage(const std::string& package_dir,
                                PackageInspectionReport& out_report) const {
    out_report = PackageInspectionReport{};

    if (IsXvdContainerFile(package_dir)) {
        XvdContainer xvd;
        Status st = OpenXvdWithVolumeKey(package_dir, config_.volume_key, xvd);
        if (!st.ok()) {
            return st;
        }
        return InspectOpenXvdContainer(xvd, package_dir, redirects_, out_report, nullptr, nullptr);
    }

    const std::string cfg_path = FindGameConfigInDir(package_dir);
    if (cfg_path.empty()) {
        return Status::Error(StatusCode::NotFound,
                             "MicrosoftGame.Config not found in package directory: " + package_dir);
    }

    Status st = LoadGameConfigFile(cfg_path, out_report.config);
    if (!st.ok()) {
        return st;
    }

    const GameExecutableEntry* exe = out_report.config.PrimaryExecutable();
    if (!exe) {
        return Status::Error(StatusCode::ParseError, "No primary executable in MicrosoftGame.Config");
    }

    const std::filesystem::path exe_path = std::filesystem::path(package_dir) / exe->name;
    std::error_code ec;
    if (!std::filesystem::exists(exe_path, ec)) {
        return Status::Error(StatusCode::NotFound, "Package executable missing: " + exe_path.string());
    }
    out_report.executable_path = exe_path.string();

    return InspectPeFile(out_report.executable_path, redirects_, out_report.main_pe);
}

Status Emulator::BootPackage(const std::string& package_input) {
    const bool is_xvd_boot = IsXvdContainerFile(package_input);
    if (is_xvd_boot && !config_.in_process) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "Zero-staging .xvc/.msixvc/.xvd streaming requires --in-process mode");
    }

    std::shared_ptr<XvdContainer> mounted_xvd;
    std::vector<uint8_t> xvd_exe_bytes;
    std::vector<std::string> xvd_dlc_stems;
    PackageInspectionReport report;

    if (is_xvd_boot) {
        mounted_xvd = std::make_shared<XvdContainer>();
        Status st = OpenXvdWithVolumeKey(package_input, config_.volume_key, *mounted_xvd);
        if (!st.ok()) {
            return st;
        }
        st = InspectOpenXvdContainer(
            *mounted_xvd, package_input, redirects_, report, &xvd_exe_bytes, &xvd_dlc_stems);
        if (!st.ok()) {
            return st;
        }
    } else {
        Status st = InspectPackage(package_input, report);
        if (!st.ok()) {
            return st;
        }
    }

    if (!report.config.IsNativeScarlett() && !config_.allow_pc_gdk) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Package is not a native XboxGen9/Scarlett Game Core target (pass --allow-pc-gdk for PC GDK testing)");
    }

#if defined(_WIN32)
    // Inject HLE shim directory at the front of PATH so guest imports of
    // d3d12_xs.dll, xg_xs.dll, AcpHal.dll, xgameruntime.dll, etc. resolve to ScarlettX shims.
    std::string shim_dir = config_.shim_directory;
    if (shim_dir.empty()) {
        char exe_buf[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
            shim_dir = std::filesystem::path(exe_buf).parent_path().string();
        }
    }

    auto set_env = [](const char* key, const char* value) {
        SetEnvironmentVariableA(key, value);
        _putenv_s(key, value);
    };

    if (!shim_dir.empty()) {
        std::string new_path = shim_dir;
        const char* cur_path = std::getenv("PATH");
        if (cur_path && *cur_path) {
            new_path += ";";
            new_path += cur_path;
        }
        set_env("PATH", new_path.c_str());
    }

    set_env("SCARLETTX_HEADLESS", config_.headless ? "1" : "0");
    set_env("SCARLETTX_PREFER_WARP", config_.prefer_warp ? "1" : "0");
    set_env("SCARLETTX_TRACE", config_.trace ? "1" : "0");
    set_env("SCARLETTX_STATS", config_.stats ? "1" : "0");
    set_env("SCARLETTX_FCAP", std::to_string(config_.fps_cap).c_str());
    if (config_.lockhart_mode || config_.emulated_device == DeviceType::XboxScarlettLockhart) {
        set_env("SCARLETTX_LOCKHART", "1");
    }
    if (config_.enable_debug_layer) {
        set_env("SCARLETTX_DEBUG_LAYER", "1");
    }
    if (!report.config.title_id.empty()) {
        set_env("SCARLETTX_TITLE_ID", report.config.title_id.c_str());
    }

    const std::string abs_pkg_dir =
        is_xvd_boot ? std::string("G:") : std::filesystem::absolute(package_input).string();
    set_env("SCARLETTX_PACKAGE_ROOT", abs_pkg_dir.c_str());

    // Zero-file DLC metadata directory stub ONLY when an XVD contains embedded DLC/<stem>
    // entries and SCARLETTX_DLC_ROOT was not explicitly set by the caller.
    struct ScopedDlcMetaGuard {
        std::filesystem::path dir;
        bool restore_env = false;
        ~ScopedDlcMetaGuard() {
            if (restore_env) {
                SetEnvironmentVariableA("SCARLETTX_DLC_ROOT", "");
                _putenv_s("SCARLETTX_DLC_ROOT", "");
            }
            if (!dir.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
            }
        }
    } dlc_meta_guard;

    const char* existing_dlc_env = std::getenv("SCARLETTX_DLC_ROOT");
    if (is_xvd_boot && !xvd_dlc_stems.empty() && (!existing_dlc_env || !*existing_dlc_env)) {
        std::error_code ec;
        dlc_meta_guard.dir =
            std::filesystem::temp_directory_path(ec) /
            ("scarlettx_dlc_meta_" + std::to_string(CurrentProcessIdPortable()) + "_" +
             std::to_string(g_temp_seq.fetch_add(1)));
        for (const auto& stem : xvd_dlc_stems) {
            std::filesystem::create_directories(dlc_meta_guard.dir / stem, ec);
        }
        set_env("SCARLETTX_DLC_ROOT", dlc_meta_guard.dir.string().c_str());
        dlc_meta_guard.restore_env = true;
    }

    const char* env_fr = std::getenv("SCARLETTX_FLIGHT_RECORDER");
    const bool flight_active = config_.flight_recorder || (env_fr && env_fr[0] != '\0' && env_fr[0] != '0');
    std::string abs_flight_dir;
    uint32_t hang_timeout_ms = config_.hang_timeout_sec * 1000u;
    if (flight_active) {
        const char* env_fr_dir = std::getenv("SCARLETTX_FLIGHT_RECORDER_DIR");
        const std::string raw_dir = !config_.flight_recorder_dir.empty()
            ? config_.flight_recorder_dir
            : ((env_fr_dir && *env_fr_dir) ? std::string(env_fr_dir) : std::string("flight_logs"));
        abs_flight_dir = std::filesystem::absolute(raw_dir).string();
        if (const char* env_hang_ms = std::getenv("SCARLETTX_HANG_TIMEOUT_MS"); env_hang_ms && *env_hang_ms) {
            hang_timeout_ms = static_cast<uint32_t>(std::strtoul(env_hang_ms, nullptr, 10));
        }
        set_env("SCARLETTX_FLIGHT_RECORDER", "1");
        set_env("SCARLETTX_FLIGHT_RECORDER_DIR", abs_flight_dir.c_str());
        set_env("SCARLETTX_HANG_TIMEOUT_MS", std::to_string(hang_timeout_ms).c_str());
    }

    struct FlightRecorderSessionGuard {
        bool active = false;
        void Flush() {
            if (!active) return;
            active = false;
            if (IsAsyncShutdownRequested()) {
                FlightRecorder::Instance().RecordEvent(
                    FlightEventCategory::Lifecycle,
                    "cli",
                    "Console interrupt -> flushing flight summary",
                    static_cast<int32_t>(GetAsyncShutdownCtrlType()));
            }
            FlightRecorder::Instance().BeatProgress("shutdown");
            std::string json_out;
            if (FlightRecorder::Instance().WriteSummaryReport(&json_out, nullptr)) {
                std::cout << "[ScarlettX][FlightRecorder] Summary written to " << json_out << "\n";
            }
            FlightRecorder::Instance().Disable();
        }
        ~FlightRecorderSessionGuard() {
            Flush();
        }
    } flight_guard;

    if (flight_active && config_.in_process) {
        FlightRecorderOptions fropts;
        fropts.output_dir = abs_flight_dir;
        fropts.hang_timeout_ms = hang_timeout_ms;
        fropts.install_veh = true;
        fropts.enable_dred = true;
        fropts.dump_failing_shaders = true;
        FlightRecorder::Instance().Enable(fropts);
        flight_guard.active = true;
    }

    const std::string effective_key = ResolveEffectiveVolumeKey(config_.volume_key);
    if (!effective_key.empty()) {
        set_env("SCARLETTX_VOLUME_KEY", effective_key.c_str());
    }

    // Mount package into process-wide VFS (zero-staging direct XvdContainer mount for .xvc/.msixvc).
    PackageVfs& pkg_vfs = PackageVfs::Global();
    pkg_vfs.Reset();
    struct PackageVfsSessionGuard {
        bool active = false;
        ~PackageVfsSessionGuard() {
            if (active) {
                PackageVfs::Global().Reset();
            }
        }
    } vfs_session_guard{is_xvd_boot};

    if (is_xvd_boot) {
        Status mount_st = pkg_vfs.MountXvdContainer("G:", mounted_xvd);
        if (!mount_st.ok()) {
            return mount_st;
        }
        std::cout << "[ScarlettX][vfs] Streaming container G:\\ <- " << package_input
                  << " (zero-staging)\n";
    } else {
        (void)pkg_vfs.MountDirectory("G:", abs_pkg_dir);
        std::error_code dir_ec;
        for (const auto& entry : std::filesystem::directory_iterator(abs_pkg_dir, dir_ec)) {
            if (!entry.is_regular_file(dir_ec)) continue;
            const std::string ext = ToLowerAscii(entry.path().extension().string());
            if (ext == ".xvd" || ext == ".xvc" || ext == ".msixvc") {
                const std::string vpath = "G:\\" + entry.path().filename().string();
                Status xvd_st = pkg_vfs.MountXvd(vpath, entry.path().string(), effective_key);
                if (!xvd_st.ok()) {
                    return xvd_st;
                }
                std::cout << "[ScarlettX][vfs] Mounted container "
                          << entry.path().filename().string() << "\n";
            }
        }
    }

    struct VirtualDriveGuard {
        bool mapped = false;
        ~VirtualDriveGuard() {
            if (mapped) {
                vfs::UnmapPackageDrive();
            }
        }
    } g_drive_guard;
    const bool drive_mapped = !is_xvd_boot && vfs::MapPackageDrive('G', abs_pkg_dir);
    g_drive_guard.mapped = drive_mapped;
    if (drive_mapped) {
        std::cout << "[ScarlettX][vfs] Mapped virtual G:\\ -> " << abs_pkg_dir << "\n";
    }
    FlightRecorder::Instance().BeatProgress("vfs_mounted");

    if (config_.max_frames > 0) {
        set_env("SCARLETTX_MAX_FRAMES", std::to_string(config_.max_frames).c_str());
    }
    if (!config_.rt_dump_path.empty()) {
        set_env("SCARLETTX_RT_DUMP",
                std::filesystem::absolute(config_.rt_dump_path).string().c_str());
    }
    if (!config_.raster_dump_path.empty()) {
        set_env("SCARLETTX_RASTER_DUMP",
                std::filesystem::absolute(config_.raster_dump_path).string().c_str());
    }

    if (config_.in_process) {
        struct CurrentDirectoryGuard {
            std::wstring saved;
            bool active = false;
            void Capture() {
                const DWORD needed = GetCurrentDirectoryW(0, nullptr);
                if (needed > 1) {
                    saved.assign(needed, L'\0');
                    const DWORD got = GetCurrentDirectoryW(needed, saved.data());
                    if (got > 0 && got < needed) {
                        saved.resize(got);
                        active = true;
                    }
                }
            }
            ~CurrentDirectoryGuard() {
                if (active) {
                    SetCurrentDirectoryW(saved.c_str());
                }
            }
        } cwd_guard;

        MappedPeImage image;
        HleSymbolRegistry hle_registry;
        PeMapOptions map_opts;
        map_opts.force_relocate = config_.force_relocate;
        map_opts.auto_stub_missing = true;
        map_opts.shim_directory = shim_dir;

        Status map_st = is_xvd_boot
            ? image.MapBytes(xvd_exe_bytes, "G:\\" + report.executable_path,
                             redirects_, hle_registry, map_opts)
            : image.MapFile(report.executable_path, redirects_, hle_registry, map_opts);
        if (!map_st.ok()) {
            return map_st;
        }
        FlightRecorder::Instance().BeatProgress("pe_mapped");
        std::cout << "[ScarlettX][loader] Mapped " << report.executable_path << " at 0x"
                  << std::hex << image.MappedBase() << std::dec
                  << " (relocated=" << (image.WasRelocated() ? "yes" : "no")
                  << ", relocs=" << image.RelocationsApplied()
                  << ", imports=" << image.Resolutions().size()
                  << ", stubs=" << image.Stubs().StubCount() << ")" << std::endl;
        if (image.Stubs().StubCount() > 0) {
            for (const auto& res : image.Resolutions()) {
                if (res.kind == ImportResolutionKind::StubThunk) {
                    std::cout << "  [stub #" << res.stub_id << "] " << res.dll_name << "!";
                    if (res.by_ordinal) {
                        std::cout << "#" << res.ordinal;
                    } else {
                        std::cout << res.symbol_name;
                    }
                    std::cout << " (target=" << res.target_dll << ")" << std::endl;
                }
            }
        }

        int exit_code = 0;
        Status call_st = Status::Ok();
        FlightRecorder::Instance().BeatProgress("entering_guest");
        if (image.GetExportAddress(config_.in_process_entry) != nullptr) {
            call_st = image.CallExportedEntry(config_.in_process_entry, exit_code);
        } else {
            set_env("SCARLETTX_REAL_TITLE", "1");
            if (!is_xvd_boot) {
                cwd_guard.Capture();
                SetCurrentDirectoryA(drive_mapped ? "G:\\" : abs_pkg_dir.c_str());
            }
            std::cout << "[ScarlettX][loader] Entering native PE entry point at "
                      << image.EntryPointAddress() << std::endl;
            call_st = image.CallEntryPoint(exit_code);
        }
        FlightRecorder::Instance().BeatProgress("guest_exited");
        // Flush summary BEFORE MappedPeImage::Unmap() runs so guest caller RVAs resolve cleanly!
        flight_guard.Flush();
        if (IsAsyncShutdownRequested()) {
            return Status::Error(StatusCode::DeviceError, "Guest interrupted by console shutdown signal");
        }
        if (!call_st.ok()) {
            return call_st;
        }
        if (exit_code != 0) {
            return Status::Error(StatusCode::DeviceError,
                                 "In-process guest exited with non-zero code: " +
                                     std::to_string(exit_code));
        }
        return Status::Ok();
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + report.executable_path + "\"";
    std::string work_dir = std::filesystem::path(report.executable_path).parent_path().string();

    const UINT previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::string original_path;
    if (!shim_dir.empty()) {
        SetDllDirectoryA(shim_dir.c_str());
        const char* p = std::getenv("PATH");
        if (p) original_path = p;
        const std::string new_path = shim_dir + ";" + original_path;
        SetEnvironmentVariableA("PATH", new_path.c_str());
    }
    std::error_code xgr_ec;
    if (std::filesystem::exists(std::filesystem::path(work_dir) / "xgameruntime.dll", xgr_ec)) {
        std::cerr << "[ScarlettX][loader] Warning: Local xgameruntime.dll in title folder overrides SetDllDirectory/PATH in --separate-process mode; use default --in-process mode to force HLE shims.\n";
    }
    const BOOL created = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        work_dir.c_str(), &si, &pi);
    if (!shim_dir.empty()) {
        SetDllDirectoryA(nullptr);
        SetEnvironmentVariableA("PATH", original_path.empty() ? nullptr : original_path.c_str());
    }
    SetErrorMode(previous_error_mode);
    if (!created) {
        return Status::Error(StatusCode::DeviceError,
                             "CreateProcessA failed for guest executable: " + report.executable_path);
    }

    DWORD timeout_ms = INFINITE;
    if (const char* env_to = std::getenv("SCARLETTX_PROCESS_TIMEOUT_MS"); env_to && *env_to) {
        const unsigned long parsed = std::strtoul(env_to, nullptr, 10);
        if (parsed > 0) timeout_ms = static_cast<DWORD>(parsed);
    } else if (config_.max_frames > 0) {
        timeout_ms = 60000;
    }
    bool timed_out = false;
    bool interrupted = false;
    DWORD elapsed_wait_ms = 0;
    for (;;) {
        if (IsAsyncShutdownRequested()) {
            interrupted = true;
            break;
        }
        DWORD slice_ms = 50;
        if (timeout_ms != INFINITE) {
            if (elapsed_wait_ms >= timeout_ms) {
                timed_out = true;
                break;
            }
            const DWORD rem_ms = timeout_ms - elapsed_wait_ms;
            if (rem_ms < slice_ms) slice_ms = rem_ms;
        }
        const DWORD wait_rc = WaitForSingleObject(pi.hProcess, slice_ms);
        if (wait_rc == WAIT_OBJECT_0) {
            break;
        }
        if (wait_rc != WAIT_TIMEOUT) {
            break;
        }
        if (timeout_ms != INFINITE) {
            elapsed_wait_ms += slice_ms;
        }
    }
    if (timed_out || interrupted) {
        TerminateProcess(pi.hProcess, interrupted ? 130u : 1u);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (interrupted) {
        return Status::Error(StatusCode::DeviceError, "Guest process interrupted by console shutdown signal");
    }
    if (timed_out) {
        return Status::Error(StatusCode::DeviceError, "Guest process timed out and was stopped");
    }

    if (exit_code != 0) {
        char hex_buf[32] = {};
        std::snprintf(hex_buf, sizeof(hex_buf), "0x%08lX", static_cast<unsigned long>(exit_code));
        return Status::Error(StatusCode::DeviceError,
                             "Guest process exited with non-zero code: " +
                                 std::to_string(exit_code) + " (" + hex_buf + ")");
    }
    return Status::Ok();
#else
    return Status::Error(StatusCode::NotImplemented, "Guest PE boot currently requires Windows");
#endif
}

}  // namespace scarlettx
