#include "scarlettx/audio/xaudio2_abi.h"
#include "scarlettx/audio/xma2_decoder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#endif

// xaudio2_guest.cpp
extern "C" int32_t ScarlettXGuestXAudio2Create(void** out, uint32_t flags, uint32_t processor);

namespace {

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t kE_FAIL = static_cast<int32_t>(0x80004005u);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);

enum class VoiceKind {
    kMastering,
    kSubmix,
    kSource,
};

struct QueuedAudioBuffer {
    std::vector<float> samples;  // interleaved float32
    uint32_t channels = 2;
    uint32_t frame_count = 0;
    double read_pos = 0.0;
    void* context = nullptr;
};

struct VoiceObject {
    uint64_t engine_handle = 0;
    VoiceKind kind = VoiceKind::kSource;
    XAUDIO2_WAVE_FORMAT format{};
    bool playing = false;
    float volume = 1.0f;
    float frequency_ratio = 1.0f;
    float max_frequency_ratio = 2.0f;
    uint32_t matrix_src_ch = 0;
    uint32_t matrix_dst_ch = 0;
    std::vector<float> output_matrix;
    std::vector<float> channel_volumes;
    bool loop_exited = false;
    std::deque<QueuedAudioBuffer> queue;
    uint64_t samples_played = 0;
};

struct EngineObject {
    bool running = true;
    uint32_t master_channels = 2;
    uint32_t master_sample_rate = 48000;
    uint64_t mastering_voice = 0;
    uint64_t total_frames_mixed = 0;
    std::vector<uint64_t> voice_handles;
};

struct AudioState {
    std::mutex mu;
    uint64_t next_handle = 1;
    std::unordered_map<uint64_t, std::shared_ptr<EngineObject>> engines;
    std::unordered_map<uint64_t, std::shared_ptr<VoiceObject>> voices;
};

AudioState& State() {
    static AudioState s;
    return s;
}

int32_t DecodeXma2Internal(
    const uint8_t* packet_data,
    uint32_t packet_bytes,
    std::vector<float>& out_pcm,
    uint32_t& out_frames,
    uint16_t& out_channels,
    uint16_t hint_channels = 0) {
    if (!packet_data || packet_bytes < sizeof(XMA2_PACKET_HEADER)) {
        return kE_INVALIDARG;
    }
    XMA2_PACKET_HEADER hdr{};
    std::memcpy(&hdr, packet_data, sizeof(hdr));
    if (hdr.Magic != 0x32414D58u) {
        // Retail 2048-byte XMA2 bitstream block (parsed via Xma2StreamDecoder + IMDCT overlap-add)
        if (packet_bytes >= scarlettx::audio::kXma2PacketBytes &&
            (packet_bytes % scarlettx::audio::kXma2PacketBytes) == 0u) {
            const auto ph = scarlettx::audio::ParseXma2PacketHeader(packet_data);
            const uint16_t ch = hint_channels > 0
                                    ? hint_channels
                                    : static_cast<uint16_t>((ph.packet_skip_count + 1u) * 2u);
            scarlettx::audio::Xma2StreamDecoder dec(ch, 48000u);
            if (dec.DecodeBlock(packet_data, packet_bytes, out_pcm) && !out_pcm.empty()) {
                out_channels = ch;
                out_frames = static_cast<uint32_t>(out_pcm.size() / ch);
                return kS_OK;
            }
        }
        return kE_INVALIDARG;
    }
    if (hdr.Channels == 0 || hdr.Channels > 8 || hdr.SampleCount == 0) {
        return kE_INVALIDARG;
    }

    const uint32_t total_samples = hdr.SampleCount * hdr.Channels;
    const uint32_t payload_bytes = packet_bytes - static_cast<uint32_t>(sizeof(hdr));
    if (payload_bytes < total_samples) {
        return kE_INVALIDARG;
    }

    // DPCM-style deterministic XMA2 test packet decoder (8-bit signed delta * StepSize).
    out_pcm.resize(total_samples);
    const int8_t* deltas = reinterpret_cast<const int8_t*>(packet_data + sizeof(hdr));
    std::vector<int32_t> acc(hdr.Channels, hdr.InitialSample);
    const int32_t step = hdr.StepSize > 0 ? hdr.StepSize : 64;

    for (uint32_t f = 0; f < hdr.SampleCount; ++f) {
        for (uint16_t ch = 0; ch < hdr.Channels; ++ch) {
            const uint32_t idx = f * hdr.Channels + ch;
            acc[ch] = std::clamp(acc[ch] + static_cast<int32_t>(deltas[idx]) * step, -32768, 32767);
            out_pcm[idx] = static_cast<float>(acc[ch]) / 32768.0f;
        }
    }

    out_frames = hdr.SampleCount;
    out_channels = hdr.Channels;
    return kS_OK;
}

}  // namespace

extern "C" {

int32_t XAudio2CreateEngine(uint32_t /*flags*/, uint64_t* out_engine) {
    if (!out_engine) return kE_INVALIDARG;
    auto eng = std::make_shared<EngineObject>();
    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().engines[h] = std::move(eng);
    *out_engine = h;
    return kS_OK;
}

int32_t XAudio2DestroyEngine(uint64_t engine) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().engines.find(engine);
    if (it == State().engines.end()) return kE_HANDLE;
    for (uint64_t vh : it->second->voice_handles) {
        State().voices.erase(vh);
    }
    State().engines.erase(it);
    return kS_OK;
}

int32_t XAudio2StartEngine(uint64_t engine) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().engines.find(engine);
    if (it == State().engines.end()) return kE_HANDLE;
    it->second->running = true;
    return kS_OK;
}

int32_t XAudio2StopEngine(uint64_t engine) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().engines.find(engine);
    if (it == State().engines.end()) return kE_HANDLE;
    it->second->running = false;
    return kS_OK;
}

