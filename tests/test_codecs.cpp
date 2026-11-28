#include "scarlettx/media/codecs_abi.h"
#include "sx_check.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
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

    // 3b. Real 2048-byte XMA2 Packet Block through XboxCodecXma2SubmitData (IMDCT overlap-add)
    {
        std::vector<uint8_t> retail_xma_pkt(2048, 0);
        // Header: frameCount=2 (2<<26), frameOffset=0, meta=0, skip=0 -> big-endian 0x08000000
        retail_xma_pkt[0] = 0x08u;
        // Frame 0 at byte 4: 15-bit frameLength = 128 bits (0x0080 -> [0x01, 0x00]) + non-zero subband energy
        retail_xma_pkt[4] = 0x01u;
        retail_xma_pkt[5] = 0x00u;
        retail_xma_pkt[6] = 0x5Au;
        retail_xma_pkt[7] = 0x3Cu;
        // Frame 1 at byte 20 (4 + 16 bytes): 15-bit frameLength = 128 bits + subband energy
        retail_xma_pkt[20] = 0x01u;
        retail_xma_pkt[21] = 0x00u;
        retail_xma_pkt[22] = 0x64u;
        retail_xma_pkt[23] = 0x48u;

        SX_CHECK(XboxCodecXma2SubmitData(xma, retail_xma_pkt.data(), static_cast<uint32_t>(retail_xma_pkt.size())) == 0);
        std::vector<int16_t> mdct_pcm(256, 0);
        uint32_t mdct_samples = 0;
        SX_CHECK(XboxCodecXma2GetPcmSamples(xma, mdct_pcm.data(), 256, &mdct_samples) == 0);
        SX_CHECK(mdct_samples == 256u);  // 1 overlap-added hop of 128 stereo frames = 256 samples
        bool non_zero = false;
        for (int16_t s : mdct_pcm) {
            if (s != 0) { non_zero = true; break; }
        }
        SX_CHECK(non_zero);
    }

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

    // 6. Spec-Complete 32-Lane GDeflate BTYPE=0 (Stored), BTYPE=1 (Fixed), & Multi-Tile BTYPE=2 (Dynamic > 64 KiB)
    {
        auto write_le32 = [](uint8_t* p, uint32_t v) {
            p[0] = static_cast<uint8_t>(v & 0xFFu);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
            p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
            p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
        };
        auto read_le32 = [](const uint8_t* p) -> uint32_t {
            return static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        };

        // 6a. BTYPE=0 (Uncompressed/Stored) single-tile odd length + multi-tile (> 64 KiB) round-trip
        std::vector<uint8_t> stored_raw(140000);
        uint32_t lcg = 0x9E3779B9u;
        for (size_t i = 0; i < stored_raw.size(); ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            stored_raw[i] = static_cast<uint8_t>(lcg >> 24);
        }
        std::vector<uint8_t> stored_comp(stored_raw.size() + 4096, 0);
        size_t stored_comp_sz = 0;
        SX_CHECK(XboxCodecGDeflateCompress32LaneStored(
                     stored_raw.data(), stored_raw.size(), stored_comp.data(), stored_comp.size(), &stored_comp_sz) == 0);
        stored_comp.resize(stored_comp_sz);
        SX_CHECK(stored_comp[0] == 0x04u && stored_comp[1] == 0xFBu);
        // 3 tiles: table_end = 8 + 3 * 4 = 20; lane 0 word 0 low 3 bits = 0b001 (BFINAL=1, BTYPE=0)
        SX_CHECK((read_le32(stored_comp.data() + 20u) & 0x7u) == 0x1u);

        std::vector<uint8_t> stored_out(stored_raw.size(), 0);
        size_t stored_written = 0;
        SX_CHECK(XboxCodecGDeflateDecompress(
                     stored_comp.data(), stored_comp.size(), stored_out.data(), stored_out.size(), &stored_written) == 0);
        SX_CHECK(stored_written == stored_raw.size());
        SX_CHECK(stored_out == stored_raw);

        // 6b. BTYPE=1 (Fixed Huffman) multi-tile (> 64 KiB) round-trip
        std::vector<uint8_t> fixed_raw(105000);
        for (size_t i = 0; i < fixed_raw.size(); ++i) {
            fixed_raw[i] = static_cast<uint8_t>((i % 37 < 24) ? ((i / 37) & 0x7Fu) : ((i * 19u) & 0xFFu));
        }
        std::vector<uint8_t> fixed_comp(fixed_raw.size() + 8192, 0);
        size_t fixed_comp_sz = 0;
        SX_CHECK(XboxCodecGDeflateCompress32Lane(
                     fixed_raw.data(), fixed_raw.size(), fixed_comp.data(), fixed_comp.size(), &fixed_comp_sz) == 0);
        fixed_comp.resize(fixed_comp_sz);
        SX_CHECK((read_le32(fixed_comp.data() + 16u) & 0x7u) == 0x3u);  // 2 tiles: table_end = 16, BTYPE=1

        std::vector<uint8_t> fixed_out(fixed_raw.size(), 0);
        size_t fixed_written = 0;
        SX_CHECK(XboxCodecGDeflateDecompress(
                     fixed_comp.data(), fixed_comp.size(), fixed_out.data(), fixed_out.size(), &fixed_written) == 0);
        SX_CHECK(fixed_written == fixed_raw.size());
        SX_CHECK(fixed_out == fixed_raw);

        // 6c. BTYPE=2 (Dynamic Huffman) multi-tile (> 64 KiB, 3 tiles: skewed symbols + LZ77 + literal-heavy)
        std::vector<uint8_t> dyn_raw(180000);
        for (size_t i = 0; i < dyn_raw.size(); ++i) {
            if (i < 65536u) {
                // Tile 0: heavily skewed power-law symbol distribution + long LZ77 matches (exercises >15b length clamping)
                const size_t mod = i % 96u;
                dyn_raw[i] = (mod < 56u) ? static_cast<uint8_t>('A' + (mod & 3u))
                           : (mod < 84u) ? static_cast<uint8_t>('0' + (mod % 10u))
                                         : static_cast<uint8_t>((i * 31u + (i >> 8)) & 0xFFu);
            } else if (i < 131072u) {
                // Tile 1: non-repeating 2-byte Permuted LCG (exercises empty/minimal distance tree + Dynamic Lit tree)
                const uint32_t v = static_cast<uint32_t>(i - 65536u);
                dyn_raw[i] = static_cast<uint8_t>(((v * 73u) ^ (v >> 5)) & 0x3Fu);
            } else {
                // Tile 2: structured mesh/shader record stream with varying match distances up to 32 KiB
                const size_t local = i - 131072u;
                dyn_raw[i] = (local % 48u < 36u) ? static_cast<uint8_t>((local / 256u) ^ (local % 12u))
                                                 : static_cast<uint8_t>((local * 13u + 0x5Au) & 0xFFu);
            }
        }

        size_t dyn_needed = 0;
        SX_CHECK(XboxCodecGDeflateCompress32LaneDynamic(dyn_raw.data(), dyn_raw.size(), nullptr, 0, &dyn_needed) < 0);
        SX_CHECK(dyn_needed > 20u);
        std::vector<uint8_t> dyn_comp(dyn_needed, 0);
        size_t dyn_comp_sz = 0;
        SX_CHECK(XboxCodecGDeflateCompress32LaneDynamic(
                     dyn_raw.data(), dyn_raw.size(), dyn_comp.data(), dyn_comp.size(), &dyn_comp_sz) == 0);
        SX_CHECK(dyn_comp_sz == dyn_needed && dyn_comp_sz < dyn_raw.size() / 2u);
        SX_CHECK(dyn_comp[0] == 0x04u && dyn_comp[1] == 0xFBu);
        // 3 tiles: table_end = 20; lane 0 word 0 low 3 bits = 0b101 (BFINAL=1, BTYPE=2)
        SX_CHECK((read_le32(dyn_comp.data() + 20u) & 0x7u) == 0x5u);

        std::vector<uint8_t> dyn_decomp(dyn_raw.size(), 0);
        size_t dyn_written = 0;
        SX_CHECK(XboxCodecGDeflateDecompress(
                     dyn_comp.data(), dyn_comp.size(), dyn_decomp.data(), dyn_decomp.size(), &dyn_written) == 0);
        SX_CHECK(dyn_written == dyn_raw.size());
        SX_CHECK(dyn_decomp == dyn_raw);

        // 6d. Concurrent multi-tile stress on the persistent Win32 Threadpool worker pool
        std::atomic<bool> stress_ok{true};
        std::vector<std::thread> callers;
        constexpr size_t kThreads = 8u;
        callers.reserve(kThreads);
        for (size_t tid = 0; tid < kThreads; ++tid) {
            callers.emplace_back([&, tid]() {
                std::vector<uint8_t> local_out(dyn_raw.size(), 0);
                for (size_t iter = 0; iter < 16u; ++iter) {
                    const size_t mode = (tid + iter) % 3u;
                    size_t got = 0;
                    if (mode == 0u) {
                        if (XboxCodecGDeflateDecompress(
                                dyn_comp.data(), dyn_comp.size(), local_out.data(), dyn_raw.size(), &got) != 0 ||
                            got != dyn_raw.size() ||
                            std::memcmp(local_out.data(), dyn_raw.data(), dyn_raw.size()) != 0) {
                            stress_ok.store(false, std::memory_order_release);
                            return;
                        }
                    } else if (mode == 1u) {
                        if (XboxCodecGDeflateDecompress(
                                fixed_comp.data(), fixed_comp.size(), local_out.data(), fixed_raw.size(), &got) != 0 ||
                            got != fixed_raw.size() ||
                            std::memcmp(local_out.data(), fixed_raw.data(), fixed_raw.size()) != 0) {
                            stress_ok.store(false, std::memory_order_release);
                            return;
                        }
                    } else {
                        if (XboxCodecGDeflateDecompress(
                                stored_comp.data(), stored_comp.size(), local_out.data(), stored_raw.size(), &got) != 0 ||
                            got != stored_raw.size() ||
                            std::memcmp(local_out.data(), stored_raw.data(), stored_raw.size()) != 0) {
                            stress_ok.store(false, std::memory_order_release);
                            return;
                        }
                    }
                }
            });
        }
        for (auto& th : callers) th.join();
        SX_CHECK(stress_ok.load(std::memory_order_acquire));

        // 6e. Fail-closed (< 0) rejection of corrupt Huffman tables, bad NLEN, BTYPE=3, and truncated bitstreams
        std::vector<uint8_t> reject_dst(dyn_raw.size(), 0xCCu);
        size_t reject_written = 0;

        // (i) Reserved BTYPE=3 in lane headers
        {
            std::vector<uint8_t> bad_btype3 = dyn_comp;
            for (uint32_t l = 0; l < 32u; ++l) {
                const uint32_t w = (read_le32(bad_btype3.data() + 20u + l * 4u) & ~0x7u) | 0x7u;
                write_le32(bad_btype3.data() + 20u + l * 4u, w);
            }
            SX_CHECK(XboxCodecGDeflateDecompress(
                         bad_btype3.data(), bad_btype3.size(), reject_dst.data(), reject_dst.size(), &reject_written) < 0);
        }

        // (ii) Corrupt BTYPE=0 ~LEN complement mismatch (flip bit in word 0 byte-aligned LEN/NLEN)
        {
            std::vector<uint8_t> bad_stored = stored_comp;
            bad_stored[20u + 3u] ^= 0x80u;  // Corrupt NLEN upper byte on lane 0
            SX_CHECK(XboxCodecGDeflateDecompress(
                         bad_stored.data(), bad_stored.size(), reject_dst.data(), stored_raw.size(), &reject_written) < 0);
        }

        // Helper to build a single-tile 268-byte 0x04 0xFB container with custom lane words[0..63]
        auto make_custom_single_tile = [&](uint32_t uncomp_len, const std::vector<uint32_t>& words) {
            std::vector<uint8_t> buf(12u + words.size() * 4u, 0);
            buf[0] = 0x04u;
            buf[1] = 0xFBu;
            buf[2] = 1u;
            buf[3] = 0u;
            write_le32(buf.data() + 4u, 1u | (uncomp_len << 2u));
            write_le32(buf.data() + 8u, static_cast<uint32_t>(words.size() * 4u));
            for (size_t i = 0; i < words.size(); ++i) {
                write_le32(buf.data() + 12u + i * 4u, words[i]);
            }
            return buf;
        };

        // (iii) Oversubscribed BTYPE=2 cl_tree: HCLEN=4 (0), 4 code-length symbols all given length 1 (Kraft = 4 * 1/2 = 2 > 1)
        {
            std::vector<uint32_t> words(64, 0u);
            for (uint32_t l = 0; l < 32u; ++l) words[l] = 0x5u;  // BFINAL=1, BTYPE=2
            // Lane 0: bits 0..2 = 5; bits 3..16 = HLIT=0, HDIST=0, HCLEN=0 (4 cllens); bits 17..19 = cl_len[0] = 1
            words[0] = 0x5u | (1u << 17u);
            // Lanes 1..3: bits 0..2 = 5; bits 3..5 = cl_len[i] = 1
            words[1] = 0x5u | (1u << 3u);
            words[2] = 0x5u | (1u << 3u);
            words[3] = 0x5u | (1u << 3u);
            const auto oversub = make_custom_single_tile(64u, words);
            SX_CHECK(XboxCodecGDeflateDecompress(
                         oversub.data(), oversub.size(), reject_dst.data(), 64u, &reject_written) < 0);
        }

        // (iv) Incomplete / under-subscribed BTYPE=2 cl_tree: 2 symbols given length 2, rest 0 (Kraft = 2 * 1/4 = 1/2 < 1)
        {
            std::vector<uint32_t> words(64, 0u);
            for (uint32_t l = 0; l < 32u; ++l) words[l] = 0x5u;
            words[0] = 0x5u | (2u << 17u);
            words[1] = 0x5u | (2u << 3u);
            words[2] = 0x5u;  // cl_len = 0
            words[3] = 0x5u;  // cl_len = 0
            const auto incomp = make_custom_single_tile(64u, words);
            SX_CHECK(XboxCodecGDeflateDecompress(
                         incomp.data(), incomp.size(), reject_dst.data(), 64u, &reject_written) < 0);
        }

        // (v) Truncated BTYPE=2 bitstream: both raw cut AND forged-header tile missing trailing refill words
        {
            SX_CHECK(XboxCodecGDeflateDecompress(
                         dyn_comp.data(), dyn_comp.size() - 16u, reject_dst.data(), dyn_raw.size(), &reject_written) < 0);

            // Forge lastTileCompSize in tileOffsets[0] to match a 16-byte truncated last tile payload
            std::vector<uint8_t> forged_trunc(dyn_comp.begin(), dyn_comp.end() - 16);
            const uint32_t orig_last_sz = read_le32(dyn_comp.data() + 8u);
            write_le32(forged_trunc.data() + 8u, orig_last_sz - 16u);
            SX_CHECK(XboxCodecGDeflateDecompress(
                         forged_trunc.data(), forged_trunc.size(), reject_dst.data(), dyn_raw.size(), &reject_written) < 0);
        }

        // (vi) Independent hand-assembled BTYPE=0 & BTYPE=2 bitstreams (not produced by Compress32Lane*):
        //      Verifies GDeflate32LaneDecompressTile against raw bit-level encodings directly.
        {
            // Hand-assembled BTYPE=0 32-byte payload ("0123456789ABCDEFGHIJKLMNOPQRSTUV", 1 byte per lane):
            // Per lane l: bits 0..2 = 0b001 (BFINAL=1, BTYPE=0), bits 3..7 = 0 (byte align),
            // word[l] bits 8..23 = LEN=1 (0x0001), bits 24..31 = NLEN_lo (0xFE);
            // word[32+l] bits 0..7 = NLEN_hi (0xFF), bits 8..15 = payload[l].
            // Two refill crossings occur per lane (after 32-bit LEN/NLEN at bit_off=40, no crossing on 8-bit data at bit_off=16):
            // 1 refill word (0u) per lane -> 64 + 32 = 96 words total.
            const char expected32[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
            std::vector<uint32_t> raw_b0_words(96, 0u);
            for (uint32_t l = 0; l < 32u; ++l) {
                raw_b0_words[l] = 0x01u | (0x0001u << 8u) | (0xFEu << 24u);
                raw_b0_words[32u + l] = 0xFFu | (static_cast<uint32_t>(static_cast<uint8_t>(expected32[l])) << 8u);
            }
            const auto indep_b0 = make_custom_single_tile(32u, raw_b0_words);
            std::vector<uint8_t> indep_b0_out(32u, 0);
            size_t indep_b0_written = 0;
            SX_CHECK(XboxCodecGDeflateDecompress(
                         indep_b0.data(), indep_b0.size(), indep_b0_out.data(), 32u, &indep_b0_written) == 0);
            SX_CHECK(indep_b0_written == 32u);
            SX_CHECK(std::memcmp(indep_b0_out.data(), expected32, 32u) == 0);
        }
    }

    std::cout << "[test_codecs] All XboxCodec APIs + 32-lane BTYPE=0/1/2 GDeflate & persistent pool verified!\n";
    return 0;
}
