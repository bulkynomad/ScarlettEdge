#include "scarlettx/runtime/emulator.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/runtime/virtual_drive.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

bool IsXvdContainerFile(const std::string& path_str) {
    std::error_code ec;
    const std::filesystem::path p(path_str);
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".xvc" || ext == ".xvd";
}

Status StageXvdPackageIfNeeded(
    const std::string& package_or_xvc,
    const std::string& volume_key,
    std::string& out_resolved_dir) {
    if (!IsXvdContainerFile(package_or_xvc)) {
        out_resolved_dir = package_or_xvc;
        return Status::Ok();
    }

    XvdContainer xvd;
    Status st = xvd.OpenFile(package_or_xvc);
    if (!st.ok()) return st;

    std::string effective_key = volume_key;
    if (effective_key.empty()) {
        if (const char* env_key = std::getenv("SCARLETTX_VOLUME_KEY"); env_key && *env_key) {
            effective_key = env_key;
        }
    }
    if (!effective_key.empty()) {
        st = std::filesystem::exists(effective_key)
                 ? xvd.LoadVolumeKeyFile(effective_key)
                 : xvd.SetVolumeKeyHex(effective_key);
        if (!st.ok()) return st;
    }
    if (xvd.Header().is_encrypted && !xvd.HasVolumeKey()) {
        return Status::Error(
            StatusCode::NotImplemented,
            "Encrypted XVC/XVD container requires --volume-key <hex|file>: " + package_or_xvc);
    }

    char cid_hex[33] = {};
    for (size_t i = 0; i < 16; ++i) {
        std::snprintf(cid_hex + i * 2, 3, "%02x", xvd.Header().content_id[i]);
    }
    const std::filesystem::path stage_dir =
        std::filesystem::temp_directory_path() /
        ("scarlettx_xvc_" + std::filesystem::path(package_or_xvc).stem().string() + "_" + cid_hex);

    st = xvd.ExtractEmbeddedFilesystem(stage_dir.string());
    if (!st.ok()) {
        return st;
    }
    out_resolved_dir = stage_dir.string();
    return Status::Ok();
}

std::string FindGameConfigInDir(const std::string& package_dir) {
    const std::filesystem::path dir(package_dir);
    for (const char* candidate : {"MicrosoftGame.Config", "MicrosoftGame.config"}) {
        const auto p = dir / candidate;
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
    return {};
}

}  // namespace

Emulator::Emulator(EmulatorConfig config) : config_(std::move(config)) {}

Status Emulator::InspectPackage(const std::string& package_dir,
                                PackageInspectionReport& out_report) const {
    out_report = PackageInspectionReport{};
    std::string resolved_dir;
    Status stage_st = StageXvdPackageIfNeeded(package_dir, config_.volume_key, resolved_dir);
    if (!stage_st.ok()) {
        return stage_st;
    }
    const std::string cfg_path = FindGameConfigInDir(resolved_dir);
    if (cfg_path.empty()) {
        return Status::Error(StatusCode::NotFound,
                             "MicrosoftGame.Config not found in package directory: " + resolved_dir);
    }

    Status st = LoadGameConfigFile(cfg_path, out_report.config);
    if (!st.ok()) {
        return st;
    }

    const GameExecutableEntry* exe = out_report.config.PrimaryExecutable();
    if (!exe) {
        return Status::Error(StatusCode::ParseError, "No primary executable in MicrosoftGame.Config");
    }

    const std::filesystem::path exe_path = std::filesystem::path(resolved_dir) / exe->name;
    if (!std::filesystem::exists(exe_path)) {
        return Status::Error(StatusCode::NotFound, "Package executable missing: " + exe_path.string());
    }
    out_report.executable_path = exe_path.string();

    return InspectPeFile(out_report.executable_path, redirects_, out_report.main_pe);
}

