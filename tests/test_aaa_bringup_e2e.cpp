#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/audio/xaudio2_abi.h"
#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/media/codecs_abi.h"
#include "scarlettx/runtime/emulator.h"

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_aaa_bringup_e2e] FAILED: " #cond                \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#include "sx_check.h"

namespace {

void WriteU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
}

// Builds a valid 32x32 4-frame KB2i video container with frame-offset table + YCbCr 4:2:0 planes.
std::vector<uint8_t> BuildSyntheticKb2Movie() {
    constexpr uint32_t w = 32, h = 32, frames = 4;
    constexpr uint32_t y_bytes = w * h;
    constexpr uint32_t uv_bytes = (w / 2) * (h / 2);
    constexpr uint32_t frame_sz = y_bytes + 2 * uv_bytes;
    const uint32_t hdr_and_table = 44u + (frames + 1u) * 4u;
    std::vector<uint8_t> kb2(hdr_and_table + frames * frame_sz, 0);

    std::memcpy(kb2.data(), "KB2i", 4);
    WriteU32LE(kb2.data() + 4, static_cast<uint32_t>(kb2.size() - 8));
    WriteU32LE(kb2.data() + 8, frames);
    WriteU32LE(kb2.data() + 12, frame_sz);
    WriteU32LE(kb2.data() + 16, frames);
    WriteU32LE(kb2.data() + 20, w);
    WriteU32LE(kb2.data() + 24, h);
    WriteU32LE(kb2.data() + 28, 60);
    WriteU32LE(kb2.data() + 32, 1);
    WriteU32LE(kb2.data() + 36, 0);
    WriteU32LE(kb2.data() + 40, 0);  // track_count = 0

    for (uint32_t f = 0; f <= frames; ++f) {
        const uint32_t off = hdr_and_table + f * frame_sz;
        WriteU32LE(kb2.data() + 44u + f * 4u, (f == 0) ? (off | 1u) : off);
    }
    for (uint32_t f = 0; f < frames; ++f) {
        uint8_t* fp = kb2.data() + hdr_and_table + f * frame_sz;
        std::memset(fp, static_cast<int>(64 + f * 40), y_bytes);
        std::memset(fp + y_bytes, static_cast<int>(100 + f * 15), uv_bytes);
        std::memset(fp + y_bytes + uv_bytes, static_cast<int>(150 - f * 15), uv_bytes);
    }
    return kb2;
}

void SetEnvStr(const char* k, const std::string& v) {
#if defined(_WIN32)
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

}  // namespace

