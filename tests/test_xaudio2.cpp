#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "scarlettx/audio/xaudio2_abi.h"

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_xaudio2] FAILED: " #cond                        \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define SX_CHECK_EQ(a, b)                                                       \
    do {                                                                        \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b)) {                                                      \
            std::cerr << "[test_xaudio2] FAILED: " #a " == " #b                 \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ \
                      << "\n";                                                  \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main() {
    uint64_t engine = 0;
    SX_CHECK_EQ(XAudio2CreateEngine(0, &engine), 0);
    SX_CHECK(engine != 0);

    uint64_t master = 0;
    SX_CHECK_EQ(XAudio2CreateMasteringVoice(engine, 2, 48000, &master), 0);

    uint64_t submix = 0;
    SX_CHECK_EQ(XAudio2CreateSubmixVoice(engine, 2, 48000, 0, &submix), 0);

    // 1. Float32 mono sine wave panned hard-left via output matrix
    XAUDIO2_WAVE_FORMAT fmt_f32{};
    fmt_f32.FormatTag = SCARLETTX_WAVE_FORMAT_IEEE_FLOAT;
    fmt_f32.Channels = 1;
    fmt_f32.SampleRate = 48000;
    fmt_f32.BitsPerSample = 32;
    fmt_f32.BlockAlign = 4;

    uint64_t src_sine = 0;
    SX_CHECK_EQ(XAudio2CreateSourceVoice(engine, &fmt_f32, 0, 2.0f, &src_sine), 0);
    SX_CHECK_EQ(XAudio2VoiceSetVolume(src_sine, 0.5f), 0);
    float vol = 0.0f;
    SX_CHECK_EQ(XAudio2VoiceGetVolume(src_sine, &vol), 0);
    SX_CHECK(std::fabs(vol - 0.5f) < 1e-5f);

    // 1x2 matrix: left=1.0, right=0.0
    const float pan_left[2] = {1.0f, 0.0f};
    SX_CHECK_EQ(XAudio2VoiceSetOutputMatrix(src_sine, 1, 2, pan_left), 0);
    float pan_out[2] = {};
    SX_CHECK_EQ(XAudio2VoiceGetOutputMatrix(src_sine, 1, 2, pan_out), 0);
    SX_CHECK(std::fabs(pan_out[0] - 1.0f) < 1e-5f);
    SX_CHECK(std::fabs(pan_out[1] - 0.0f) < 1e-5f);

    const float ch_vols[2] = {0.8f, 0.9f};
    SX_CHECK_EQ(XAudio2VoiceSetChannelVolumes(src_sine, 2, ch_vols), 0);
    float ch_vols_out[2] = {};
    SX_CHECK_EQ(XAudio2VoiceGetChannelVolumes(src_sine, 2, ch_vols_out), 0);
    SX_CHECK(std::fabs(ch_vols_out[0] - 0.8f) < 1e-5f);
    SX_CHECK(std::fabs(ch_vols_out[1] - 0.9f) < 1e-5f);

    SX_CHECK_EQ(XAudio2VoiceEnableEffect(src_sine, 0), 0);
    SX_CHECK_EQ(XAudio2VoiceDisableEffect(src_sine, 0), 0);
    SX_CHECK_EQ(XAudio2VoiceDiscontinuity(src_sine), 0);
    SX_CHECK_EQ(XAudio2VoiceExitLoop(src_sine), 0);
    SX_CHECK_EQ(XAudio2CommitChanges(engine, 0), 0);
    SX_CHECK_EQ(XAudio2VoiceSetVolumeRamp(src_sine, 0.7f, 100), 0);

    constexpr uint32_t kFrames = 480;
    std::vector<float> sine(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        sine[i] = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }

    XAUDIO2_BUFFER buf1{};
    buf1.AudioBytes = static_cast<uint32_t>(sine.size() * sizeof(float));
    buf1.AudioData = reinterpret_cast<const uint8_t*>(sine.data());
    SX_CHECK_EQ(XAudio2VoiceSubmitSourceBuffer(src_sine, &buf1), 0);
    SX_CHECK_EQ(XAudio2VoiceStart(src_sine, 0), 0);

    // 2. XMA2 synthetic packet decoded directly through XAudio2 source voice (panned right)
    XMA2_PACKET_HEADER xma_hdr{};
    xma_hdr.Magic = 0x32414D58u;
    xma_hdr.Channels = 1;
    xma_hdr.SampleRateDiv100 = 480;
    xma_hdr.SampleCount = kFrames;
    xma_hdr.InitialSample = 0;
    xma_hdr.StepSize = 128;

    std::vector<uint8_t> xma_pkt(sizeof(xma_hdr) + kFrames);
    std::memcpy(xma_pkt.data(), &xma_hdr, sizeof(xma_hdr));
    for (uint32_t i = 0; i < kFrames; ++i) {
        int8_t delta = (i % 16 < 8) ? 4 : -4;
        xma_pkt[sizeof(xma_hdr) + i] = static_cast<uint8_t>(delta);
    }

    // Verify standalone Xma2DecodePacket
    std::vector<float> decoded_xma(kFrames);
    uint32_t decoded_frames = 0;
    uint16_t decoded_ch = 0;
    SX_CHECK_EQ(
        Xma2DecodePacket(
            xma_pkt.data(),
            static_cast<uint32_t>(xma_pkt.size()),
            decoded_xma.data(),
            kFrames,
            &decoded_frames,
            &decoded_ch),
        0);
    SX_CHECK_EQ(decoded_frames, kFrames);
    SX_CHECK_EQ(decoded_ch, 1u);

    XAUDIO2_WAVE_FORMAT fmt_xma{};
    fmt_xma.FormatTag = SCARLETTX_WAVE_FORMAT_XMA2;
    fmt_xma.Channels = 1;
    fmt_xma.SampleRate = 48000;
    fmt_xma.BitsPerSample = 16;
    fmt_xma.BlockAlign = 2;

    uint64_t src_xma = 0;
    SX_CHECK_EQ(XAudio2CreateSourceVoice(engine, &fmt_xma, 0, 2.0f, &src_xma), 0);
    const float pan_right[2] = {0.0f, 1.0f};
    SX_CHECK_EQ(XAudio2VoiceSetOutputMatrix(src_xma, 1, 2, pan_right), 0);

    XAUDIO2_BUFFER buf2{};
    buf2.AudioBytes = static_cast<uint32_t>(xma_pkt.size());
    buf2.AudioData = xma_pkt.data();
    SX_CHECK_EQ(XAudio2VoiceSubmitSourceBuffer(src_xma, &buf2), 0);
    SX_CHECK_EQ(XAudio2VoiceStart(src_xma, 0), 0);

    // 3. Render mixed stereo frames and verify both channels have non-zero energy
    std::vector<float> mixed(kFrames * 2, 0.0f);
    SX_CHECK_EQ(
        XAudio2RenderAudioFrames(engine, kFrames, mixed.data(), static_cast<uint32_t>(mixed.size())),
        0);

    float left_energy = 0.0f;
    float right_energy = 0.0f;
    for (uint32_t i = 0; i < kFrames; ++i) {
        left_energy += std::fabs(mixed[i * 2 + 0]);
        right_energy += std::fabs(mixed[i * 2 + 1]);
    }
    SX_CHECK(left_energy > 10.0f);
    SX_CHECK(right_energy > 1.0f);

    // 4. Write deterministic WAV file
    const auto wav_path = (std::filesystem::path(SCARLETTX_BINARY_DIR) / "test_xaudio2_mix.wav").string();
    SX_CHECK_EQ(XAudio2WriteWavFile(wav_path.c_str(), 48000, 2, mixed.data(), kFrames), 0);
    SX_CHECK(std::filesystem::exists(wav_path));
    SX_CHECK_EQ(std::filesystem::file_size(wav_path), 44u + kFrames * 2u * sizeof(int16_t));

    // 5. Voice state & performance data
    XAUDIO2_VOICE_STATE st{};
    SX_CHECK_EQ(XAudio2VoiceGetState(src_sine, &st), 0);
    SX_CHECK_EQ(st.SamplesPlayed, kFrames);
    SX_CHECK_EQ(st.BuffersQueued, 0u);

    XAUDIO2_PERFORMANCE_DATA perf{};
    SX_CHECK_EQ(XAudio2GetPerformanceData(engine, &perf), 0);
    SX_CHECK_EQ(perf.TotalFramesMixed, kFrames);
    SX_CHECK_EQ(perf.TotalSourceVoiceCount, 2u);
    SX_CHECK_EQ(perf.ActiveSubmixVoiceCount, 1u);

    SX_CHECK_EQ(XAudio2DestroyVoice(src_sine), 0);
    SX_CHECK_EQ(XAudio2DestroyVoice(src_xma), 0);
    SX_CHECK_EQ(XAudio2DestroyVoice(submix), 0);
    SX_CHECK_EQ(XAudio2DestroyVoice(master), 0);
    SX_CHECK_EQ(XAudio2DestroyEngine(engine), 0);

    std::cout << "[test_xaudio2] All XAudio2 + XMA2 HLE checks passed.\n";
    return 0;
}
