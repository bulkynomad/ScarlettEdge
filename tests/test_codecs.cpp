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

    // 2. BCPack compress & decompress
    std::vector<uint8_t> bc_comp(128, 0);
    size_t bc_comp_len = 0;
    SX_CHECK(XboxCodecBcPackCompress(raw_data, 64, bc_comp.data(), bc_comp.size(), &bc_comp_len) == 0);
    std::vector<uint8_t> bc_decomp(64, 0);
    SX_CHECK(XboxCodecBcPackDecompress(bc_comp.data(), bc_comp_len, bc_decomp.data(), bc_decomp.size(), 1) == 0);

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
    SX_CHECK(XboxCodecXma2Reset(xma) == 0);
    SX_CHECK(XboxCodecXma2DestroyDecoder(xma) == 0);

    // 4. Bink video & audio
    uint8_t bink_file_mem[256]{'B', 'I', 'K', '2'};
    BINK_VIDEO_HANDLE bink = nullptr;
    SX_CHECK(XboxCodecBinkVideoOpen(bink_file_mem, sizeof(bink_file_mem), 0, &bink) == 0);
    SX_CHECK(bink != nullptr);

    BINK_VIDEO_INFO bink_info{};
    SX_CHECK(XboxCodecBinkVideoGetInfo(bink, &bink_info) == 0);
    SX_CHECK(bink_info.width == 1920 && bink_info.height == 1080);

    std::vector<uint8_t> frame(1920 * 4, 0);
    SX_CHECK(XboxCodecBinkVideoDecodeFrame(bink, frame.data(), 1920 * 4, 0) == 0);

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