int main() {
    sx_test::ScopedTempDir sandbox("test_aaa_bringup");
    const std::filesystem::path bin_dir = SCARLETTX_BINARY_DIR;
    const std::filesystem::path& tmp_dir = sandbox.path();

    // -----------------------------------------------------------------------
    // 1. Package UE5 Vertical Slice into an AES-256-XTS Encrypted GDFX .xvc
    // -----------------------------------------------------------------------
    const auto ue5_pkg_dir = bin_dir / "packages" / "ue5_slice";
    const auto ue5_exe_bytes = ReadAllBytes(ue5_pkg_dir / "ue5_slice.exe");
    const auto ue5_cfg_bytes = ReadAllBytes(ue5_pkg_dir / "MicrosoftGame.Config");
    SX_CHECK(!ue5_exe_bytes.empty());
    SX_CHECK(!ue5_cfg_bytes.empty());

    // Build streamed asset pak: 16 bytes BC1 blocks + 2048-byte retail XMA2 MDCT audio block, compressed with RFC 1951 GDeflate
    std::vector<float> sine_pcm(512, 0.0f);
    for (size_t i = 0; i < sine_pcm.size(); ++i) {
        sine_pcm[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }
    std::vector<uint8_t> xma2_block(2048, 0);
    uint32_t xma2_written = 0;
    SX_CHECK(Xma2EncodeMdctBlock(sine_pcm.data(), 512, 1, xma2_block.data(), 2048, &xma2_written) == 0);
    SX_CHECK(xma2_written == 2048u);

    std::vector<uint8_t> raw_pak(16 + 2048, 0);
    const uint8_t kBc1Blocks[16] = {
        0x00, 0xF8, 0x1F, 0x00, 0xAA, 0x55, 0xAA, 0x55,
        0xE0, 0x07, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
    };
    std::memcpy(raw_pak.data(), kBc1Blocks, 16);
    std::memcpy(raw_pak.data() + 16, xma2_block.data(), 2048);

    std::vector<uint8_t> comp_pak(raw_pak.size() * 2 + 128, 0);
    size_t comp_pak_sz = 0;
    SX_CHECK(XboxCodecGDeflateCompress(raw_pak.data(), raw_pak.size(), comp_pak.data(), comp_pak.size(), &comp_pak_sz) == 0);
    comp_pak.resize(comp_pak_sz);

    const auto kb2_movie = BuildSyntheticKb2Movie();
    const std::string dlc_manifest = "{\"dlc\":\"OpenWorldExpansion\",\"version\":\"1.0\"}";

    std::vector<scarlettx::XvdInputFile> xvc_files = {
        {"MicrosoftGame.Config", ue5_cfg_bytes},
        {"ue5_slice.exe", ue5_exe_bytes},
        {"Content/Movies/ue5_intro.kb2", kb2_movie},
        {"Content/Paks/nanite_chunk0.gdeflate", comp_pak},
        {"DLC/Expansion1/dlc.json", std::vector<uint8_t>(dlc_manifest.begin(), dlc_manifest.end())},
    };

    std::array<uint8_t, 32> aes_xts_key{};
    std::string hex_key;
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < aes_xts_key.size(); ++i) {
        aes_xts_key[i] = static_cast<uint8_t>(0x5Au ^ (i * 7u));
        hex_key.push_back(kHex[aes_xts_key[i] >> 4]);
        hex_key.push_back(kHex[aes_xts_key[i] & 0xF]);
    }

    const auto ue5_xvc_bytes = scarlettx::XvdContainer::BuildEncryptedGdfxXvc(
        xvc_files, aes_xts_key.data(), aes_xts_key.size(), 1);
    const auto ue5_xvc_path = tmp_dir / "ue5_vertical_slice.xvc";
    {
        std::ofstream ofs(ue5_xvc_path, std::ios::binary);
        SX_CHECK(ofs.good());
        ofs.write(reinterpret_cast<const char*>(ue5_xvc_bytes.data()),
                  static_cast<std::streamsize>(ue5_xvc_bytes.size()));
    }

    // Must reject encrypted .xvc boot when --volume-key is omitted!
    SetEnvStr("SCARLETTX_VOLUME_KEY", "");
    SetEnvStr("SCARLETTX_UE5_PAK_UNCOMP_SIZE", std::to_string(raw_pak.size()));
    {
        scarlettx::EmulatorConfig no_key_cfg{};
        no_key_cfg.headless = true;
        no_key_cfg.prefer_warp = true;
        no_key_cfg.in_process = true;
        scarlettx::Emulator emu_nokey(no_key_cfg);
        SX_CHECK(!emu_nokey.BootPackage(ue5_xvc_path.string()).ok());
    }

    // Boot UE5 Vertical Slice directly from the AES-XTS encrypted GDFX .xvc with volume key!
    {
        scarlettx::EmulatorConfig xvc_cfg{};
        xvc_cfg.headless = true;
        xvc_cfg.prefer_warp = true;
        xvc_cfg.in_process = true;
        xvc_cfg.force_relocate = true;
        xvc_cfg.volume_key = hex_key;
        scarlettx::Emulator emu_ue5(xvc_cfg);

        scarlettx::PackageInspectionReport rep{};
        SX_CHECK(emu_ue5.InspectPackage(ue5_xvc_path.string(), rep).ok());
        SX_CHECK(rep.config.IsNativeScarlett());
        SX_CHECK(rep.main_pe.is_pe64);

        auto boot_st = emu_ue5.BootPackage(ue5_xvc_path.string());
        if (!boot_st.ok()) {
            std::cerr << "[test_aaa_bringup_e2e] UE5 encrypted XVC boot error: " << boot_st.message << "\n";
        }
        SX_CHECK(boot_st.ok());
    }

    // -----------------------------------------------------------------------
    // 2. Boot Open-World Vertical Slice (In-Process + Out-of-Process Persistence)
    // -----------------------------------------------------------------------
    std::vector<uint8_t> openworld_sector(4096, 0);
    for (size_t i = 0; i < openworld_sector.size(); ++i) {
        openworld_sector[i] = static_cast<uint8_t>((i * 13u + 0x21u) & 0xFFu);
    }
    std::vector<uint8_t> openworld_comp(8192, 0);
    size_t openworld_comp_sz = 0;
    SX_CHECK(XboxCodecGDeflateCompress(openworld_sector.data(), openworld_sector.size(),
                                       openworld_comp.data(), openworld_comp.size(), &openworld_comp_sz) == 0);
    const auto openworld_pak_path = tmp_dir / "openworld_sector0.gdeflate";
    {
        std::ofstream ofs(openworld_pak_path, std::ios::binary);
        SX_CHECK(ofs.good());
        ofs.write(reinterpret_cast<const char*>(openworld_comp.data()), static_cast<std::streamsize>(openworld_comp_sz));
    }

    const auto openworld_pkg_dir = bin_dir / "packages" / "openworld_slice";
    const auto title_storage_root = tmp_dir / "openworld_title_storage";
    std::error_code ec;
    std::filesystem::remove_all(title_storage_root, ec);
    SetEnvStr("SCARLETTX_OPENWORLD_STREAM_PAK", openworld_pak_path.string());
    SetEnvStr("SCARLETTX_SAVE_ROOT", title_storage_root.string());
    SetEnvStr("SCARLETTX_OPENWORLD_VERIFY_PERSIST", "0");

    // Run 1: In-process relocated PE64 boot (uploads persistent TitleStorage checkpoint + Vulkan Wave32 + 2MB alias)
    {
        scarlettx::EmulatorConfig openworld_inproc{};
        openworld_inproc.headless = true;
        openworld_inproc.prefer_warp = true;
        openworld_inproc.in_process = true;
        openworld_inproc.force_relocate = true;
        scarlettx::Emulator emu_openworld(openworld_inproc);
        auto st = emu_openworld.BootPackage(openworld_pkg_dir.string());
        if (!st.ok()) {
            std::cerr << "[test_aaa_bringup_e2e] Open-World in-process boot error: " << st.message << "\n";
        }
        SX_CHECK(st.ok());
    }

    // Run 2: Separate child-process boot verifying persistent XblTitleStorage blob survived on disk!
    SetEnvStr("SCARLETTX_OPENWORLD_VERIFY_PERSIST", "1");
    {
        scarlettx::EmulatorConfig openworld_proc{};
        openworld_proc.headless = true;
        openworld_proc.prefer_warp = true;
        openworld_proc.in_process = false;
        scarlettx::Emulator emu_openworld_proc(openworld_proc);
        auto st = emu_openworld_proc.BootPackage(openworld_pkg_dir.string());
        if (!st.ok()) {
            std::cerr << "[test_aaa_bringup_e2e] Open-World child-process boot error: " << st.message << "\n";
        }
        SX_CHECK(st.ok());
    }

    SetEnvStr("SCARLETTX_SAVE_ROOT", "");
    std::filesystem::remove_all(tmp_dir, ec);
    std::cout << "[PASS] test_aaa_bringup_e2e (Encrypted GDFX .xvc UE5 boot + Open-World Vulkan Wave32 & 2MB alias slice)\n";
    return 0;
}
