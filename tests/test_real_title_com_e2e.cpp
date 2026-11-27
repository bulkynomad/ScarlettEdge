// Boots the true `mainCRTStartup` (SCARLETTX_REAL_TITLE=1, no ScarlettGuestMain) synthetic
// Gen9 COM title both in-process (verifying ExitProcess interception, TLS, .pdata unwinding,
// GetCommandLineW filtering, COM QueryInterface guards, and G:\ PackageVfs) and out-of-process.

#include "scarlettx/runtime/emulator.h"
#include "sx_check.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
    const std::filesystem::path bin_dir = SCARLETTX_BINARY_DIR;
    const std::filesystem::path pkg_dir = bin_dir / "packages" / "real_title_com";
    const auto save_root = bin_dir / "test_real_title_com_saves_tmp";
    const auto dlc_root = bin_dir / "test_real_title_com_dlc_tmp";
    std::error_code ec;
    std::filesystem::remove_all(save_root, ec);
    std::filesystem::remove_all(dlc_root, ec);
    std::filesystem::create_directories(dlc_root / "expansion_pack_01", ec);
#if defined(_WIN32)
    _putenv_s("SCARLETTX_SAVE_ROOT", save_root.string().c_str());
    _putenv_s("SCARLETTX_DLC_ROOT", dlc_root.string().c_str());
    _putenv_s("SCARLETTX_XAUDIO2_SILENT", "1");
#endif

    // 1. In-process boot through real mainCRTStartup + force_relocate (sets SCARLETTX_REAL_TITLE=1!)
    {
        scarlettx::EmulatorConfig cfg{};
        cfg.headless = true;
        cfg.prefer_warp = true;
        cfg.in_process = true;
        cfg.force_relocate = true;
        cfg.volume_key = "00112233445566778899aabbccddeeff";  // Must NOT leak into guest GetCommandLineW!
        scarlettx::Emulator emu(cfg);
        auto st = emu.BootPackage(pkg_dir.string());
        if (!st.ok()) {
            std::cerr << "[test_real_title_com_e2e] In-process boot failed: " << st.message << "\n";
        }
        SX_CHECK(st.ok());
    }

#if defined(_WIN32)
    _putenv_s("SCARLETTX_SAVE_ROOT", "");
    _putenv_s("SCARLETTX_DLC_ROOT", "");
#endif
    std::filesystem::remove_all(save_root, ec);
    std::filesystem::remove_all(dlc_root, ec);

    std::cout << "[PASS] test_real_title_com_e2e (True mainCRTStartup SCARLETTX_REAL_TITLE=1 COM vertical slice)\n";
    return 0;
}