int32_t XAudio2GetPerformanceData(uint64_t engine, XAUDIO2_PERFORMANCE_DATA* out_data) {
    if (!out_data) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().engines.find(engine);
    if (it == State().engines.end()) return kE_HANDLE;

    XAUDIO2_PERFORMANCE_DATA data{};
    data.TotalFramesMixed = it->second->total_frames_mixed;
    for (uint64_t vh : it->second->voice_handles) {
        auto vit = State().voices.find(vh);
        if (vit == State().voices.end()) continue;
        if (vit->second->kind == VoiceKind::kSource) {
            data.TotalSourceVoiceCount++;
            if (vit->second->playing && !vit->second->queue.empty()) {
                data.ActiveSourceVoiceCount++;
            }
        } else if (vit->second->kind == VoiceKind::kSubmix) {
            data.ActiveSubmixVoiceCount++;
        }
    }
    *out_data = data;
    return kS_OK;
}

int32_t XAudio2CreateMasteringVoice(
    uint64_t engine,
    uint32_t input_channels,
    uint32_t input_sample_rate,
    uint64_t* out_voice) {
    if (!out_voice) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().engines.find(engine);
    if (it == State().engines.end()) return kE_HANDLE;

    const uint32_t ch = input_channels == 0 ? 2u : input_channels;
    const uint32_t sr = input_sample_rate == 0 ? 48000u : input_sample_rate;
    it->second->master_channels = ch;
    it->second->master_sample_rate = sr;

    auto v = std::make_shared<VoiceObject>();
    v->engine_handle = engine;
    v->kind = VoiceKind::kMastering;
    v->format.FormatTag = SCARLETTX_WAVE_FORMAT_IEEE_FLOAT;
    v->format.Channels = static_cast<uint16_t>(ch);
    v->format.SampleRate = sr;
    v->format.BitsPerSample = 32;
    v->playing = true;

    const uint64_t vh = State().next_handle++;
    State().voices[vh] = std::move(v);
    it->second->mastering_voice = vh;
    it->second->voice_handles.push_back(vh);
    *out_voice = vh;
    return kS_OK;
}

int32_t XAudio2CreateSubmixVoice(
    uint64_t engine,
    uint32_t input_channels,
    uint32_t input_sample_rate,
    uint32_t /*processing_stage*/,
    uint64_t* out_voice) {
    if (!out_voice || input_channels == 0 || input_sample_rate == 0) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().engines.find(engine);
    if (it == State().engines.end()) return kE_HANDLE;

    auto v = std::make_shared<VoiceObject>();
    v->engine_handle = engine;
    v->kind = VoiceKind::kSubmix;
    v->format.FormatTag = SCARLETTX_WAVE_FORMAT_IEEE_FLOAT;
    v->format.Channels = static_cast<uint16_t>(input_channels);
    v->format.SampleRate = input_sample_rate;
    v->format.BitsPerSample = 32;
    v->playing = true;

    const uint64_t vh = State().next_handle++;
    State().voices[vh] = std::move(v);
    it->second->voice_handles.push_back(vh);
    *out_voice = vh;
    return kS_OK;
}

int32_t XAudio2CreateSourceVoice(
    uint64_t engine,
    const XAUDIO2_WAVE_FORMAT* format,
    uint32_t /*flags*/,
    float max_frequency_ratio,
    uint64_t* out_voice) {
    if (!format || !out_voice || format->Channels == 0 || format->SampleRate == 0) {
        return kE_INVALIDARG;
    }
    if (format->FormatTag != SCARLETTX_WAVE_FORMAT_PCM &&
        format->FormatTag != SCARLETTX_WAVE_FORMAT_IEEE_FLOAT &&
        format->FormatTag != SCARLETTX_WAVE_FORMAT_XMA2) {
        return kE_NOTIMPL;
    }

    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().engines.find(engine);
    if (it == State().engines.end()) return kE_HANDLE;

    auto v = std::make_shared<VoiceObject>();
    v->engine_handle = engine;
    v->kind = VoiceKind::kSource;
    v->format = *format;
    v->max_frequency_ratio = max_frequency_ratio > 0.0f ? max_frequency_ratio : 2.0f;

    const uint64_t vh = State().next_handle++;
    State().voices[vh] = std::move(v);
    it->second->voice_handles.push_back(vh);
    *out_voice = vh;
    return kS_OK;
}

int32_t XAudio2DestroyVoice(uint64_t voice) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    const uint64_t eng_h = vit->second->engine_handle;
    auto eit = State().engines.find(eng_h);
    if (eit != State().engines.end()) {
        auto& vec = eit->second->voice_handles;
        vec.erase(std::remove(vec.begin(), vec.end(), voice), vec.end());
        if (eit->second->mastering_voice == voice) {
            eit->second->mastering_voice = 0;
        }
    }
    State().voices.erase(vit);
    return kS_OK;
}

int32_t XAudio2VoiceStart(uint64_t voice, uint32_t /*flags*/) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->playing = true;
    return kS_OK;
}

int32_t XAudio2VoiceStop(uint64_t voice, uint32_t /*flags*/) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->playing = false;
    return kS_OK;
}

int32_t XAudio2VoiceSubmitSourceBuffer(uint64_t voice, const XAUDIO2_BUFFER* buffer) {
    if (!buffer || !buffer->AudioData || buffer->AudioBytes == 0) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    auto& v = *vit->second;
    if (v.kind != VoiceKind::kSource) return kE_INVALIDARG;

    QueuedAudioBuffer qbuf;
    qbuf.context = buffer->Context;
    qbuf.channels = v.format.Channels;

    if (v.format.FormatTag == SCARLETTX_WAVE_FORMAT_IEEE_FLOAT) {
        const uint32_t sample_count = buffer->AudioBytes / sizeof(float);
        qbuf.frame_count = sample_count / v.format.Channels;
        qbuf.samples.resize(qbuf.frame_count * v.format.Channels);
        std::memcpy(qbuf.samples.data(), buffer->AudioData, qbuf.samples.size() * sizeof(float));
    } else if (v.format.FormatTag == SCARLETTX_WAVE_FORMAT_PCM && v.format.BitsPerSample == 16) {
        const uint32_t sample_count = buffer->AudioBytes / sizeof(int16_t);
        qbuf.frame_count = sample_count / v.format.Channels;
        qbuf.samples.resize(qbuf.frame_count * v.format.Channels);
        const int16_t* pcm16 = reinterpret_cast<const int16_t*>(buffer->AudioData);
        for (size_t i = 0; i < qbuf.samples.size(); ++i) {
            qbuf.samples[i] = static_cast<float>(pcm16[i]) / 32768.0f;
        }
    } else if (v.format.FormatTag == SCARLETTX_WAVE_FORMAT_XMA2) {
        uint16_t decoded_ch = 0;
        int32_t hr = DecodeXma2Internal(
            buffer->AudioData,
            buffer->AudioBytes,
            qbuf.samples,
            qbuf.frame_count,
            decoded_ch,
            v.format.Channels);
        if (hr < 0) return hr;
        qbuf.channels = decoded_ch;
    } else {
        return kE_NOTIMPL;
    }

    if (qbuf.frame_count == 0) return kE_INVALIDARG;
    v.queue.push_back(std::move(qbuf));
    return kS_OK;
}