Status Emulator::BootPackage(const std::string& package_input) {
    std::string package_dir;
    Status stage_st = StageXvdPackageIfNeeded(package_input, config_.volume_key, package_dir);
    if (!stage_st.ok()) {
        return stage_st;
    }
    PackageInspectionReport report;
    Status st = InspectPackage(package_dir, report);
    if (!st.ok()) {
        return st;
    }

    if (!report.config.IsNativeScarlett()) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Package is not a native XboxGen9/Scarlett Game Core target");
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
    if (!report.config.title_id.empty()) {
        set_env("SCARLETTX_TITLE_ID", report.config.title_id.c_str());
    }
    const std::string abs_pkg_dir = std::filesystem::absolute(package_dir).string();
    set_env("SCARLETTX_PACKAGE_ROOT", abs_pkg_dir.c_str());

    if (!config_.volume_key.empty()) {
        set_env("SCARLETTX_VOLUME_KEY", config_.volume_key.c_str());
    }

    // Mount package directory and any embedded .xvd / .xvc containers into VFS.
    PackageVfs pkg_vfs;
    (void)pkg_vfs.MountDirectory("G:", abs_pkg_dir);
    std::error_code dir_ec;
    for (const auto& entry : std::filesystem::directory_iterator(abs_pkg_dir, dir_ec)) {
        if (!entry.is_regular_file(dir_ec)) continue;
        std::string ext = entry.path().extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext == ".xvd" || ext == ".xvc") {
            const std::string vpath = "G:\\" + entry.path().filename().string();
            Status xvd_st = pkg_vfs.MountXvd(vpath, entry.path().string(), config_.volume_key);
            if (!xvd_st.ok()) {
                return xvd_st;
            }
            std::cout << "[ScarlettX][vfs] Mounted container " << entry.path().filename().string() << "\n";
        }
    }

    // Real Xbox Series X|S titles (e.g. Unity Game Core nativePlugins.txt) reference G:/.
    // The mapping is skipped when G: is already assigned on the host.
    struct VirtualDriveGuard {
        ~VirtualDriveGuard() { vfs::UnmapPackageDrive(); }
    } g_drive_guard;
    const bool drive_mapped = vfs::MapPackageDrive('G', abs_pkg_dir);
    if (drive_mapped) {
        std::cout << "[ScarlettX][vfs] Mapped virtual G:\\ -> " << abs_pkg_dir << "\n";
    }

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
        MappedPeImage image;
        HleSymbolRegistry hle_registry;
        PeMapOptions map_opts;
        map_opts.force_relocate = config_.force_relocate;
        map_opts.auto_stub_missing = true;
        map_opts.shim_directory = shim_dir;

        Status map_st = image.MapFile(report.executable_path, redirects_, hle_registry, map_opts);
        if (!map_st.ok()) {
            return map_st;
        }
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
        if (image.GetExportAddress(config_.in_process_entry) != nullptr) {
            call_st = image.CallExportedEntry(config_.in_process_entry, exit_code);
        } else {
            set_env("SCARLETTX_REAL_TITLE", "1");
            // A title's relative paths start at its package root, as on the console.
            SetCurrentDirectoryA(drive_mapped ? "G:\\" : abs_pkg_dir.c_str());
            std::cout << "[ScarlettX][loader] Entering native PE entry point at "
                      << image.EntryPointAddress() << std::endl;
            call_st = image.CallEntryPoint(exit_code);
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

    // The guest inherits this: if it cannot load it must end with an exit
    // code instead of waiting on a system error dialog.
    const UINT previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::string original_path;
    // Searched before System32 and inherited by the child; PATH is also prepended to guarantee shim precedence.
    if (!shim_dir.empty()) {
        SetDllDirectoryA(shim_dir.c_str());
        const char* p = std::getenv("PATH");
        if (p) original_path = p;
        const std::string new_path = shim_dir + ";" + original_path;
        SetEnvironmentVariableA("PATH", new_path.c_str());
    }
    if (std::filesystem::exists(std::filesystem::path(work_dir) / "xgameruntime.dll")) {
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
    const bool timed_out = WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT;
    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

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
