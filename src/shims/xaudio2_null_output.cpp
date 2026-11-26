// Silent XAudio2 output for hosts without an audio device.
//
// Objects with the vtable shapes of the host's IXAudio2 and voices, so the
// guest-facing proxies in xaudio2_guest.cpp can sit on top of them unchanged.
// Source voices consume their queued buffers in real time, in 10 ms steps like
// the real engine, and report progress through the voice callbacks. No sample
// is read or mixed.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr uint32_t kEndOfStream = 0x40;   // XAUDIO2_END_OF_STREAM
constexpr uint32_t kLoopInfinite = 255;   // XAUDIO2_LOOP_INFINITE

template <typename Ret, typename... Args>
Ret VCall(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = Ret (WINAPI*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

// Layouts from xaudio2.h (1-byte packing there as well).
#pragma pack(push, 1)
struct Buffer {
    uint32_t flags;
    uint32_t audio_bytes;
    const uint8_t* audio_data;
    uint32_t play_begin;
    uint32_t play_length;
    uint32_t loop_begin;
    uint32_t loop_length;
    uint32_t loop_count;
    void* context;
};
struct VoiceState {
    void* current_buffer_context;
    uint32_t buffers_queued;
    uint64_t samples_played;
};
struct VoiceDetails {
    uint32_t creation_flags;
    uint32_t active_flags;
    uint32_t input_channels;
    uint32_t input_sample_rate;
};
#pragma pack(pop)

struct NullEngine;

struct NullVoice {
    void* const* vtbl;
    NullEngine* engine = nullptr;
    bool is_source = false;
    void* callback = nullptr;  // IXAudio2VoiceCallback, may be null
    uint32_t flags = 0;
    uint32_t channels = 2;
    uint32_t rate = 48000;
    uint32_t block_align = 4;
    float volume = 1.0f;
    float frequency_ratio = 1.0f;
    bool started = false;
    std::deque<Buffer> queue;
    bool front_started = false;
    uint64_t front_pos = 0;      // next frame of the front buffer
    uint32_t front_loops_left = 0;
    uint64_t samples_played = 0;
};

struct NullEngine {
    void* const* vtbl;
    std::atomic<uint32_t> refs{1};
    // Held while callbacks run; a callback may submit buffers.
    std::recursive_mutex mu;
    std::vector<NullVoice*> voices;
    std::vector<void*> callbacks;  // IXAudio2EngineCallback
    bool running = true;
    HANDLE thread = nullptr;
    HANDLE stop = nullptr;
};

// The frames a buffer plays, and the region it repeats while loops remain.
struct Region {
    uint64_t play_begin, play_end, loop_begin, loop_end;
};
Region RegionOf(const NullVoice& v, const Buffer& b) {
    const uint64_t total = v.block_align ? b.audio_bytes / v.block_align : 0;
    Region r{};
    r.play_begin = b.play_begin < total ? b.play_begin : total;
    r.play_end = b.play_length != 0 && r.play_begin + b.play_length < total ? r.play_begin + b.play_length : total;
    r.loop_begin = b.loop_begin < r.play_end ? b.loop_begin : r.play_end;
    r.loop_end = b.loop_length != 0 && r.loop_begin + b.loop_length < r.play_end ? r.loop_begin + b.loop_length
                                                                              : r.play_end;
    return r;
}

// One processing pass of a started source voice. Engine lock held.
void ProcessVoice(NullVoice& v, uint32_t quantum_ms) {
    uint64_t remaining = static_cast<uint64_t>(v.rate * static_cast<double>(v.frequency_ratio)) * quantum_ms / 1000;
    if (v.callback) {
        const uint32_t needed = v.queue.empty() ? static_cast<uint32_t>(remaining * v.block_align) : 0;
        VCall<void, uint32_t>(v.callback, 0, needed);
    }
    while (remaining > 0 && !v.queue.empty()) {
        Buffer b = v.queue.front();
        if (!v.front_started) {
            v.front_started = true;
            v.front_pos = RegionOf(v, b).play_begin;
            v.front_loops_left = b.loop_count;
            if (v.callback) VCall<void, void*>(v.callback, 3, b.context);
            if (v.queue.empty()) break;  // flushed from the callback
        }
        const Region r = RegionOf(v, b);
        const bool looping = v.front_loops_left != 0 && r.loop_end > r.loop_begin;
        const uint64_t target = looping ? r.loop_end : r.play_end;
        const uint64_t left = target > v.front_pos ? target - v.front_pos : 0;
        const uint64_t take = left < remaining ? left : remaining;
        v.front_pos += take;
        v.samples_played += take;
        remaining -= take;
        if (v.front_pos < target) break;
        if (looping) {
            if (v.front_loops_left != kLoopInfinite) --v.front_loops_left;
            v.front_pos = r.loop_begin;
            if (v.callback) VCall<void, void*>(v.callback, 5, b.context);
            continue;
        }
        b.flags = v.queue.front().flags;  // Discontinuity may have marked it meanwhile
        v.queue.pop_front();
        v.front_started = false;
        if (v.callback) VCall<void, void*>(v.callback, 4, b.context);
        if ((b.flags & kEndOfStream) != 0) {
            v.samples_played = 0;  // counted per stream
            if (v.callback) VCall<void>(v.callback, 2);
        }
    }
    if (v.callback) VCall<void>(v.callback, 1);
}

DWORD WINAPI EngineThread(void* param) {
    auto* e = static_cast<NullEngine*>(param);
    constexpr uint32_t kQuantumMs = 10;
    uint64_t next = GetTickCount64() + kQuantumMs;
    for (;;) {
        const uint64_t now = GetTickCount64();
        if (WaitForSingleObject(e->stop, next > now ? static_cast<DWORD>(next - now) : 0) == WAIT_OBJECT_0) break;
        next += kQuantumMs;
        if (next + 500 < GetTickCount64()) next = GetTickCount64() + kQuantumMs;  // do not replay a long stall
        std::lock_guard<std::recursive_mutex> lock(e->mu);
        if (!e->running) continue;
        for (size_t i = 0; i < e->callbacks.size(); ++i) VCall<void>(e->callbacks[i], 0);
        for (size_t i = 0; i < e->voices.size(); ++i) {
            if (e->voices[i]->is_source && e->voices[i]->started) ProcessVoice(*e->voices[i], kQuantumMs);
        }
        for (size_t i = 0; i < e->callbacks.size(); ++i) VCall<void>(e->callbacks[i], 1);
    }
    return 0;
}

// ---- voices (host IXAudio2Voice / IXAudio2SourceVoice / IXAudio2MasteringVoice shapes) ----

void WINAPI VoiceGetDetails(NullVoice* v, VoiceDetails* out) {
    if (!out) return;
    out->creation_flags = v->flags;
    out->active_flags = v->flags;
    out->input_channels = v->channels;
    out->input_sample_rate = v->rate;
}
int32_t WINAPI VoiceOk1(NullVoice*, uintptr_t) { return kOk; }
int32_t WINAPI VoiceOk2(NullVoice*, uintptr_t, uintptr_t) { return kOk; }
int32_t WINAPI VoiceOk3(NullVoice*, uintptr_t, uintptr_t, uintptr_t) { return kOk; }
int32_t WINAPI VoiceOk4(NullVoice*, uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return kOk; }
int32_t WINAPI VoiceOk5(NullVoice*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return kOk; }
void WINAPI VoiceGetEffectState(NullVoice*, uint32_t, int32_t* enabled) { if (enabled) *enabled = 0; }
int32_t WINAPI VoiceNoEffect(NullVoice*, uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return kInvalidArg; }
void WINAPI VoiceGetNothing1(NullVoice*, uintptr_t) {}
void WINAPI VoiceGetNothing2(NullVoice*, uintptr_t, uintptr_t) {}
int32_t WINAPI VoiceSetVolume(NullVoice* v, float volume, uint32_t) { v->volume = volume; return kOk; }
void WINAPI VoiceGetVolume(NullVoice* v, float* out) { if (out) *out = v->volume; }
void WINAPI VoiceGetChannelVolumes(NullVoice*, uint32_t channels, float* out) {
    for (uint32_t i = 0; out && i < channels; ++i) out[i] = 1.0f;
}
void WINAPI VoiceGetOutputMatrix(NullVoice*, void*, uint32_t src, uint32_t dst, float* out) {
    for (uint32_t i = 0; out && i < src * dst; ++i) out[i] = 0.0f;
}
void WINAPI VoiceDestroy(NullVoice* v) {
    NullEngine* e = v->engine;
    {
        // Waits for a processing pass that is inside this voice's callbacks.
        std::lock_guard<std::recursive_mutex> lock(e->mu);
        for (auto it = e->voices.begin(); it != e->voices.end(); ++it) {
            if (*it == v) { e->voices.erase(it); break; }
        }
    }
    delete v;
}

int32_t WINAPI SourceStart(NullVoice* v, uint32_t, uint32_t) {
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    v->started = true;
    return kOk;
}
int32_t WINAPI SourceStop(NullVoice* v, uint32_t, uint32_t) {
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    v->started = false;
    return kOk;
}
int32_t WINAPI SourceSubmit(NullVoice* v, const Buffer* buffer, const void*) {
    if (!buffer || !buffer->audio_data || buffer->audio_bytes == 0) return kInvalidArg;
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    if (v->queue.size() >= 64) return kInvalidArg;  // XAUDIO2_MAX_QUEUED_BUFFERS
    v->queue.push_back(*buffer);
    return kOk;
}
int32_t WINAPI SourceFlush(NullVoice* v) {
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    // Only a buffer that is playing on a started voice stays; each dropped
    // buffer still reports its end.
    const size_t keep = v->started && v->front_started ? 1u : 0u;
    std::deque<Buffer> dropped;
    while (v->queue.size() > keep) {
        dropped.push_front(v->queue.back());
        v->queue.pop_back();
    }
    if (keep == 0) v->front_started = false;
    for (const Buffer& b : dropped) {
        if (v->callback) VCall<void, void*>(v->callback, 4, b.context);
    }
    return kOk;
}
int32_t WINAPI SourceDiscontinuity(NullVoice* v) {
    // No more buffers follow the last queued one: it ends the stream.
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    if (!v->queue.empty()) v->queue.back().flags |= kEndOfStream;
    return kOk;
}
int32_t WINAPI SourceExitLoop(NullVoice* v, uint32_t) {
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    v->front_loops_left = 0;
    return kOk;
}
void WINAPI SourceGetState(NullVoice* v, VoiceState* out, uint32_t) {
    if (!out) return;
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    out->current_buffer_context = v->front_started && !v->queue.empty() ? v->queue.front().context : nullptr;
    out->buffers_queued = static_cast<uint32_t>(v->queue.size());
    out->samples_played = v->samples_played;
}
int32_t WINAPI SourceSetFrequencyRatio(NullVoice* v, float ratio, uint32_t) {
    if (!(ratio > 0.0f)) return kInvalidArg;
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    v->frequency_ratio = ratio;
    return kOk;
}
void WINAPI SourceGetFrequencyRatio(NullVoice* v, float* out) { if (out) *out = v->frequency_ratio; }
int32_t WINAPI SourceSetSampleRate(NullVoice* v, uint32_t rate) {
    if (rate == 0) return kInvalidArg;
    std::lock_guard<std::recursive_mutex> lock(v->engine->mu);
    v->rate = rate;
    return kOk;
}
int32_t WINAPI MasteringGetChannelMask(NullVoice* v, uint32_t* mask) {
    if (!mask) return kInvalidArg;
    *mask = v->channels == 1 ? 0x4u : 0x3u;  // front centre, or front left and right
    return kOk;
}

#define NULL_FN(fn) reinterpret_cast<void*>(&fn)
#define NULL_VOICE_COMMON                                                   \
    NULL_FN(VoiceGetDetails),        /* 0 GetVoiceDetails */                \
    NULL_FN(VoiceOk1),               /* 1 SetOutputVoices */                \
    NULL_FN(VoiceOk1),               /* 2 SetEffectChain */                 \
    NULL_FN(VoiceOk2),               /* 3 EnableEffect */                   \
    NULL_FN(VoiceOk2),               /* 4 DisableEffect */                  \
    NULL_FN(VoiceGetEffectState),    /* 5 GetEffectState */                 \
    NULL_FN(VoiceNoEffect),          /* 6 SetEffectParameters */            \
    NULL_FN(VoiceNoEffect),          /* 7 GetEffectParameters */            \
    NULL_FN(VoiceOk2),               /* 8 SetFilterParameters */            \
    NULL_FN(VoiceGetNothing1),       /* 9 GetFilterParameters */            \
    NULL_FN(VoiceOk3),               /* 10 SetOutputFilterParameters */     \
    NULL_FN(VoiceGetNothing2),       /* 11 GetOutputFilterParameters */     \
    NULL_FN(VoiceSetVolume),         /* 12 SetVolume */                     \
    NULL_FN(VoiceGetVolume),         /* 13 GetVolume */                     \
    NULL_FN(VoiceOk3),               /* 14 SetChannelVolumes */             \
    NULL_FN(VoiceGetChannelVolumes), /* 15 GetChannelVolumes */             \
    NULL_FN(VoiceOk5),               /* 16 SetOutputMatrix */               \
    NULL_FN(VoiceGetOutputMatrix),   /* 17 GetOutputMatrix */               \
    NULL_FN(VoiceDestroy)            /* 18 DestroyVoice */

void* const kNullSourceVtbl[29] = {
    NULL_VOICE_COMMON,
    NULL_FN(SourceStart), NULL_FN(SourceStop), NULL_FN(SourceSubmit), NULL_FN(SourceFlush),
    NULL_FN(SourceDiscontinuity), NULL_FN(SourceExitLoop), NULL_FN(SourceGetState),
    NULL_FN(SourceSetFrequencyRatio), NULL_FN(SourceGetFrequencyRatio), NULL_FN(SourceSetSampleRate),
};
void* const kNullSubmixVtbl[19] = {NULL_VOICE_COMMON};
void* const kNullMasteringVtbl[20] = {NULL_VOICE_COMMON, NULL_FN(MasteringGetChannelMask)};

// ---- engine (host IXAudio2 shape) --------------------------------------------

int32_t WINAPI EngineQueryInterface(NullEngine*, const void*, void** out) {
    if (out) *out = nullptr;
    return static_cast<int32_t>(0x80004002u);  // only used through the guest-facing proxy
}
uint32_t WINAPI EngineAddRef(NullEngine* e) { return e->refs.fetch_add(1) + 1; }
uint32_t WINAPI EngineRelease(NullEngine* e) {
    const uint32_t remaining = e->refs.fetch_sub(1) - 1;
    if (remaining != 0) return remaining;
    SetEvent(e->stop);
    WaitForSingleObject(e->thread, INFINITE);
    CloseHandle(e->thread);
    CloseHandle(e->stop);
    for (NullVoice* v : e->voices) delete v;
    delete e;
    return 0;
}
int32_t WINAPI EngineRegister(NullEngine* e, void* callback) {
    if (!callback) return kInvalidArg;
    std::lock_guard<std::recursive_mutex> lock(e->mu);
    e->callbacks.push_back(callback);
    return kOk;
}
void WINAPI EngineUnregister(NullEngine* e, void* callback) {
    std::lock_guard<std::recursive_mutex> lock(e->mu);
    for (auto it = e->callbacks.begin(); it != e->callbacks.end(); ++it) {
        if (*it == callback) { e->callbacks.erase(it); break; }
    }
}
NullVoice* AddVoice(NullEngine* e, void* const* vtbl, uint32_t flags, uint32_t channels, uint32_t rate) {
    auto* v = new NullVoice{};
    v->vtbl = vtbl;
    v->engine = e;
    v->flags = flags;
    v->channels = channels ? channels : 2;
    v->rate = rate ? rate : 48000;
    std::lock_guard<std::recursive_mutex> lock(e->mu);
    e->voices.push_back(v);
    return v;
}
int32_t WINAPI EngineCreateSourceVoice(NullEngine* e, void** out, const uint8_t* format, uint32_t flags, float,
                                       void* callback, const void*, const void*) {
    if (!out || !format) return kInvalidArg;
    // WAVEFORMATEX: channels at +2, samples per second at +4, block align at +12.
    uint16_t channels, block_align;
    uint32_t rate;
    std::memcpy(&channels, format + 2, 2);
    std::memcpy(&rate, format + 4, 4);
    std::memcpy(&block_align, format + 12, 2);
    if (channels == 0 || rate == 0 || block_align == 0) return kInvalidArg;
    NullVoice* v = AddVoice(e, kNullSourceVtbl, flags, channels, rate);
    std::lock_guard<std::recursive_mutex> lock(e->mu);
    v->block_align = block_align;
    v->callback = callback;
    v->is_source = true;
    *out = v;
    return kOk;
}
int32_t WINAPI EngineCreateSubmixVoice(NullEngine* e, void** out, uint32_t channels, uint32_t rate, uint32_t flags,
                                       uint32_t, const void*, const void*) {
    if (!out) return kInvalidArg;
    *out = AddVoice(e, kNullSubmixVtbl, flags, channels, rate);
    return kOk;
}
int32_t WINAPI EngineCreateMasteringVoice(NullEngine* e, void** out, uint32_t channels, uint32_t rate,
                                          uint32_t flags, const void*, const void*, uint32_t) {
    if (!out) return kInvalidArg;
    *out = AddVoice(e, kNullMasteringVtbl, flags, channels, rate);
    return kOk;
}
int32_t WINAPI EngineStart(NullEngine* e) {
    std::lock_guard<std::recursive_mutex> lock(e->mu);
    e->running = true;
    return kOk;
}
void WINAPI EngineStop(NullEngine* e) {
    std::lock_guard<std::recursive_mutex> lock(e->mu);
    e->running = false;
}
int32_t WINAPI EngineCommitChanges(NullEngine*, uint32_t) { return kOk; }
void WINAPI EngineGetPerformanceData(NullEngine*, void* out) {
    if (out) std::memset(out, 0, 64);  // sizeof(XAUDIO2_PERFORMANCE_DATA)
}
void WINAPI EngineSetDebugConfiguration(NullEngine*, const void*, void*) {}

void* const kNullEngineVtbl[13] = {
    NULL_FN(EngineQueryInterface), NULL_FN(EngineAddRef), NULL_FN(EngineRelease), NULL_FN(EngineRegister),
    NULL_FN(EngineUnregister), NULL_FN(EngineCreateSourceVoice), NULL_FN(EngineCreateSubmixVoice),
    NULL_FN(EngineCreateMasteringVoice), NULL_FN(EngineStart), NULL_FN(EngineStop),
    NULL_FN(EngineCommitChanges), NULL_FN(EngineGetPerformanceData), NULL_FN(EngineSetDebugConfiguration),
};

}  // namespace

extern "C" void* ScarlettXCreateNullXAudio2() {
    auto* e = new NullEngine;
    e->vtbl = kNullEngineVtbl;
    e->stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    e->thread = e->stop ? CreateThread(nullptr, 0, &EngineThread, e, 0, nullptr) : nullptr;
    if (!e->thread) {
        if (e->stop) CloseHandle(e->stop);
        delete e;
        return nullptr;
    }
    return e;
}

#else  // !_WIN32

extern "C" void* ScarlettXCreateNullXAudio2() { return nullptr; }

#endif