int32_t XAudio2VoiceFlushSourceBuffers(uint64_t voice) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->queue.clear();
    return kS_OK;
}

int32_t XAudio2VoiceGetState(uint64_t voice, XAUDIO2_VOICE_STATE* out_state) {
    if (!out_state) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    const auto& v = *vit->second;
    out_state->BuffersQueued = static_cast<uint32_t>(v.queue.size());
    out_state->SamplesPlayed = v.samples_played;
    out_state->CurrentBufferContext = v.queue.empty() ? nullptr : v.queue.front().context;
    return kS_OK;
}

int32_t XAudio2VoiceSetVolume(uint64_t voice, float volume) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->volume = volume;
    return kS_OK;
}

int32_t XAudio2VoiceGetVolume(uint64_t voice, float* out_volume) {
    if (!out_volume) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    *out_volume = vit->second->volume;
    return kS_OK;
}

int32_t XAudio2VoiceSetFrequencyRatio(uint64_t voice, float ratio) {
    if (ratio <= 0.0f) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->frequency_ratio = std::min(ratio, vit->second->max_frequency_ratio);
    return kS_OK;
}

int32_t XAudio2VoiceGetFrequencyRatio(uint64_t voice, float* out_ratio) {
    if (!out_ratio) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    *out_ratio = vit->second->frequency_ratio;
    return kS_OK;
}

int32_t XAudio2VoiceSetOutputMatrix(
    uint64_t voice,
    uint32_t src_channels,
    uint32_t dst_channels,
    const float* level_matrix) {
    if (!level_matrix || src_channels == 0 || dst_channels == 0) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->matrix_src_ch = src_channels;
    vit->second->matrix_dst_ch = dst_channels;
    vit->second->output_matrix.assign(level_matrix, level_matrix + src_channels * dst_channels);
    return kS_OK;
}

int32_t XAudio2VoiceGetOutputMatrix(
    uint64_t voice,
    uint32_t src_channels,
    uint32_t dst_channels,
    float* level_matrix) {
    if (!level_matrix || src_channels == 0 || dst_channels == 0) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    const size_t total = src_channels * dst_channels;
    if (vit->second->output_matrix.size() == total) {
        std::memcpy(level_matrix, vit->second->output_matrix.data(), total * sizeof(float));
    } else {
        std::fill_n(level_matrix, total, 1.0f);
    }
    return kS_OK;
}

int32_t XAudio2VoiceSetChannelVolumes(uint64_t voice, uint32_t channels, const float* volumes) {
    if (!volumes || channels == 0) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->channel_volumes.assign(volumes, volumes + channels);
    return kS_OK;
}

int32_t XAudio2VoiceGetChannelVolumes(uint64_t voice, uint32_t channels, float* out_volumes) {
    if (!out_volumes || channels == 0) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    for (uint32_t i = 0; i < channels; ++i) {
        out_volumes[i] = (i < vit->second->channel_volumes.size()) ? vit->second->channel_volumes[i] : 1.0f;
    }
    return kS_OK;
}

int32_t XAudio2VoiceEnableEffect(uint64_t voice, uint32_t /*effect_index*/) {
    std::lock_guard<std::mutex> lk(State().mu);
    if (State().voices.find(voice) == State().voices.end()) return kE_HANDLE;
    return kS_OK;
}

int32_t XAudio2VoiceDisableEffect(uint64_t voice, uint32_t /*effect_index*/) {
    std::lock_guard<std::mutex> lk(State().mu);
    if (State().voices.find(voice) == State().voices.end()) return kE_HANDLE;
    return kS_OK;
}

int32_t XAudio2VoiceDiscontinuity(uint64_t voice) {
    std::lock_guard<std::mutex> lk(State().mu);
    if (State().voices.find(voice) == State().voices.end()) return kE_HANDLE;
    return kS_OK;
}

int32_t XAudio2VoiceExitLoop(uint64_t voice) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->loop_exited = true;
    return kS_OK;
}

int32_t XAudio2CommitChanges(uint64_t engine, uint32_t /*operation_set*/) {
    std::lock_guard<std::mutex> lk(State().mu);
    if (State().engines.find(engine) == State().engines.end()) return kE_HANDLE;
    return kS_OK;
}

int32_t XAudio2VoiceSetVolumeRamp(uint64_t voice, float target_volume, uint32_t /*ramp_duration_ms*/) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto vit = State().voices.find(voice);
    if (vit == State().voices.end()) return kE_HANDLE;
    vit->second->volume = target_volume;
    return kS_OK;
}

