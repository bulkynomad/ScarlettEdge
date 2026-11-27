#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#endif

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/core/flight_triage.h"
#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/rt_bvh.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/runtime/emulator.h"

namespace {

// Ensures scarlettx.exe itself always has an IMAGE_TLS_DIRECTORY64 and owns
// _tls_index == 0 with an unused 64-byte slot-0 block, preventing any host DLL
// (such as ucrtbase/msvcp140/d3d12) from acquiring _tls_index == 0 before a
// mapped guest executable swaps TEB ThreadLocalStoragePointer[0].
thread_local volatile uint8_t g_scarlettx_slot0_anchor[64] = {};

void PrintUsage() {
    std::cout << "ScarlettEdge (ScarlettX Fork) - Xbox Series X|S (Scarlett) HLE Emulator\n"
              << "Usage:\n"
              << "  scarlettx inspect <package_dir_or_xvc> [--volume-key hex|file] [--keyring dir]\n"
              << "  scarlettx compat <package_dir_or_xvc> [--volume-key hex|file] [--keyring dir] [--lockhart|--series-s]\n"
              << "  scarlettx boot <package_dir_or_xvc> [--headless|--windowed] [--in-process|--separate-process] [--gpu|--warp] [--lockhart|--series-s] [--debug-layer] [--trace] [--stats] [--fcap N] [--force-relocate] [--frames N] [--volume-key hex|file] [--keyring dir] [--rt-dump path.ppm] [--raster-dump path.ppm] [--flight-recorder [dir]] [--hang-timeout sec]\n"
              << "  scarlettx diag [--full] [--json] [--flight-recorder [dir]]\n"
              << "  scarlettx triage <flight_dir> [--emit-stubs] [--json]\n";
}

std::string ResolveVolumeKeyWithEnv(const std::string& cli_key) {
    if (!cli_key.empty()) {
        return cli_key;
    }
    if (const char* env_key = std::getenv("SCARLETTX_VOLUME_KEY"); env_key && *env_key) {
        return env_key;
    }
    return {};
}

bool ParseInspectOrCompatFlags(int argc, char** argv, const char* subcmd, scarlettx::EmulatorConfig& cfg) {
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--volume-key" || arg == "--keyring") && i + 1 < argc) {
            cfg.volume_key = argv[++i];
        } else if (arg == "--lockhart" || arg == "--series-s") {
            cfg.lockhart_mode = true;
            cfg.emulated_device = scarlettx::DeviceType::XboxScarlettLockhart;
        } else if (arg == "--anaconda" || arg == "--series-x") {
            cfg.lockhart_mode = false;
            cfg.emulated_device = scarlettx::DeviceType::XboxScarlettAnaconda;
        } else {
            std::cerr << "[" << subcmd << "] Unknown or incomplete option: " << arg << "\n";
            PrintUsage();
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    g_scarlettx_slot0_anchor[0] = 1;

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::string cmd = argv[1];
    if (cmd == "triage" && argc >= 3) {
        const std::string dir = argv[2];
        bool emit_stubs = false;
        bool as_json = false;
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--emit-stubs") {
                emit_stubs = true;
            } else if (arg == "--json") {
                as_json = true;
            } else {
                std::cerr << "[triage] Unknown option: " << arg << "\n";
                PrintUsage();
                return 1;
            }
        }
        scarlettx::FlightTriageReport report;
        const auto st = scarlettx::TriageFlightDirectory(dir, report);
        if (!st.ok()) {
            std::cerr << "[triage] Error: " << st.message << "\n";
            return 1;
        }
        if (as_json) {
            std::cout << scarlettx::FormatFlightTriageJson(report);
        } else {
            std::cout << scarlettx::FormatFlightTriageText(report, emit_stubs);
        }
        return 0;
    }

    if (cmd == "diag") {
        bool diag_flight = false;
        bool diag_full = false;
        bool diag_json = false;
        std::string diag_flight_dir;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--flight-recorder") {
                diag_flight = true;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    diag_flight_dir = argv[++i];
                }
            } else if (arg == "--full") {
                diag_full = true;
            } else if (arg == "--json") {
                diag_json = true;
            } else {
                std::cerr << "[diag] Unexpected extra arguments: " << arg << "\n";
                PrintUsage();
                return 1;
            }
        }
        if (diag_flight) {
            scarlettx::FlightRecorderOptions fropts;
            fropts.output_dir = diag_flight_dir.empty() ? "flight_logs" : diag_flight_dir;
            fropts.hang_timeout_ms = 0;
            scarlettx::FlightRecorder::Instance().Enable(fropts);
        }
        scarlettx::D3D12Backend backend;
        const bool prefer_warp = !(diag_full || diag_json);
        auto st = backend.Initialize(prefer_warp, false);
        if (!st.ok()) {
            if (diag_flight) {
                scarlettx::FlightRecorder::Instance().RecordEvent(
                    scarlettx::FlightEventCategory::GpuDeviceRemoved,
                    "diag", st.message.c_str(), static_cast<int32_t>(st.code));
                scarlettx::FlightRecorder::Instance().WriteSummaryReport();
                scarlettx::FlightRecorder::Instance().Disable();
            }
            std::cerr << "[diag] D3D12 init failed: " << st.message << "\n";
            return 1;
        }
        const auto& info = backend.AdapterInfo();

        std::string sum_json;
        if (diag_flight) {
            scarlettx::FlightRecorder::Instance().RecordEvent(
                scarlettx::FlightEventCategory::HleCall,
                "diag", info.description.c_str(), 0);
            scarlettx::FlightRecorder::Instance().WriteSummaryReport(&sum_json, nullptr);
            scarlettx::FlightRecorder::Instance().Disable();
        }

        if (!diag_full && !diag_json) {
            std::cout << "[diag] Adapter: " << info.description
                      << " (WARP=" << (info.is_warp ? "yes" : "no")
                      << ", Device5=" << (info.supports_device5 ? "yes" : "no")
                      << ", DXR Tier=" << info.raytracing_tier << ")\n";
            if (diag_flight) {
                std::cout << "[diag] Flight summary: " << sum_json << "\n";
            }
            return 0;
        }

        // Full host readiness diagnostics
        std::string highest_sm = "unknown";
        bool sm_6_6_plus = false;
        uint32_t mesh_shader_tier = 0;
        std::string mesh_shader_tier_name = "None";
        bool vulkan_dll_present = false;
        bool vulkan_1_3_ready = false;
        std::string vulkan_api_version = "unavailable";
        bool virtual_alloc2_available = false;
        bool mfplat_present = false;
        bool mfreadwrite_present = false;
        bool xaudio2_host_ready = false;
        std::string xaudio2_host_dll = "none";

