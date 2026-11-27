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

#include "sx_check.h"

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
    sx_test::ScopedTempDir sandbox("test_xaudio2");
    const auto wav_path = (sandbox.path() / "test_xaudio2_mix.wav").string();
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

    // 6. Retail 2048-byte XMA2 MDCT Bitstream + Overlap-Add Roundtrip Verification
    {
        constexpr uint32_t kMdctFrames = 512;
        std::vector<float> orig_stereo(kMdctFrames * 2);
        for (uint32_t i = 0; i < kMdctFrames; ++i) {
            const float t = static_cast<float>(i) / 48000.0f;
            orig_stereo[i * 2 + 0] = 0.65f * std::sin(2.0f * 3.14159265f * 375.0f * t);
            orig_stereo[i * 2 + 1] = 0.45f * std::cos(2.0f * 3.14159265f * 750.0f * t);
        }
        uint32_t needed_bytes = 0;
        (void)Xma2EncodeMdctBlock(orig_stereo.data(), kMdctFrames, 2, nullptr, 0, &needed_bytes);
        SX_CHECK(needed_bytes >= 2048u && (needed_bytes % 2048u) == 0u);

        std::vector<uint8_t> xma_block(needed_bytes, 0);
        uint32_t written_bytes = 0;
        SX_CHECK_EQ(Xma2EncodeMdctBlock(orig_stereo.data(), kMdctFrames, 2,
                                        xma_block.data(), needed_bytes, &written_bytes), 0);
        SX_CHECK_EQ(written_bytes, needed_bytes);

        std::vector<float> recon(kMdctFrames * 4, 0.0f);
        uint32_t recon_frames = 0;
        uint16_t recon_ch = 0;
        SX_CHECK_EQ(Xma2DecodePacket(xma_block.data(), written_bytes,
                                     recon.data(), static_cast<uint32_t>(recon.size()),
                                     &recon_frames, &recon_ch), 0);
        SX_CHECK(recon_frames >= kMdctFrames);
        SX_CHECK_EQ(recon_ch, 2u);

        // Verify TDAC overlap-add waveform reconstruction fidelity (interior hops 128..384)
        double sig_pow = 0.0;
        double err_pow = 0.0;
        for (uint32_t f = 128; f < 384; ++f) {
            for (uint32_t c = 0; c < 2; ++c) {
                const double s = orig_stereo[f * 2 + c];
                const double r = recon[f * 2 + c];
                sig_pow += s * s;
                err_pow += (s - r) * (s - r);
            }
        }
        SX_CHECK(sig_pow > 10.0 && err_pow < sig_pow * 0.01);  // > 20 dB SNR clean reconstruction
    }

    // 7. ITU-R BS.775 5.1 & 7.1 Spatial Downmix + X3DAudioCalculate + Host CreateFX (CLSID_FXReverb)
    {
        // 5.1 voice with ONLY Center channel (channel 2 = dialogue) active -> must fold into BOTH L and R!
        XAUDIO2_WAVE_FORMAT fmt_51{};
        fmt_51.FormatTag = SCARLETTX_WAVE_FORMAT_IEEE_FLOAT;
        fmt_51.Channels = 6;
        fmt_51.SampleRate = 48000;
        fmt_51.BitsPerSample = 32;
        fmt_51.BlockAlign = 24;

        uint64_t master_st = 0, voice_51 = 0;
        SX_CHECK_EQ(XAudio2CreateMasteringVoice(engine, 2, 48000, &master_st), 0);
        SX_CHECK_EQ(XAudio2CreateSourceVoice(engine, &fmt_51, 0, 2.0f, &voice_51), 0);

        std::vector<float> pcm_51(128 * 6, 0.0f);
        for (uint32_t f = 0; f < 128; ++f) pcm_51[f * 6 + 2] = 0.8f;  // Center speaker only!
        XAUDIO2_BUFFER b51{};
        b51.AudioBytes = static_cast<uint32_t>(pcm_51.size() * sizeof(float));
        b51.AudioData = reinterpret_cast<const uint8_t*>(pcm_51.data());
        SX_CHECK_EQ(XAudio2VoiceSubmitSourceBuffer(voice_51, &b51), 0);
        SX_CHECK_EQ(XAudio2VoiceStart(voice_51, 0), 0);

        std::vector<float> downmix(128 * 2, 0.0f);
        SX_CHECK_EQ(XAudio2RenderAudioFrames(engine, 128, downmix.data(), 256), 0);
        SX_CHECK(std::fabs(downmix[0] - 0.8f * 0.70710678f) < 1e-3f);
        SX_CHECK(std::fabs(downmix[1] - 0.8f * 0.70710678f) < 1e-3f);

        SX_CHECK_EQ(XAudio2DestroyVoice(voice_51), 0);
        SX_CHECK_EQ(XAudio2DestroyVoice(master_st), 0);

        // X3DAudio 3D spatialization + Doppler verification (emitter to the right of listener)
        X3DAUDIO_HANDLE x3d{};
        SX_CHECK_EQ(X3DAudioInitialize(0x3u /* SPEAKER_STEREO */, 343.5f, x3d), 0);
        X3DAUDIO_LISTENER listener{};
        listener.OrientFront = {0.0f, 0.0f, 1.0f};
        listener.OrientTop = {0.0f, 1.0f, 0.0f};
        X3DAUDIO_EMITTER emitter{};
        emitter.OrientFront = {0.0f, 0.0f, 1.0f};
        emitter.OrientTop = {0.0f, 1.0f, 0.0f};
        emitter.Position = {10.0f, 0.0f, 0.0f};  // 10m directly right
        emitter.Velocity = {-34.35f, 0.0f, 0.0f};  // Approaching listener -> Doppler > 1.0
        emitter.ChannelCount = 1;
        emitter.CurveDistanceScaler = 5.0f;
        emitter.DopplerScaler = 1.0f;

        float mtx[2] = {};
        X3DAUDIO_DSP_SETTINGS dsp{};
        dsp.SrcChannelCount = 1;
        dsp.DstChannelCount = 2;
        dsp.pMatrixCoefficients = mtx;
        X3DAudioCalculate(x3d, &listener, &emitter,
                          SCARLETTX_X3DAUDIO_CALCULATE_MATRIX | SCARLETTX_X3DAUDIO_CALCULATE_DOPPLER, &dsp);
        SX_CHECK(std::fabs(dsp.EmitterToListenerDistance - 10.0f) < 1e-4f);
        SX_CHECK(mtx[1] > 0.45f && mtx[0] < 0.01f);  // Hard-right attenuated to 0.5x at 2x CurveDistanceScaler
        SX_CHECK(dsp.DopplerFactor > 1.05f);

        // Host CreateFX: CLSID_FXReverb {CACA5E1D-B1B5-42F7-B23D-BD0F3863EC74} / XAPOFX FXReverb {7D9ACA56-CB68-4807-B632-B137352E8596}
        struct FxClsid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; };
        const FxClsid kClsidFxReverb = {0x7D9ACA56u, 0xCB68u, 0x4807u, {0xB6, 0x32, 0xB1, 0x37, 0x35, 0x2E, 0x85, 0x96}};
        void* fx_obj = nullptr;
        SX_CHECK_EQ(CreateFX(&kClsidFxReverb, &fx_obj, nullptr, 0), 0);
        SX_CHECK(fx_obj != nullptr);
        auto* fx_vtbl = *reinterpret_cast<void* const* const*>(fx_obj);
        using FnRel = uint32_t(
#if defined(_WIN32)
            __stdcall*
#else
            *
#endif
        )(void*);
        (void)reinterpret_cast<FnRel>(fx_vtbl[2])(fx_obj);
    }

    SX_CHECK_EQ(XAudio2DestroyEngine(engine), 0);

    std::cout << "[test_xaudio2] All XAudio2 + XMA2 HLE checks passed.\n";
    return 0;
}