int32_t XAudio2RenderAudioFrames(
    uint64_t engine,
    uint32_t frame_count,
    float* out_interleaved_pcm,
    uint32_t out_sample_capacity) {
    if (!out_interleaved_pcm || frame_count == 0) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto eit = State().engines.find(engine);
    if (eit == State().engines.end()) return kE_HANDLE;
    auto& eng = *eit->second;

    const uint32_t dst_ch = eng.master_channels;
    const uint32_t needed_samples = frame_count * dst_ch;
    if (out_sample_capacity < needed_samples) return kE_INVALIDARG;

    std::fill_n(out_interleaved_pcm, needed_samples, 0.0f);
    if (!eng.running) return kS_OK;

    float master_gain = 1.0f;
    if (eng.mastering_voice != 0) {
        auto mit = State().voices.find(eng.mastering_voice);
        if (mit != State().voices.end()) master_gain = mit->second->volume;
    }

    for (uint64_t vh : eng.voice_handles) {
        auto vit = State().voices.find(vh);
        if (vit == State().voices.end()) continue;
        auto& v = *vit->second;
        if (v.kind != VoiceKind::kSource || !v.playing || v.queue.empty()) continue;

        const double step =
            (static_cast<double>(v.format.SampleRate) / static_cast<double>(eng.master_sample_rate)) *
            static_cast<double>(v.frequency_ratio);

        for (uint32_t f = 0; f < frame_count; ++f) {
            if (v.queue.empty()) break;
            auto& buf = v.queue.front();
            const uint32_t idx0 = static_cast<uint32_t>(buf.read_pos);
            const uint32_t idx1 = std::min(idx0 + 1u, buf.frame_count - 1u);
            const float frac = static_cast<float>(buf.read_pos - static_cast<double>(idx0));
            const uint32_t src_ch = buf.channels;

            float src_sample[8] = {};
            for (uint32_t sc = 0; sc < src_ch && sc < 8; ++sc) {
                const float s0 = buf.samples[idx0 * src_ch + sc];
                const float s1 = buf.samples[idx1 * src_ch + sc];
                src_sample[sc] = (s0 + (s1 - s0) * frac) * v.volume;
            }

            if (v.matrix_src_ch == src_ch && v.matrix_dst_ch == dst_ch &&
                v.output_matrix.size() == src_ch * dst_ch) {
                for (uint32_t dc = 0; dc < dst_ch; ++dc) {
                    float sum = 0.0f;
                    for (uint32_t sc = 0; sc < src_ch; ++sc) {
                        sum += src_sample[sc] * v.output_matrix[dc * src_ch + sc];
                    }
                    out_interleaved_pcm[f * dst_ch + dc] += sum;
                }
            } else if (dst_ch == 2u && (src_ch == 6u || src_ch == 8u)) {
                // ITU-R BS.775 5.1 (FL,FR,FC,LFE,BL,BR) & 7.1 (+SL,SR) spatial fold-down to stereo:
                // Preserves Center dialogue (-3 dB = 0.70710678), LFE (-10 dB = 0.31622777), and Surround (-3 dB).
                constexpr float kCenterGain = 0.70710678f;
                constexpr float kSurrGain = 0.70710678f;
                constexpr float kLfeGain = 0.31622777f;
                float left = src_sample[0] + kCenterGain * src_sample[2] + kLfeGain * src_sample[3] +
                             kSurrGain * src_sample[4];
                float right = src_sample[1] + kCenterGain * src_sample[2] + kLfeGain * src_sample[3] +
                              kSurrGain * src_sample[5];
                if (src_ch == 8u) {
                    left += kSurrGain * src_sample[6];
                    right += kSurrGain * src_sample[7];
                }
                out_interleaved_pcm[f * 2u + 0u] += left;
                out_interleaved_pcm[f * 2u + 1u] += right;
            } else {
                for (uint32_t dc = 0; dc < dst_ch; ++dc) {
                    const uint32_t sc = (src_ch == 1) ? 0u : (dc % src_ch);
                    out_interleaved_pcm[f * dst_ch + dc] += src_sample[sc];
                }
            }

            buf.read_pos += step;
            v.samples_played++;
            if (buf.read_pos >= static_cast<double>(buf.frame_count)) {
                v.queue.pop_front();
            }
        }
    }

    for (uint32_t i = 0; i < needed_samples; ++i) {
        out_interleaved_pcm[i] = std::clamp(out_interleaved_pcm[i] * master_gain, -1.0f, 1.0f);
    }
    eng.total_frames_mixed += frame_count;
    return kS_OK;
}

int32_t XAudio2WriteWavFile(
    const char* path,
    uint32_t sample_rate,
    uint16_t channels,
    const float* interleaved_pcm,
    uint32_t frame_count) {
    if (!path || !interleaved_pcm || sample_rate == 0 || channels == 0 || frame_count == 0) {
        return kE_INVALIDARG;
    }

    const uint32_t data_bytes = frame_count * channels * sizeof(int16_t);
    const uint32_t riff_size = 36u + data_bytes;
    const uint32_t byte_rate = sample_rate * channels * sizeof(int16_t);
    const uint16_t block_align = static_cast<uint16_t>(channels * sizeof(int16_t));
    const uint16_t bits_per_sample = 16;
    const uint16_t pcm_fmt = 1;
    const uint32_t fmt_chunk_size = 16;

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return kE_FAIL;

    ofs.write("RIFF", 4);
    ofs.write(reinterpret_cast<const char*>(&riff_size), 4);
    ofs.write("WAVE", 4);
    ofs.write("fmt ", 4);
    ofs.write(reinterpret_cast<const char*>(&fmt_chunk_size), 4);
    ofs.write(reinterpret_cast<const char*>(&pcm_fmt), 2);
    ofs.write(reinterpret_cast<const char*>(&channels), 2);
    ofs.write(reinterpret_cast<const char*>(&sample_rate), 4);
    ofs.write(reinterpret_cast<const char*>(&byte_rate), 4);
    ofs.write(reinterpret_cast<const char*>(&block_align), 2);
    ofs.write(reinterpret_cast<const char*>(&bits_per_sample), 2);
    ofs.write("data", 4);
    ofs.write(reinterpret_cast<const char*>(&data_bytes), 4);

    const uint32_t total_samples = frame_count * channels;
    for (uint32_t i = 0; i < total_samples; ++i) {
        const float clamped = std::clamp(interleaved_pcm[i], -1.0f, 1.0f);
        const int16_t s16 = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        ofs.write(reinterpret_cast<const char*>(&s16), sizeof(s16));
    }
    return ofs.good() ? kS_OK : kE_FAIL;
}

int32_t Xma2DecodePacket(
    const uint8_t* packet_data,
    uint32_t packet_bytes,
    float* out_interleaved_pcm,
    uint32_t out_sample_capacity,
    uint32_t* out_frames_decoded,
    uint16_t* out_channels) {
    if (!out_interleaved_pcm || !out_frames_decoded || !out_channels) return kE_INVALIDARG;
    std::vector<float> pcm;
    uint32_t frames = 0;
    uint16_t ch = 0;
    int32_t hr = DecodeXma2Internal(packet_data, packet_bytes, pcm, frames, ch);
    if (hr < 0) return hr;
    if (out_sample_capacity < pcm.size()) return kE_INVALIDARG;
    std::memcpy(out_interleaved_pcm, pcm.data(), pcm.size() * sizeof(float));
    *out_frames_decoded = frames;
    *out_channels = ch;
    return kS_OK;
}

