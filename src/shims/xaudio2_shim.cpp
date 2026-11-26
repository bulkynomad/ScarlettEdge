#include "scarlettx/audio/xaudio2_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
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
    uint16_t& out_channels) {
    if (!packet_data || packet_bytes < sizeof(XMA2_PACKET_HEADER)) {
        return kE_INVALIDARG;
    }
    XMA2_PACKET_HEADER hdr{};
    std::memcpy(&hdr, packet_data, sizeof(hdr));
    if (hdr.Magic != 0x32414D58u || hdr.Channels == 0 || hdr.Channels > 8 || hdr.SampleCount == 0) {
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
            decoded_ch);
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

int32_t XAudio2Create(void** ppXAudio2, uint32_t flags, uint32_t xaudio2_processor) {
    // Always guest-facing objects, never the host COM pointer.
    return ScarlettXGuestXAudio2Create(ppXAudio2, flags, xaudio2_processor);
}

}  // extern "C"