#if defined(_WIN32)
        if (auto* dev = static_cast<ID3D12Device*>(backend.NativeDevice())) {
            static constexpr uint32_t kShaderModelCandidates[] = {
                0x69u, 0x68u, 0x67u, 0x66u, 0x65u, 0x64u, 0x63u, 0x62u, 0x61u, 0x60u, 0x51u
            };
            for (uint32_t cand : kShaderModelCandidates) {
                D3D12_FEATURE_DATA_SHADER_MODEL sm_data{};
                sm_data.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(cand);
                if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm_data, sizeof(sm_data)))) {
                    const uint32_t supported = static_cast<uint32_t>(sm_data.HighestShaderModel);
                    const uint32_t sm_major = (supported >> 4u) & 0xFu;
                    const uint32_t sm_minor = supported & 0xFu;
                    highest_sm = std::to_string(sm_major) + "." + std::to_string(sm_minor);
                    sm_6_6_plus = (supported >= 0x66u);
                    break;
                }
            }

            D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7)))) {
                mesh_shader_tier = static_cast<uint32_t>(opts7.MeshShaderTier);
                if (opts7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1) {
                    mesh_shader_tier_name = "Tier 1";
                }
            }
        }

        // Probe Vulkan 1.3 loader presence & instance version
        if (HMODULE vk_mod = LoadLibraryA("vulkan-1.dll")) {
            vulkan_dll_present = true;
            using VkEnumVerFn = int32_t(__stdcall*)(uint32_t*);
            if (auto fn = reinterpret_cast<VkEnumVerFn>(GetProcAddress(vk_mod, "vkEnumerateInstanceVersion"))) {
                uint32_t ver = 0;
                if (fn(&ver) == 0 && ver != 0) {
                    const uint32_t vk_maj = (ver >> 22u) & 0x7Fu;
                    const uint32_t vk_min = (ver >> 12u) & 0x3FFu;
                    const uint32_t vk_pat = ver & 0xFFFu;
                    vulkan_api_version = std::to_string(vk_maj) + "." + std::to_string(vk_min) + "." + std::to_string(vk_pat);
                    vulkan_1_3_ready = (vk_maj > 1u) || (vk_maj == 1u && vk_min >= 3u);
                } else {
                    vulkan_api_version = "1.0";
                }
            } else {
                vulkan_api_version = "1.0";
            }
            FreeLibrary(vk_mod);
        }

        // Probe VirtualAlloc2 in kernelbase.dll
        {
            bool loaded_kb = false;
            HMODULE kb_mod = GetModuleHandleA("kernelbase.dll");
            if (!kb_mod) {
                kb_mod = LoadLibraryA("kernelbase.dll");
                loaded_kb = (kb_mod != nullptr);
            }
            if (kb_mod) {
                virtual_alloc2_available = (GetProcAddress(kb_mod, "VirtualAlloc2") != nullptr);
                if (loaded_kb) {
                    FreeLibrary(kb_mod);
                }
            }
        }

        // Probe Windows Media Foundation DLLs
        if (HMODULE mf_mod = LoadLibraryA("mfplat.dll")) {
            mfplat_present = true;
            FreeLibrary(mf_mod);
        }
        if (HMODULE mfrw_mod = LoadLibraryA("mfreadwrite.dll")) {
            mfreadwrite_present = true;
            FreeLibrary(mfrw_mod);
        }

        // Probe host OS XAudio2 in System32 (avoiding our own build/xaudio2_9.dll shim)
        {
            wchar_t sys_dir[MAX_PATH] = {};
            if (GetSystemDirectoryW(sys_dir, MAX_PATH) > 0) {
                static constexpr const wchar_t* kHostXAudioCandidates[] = {
                    L"xaudio2_9.dll",
                    L"xaudio2_9redist.dll",
                    L"xaudio2_8.dll"
                };
                static constexpr const char* kHostXAudioNames[] = {
                    "xaudio2_9.dll",
                    "xaudio2_9redist.dll",
                    "xaudio2_8.dll"
                };
                for (size_t idx = 0; idx < 3; ++idx) {
                    std::wstring full_path = std::wstring(sys_dir) + L"\\" + kHostXAudioCandidates[idx];
                    if (HMODULE xa_mod = LoadLibraryW(full_path.c_str())) {
                        if (GetProcAddress(xa_mod, "XAudio2Create") != nullptr) {
                            xaudio2_host_ready = true;
                            xaudio2_host_dll = kHostXAudioNames[idx];
                            FreeLibrary(xa_mod);
                            break;
                        }
                        FreeLibrary(xa_mod);
                    }
                }
            }
        }