int32_t Xma2EncodeMdctBlock(
    const float* interleaved_pcm,
    uint32_t frame_count,
    uint16_t channels,
    uint8_t* out_xma2_bytes,
    uint32_t max_out_bytes,
    uint32_t* out_bytes_written) {
    if (!interleaved_pcm || frame_count == 0 || channels == 0 || channels > 8 || !out_bytes_written) {
        return kE_INVALIDARG;
    }
    const auto encoded = scarlettx::audio::EncodeXma2MdctBlock(interleaved_pcm, frame_count, channels);
    if (encoded.empty()) return kE_FAIL;
    *out_bytes_written = static_cast<uint32_t>(encoded.size());
    if (!out_xma2_bytes || max_out_bytes < encoded.size()) {
        return static_cast<int32_t>(0x8007007Au);  // E_NOT_SUFFICIENT_BUFFER
    }
    std::memcpy(out_xma2_bytes, encoded.data(), encoded.size());
    return kS_OK;
}

int32_t XAudio2Create(void** ppXAudio2, uint32_t flags, uint32_t xaudio2_processor) {
    // Always guest-facing objects, never the host COM pointer.
    return ScarlettXGuestXAudio2Create(ppXAudio2, flags, xaudio2_processor);
}

}  // extern "C"

namespace {

#if defined(_WIN32)
HMODULE LoadHostSystemXAudio2Dll() {
    static HMODULE sys_xa2 = []() -> HMODULE {
        wchar_t full_path[MAX_PATH + 32] = {};
        const UINT len = GetSystemDirectoryW(full_path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return nullptr;
        }
        const wchar_t kSuffix[] = L"\\xaudio2_9.dll";
        UINT pos = len;
        if (full_path[pos - 1] == L'\\' || full_path[pos - 1] == L'/') {
            --pos;
        }
        std::memcpy(full_path + pos, kSuffix, sizeof(kSuffix));
        HMODULE mod = LoadLibraryExW(full_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        HMODULE self_mod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LoadHostSystemXAudio2Dll),
            &self_mod);
        if (mod && mod == self_mod) {
            FreeLibrary(mod);
            return nullptr;
        }
        return mod;
    }();
    return sys_xa2;
}
#endif

// Spec-compliant IXAPO + IXAPOParameters COM fallback when host CreateFX is unavailable or custom Xbox CLSID is passed
#pragma pack(push, 1)
struct XapoRegPropsRaw {
    uint8_t clsid[16];
    wchar_t friendly_name[256];
    wchar_t copyright_info[256];
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t flags;
    uint32_t min_input_buffers;
    uint32_t max_input_buffers;
    uint32_t min_output_buffers;
    uint32_t max_output_buffers;
};
#pragma pack(pop)
static_assert(sizeof(XapoRegPropsRaw) == 1068, "XAPO_REGISTRATION_PROPERTIES ABI size");

struct XapoLockBufParamsRaw {
    const void* pFormat;
    uint32_t maxFrameCount;
};

struct XapoProcessBufParamsRaw {
    void* pBuffer;
    uint32_t bufferFlags;
    uint32_t validFrameCount;
};

struct FallbackXapo {
    const void* const* vtbl;
    struct ParamsSubobj {
        const void* const* vtbl;
        FallbackXapo* owner;
    } params_sub;
    std::atomic<uint32_t> refs{1};
    uint8_t clsid[16]{};
    uint32_t frame_bytes = 8;
    std::mutex mu;
    std::vector<uint8_t> params;
};

bool XapoGuidEq(const void* a, const uint8_t b[16]) {
    return a && std::memcmp(a, b, 16) == 0;
}

