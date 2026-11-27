#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/rt_bvh.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/runtime/emulator.h"

namespace {

void PrintUsage() {
    std::cout << "ScarlettEdge (ScarlettX Fork) - Xbox Series X|S (Scarlett) HLE Emulator\n"
              << "Usage:\n"
              << "  scarlettx inspect <package_dir>\n"
              << "  scarlettx compat <package_dir>\n"
              << "  scarlettx boot <package_dir> [--headless] [--in-process|--separate-process] [--gpu|--warp] [--trace] [--stats] [--fcap N] [--force-relocate] [--frames N] [--volume-key hex|file] [--rt-dump path.ppm] [--raster-dump path.ppm]\n"
              << "  scarlettx diag\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::string cmd = argv[1];
    if (cmd == "diag") {
        scarlettx::D3D12Backend backend;
        auto st = backend.Initialize(true, false);
        if (!st.ok()) {
            std::cerr << "[diag] D3D12 init failed: " << st.message << "\n";
            return 1;
        }
        const auto& info = backend.AdapterInfo();
        std::cout << "[diag] Adapter: " << info.description
                  << " (WARP=" << (info.is_warp ? "yes" : "no")
                  << ", Device5=" << (info.supports_device5 ? "yes" : "no")
                  << ", DXR Tier=" << info.raytracing_tier << ")\n";
        return 0;
    }

    if (cmd == "inspect" && argc >= 3) {
        scarlettx::EmulatorConfig cfg;
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--volume-key") cfg.volume_key = argv[++i];
        }
        scarlettx::Emulator emu(cfg);
        scarlettx::PackageInspectionReport rep;
        auto st = emu.InspectPackage(argv[2], rep);
        if (!st.ok()) {
            std::cerr << "[inspect] Error: " << st.message << "\n";
            return 1;
        }
        std::cout << "Title: " << rep.config.display_name << " (" << rep.config.title_id << ")\n"
                  << "Identity: " << rep.config.identity_name << " v" << rep.config.version << "\n"
                  << "Scarlett Native: " << (rep.config.IsNativeScarlett() ? "YES (XboxGen9)" : "NO") << "\n"
                  << "Executable: " << rep.executable_path << " (PE64=" << (rep.main_pe.is_pe64 ? "yes" : "no") << ")\n"
                  << "Imports (" << rep.main_pe.imports.size() << " modules):\n";
        for (const auto& mod : rep.main_pe.imports) {
            std::cout << "  - " << mod.dll_name
                      << (mod.is_scarlett_hle ? " [HLE -> " + mod.redirected_target + "]" : "")
                      << " (" << mod.symbols.size() << " symbols)\n";
        }
        return 0;
    }