#endif

        const bool media_foundation_ready = mfplat_present && mfreadwrite_present;
        std::string dxr_tier_name = "None";
        if (info.raytracing_tier >= 12u) {
            dxr_tier_name = "1.2";
        } else if (info.raytracing_tier >= 11u) {
            dxr_tier_name = "1.1";
        } else if (info.raytracing_tier >= 10u) {
            dxr_tier_name = "1.0";
        }
        const std::string adapter_type = info.is_warp ? "WARP" : "HW";

        if (diag_json) {
            auto json_escape = [](const std::string& s) -> std::string {
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
                                char buf[8];
                                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                                out += buf;
                            } else {
                                out.push_back(static_cast<char>(c));
                            }
                            break;
                    }
                }
                return out;
            };

            std::cout << "{\n"
                      << "  \"adapter_name\": \"" << json_escape(info.description) << "\",\n"
                      << "  \"adapter_type\": \"" << adapter_type << "\",\n"
                      << "  \"is_warp\": " << (info.is_warp ? "true" : "false") << ",\n"
                      << "  \"vendor_id\": " << info.vendor_id << ",\n"
                      << "  \"device_id\": " << info.device_id << ",\n"
                      << "  \"d3d12_device5\": " << (info.supports_device5 ? "true" : "false") << ",\n"
                      << "  \"highest_shader_model\": \"" << json_escape(highest_sm) << "\",\n"
                      << "  \"shader_model_6_6_plus\": " << (sm_6_6_plus ? "true" : "false") << ",\n"
                      << "  \"dxr_tier\": " << info.raytracing_tier << ",\n"
                      << "  \"dxr_tier_name\": \"" << dxr_tier_name << "\",\n"
                      << "  \"mesh_shader_tier\": " << mesh_shader_tier << ",\n"
                      << "  \"mesh_shader_tier_name\": \"" << mesh_shader_tier_name << "\",\n"
                      << "  \"vulkan_dll_present\": " << (vulkan_dll_present ? "true" : "false") << ",\n"
                      << "  \"vulkan_1_3_ready\": " << (vulkan_1_3_ready ? "true" : "false") << ",\n"
                      << "  \"vulkan_api_version\": \"" << json_escape(vulkan_api_version) << "\",\n"
                      << "  \"virtual_alloc2_available\": " << (virtual_alloc2_available ? "true" : "false") << ",\n"
                      << "  \"media_foundation_ready\": " << (media_foundation_ready ? "true" : "false") << ",\n"
                      << "  \"mfplat_present\": " << (mfplat_present ? "true" : "false") << ",\n"
                      << "  \"mfreadwrite_present\": " << (mfreadwrite_present ? "true" : "false") << ",\n"
                      << "  \"xaudio2_host_ready\": " << (xaudio2_host_ready ? "true" : "false") << ",\n"
                      << "  \"xaudio2_host_dll\": \"" << json_escape(xaudio2_host_dll) << "\"";
            if (diag_flight) {
                std::cout << ",\n  \"flight_summary_path\": \"" << json_escape(sum_json) << "\"";
            }
            std::cout << "\n}\n";
            return 0;
        }

        auto tag = [](bool ok) -> const char* { return ok ? "[OK]  " : "[WARN]"; };
        std::cout << "=== ScarlettEdge Host Diagnostics (--full) ===\n"
                  << "  " << tag(!info.description.empty()) << " Host GPU Adapter:       "
                  << (info.description.empty() ? "unknown" : info.description)
                  << " (" << adapter_type << ")\n"
                  << "  " << tag(info.supports_device5) << " D3D12 Device5:          "
                  << (info.supports_device5 ? "yes" : "no") << "\n"
                  << "  " << tag(sm_6_6_plus) << " Highest Shader Model:   SM " << highest_sm
                  << " (SM 6.6+ ready: " << (sm_6_6_plus ? "yes" : "no") << ")\n"
                  << "  " << tag(info.raytracing_tier >= 10u) << " DXR Tier:               "
                  << dxr_tier_name << " (tier=" << info.raytracing_tier << ")\n"
                  << "  " << tag(mesh_shader_tier >= 10u) << " Mesh Shader Tier:       "
                  << mesh_shader_tier_name << " (tier=" << mesh_shader_tier << ")\n"
                  << "  " << tag(vulkan_1_3_ready) << " Vulkan 1.3:             "
                  << (vulkan_dll_present ? ("vulkan-1.dll present (API " + vulkan_api_version + ")")
                                         : "vulkan-1.dll not found") << "\n"
                  << "  " << tag(virtual_alloc2_available) << " VirtualAlloc2:          "
                  << (virtual_alloc2_available ? "kernelbase.dll!VirtualAlloc2 available"
                                               : "unavailable") << "\n"
                  << "  " << tag(media_foundation_ready) << " Media Foundation:       "
                  << "mfplat.dll=" << (mfplat_present ? "yes" : "no")
                  << ", mfreadwrite.dll=" << (mfreadwrite_present ? "yes" : "no") << "\n"
                  << "  " << tag(xaudio2_host_ready) << " XAudio2 Host Audio:     "
                  << (xaudio2_host_ready ? ("ready (" + xaudio2_host_dll + ")") : "unavailable") << "\n";
        if (diag_flight) {
            std::cout << "[diag] Flight summary: " << sum_json << "\n";
        }
        return 0;
    }

    if (cmd == "inspect" && argc >= 3) {
        scarlettx::EmulatorConfig cfg;
        if (!ParseInspectOrCompatFlags(argc, argv, "inspect", cfg)) {
            return 1;
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
        if (!ParseInspectOrCompatFlags(argc, argv, "compat", cfg)) {
            return 1;
        }
        const std::string effective_key = ResolveVolumeKeyWithEnv(cfg.volume_key);
        if (!effective_key.empty()) {
            cfg.volume_key = effective_key;
        }

        const std::filesystem::path target_p(argv[2]);
        std::error_code file_ec;
        if (std::filesystem::is_regular_file(target_p, file_ec)) {
            scarlettx::XvdContainer xvd;
            if (xvd.OpenFile(argv[2]).ok() && xvd.Header().is_encrypted && effective_key.empty()) {
                std::vector<scarlettx::XvdEmbeddedFile> fs_files;
                const bool has_fs = xvd.ListEmbeddedFiles(fs_files).ok() && !fs_files.empty();
                std::cout << "=== ScarlettEdge Compatibility Report ===\n"
                          << "Container:       " << target_p.filename().string() << "\n"
                          << "Drive Size:      " << xvd.Header().drive_size << " bytes\n"
                          << "XVD Containers:  1 found (1 encrypted)\n";
                if (has_fs) {
                    std::cout << "Embedded Files:  " << fs_files.size()
                              << " entries discovered via unencrypted FS-MD NTFS ($MFT)\n";
                }
                std::cout << "Readiness:       ENCRYPTED_CONTAINER (supply --volume-key <hex|file>)\n";
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
            if (ext == ".xvd" || ext == ".xvc" || ext == ".msixvc") {
                scarlettx::XvdContainer xvd;
                if (xvd.OpenFile(p.string()).ok()) {
                    ++xvd_total;
                    if (xvd.Header().is_encrypted) {
                        ++xvd_encrypted;
                        if (!effective_key.empty()) ++xvd_decrypted;
                    }
                }
            }
        };
        if (std::filesystem::is_regular_file(target_p, ec)) {
            inspect_xvd_file(target_p);
        } else {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[2], ec)) {
                if (entry.is_regular_file(ec)) inspect_xvd_file(entry.path());
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
            } else if (arg == "--windowed") {
                cfg.headless = false;
            } else if (arg == "--in-process") {
                cfg.in_process = true;
            } else if (arg == "--separate-process" || arg == "--out-of-process") {
                cfg.in_process = false;
            } else if (arg == "--gpu") {
                cfg.prefer_warp = false;
            } else if (arg == "--warp") {
                cfg.prefer_warp = true;
            } else if (arg == "--lockhart" || arg == "--series-s") {
                cfg.lockhart_mode = true;
                cfg.emulated_device = scarlettx::DeviceType::XboxScarlettLockhart;
            } else if (arg == "--anaconda" || arg == "--series-x") {
                cfg.lockhart_mode = false;
                cfg.emulated_device = scarlettx::DeviceType::XboxScarlettAnaconda;
            } else if (arg == "--debug-layer") {
                cfg.enable_debug_layer = true;
            } else if (arg == "--trace") {
                cfg.trace = true;
            } else if (arg == "--stats") {
                cfg.stats = true;
            } else if (arg == "--fcap" && i + 1 < argc) {
                const char* val_str = argv[++i];
                if (std::string(val_str) == "-1") {
                    cfg.fps_cap = -1;
                } else {
                    if (val_str[0] == '-') {
                        std::cerr << "[boot] --fcap needs a frame rate (0 = no cap), got '" << val_str << "'\n";
                        return 1;
                    }
                    char* end = nullptr;
                    const unsigned long cap = std::strtoul(val_str, &end, 10);
                    if (end == val_str || *end != '\0' || cap > 100000ul) {
                        std::cerr << "[boot] --fcap needs a frame rate (0 = no cap), got '" << val_str << "'\n";
                        return 1;
                    }
                    cfg.fps_cap = static_cast<int32_t>(cap);
                }
            } else if (arg == "--force-relocate") {
                cfg.force_relocate = true;
            } else if (arg == "--frames" && i + 1 < argc) {
                const char* val_str = argv[++i];
                if (val_str[0] == '-') {
                    std::cerr << "[boot] --frames needs a non-negative number, got '" << val_str << "'\n";
                    return 1;
                }
                char* end = nullptr;
                const unsigned long long frames = std::strtoull(val_str, &end, 10);
                if (end == val_str || *end != '\0' || frames > 0xFFFFFFFFull) {
                    std::cerr << "[boot] --frames needs a number, got '" << val_str << "'\n";
                    return 1;
                }
                cfg.max_frames = static_cast<uint32_t>(frames);
            } else if (arg == "--rt-dump" && i + 1 < argc) {
                cfg.rt_dump_path = argv[++i];
            } else if (arg == "--raster-dump" && i + 1 < argc) {
                cfg.raster_dump_path = argv[++i];
            } else if ((arg == "--volume-key" || arg == "--keyring") && i + 1 < argc) {
                cfg.volume_key = argv[++i];
            } else if (arg == "--flight-recorder") {
                cfg.flight_recorder = true;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    cfg.flight_recorder_dir = argv[++i];
                }
            } else if (arg == "--hang-timeout" && i + 1 < argc) {
                const char* val_str = argv[++i];
                if (val_str[0] == '-') {
                    std::cerr << "[boot] --hang-timeout needs non-negative seconds, got '" << val_str << "'\n";
                    return 1;
                }
                char* end = nullptr;
                const unsigned long sec = std::strtoul(val_str, &end, 10);
                if (end == val_str || *end != '\0' || sec > 86400ul) {
                    std::cerr << "[boot] --hang-timeout needs seconds (0..86400), got '" << val_str << "'\n";
                    return 1;
                }
                cfg.hang_timeout_sec = static_cast<uint32_t>(sec);
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