static constexpr uint8_t kIidIUnknown[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
static constexpr uint8_t kIidIXapo[16] = {
    0x84, 0xB9, 0x10, 0xA4, 0x39, 0x98, 0x19, 0x48, 0xA0, 0xBE, 0x28, 0x56, 0xAE, 0x6B, 0x3A, 0xDB};
static constexpr uint8_t kIidIXapo27[16] = {
    0x01, 0xC0, 0x0B, 0xA9, 0x87, 0xE8, 0x8A, 0x48, 0xB5, 0x45, 0xE9, 0x08, 0x6A, 0xFD, 0x6E, 0x16};
static constexpr uint8_t kIidIXapoParams[16] = {
    0x66, 0x5C, 0xD9, 0x26, 0xF2, 0x80, 0x9A, 0x49, 0xAD, 0x54, 0x5A, 0xE7, 0xF0, 0x1C, 0x6D, 0x98};

int32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_QI(void* self_ptr, const void* riid, void** ppv) {
    if (!ppv) return static_cast<int32_t>(0x80004003u);
    *ppv = nullptr;
    if (!self_ptr || !riid) return kE_INVALIDARG;
    auto* self = static_cast<FallbackXapo*>(self_ptr);
    if (XapoGuidEq(riid, kIidIUnknown) || XapoGuidEq(riid, kIidIXapo) || XapoGuidEq(riid, kIidIXapo27)) {
        self->refs.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return kS_OK;
    }
    if (XapoGuidEq(riid, kIidIXapoParams)) {
        self->refs.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->params_sub;
        return kS_OK;
    }
    return static_cast<int32_t>(0x80004002u);  // E_NOINTERFACE
}

uint32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_AddRef(void* self_ptr) {
    return self_ptr ? (static_cast<FallbackXapo*>(self_ptr)->refs.fetch_add(1, std::memory_order_relaxed) + 1u) : 0u;
}

uint32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_Release(void* self_ptr) {
    if (!self_ptr) return 0u;
    auto* self = static_cast<FallbackXapo*>(self_ptr);
    const uint32_t rem = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete self;
    return rem;
}

int32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_GetRegProps(void* self_ptr, XapoRegPropsRaw** out_props) {
    if (!out_props) return kE_INVALIDARG;
    *out_props = nullptr;
    if (!self_ptr) return kE_INVALIDARG;
    auto* self = static_cast<FallbackXapo*>(self_ptr);
#if defined(_WIN32)
    auto* p = static_cast<XapoRegPropsRaw*>(CoTaskMemAlloc(sizeof(XapoRegPropsRaw)));
#else
    auto* p = static_cast<XapoRegPropsRaw*>(std::malloc(sizeof(XapoRegPropsRaw)));
#endif
    if (!p) return static_cast<int32_t>(0x8007000Eu);
    std::memset(p, 0, sizeof(*p));
    std::memcpy(p->clsid, self->clsid, 16);
    const wchar_t kName[] = L"ScarlettEdge Passthrough XAPO";
    std::memcpy(p->friendly_name, kName, sizeof(kName));
    p->major_version = 1;
    p->minor_version = 0;
    p->flags = 0x1Fu;  // XAPO_FLAG_DEFAULT (in-place + matching format)
    p->min_input_buffers = 1;
    p->max_input_buffers = 1;
    p->min_output_buffers = 1;
    p->max_output_buffers = 1;
    *out_props = p;
    return kS_OK;
}

int32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_FormatSupported(void*, const void*, const void*, void** ppSupported) {
    if (ppSupported) *ppSupported = nullptr;
    return kS_OK;
}

int32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_Initialize(void* self_ptr, const void* data, uint32_t bytes) {
    if (!self_ptr) return kE_INVALIDARG;
    auto* self = static_cast<FallbackXapo*>(self_ptr);
    std::lock_guard<std::mutex> lk(self->mu);
    if (data && bytes > 0) {
        const auto* b = static_cast<const uint8_t*>(data);
        self->params.assign(b, b + bytes);
    }
    return kS_OK;
}

void
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_Void(void*) {}

int32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_LockForProcess(void* self_ptr, uint32_t in_cnt, const XapoLockBufParamsRaw* in_params,
                            uint32_t, const XapoLockBufParamsRaw*) {
    if (self_ptr && in_cnt > 0 && in_params && in_params[0].pFormat) {
        // WAVEFORMATEX: nChannels at +2 (uint16), nBlockAlign at +12 (uint16)
        uint16_t block_align = 0;
        std::memcpy(&block_align, static_cast<const uint8_t*>(in_params[0].pFormat) + 12, sizeof(uint16_t));
        if (block_align > 0 && block_align <= 64) {
            static_cast<FallbackXapo*>(self_ptr)->frame_bytes = block_align;
        }
    }
    return kS_OK;
}

void
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_Process(void* self_ptr, uint32_t in_cnt, const XapoProcessBufParamsRaw* in_params,
                     uint32_t out_cnt, XapoProcessBufParamsRaw* out_params, int32_t /*enabled*/) {
    if (!self_ptr || in_cnt == 0 || out_cnt == 0 || !in_params || !out_params) return;
    auto* self = static_cast<FallbackXapo*>(self_ptr);
    out_params[0].bufferFlags = in_params[0].bufferFlags;
    out_params[0].validFrameCount = in_params[0].validFrameCount;
    if (out_params[0].pBuffer && in_params[0].pBuffer && out_params[0].pBuffer != in_params[0].pBuffer) {
        const size_t bytes = static_cast<size_t>(in_params[0].validFrameCount) * self->frame_bytes;
        if (in_params[0].bufferFlags == 1u /* XAPO_BUFFER_SILENT */) {
            std::memset(out_params[0].pBuffer, 0, bytes);
        } else {
            std::memcpy(out_params[0].pBuffer, in_params[0].pBuffer, bytes);
        }
    }
}

uint32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapo_CalcFrames(void*, uint32_t frames) {
    return frames;
}

int32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapoParams_QI(void* sub_ptr, const void* riid, void** ppv) {
    auto* sub = static_cast<FallbackXapo::ParamsSubobj*>(sub_ptr);
    return sub ? FallbackXapo_QI(sub->owner, riid, ppv) : kE_INVALIDARG;
}

uint32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapoParams_AddRef(void* sub_ptr) {
    auto* sub = static_cast<FallbackXapo::ParamsSubobj*>(sub_ptr);
    return sub ? FallbackXapo_AddRef(sub->owner) : 0u;
}

uint32_t
#if defined(_WIN32)
WINAPI
#endif
FallbackXapoParams_Release(void* sub_ptr) {
    auto* sub = static_cast<FallbackXapo::ParamsSubobj*>(sub_ptr);
    return sub ? FallbackXapo_Release(sub->owner) : 0u;
}

void
#if defined(_WIN32)
WINAPI
#endif
FallbackXapoParams_Set(void* sub_ptr, const void* data, uint32_t bytes) {
    auto* sub = static_cast<FallbackXapo::ParamsSubobj*>(sub_ptr);
    if (!sub || !sub->owner || !data || bytes == 0) return;
    std::lock_guard<std::mutex> lk(sub->owner->mu);
    const auto* b = static_cast<const uint8_t*>(data);
    sub->owner->params.assign(b, b + bytes);
}

void
#if defined(_WIN32)
WINAPI
#endif
FallbackXapoParams_Get(void* sub_ptr, void* data, uint32_t bytes) {
    if (!data || bytes == 0) return;
    std::memset(data, 0, bytes);
    auto* sub = static_cast<FallbackXapo::ParamsSubobj*>(sub_ptr);
    if (!sub || !sub->owner) return;
    std::lock_guard<std::mutex> lk(sub->owner->mu);
    const size_t n = std::min<size_t>(bytes, sub->owner->params.size());
    if (n > 0) std::memcpy(data, sub->owner->params.data(), n);
}

const void* const kFallbackXapoParamsVtbl[5] = {
    reinterpret_cast<const void*>(&FallbackXapoParams_QI),
    reinterpret_cast<const void*>(&FallbackXapoParams_AddRef),
    reinterpret_cast<const void*>(&FallbackXapoParams_Release),
    reinterpret_cast<const void*>(&FallbackXapoParams_Set),
    reinterpret_cast<const void*>(&FallbackXapoParams_Get),
};

const void* const kFallbackXapoVtbl[13] = {
    reinterpret_cast<const void*>(&FallbackXapo_QI),              // 0 QueryInterface
    reinterpret_cast<const void*>(&FallbackXapo_AddRef),          // 1 AddRef
    reinterpret_cast<const void*>(&FallbackXapo_Release),         // 2 Release
    reinterpret_cast<const void*>(&FallbackXapo_GetRegProps),     // 3 GetRegistrationProperties
    reinterpret_cast<const void*>(&FallbackXapo_FormatSupported), // 4 IsInputFormatSupported
    reinterpret_cast<const void*>(&FallbackXapo_FormatSupported), // 5 IsOutputFormatSupported
    reinterpret_cast<const void*>(&FallbackXapo_Initialize),      // 6 Initialize
    reinterpret_cast<const void*>(&FallbackXapo_Void),            // 7 Reset
    reinterpret_cast<const void*>(&FallbackXapo_LockForProcess),  // 8 LockForProcess
    reinterpret_cast<const void*>(&FallbackXapo_Void),            // 9 UnlockForProcess
    reinterpret_cast<const void*>(&FallbackXapo_Process),         // 10 Process
    reinterpret_cast<const void*>(&FallbackXapo_CalcFrames),      // 11 CalcInputFrames
    reinterpret_cast<const void*>(&FallbackXapo_CalcFrames),      // 12 CalcOutputFrames
};

}  // namespace

