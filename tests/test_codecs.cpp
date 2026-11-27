#include "scarlettx/media/codecs_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
    std::cout << "[test_codecs] Testing Production Codecs (GDeflate/BCPack/XMA2/Bink)...\n";

    // 1. GDeflate compress & decompress
    const char raw_data[] = "ScarlettX GDeflate High-Throughput DirectStorage Stream Data Test 123456";
    size_t raw_len = sizeof(raw_data);
    std::vector<uint8_t> comp(256, 0);
    size_t comp_written = 0;
    SX_CHECK(XboxCodecGDeflateCompress(raw_data, raw_len, comp.data(), comp.size(), &comp_written) == 0);
    SX_CHECK(comp_written > 0);

    std::vector<uint8_t> decomp(raw_len, 0);
    size_t decomp_written = 0;
    SX_CHECK(XboxCodecGDeflateDecompress(comp.data(), comp_written, decomp.data(), decomp.size(), &decomp_written) == 0);
    SX_CHECK(decomp_written == raw_len);
    SX_CHECK(std::memcmp(raw_data, decomp.data(), raw_len) == 0);

    // 1b. Real GDeflate tile compression ratio & lossless roundtrip on 8 KiB structured asset
    {
        std::vector<uint8_t> asset(8192);
        for (size_t i = 0; i < asset.size(); ++i) {
            asset[i] = static_cast<uint8_t>((i / 16) % 7 + ((i % 16) < 4 ? 0xA0 : 0x20));
        }
        std::vector<uint8_t> gdef_out(8192 + 128, 0);
        size_t gdef_sz = 0;
        SX_CHECK(XboxCodecGDeflateCompress(asset.data(), asset.size(), gdef_out.data(), gdef_out.size(), &gdef_sz) == 0);
        SX_CHECK(gdef_out[0] == 0x04u && gdef_out[1] == 0xFBu);  // Microsoft GDeflate tile magic
        SX_CHECK(gdef_sz < asset.size() / 2);                   // Real compression (> 2:1 ratio)
        std::vector<uint8_t> restored(asset.size(), 0);
        size_t restored_sz = 0;
        SX_CHECK(XboxCodecGDeflateDecompress(gdef_out.data(), gdef_sz, restored.data(), restored.size(), &restored_sz) == 0);
        SX_CHECK(restored_sz == asset.size());
        SX_CHECK(std::memcmp(asset.data(), restored.data(), asset.size()) == 0);
    }

    // 2. BCPack compress & decompress + Hardware BC1 4x4 RGBA8 block unpack (format=0x101)
    std::vector<uint8_t> bc_comp(128, 0);
    size_t bc_comp_len = 0;
    SX_CHECK(XboxCodecBcPackCompress(raw_data, 64, bc_comp.data(), bc_comp.size(), &bc_comp_len) == 0);
    std::vector<uint8_t> bc_decomp(64, 0);
    SX_CHECK(XboxCodecBcPackDecompress(bc_comp.data(), bc_comp_len, bc_decomp.data(), bc_decomp.size(), 1) == 0);

    {
        // BC1 4x4 block: c0=0xF800 (pure red), c1=0x001F (pure blue), all 16 indices=0 -> 16 opaque red pixels
        const uint8_t bc1_blk[8] = {0x00u, 0xF8u, 0x1Fu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
        uint8_t unpacked_rgba[64]{};
        SX_CHECK(XboxCodecBcPackDecompress(bc1_blk, sizeof(bc1_blk), unpacked_rgba, sizeof(unpacked_rgba), 0x101u) == 0);
        for (size_t px = 0; px < 16u; ++px) {
            SX_CHECK(unpacked_rgba[px * 4u + 0] == 255u);
            SX_CHECK(unpacked_rgba[px * 4u + 1] == 0u);
            SX_CHECK(unpacked_rgba[px * 4u + 2] == 0u);
            SX_CHECK(unpacked_rgba[px * 4u + 3] == 255u);
        }
    }

    // 2b. Spec-compliant RFC 1950 Zlib & RFC 1951 Canonical Huffman DEFLATE (verified against Python zlib!)
    {
        const uint8_t zlib_fixed[] = {
            0x78u, 0x01u, 0x0Bu, 0x4Eu, 0x4Eu, 0x2Cu, 0xCAu, 0x49u, 0x2Du, 0x29u, 0x71u, 0x4Du, 0x49u,
            0x4Fu, 0x55u, 0x08u, 0x72u, 0x73u, 0x36u, 0xB4u, 0x34u, 0x35u, 0x54u, 0x70u, 0xCBu, 0xACu,
            0x48u, 0x4Du, 0x51u, 0xF0u, 0x28u, 0x4Du, 0x4Bu, 0xCBu, 0x4Du, 0xCCu, 0x53u, 0x08u, 0x49u,
            0x2Du, 0x2Eu, 0x51u, 0x54u, 0x08u, 0xA6u, 0xBAu, 0x42u, 0x00u, 0xE2u, 0x05u, 0x28u, 0x69u
        };
        const std::string expected_str =
            "ScarlettEdge RFC1951 Fixed Huffman Test! "
            "ScarlettEdge RFC1951 Fixed Huffman Test! "
            "ScarlettEdge RFC1951 Fixed Huffman Test! ";
        std::vector<uint8_t> z_out(expected_str.size(), 0);
        size_t z_written = 0;
        SX_CHECK(XboxCodecGDeflateDecompress(zlib_fixed, sizeof(zlib_fixed), z_out.data(), z_out.size(), &z_written) == 0);
        SX_CHECK(z_written == expected_str.size());
        SX_CHECK(std::memcmp(z_out.data(), expected_str.data(), expected_str.size()) == 0);

        // Dynamic Huffman (BTYPE=2, Python zlib level=9) golden vector: 125B -> 464B
        const uint8_t zlib_dyn[] = {
            0x78u, 0xDAu, 0xEDu, 0xCDu, 0x4Bu, 0x0Au, 0xC2u, 0x30u, 0x14u, 0x05u, 0xD0u, 0x2Du, 0xB5u,
            0xE9u, 0x7Fu, 0xD8u, 0x12u, 0xD1u, 0x82u, 0x88u, 0x34u, 0x20u, 0x99u, 0x5Du, 0x9Eu, 0xF1u,
            0xB5u, 0x06u, 0x93u, 0x46u, 0x62u, 0x04u, 0xDDu, 0xBDu, 0x6Eu, 0xC2u, 0x99u, 0x1Bu, 0x38u,
            0x47u, 0x9Fu, 0xC3u, 0x4Bu, 0x71u, 0xB4u, 0xFCu, 0xD0u, 0x50u, 0x86u, 0xA2u, 0xE3u, 0x94u,
            0x36u, 0x97u, 0x85u, 0x21u, 0xDFu, 0x2Bu, 0x79u, 0x6Bu, 0x76u, 0xCFu, 0x79u, 0xF6u, 0xB4u,
            0x62u, 0x70u, 0xC1u, 0xDCu, 0xD0u, 0xBBu, 0xFBu, 0x95u, 0x90u, 0xE5u, 0xA2u, 0x28u, 0xABu,
            0xBAu, 0x69u, 0x3Bu, 0x0Cu, 0x9Cu, 0x08u, 0x5Du, 0xDBu, 0xD4u, 0x55u, 0x59u, 0x88u, 0x3Cu,
            0xC3u, 0x96u, 0xBCu, 0x27u, 0x48u, 0x1Bu, 0xD9u, 0x24u, 0x95u, 0x42u, 0xA4u, 0xAFu, 0x34u,
            0xC9u, 0x43u, 0x2Fu, 0xA0u, 0x8Eu, 0xE3u, 0x74u, 0x82u, 0xD4u, 0xE3u, 0x1Eu, 0xFAu, 0x7Fu,
            0xFEu, 0xECu, 0xFCu, 0x00u, 0x12u, 0x61u, 0xA0u, 0x39u
        };
        const std::string dyn_unit =
            "XboxSeriesX_ScarlettEdge_DynamicHuffman_Block_Alpha_0123456789_"
            "Beta_9876543210_Gamma_DirectStorage_RDNA2_SPIRV_DXIL_";
        const std::string expected_dyn = dyn_unit + dyn_unit + dyn_unit + dyn_unit;
        std::vector<uint8_t> dyn_out(expected_dyn.size(), 0);
        size_t dyn_written = 0;
        SX_CHECK(XboxCodecGDeflateDecompress(zlib_dyn, sizeof(zlib_dyn), dyn_out.data(), dyn_out.size(), &dyn_written) == 0);
        SX_CHECK(dyn_written == expected_dyn.size());
        SX_CHECK(std::memcmp(dyn_out.data(), expected_dyn.data(), expected_dyn.size()) == 0);
    }

    // 3. XMA2 decoder lifecycle
    XMA2_FORMAT fmt{};
    fmt.sampleRate = 48000;
    fmt.channelCount = 2;
    fmt.bitsPerSample = 16;
    XMA2_DECODER_HANDLE xma = nullptr;
    SX_CHECK(XboxCodecXma2CreateDecoder(&fmt, &xma) == 0);
    SX_CHECK(xma != nullptr);

    uint8_t dummy_xma[128]{0x55};
    SX_CHECK(XboxCodecXma2SubmitData(xma, dummy_xma, sizeof(dummy_xma)) == 0);

    uint8_t pcm_out[256]{};
    uint32_t samples = 0;
    SX_CHECK(XboxCodecXma2GetPcmSamples(xma, pcm_out, 64, &samples) == 0);
    SX_CHECK(samples == 64);
    SX_CHECK(pcm_out[0] != 0 || pcm_out[1] != 0);  // Real non-zero PCM16 output
    SX_CHECK(XboxCodecXma2Reset(xma) == 0);
    SX_CHECK(XboxCodecXma2DestroyDecoder(xma) == 0);

    // 4. Bink video & audio (including KB2 header parsing & BT.709 YUV-to-RGBA decode)
    uint8_t bink_file_mem[256]{'K', 'B', '2', 'i'};
    // Set frames=120 at +8, width=1280 at +20, height=720 at +24
    uint32_t kb2_frames = 120, kb2_w = 1280, kb2_h = 720;
    std::memcpy(bink_file_mem + 8, &kb2_frames, 4);
    std::memcpy(bink_file_mem + 20, &kb2_w, 4);
    std::memcpy(bink_file_mem + 24, &kb2_h, 4);
    BINK_VIDEO_HANDLE bink = nullptr;
    SX_CHECK(XboxCodecBinkVideoOpen(bink_file_mem, sizeof(bink_file_mem), 0, &bink) == 0);
    SX_CHECK(bink != nullptr);

    BINK_VIDEO_INFO bink_info{};
    SX_CHECK(XboxCodecBinkVideoGetInfo(bink, &bink_info) == 0);
    SX_CHECK(bink_info.width == 1280 && bink_info.height == 720 && bink_info.frameCount == 120);

    std::vector<uint8_t> frame(1280 * 4, 0);
    SX_CHECK(XboxCodecBinkVideoDecodeFrame(bink, frame.data(), 1280 * 4, 0) == 0);
    SX_CHECK(frame[3] == 255u);  // Opaque alpha from BT.709 RGBA decode

    uint8_t audio_out[128]{};
    uint32_t audio_bytes = 0;
    SX_CHECK(XboxCodecBinkAudioExtractPcm(bink, audio_out, sizeof(audio_out), &audio_bytes) == 0);
    SX_CHECK(audio_bytes == sizeof(audio_out));
    SX_CHECK(XboxCodecBinkVideoClose(bink) == 0);

    // 5. Hardware support & version info
    int32_t supported = 0;
    SX_CHECK(XboxCodecHardwareAccelSupported(1, &supported) == 0);
    SX_CHECK(supported == 1);

    CODEC_VERSION_INFO ver{};
    SX_CHECK(XboxCodecGetVersion(&ver) == 0);
    SX_CHECK(ver.major == 2);

    std::cout << "[test_codecs] All 16 XboxCodec APIs verified successfully!\n";
    return 0;
}