    if (cmd == "compat" && argc >= 3) {
        scarlettx::EmulatorConfig cfg;
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--volume-key") cfg.volume_key = argv[++i];
        }
        const std::filesystem::path target_p(argv[2]);
        std::error_code file_ec;
        if (std::filesystem::is_regular_file(target_p, file_ec)) {
            scarlettx::XvdContainer xvd;
            if (xvd.OpenFile(argv[2]).ok() && xvd.Header().is_encrypted && cfg.volume_key.empty()) {
                std::cout << "=== ScarlettEdge Compatibility Report ===\n"
                          << "Container:       " << target_p.filename().string() << "\n"
                          << "Drive Size:      " << xvd.Header().drive_size << " bytes\n"
                          << "XVD Containers:  1 found (1 encrypted)\n"
                          << "Readiness:       ENCRYPTED_CONTAINER (supply --volume-key <hex|file>)\n";
                return 0;
            }
        }
        scarlettx::Emulator emu(cfg);
        scarlettx::PackageInspectionReport rep;
        auto st = emu.InspectPackage(argv[2], rep);
        if (!st.ok()) {
            std::cerr << "[compat] Error: " << st.message << "\n";
            return 1;
        }
        size_t hle_mods = 0, os_mods = 0, total_syms = 0;
        for (const auto& mod : rep.main_pe.imports) {
            total_syms += mod.symbols.size();
            if (mod.is_scarlett_hle) ++hle_mods;
            else ++os_mods;
        }
        size_t xvd_total = 0, xvd_encrypted = 0, xvd_decrypted = 0;
        std::error_code ec;
        auto inspect_xvd_file = [&](const std::filesystem::path& p) {
            auto ext = p.extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".xvd" || ext == ".xvc") {
                scarlettx::XvdContainer xvd;
                if (xvd.OpenFile(p.string()).ok()) {
                    ++xvd_total;
                    if (xvd.Header().is_encrypted) {
                        ++xvd_encrypted;
                        if (!cfg.volume_key.empty()) ++xvd_decrypted;
                    }
                }
            }
        };
        if (std::filesystem::is_regular_file(target_p, ec)) {
            inspect_xvd_file(target_p);
        } else {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[2], ec)) {
                if (entry.is_regular_file()) inspect_xvd_file(entry.path());
            }
        }
        const bool locked = (xvd_encrypted > xvd_decrypted);
        std::cout << "=== ScarlettEdge Compatibility Report ===\n"
                  << "Title:           " << rep.config.display_name << " (" << rep.config.title_id << ")\n"
                  << "Gen9 Native:     " << (rep.config.IsNativeScarlett() ? "YES (XboxGen9)" : "NO") << "\n"
                  << "Executable:      " << rep.executable_path << " (PE64=" << (rep.main_pe.is_pe64 ? "yes" : "no") << ")\n"
                  << "Import Modules:  " << rep.main_pe.imports.size() << " total ("
                  << hle_mods << " Scarlett HLE, " << os_mods << " OS/local, " << total_syms << " symbols)\n"
                  << "XVD Containers:  " << xvd_total << " found (" << xvd_encrypted << " encrypted)\n"
                  << "Readiness:       "
                  << (locked
                          ? "ENCRYPTED_CONTAINER (supply --volume-key <hex|file>)"
                          : (rep.main_pe.is_pe64 ? "READY_TO_BOOT" : "UNSUPPORTED_ARCH"))
                  << "\n";
        return 0;
    }

    if (cmd == "boot" && argc >= 3) {
        scarlettx::EmulatorConfig cfg;
        cfg.in_process = true;  // Default to reliable in-process PE mapper with direct IAT redirection
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--headless") {
                cfg.headless = true;
            } else if (arg == "--in-process") {
                cfg.in_process = true;
            } else if (arg == "--separate-process" || arg == "--out-of-process") {
                cfg.in_process = false;
            } else if (arg == "--gpu") {
                cfg.prefer_warp = false;
            } else if (arg == "--warp") {
                cfg.prefer_warp = true;
            } else if (arg == "--trace") {
                cfg.trace = true;
            } else if (arg == "--stats") {
                cfg.stats = true;
            } else if (arg == "--fcap" && i + 1 < argc) {
                char* end = nullptr;
                const unsigned long cap = std::strtoul(argv[++i], &end, 10);
                if (end == argv[i] || *end != '\0' || cap > 100000) {
                    std::cerr << "[boot] --fcap needs a frame rate (0 = no cap), got '" << argv[i] << "'\n";
                    return 1;
                }
                cfg.fps_cap = static_cast<int32_t>(cap);
            } else if (arg == "--force-relocate") {
                cfg.force_relocate = true;
            } else if (arg == "--frames" && i + 1 < argc) {
                char* end = nullptr;
                const unsigned long frames = std::strtoul(argv[++i], &end, 10);
                if (end == argv[i] || *end != '\0') {
                    std::cerr << "[boot] --frames needs a number, got '" << argv[i] << "'\n";
                    return 1;
                }
                cfg.max_frames = static_cast<uint32_t>(frames);
            } else if (arg == "--rt-dump" && i + 1 < argc) {
                cfg.rt_dump_path = argv[++i];
            } else if (arg == "--raster-dump" && i + 1 < argc) {
                cfg.raster_dump_path = argv[++i];
            } else if (arg == "--volume-key" && i + 1 < argc) {
                cfg.volume_key = argv[++i];
            } else {
                std::cerr << "[boot] Unknown or incomplete option: " << arg << "\n";
                PrintUsage();
                return 1;
            }
        }
        scarlettx::Emulator emu(cfg);
        auto st = emu.BootPackage(argv[2]);
        if (!st.ok()) {
            std::cerr << "[boot] Error: " << st.message << "\n";
            return 1;
        }
        std::cout << "[boot] Guest exited cleanly.\n";
        return 0;
    }

    PrintUsage();
    return 1;
}