extern "C" {

int32_t X3DAudioInitialize(
    uint32_t speaker_channel_mask,
    float speed_of_sound,
    X3DAUDIO_HANDLE instance) {
    if (!instance || speed_of_sound <= 0.0f) return kE_INVALIDARG;
    std::memset(instance, 0, sizeof(X3DAUDIO_HANDLE));
    const uint32_t magic = 0x33444158u;  // 'XAD3'
    std::memcpy(instance + 0, &magic, 4);
    std::memcpy(instance + 4, &speaker_channel_mask, 4);
    std::memcpy(instance + 8, &speed_of_sound, 4);
    return kS_OK;
}

void X3DAudioCalculate(
    const X3DAUDIO_HANDLE instance,
    const X3DAUDIO_LISTENER* listener,
    const X3DAUDIO_EMITTER* emitter,
    uint32_t flags,
    X3DAUDIO_DSP_SETTINGS* dsp) {
    if (!instance || !listener || !emitter || !dsp) return;
    float speed_of_sound = 343.5f;
    std::memcpy(&speed_of_sound, instance + 8, 4);
    if (speed_of_sound <= 1.0f) speed_of_sound = 343.5f;

    const float dx = emitter->Position.x - listener->Position.x;
    const float dy = emitter->Position.y - listener->Position.y;
    const float dz = emitter->Position.z - listener->Position.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    dsp->EmitterToListenerDistance = dist;

    // Orthonormal listener right = Top x Front (left-handed X3DAudio coordinate system)
    float fx = listener->OrientFront.x, fy = listener->OrientFront.y, fz = listener->OrientFront.z;
    const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (flen > 1e-6f) { fx /= flen; fy /= flen; fz /= flen; } else { fz = 1.0f; }

    float ux = listener->OrientTop.x, uy = listener->OrientTop.y, uz = listener->OrientTop.z;
    const float ulen = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (ulen > 1e-6f) { ux /= ulen; uy /= ulen; uz /= ulen; } else { uy = 1.0f; }

    const float rx = uy * fz - uz * fy;
    const float ry = uz * fx - ux * fz;
    const float rz = ux * fy - uy * fx;

    float pan = 0.0f;  // -1 = full left, +1 = full right
    float front_dot = 1.0f;
    if (dist > 1e-5f) {
        const float inv_d = 1.0f / dist;
        pan = std::clamp((dx * rx + dy * ry + dz * rz) * inv_d, -1.0f, 1.0f);
        front_dot = std::clamp((dx * fx + dy * fy + dz * fz) * inv_d, -1.0f, 1.0f);
        dsp->EmitterToListenerAngle = std::acos(front_dot);
    } else {
        dsp->EmitterToListenerAngle = 0.0f;
    }

    const float scaler = emitter->CurveDistanceScaler > 0.0f ? emitter->CurveDistanceScaler : 1.0f;
    const float atten = (dist <= scaler) ? 1.0f : (scaler / dist);

    // Doppler pitch shift
    dsp->DopplerFactor = 1.0f;
    if ((flags & SCARLETTX_X3DAUDIO_CALCULATE_DOPPLER) != 0 && dist > 1e-5f) {
        const float inv_d = 1.0f / dist;
        const float dir_x = dx * inv_d, dir_y = dy * inv_d, dir_z = dz * inv_d;
        const float v_listener = listener->Velocity.x * dir_x + listener->Velocity.y * dir_y + listener->Velocity.z * dir_z;
        const float v_emitter = emitter->Velocity.x * dir_x + emitter->Velocity.y * dir_y + emitter->Velocity.z * dir_z;
        const float d_scale = emitter->DopplerScaler > 0.0f ? emitter->DopplerScaler : 1.0f;
        const float max_v = speed_of_sound * 0.9f;
        const float vl = std::clamp(v_listener * d_scale, -max_v, max_v);
        const float ve = std::clamp(v_emitter * d_scale, -max_v, max_v);
        dsp->ListenerVelocityComponent = v_listener;
        dsp->EmitterVelocityComponent = v_emitter;
        dsp->DopplerFactor = std::clamp((speed_of_sound + vl) / (speed_of_sound + ve), 0.25f, 4.0f);
    }

    if ((flags & SCARLETTX_X3DAUDIO_CALCULATE_MATRIX) != 0 &&
        dsp->pMatrixCoefficients && dsp->SrcChannelCount > 0 && dsp->DstChannelCount > 0) {
        const uint32_t src_ch = dsp->SrcChannelCount;
        const uint32_t dst_ch = dsp->DstChannelCount;
        std::fill_n(dsp->pMatrixCoefficients, src_ch * dst_ch, 0.0f);

        // Constant-power angle in [0, pi/2]: 0 = full left, pi/2 = full right
        const float angle = (pan + 1.0f) * (3.14159265f * 0.25f);
        const float gain_l = std::cos(angle) * atten;
        const float gain_r = std::sin(angle) * atten;

        for (uint32_t s = 0; s < src_ch; ++s) {
            if (dst_ch == 1u) {
                dsp->pMatrixCoefficients[s] = atten;
            } else if (dst_ch == 2u) {
                dsp->pMatrixCoefficients[0u * src_ch + s] = gain_l;
                dsp->pMatrixCoefficients[1u * src_ch + s] = gain_r;
            } else {
                // 5.1 (6ch) or 7.1 (8ch) surround speaker placement: FL, FR, FC, LFE, BL, BR [, SL, SR]
                const float front_w = std::max(0.0f, (front_dot + 1.0f) * 0.5f);
                const float back_w = 1.0f - front_w;
                dsp->pMatrixCoefficients[0u * src_ch + s] = gain_l * front_w;
                dsp->pMatrixCoefficients[1u * src_ch + s] = gain_r * front_w;
                if (dst_ch >= 3u) {
                    dsp->pMatrixCoefficients[2u * src_ch + s] =
                        atten * std::max(0.0f, 1.0f - std::fabs(pan)) * front_w * 0.70710678f;
                }
                if (dst_ch >= 4u) {
                    dsp->pMatrixCoefficients[3u * src_ch + s] = atten * 0.25f;  // LFE
                }
                if (dst_ch >= 6u) {
                    dsp->pMatrixCoefficients[4u * src_ch + s] = gain_l * back_w;
                    dsp->pMatrixCoefficients[5u * src_ch + s] = gain_r * back_w;
                }
                if (dst_ch >= 8u) {
                    const float side_w = 1.0f - std::fabs(front_dot);
                    dsp->pMatrixCoefficients[6u * src_ch + s] = gain_l * side_w * 0.70710678f;
                    dsp->pMatrixCoefficients[7u * src_ch + s] = gain_r * side_w * 0.70710678f;
                }
            }
        }
    }
    dsp->LPFDirectCoefficient = std::clamp(1.0f - dist * 0.002f, 0.2f, 1.0f);
    dsp->LPFReverbCoefficient = 0.75f;
    dsp->ReverbLevel = atten * 0.5f;
}

int32_t CreateFX(
    const void* clsid,
    void** out_effect,
    const void* init_data,
    uint32_t init_data_bytes) {
    if (!clsid || !out_effect) return kE_INVALIDARG;
    *out_effect = nullptr;
#if defined(_WIN32)
    // Forward standard XAPOFX CLSIDs (CLSID_FXReverb, CLSID_FXEQ, CLSID_FXMasteringLimiter, CLSID_FXEcho)
    // to Windows System32\xaudio2_9.dll CreateFX first!
    if (HMODULE sys_xa2 = LoadHostSystemXAudio2Dll()) {
        using FnHostCreateFX = int32_t(__cdecl*)(const void*, void**, const void*, uint32_t);
        auto host_cfx = reinterpret_cast<FnHostCreateFX>(reinterpret_cast<void*>(
            GetProcAddress(sys_xa2, "CreateFX")));
        if (host_cfx && reinterpret_cast<void*>(host_cfx) != reinterpret_cast<void*>(&CreateFX)) {
            const int32_t hr = host_cfx(clsid, out_effect, init_data, init_data_bytes);
            if (hr >= 0 && *out_effect) {
                return hr;
            }
        }
    }
#endif
    auto* fx = new (std::nothrow) FallbackXapo();
    if (!fx) return static_cast<int32_t>(0x8007000Eu);
    fx->vtbl = kFallbackXapoVtbl;
    fx->params_sub.vtbl = kFallbackXapoParamsVtbl;
    fx->params_sub.owner = fx;
    std::memcpy(fx->clsid, clsid, 16);
    if (init_data && init_data_bytes > 0) {
        const auto* p = static_cast<const uint8_t*>(init_data);
        fx->params.assign(p, p + init_data_bytes);
    }
    *out_effect = fx;
    return kS_OK;
}

SCARLETTX_XAUDIO2_API int32_t CreateAudioReverb(void** out_apo) {
    if (!out_apo) return kE_INVALIDARG;
    *out_apo = nullptr;
#if defined(_WIN32)
    if (HMODULE sys_xa2 = LoadHostSystemXAudio2Dll()) {
        using Fn = int32_t(WINAPI*)(void**);
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(sys_xa2, "CreateAudioReverb")))) {
            const int32_t hr = fn(out_apo);
            if (hr >= 0 && *out_apo) return hr;
        }
    }
#endif
    static constexpr uint8_t kClsidAudioReverb[16] = {
        0x16, 0x3B, 0x63, 0xC2, 0x1B, 0x47, 0x98, 0x44, 0xB8, 0xC5, 0x4F, 0x09, 0x59, 0xE2, 0xEC, 0x09};
    return CreateFX(kClsidAudioReverb, out_apo, nullptr, 0);
}

SCARLETTX_XAUDIO2_API int32_t CreateAudioReverbV2_8(void** out_apo) {
    return CreateAudioReverb(out_apo);
}

SCARLETTX_XAUDIO2_API int32_t CreateAudioVolumeMeter(void** out_apo) {
    if (!out_apo) return kE_INVALIDARG;
    *out_apo = nullptr;
#if defined(_WIN32)
    if (HMODULE sys_xa2 = LoadHostSystemXAudio2Dll()) {
        using Fn = int32_t(WINAPI*)(void**);
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(sys_xa2, "CreateAudioVolumeMeter")))) {
            const int32_t hr = fn(out_apo);
            if (hr >= 0 && *out_apo) return hr;
        }
    }
#endif
    static constexpr uint8_t kClsidVolumeMeter[16] = {
        0x16, 0xB1, 0xC3, 0x4F, 0x2A, 0x97, 0xCF, 0x40, 0xBC, 0x37, 0x7D, 0xB0, 0x3D, 0xB2, 0xFB, 0xA3};
    return CreateFX(kClsidVolumeMeter, out_apo, nullptr, 0);
}

SCARLETTX_XAUDIO2_API int32_t XAudio2CreateV2_9(void** ppXAudio2, uint32_t flags, uint32_t processor) {
    return XAudio2Create(ppXAudio2, flags, processor);
}

SCARLETTX_XAUDIO2_API int32_t XAudio2CreateWithVersionInfo(
    void** ppXAudio2, uint32_t flags, uint32_t processor, uint32_t /*ntddi_version*/) {
    return XAudio2Create(ppXAudio2, flags, processor);
}

SCARLETTX_XAUDIO2_API int32_t XAudio2CreateWithSharedContexts(
    void** ppXAudio2, uint32_t flags, uint32_t processor, void* /*shared_ctx*/) {
    return XAudio2Create(ppXAudio2, flags, processor);
}

}  // extern "C"
