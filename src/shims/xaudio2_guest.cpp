// Guest-facing XAudio2 2.9 objects for real titles.
//
// A title gets objects owned by this module, never a host COM pointer. Each
// object forwards to the host's XAudio2 2.9 engine or voice. Voices handed
// back in send lists are translated to host voices, and the title's callback
// objects are called through bridges owned by this module.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "scarlettx/audio/xma2_decoder.h"

#if defined(_WIN32)
#include <windows.h>

namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kNoInterface = static_cast<int32_t>(0x80004002u);
constexpr int32_t kNotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t kPointer = static_cast<int32_t>(0x80004003u);

bool TraceEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

// Traces the first calls of a kind only; voices are driven every few ms.
#define XA2_TRACE(limit, ...)                                              \
    do {                                                                   \
        static std::atomic<int> xa2_trace_count{0};                        \
        if (TraceEnabled() && xa2_trace_count.fetch_add(1) < (limit)) {    \
            std::fprintf(stderr, "[ScarlettX][xaudio2_9] " __VA_ARGS__);   \
            std::fputc('\n', stderr);                                      \
        }                                                                  \
    } while (0)

// Calls slot `slot` of a COM-style object (host object or title callback).
template <typename Ret, typename... Args>
Ret VCall(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = Ret (WINAPI*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

// Layouts from xaudio2.h (1-byte packing there as well).
#pragma pack(push, 1)
struct SendDescriptor {
    uint32_t flags;
    void* output_voice;
};
struct VoiceSends {
    uint32_t send_count;
    SendDescriptor* sends;
};
struct EffectDescriptor {
    void* effect;
    int32_t initial_state;
    uint32_t output_channels;
};
struct EffectChain {
    uint32_t effect_count;
    EffectDescriptor* effect_descriptors;
};
#pragma pack(pop)

struct Guid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};
bool SameGuid(const Guid& x, const Guid& y) { return std::memcmp(&x, &y, sizeof(Guid)) == 0; }
const Guid kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
// IXAudio2 of XAudio2 2.9.
const Guid kIidXAudio2 = {0x2B02E3CF, 0x2E0B, 0x4EC3, {0xBE, 0x45, 0x1B, 0x2A, 0x3F, 0xE7, 0x21, 0x0D}};

// ---- bridges: host -> title callback objects --------------------------------

struct CallbackBridge {
    void* const* vtbl;  // what the host calls
    void* guest;        // the title's callback object
};

void WINAPI VoiceCbPassStart(CallbackBridge* b, uint32_t bytes) {
    XA2_TRACE(4, "voice callback %p: processing pass start, %u bytes required", b->guest, bytes);
    VCall<void>(b->guest, 0, bytes);
}
void WINAPI VoiceCbPassEnd(CallbackBridge* b) { VCall<void>(b->guest, 1); }
void WINAPI VoiceCbStreamEnd(CallbackBridge* b) {
    XA2_TRACE(4, "voice callback %p: stream end", b->guest);
    VCall<void>(b->guest, 2);
}
void WINAPI VoiceCbBufferStart(CallbackBridge* b, void* ctx) {
    XA2_TRACE(4, "voice callback %p: buffer start, context %p", b->guest, ctx);
    VCall<void>(b->guest, 3, ctx);
}
void WINAPI VoiceCbBufferEnd(CallbackBridge* b, void* ctx) {
    XA2_TRACE(4, "voice callback %p: buffer end, context %p", b->guest, ctx);
    VCall<void>(b->guest, 4, ctx);
}
void WINAPI VoiceCbLoopEnd(CallbackBridge* b, void* ctx) { VCall<void>(b->guest, 5, ctx); }
void WINAPI VoiceCbError(CallbackBridge* b, void* ctx, int32_t hr) { VCall<void>(b->guest, 6, ctx, hr); }
void* const kVoiceCallbackVtbl[] = {
    reinterpret_cast<void*>(&VoiceCbPassStart), reinterpret_cast<void*>(&VoiceCbPassEnd),
    reinterpret_cast<void*>(&VoiceCbStreamEnd), reinterpret_cast<void*>(&VoiceCbBufferStart),
    reinterpret_cast<void*>(&VoiceCbBufferEnd), reinterpret_cast<void*>(&VoiceCbLoopEnd),
    reinterpret_cast<void*>(&VoiceCbError),
};

void WINAPI EngineCbPassStart(CallbackBridge* b) { VCall<void>(b->guest, 0); }
void WINAPI EngineCbPassEnd(CallbackBridge* b) { VCall<void>(b->guest, 1); }
void WINAPI EngineCbCriticalError(CallbackBridge* b, int32_t hr) { VCall<void>(b->guest, 2, hr); }
void* const kEngineCallbackVtbl[] = {
    reinterpret_cast<void*>(&EngineCbPassStart), reinterpret_cast<void*>(&EngineCbPassEnd),
    reinterpret_cast<void*>(&EngineCbCriticalError),
};

// ---- objects -----------------------------------------------------------------

enum class VoiceKind { kSource, kSubmix, kMastering };

struct EngineProxy;

struct EffectSlotState {
    bool enabled = true;
    uint32_t output_channels = 2;
    std::vector<uint8_t> params;
};

struct VoiceProxy {
    void* const* vtbl;
    void* host = nullptr;  // host IXAudio2Voice
    VoiceKind kind = VoiceKind::kSource;
    CallbackBridge* callback = nullptr;
    EngineProxy* engine = nullptr;
    bool host_effects_active = false;
    std::vector<EffectSlotState> emulated_effects;
    bool is_xma2_voice = false;
    uint16_t xma2_channels = 2;
    uint32_t xma2_rate = 48000;
    std::mutex xma2_mu;
    std::unique_ptr<scarlettx::audio::Xma2StreamDecoder> xma2_decoder;
    std::deque<std::vector<int16_t>> xma2_pcm_ring;
};

struct EngineProxy {
    void* const* vtbl;
    std::atomic<uint32_t> refs{1};
    std::atomic<void*> host{nullptr};  // host IXAudio2, or the silent engine
    bool silent = false;
    std::mutex mu;
    std::vector<CallbackBridge*> callbacks;
    std::vector<VoiceProxy*> voices;  // not yet destroyed by the title
    std::vector<void*> retired_hosts;  // kept until release: voices may still use them
};

// xaudio2_null_output.cpp
extern "C" void* ScarlettXCreateNullXAudio2();

// Failures of CreateMasteringVoice that are about the call, not the device.
bool IsCallError(int32_t hr) {
    return hr == kInvalidArg || hr == static_cast<int32_t>(0x8007000Eu) ||  // E_OUTOFMEMORY
           hr == static_cast<int32_t>(0x88960001u);                         // XAUDIO2_E_INVALID_CALL
}

extern void* const kSourceVoiceVtbl[29];
extern void* const kSubmixVoiceVtbl[19];
extern void* const kMasteringVoiceVtbl[20];

bool IsVoiceProxy(const void* p) {
    if (!p) return false;
    void* const* v = *reinterpret_cast<void* const* const*>(p);
    return v == kSourceVoiceVtbl || v == kSubmixVoiceVtbl || v == kMasteringVoiceVtbl;
}

// Rewrites a title's send list to host voices. False if an entry is not one
// of this module's voices.
bool TranslateSends(const VoiceSends* in, VoiceSends* out, std::vector<SendDescriptor>* storage) {
    storage->clear();
    if (in->send_count != 0 && !in->sends) return false;
    for (uint32_t i = 0; i < in->send_count; ++i) {
        if (!IsVoiceProxy(in->sends[i].output_voice)) return false;
        storage->push_back({in->sends[i].flags, static_cast<VoiceProxy*>(in->sends[i].output_voice)->host});
    }
    out->send_count = in->send_count;
    out->sends = storage->empty() ? nullptr : storage->data();
    return true;
}

// ---- voices ------------------------------------------------------------------
// Slot numbers and signatures are those of IXAudio2Voice, IXAudio2SourceVoice
// and IXAudio2MasteringVoice in xaudio2.h. Voices have no IUnknown.

// Same call on the host voice, same arguments.
template <int Slot, typename Ret, typename... Args>
Ret WINAPI VoiceForward(VoiceProxy* v, Args... args) {
    XA2_TRACE(1, "voice slot %d first used", Slot);
    return VCall<Ret, Args...>(v->host, Slot, args...);
}

// A destination voice argument: null means "the only output voice".
bool HostDestination(void* guest_voice, void** host_voice) {
    *host_voice = nullptr;
    if (!guest_voice) return true;
    if (!IsVoiceProxy(guest_voice)) return false;
    *host_voice = static_cast<VoiceProxy*>(guest_voice)->host;
    return true;
}

int32_t WINAPI VoiceSetOutputVoices(VoiceProxy* v, const VoiceSends* sends) {
    if (!sends) return VCall<int32_t, const VoiceSends*>(v->host, 1, nullptr);
    VoiceSends host_sends{};
    std::vector<SendDescriptor> storage;
    if (!TranslateSends(sends, &host_sends, &storage)) return kInvalidArg;
    return VCall<int32_t, const VoiceSends*>(v->host, 1, &host_sends);
}

bool ValidateAndCopyEffectChain(
    const EffectChain* in,
    EffectChain* out,
    std::vector<EffectDescriptor>* storage,
    std::vector<EffectSlotState>* emulated_out) {
    storage->clear();
    if (emulated_out) emulated_out->clear();
    if (!in || in->effect_count == 0) {
        out->effect_count = 0;
        out->effect_descriptors = nullptr;
        return true;
    }
    if (!in->effect_descriptors || in->effect_count > 64) return false;
    storage->reserve(in->effect_count);
    for (uint32_t i = 0; i < in->effect_count; ++i) {
        const auto& d = in->effect_descriptors[i];
        if (!d.effect || d.output_channels == 0 || d.output_channels > 64) {
            return false;
        }
        storage->push_back(d);
        if (emulated_out) {
            EffectSlotState slot;
            slot.enabled = (d.initial_state != 0);
            slot.output_channels = d.output_channels;
            emulated_out->push_back(std::move(slot));
        }
    }
    out->effect_count = static_cast<uint32_t>(storage->size());
    out->effect_descriptors = storage->data();
    return true;
}

int32_t WINAPI VoiceSetEffectChain(VoiceProxy* v, const EffectChain* chain) {
    if (!chain || chain->effect_count == 0) {
        v->host_effects_active = false;
        v->emulated_effects.clear();
        return VCall<int32_t, const EffectChain*>(v->host, 2, nullptr);
    }
    EffectChain host_chain{};
    std::vector<EffectDescriptor> storage;
    std::vector<EffectSlotState> emulated;
    if (!ValidateAndCopyEffectChain(chain, &host_chain, &storage, &emulated)) {
        return kInvalidArg;
    }
    if (!v->engine->silent) {
        const int32_t hr = VCall<int32_t, const EffectChain*>(v->host, 2, &host_chain);
        if (hr >= 0) {
            v->host_effects_active = true;
            v->emulated_effects = std::move(emulated);
            XA2_TRACE(8, "voice SetEffectChain(%u effects) passed through to host XAudio2", host_chain.effect_count);
            return hr;
        }
        XA2_TRACE(8, "voice SetEffectChain(%u effects) host returned 0x%08x; tracking emulated DSP chain",
                  host_chain.effect_count, static_cast<uint32_t>(hr));
    }
    v->host_effects_active = false;
    v->emulated_effects = std::move(emulated);
    return kOk;
}

int32_t WINAPI VoiceEnableEffect(VoiceProxy* v, uint32_t index, uint32_t op_set) {
    if (v->host_effects_active) {
        return VCall<int32_t, uint32_t, uint32_t>(v->host, 3, index, op_set);
    }
    if (index >= v->emulated_effects.size()) return kInvalidArg;
    v->emulated_effects[index].enabled = true;
    return kOk;
}

int32_t WINAPI VoiceDisableEffect(VoiceProxy* v, uint32_t index, uint32_t op_set) {
    if (v->host_effects_active) {
        return VCall<int32_t, uint32_t, uint32_t>(v->host, 4, index, op_set);
    }
    if (index >= v->emulated_effects.size()) return kInvalidArg;
    v->emulated_effects[index].enabled = false;
    return kOk;
}

void WINAPI VoiceGetEffectState(VoiceProxy* v, uint32_t index, int32_t* enabled) {
    if (!enabled) return;
    if (v->host_effects_active) {
        VCall<void, uint32_t, int32_t*>(v->host, 5, index, enabled);
        return;
    }
    *enabled = (index < v->emulated_effects.size() && v->emulated_effects[index].enabled) ? 1 : 0;
}

int32_t WINAPI VoiceSetEffectParameters(
    VoiceProxy* v, uint32_t index, const void* params, uint32_t param_bytes, uint32_t op_set) {
    if (v->host_effects_active) {
        return VCall<int32_t, uint32_t, const void*, uint32_t, uint32_t>(v->host, 6, index, params, param_bytes, op_set);
    }
    if (index >= v->emulated_effects.size() || (!params && param_bytes > 0)) return kInvalidArg;
    auto& dst = v->emulated_effects[index].params;
    dst.resize(param_bytes);
    if (param_bytes > 0) std::memcpy(dst.data(), params, param_bytes);
    return kOk;
}

int32_t WINAPI VoiceGetEffectParameters(
    VoiceProxy* v, uint32_t index, void* params, uint32_t param_bytes) {
    if (v->host_effects_active) {
        return VCall<int32_t, uint32_t, void*, uint32_t>(v->host, 7, index, params, param_bytes);
    }
    if (index >= v->emulated_effects.size() || (!params && param_bytes > 0)) return kInvalidArg;
    const auto& src = v->emulated_effects[index].params;
    if (param_bytes > 0) {
        std::memset(params, 0, param_bytes);
        if (!src.empty()) {
            std::memcpy(params, src.data(), src.size() < param_bytes ? src.size() : param_bytes);
        }
    }
    return kOk;
}

int32_t WINAPI VoiceSetOutputFilterParameters(VoiceProxy* v, void* dest, const void* params, uint32_t op_set) {
    void* host_dest = nullptr;
    if (!HostDestination(dest, &host_dest)) return kInvalidArg;
    return VCall<int32_t, void*, const void*, uint32_t>(v->host, 10, host_dest, params, op_set);
}

void WINAPI VoiceGetOutputFilterParameters(VoiceProxy* v, void* dest, void* params) {
    void* host_dest = nullptr;
    if (!HostDestination(dest, &host_dest)) return;
    VCall<void, void*, void*>(v->host, 11, host_dest, params);
}

int32_t WINAPI VoiceSetOutputMatrix(VoiceProxy* v, void* dest, uint32_t src_channels, uint32_t dst_channels,
                                    const float* matrix, uint32_t op_set) {
    void* host_dest = nullptr;
    if (!HostDestination(dest, &host_dest)) return kInvalidArg;
    return VCall<int32_t, void*, uint32_t, uint32_t, const float*, uint32_t>(
        v->host, 16, host_dest, src_channels, dst_channels, matrix, op_set);
}

void WINAPI VoiceGetOutputMatrix(VoiceProxy* v, void* dest, uint32_t src_channels, uint32_t dst_channels,
                                 float* matrix) {
    void* host_dest = nullptr;
    if (!HostDestination(dest, &host_dest)) return;
    VCall<void, void*, uint32_t, uint32_t, float*>(v->host, 17, host_dest, src_channels, dst_channels, matrix);
}

void WINAPI VoiceDestroy(VoiceProxy* v) {
    XA2_TRACE(16, "voice %p DestroyVoice", static_cast<void*>(v));
    VCall<void>(v->host, 18);  // returns once no callback of this voice is running
    {
        std::lock_guard<std::mutex> lock(v->engine->mu);
        auto& voices = v->engine->voices;
        for (auto it = voices.begin(); it != voices.end(); ++it) {
            if (*it == v) { voices.erase(it); break; }
        }
    }
    delete v->callback;
    delete v;
}

int32_t WINAPI SourceStart(VoiceProxy* v, uint32_t flags, uint32_t op_set) {
    const int32_t hr = VCall<int32_t, uint32_t, uint32_t>(v->host, 19, flags, op_set);
    XA2_TRACE(8, "source voice %p Start(flags=0x%x, set=%u) -> 0x%08x", static_cast<void*>(v), flags, op_set,
              static_cast<uint32_t>(hr));
    return hr;
}

int32_t WINAPI SourceSubmitBuffer(VoiceProxy* v, const void* buffer, const void* buffer_wma) {
    if (buffer && v->is_xma2_voice) {
        #pragma pack(push, 1)
        struct XAudio2BufRaw {
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
        #pragma pack(pop)
        const auto* in_buf = static_cast<const XAudio2BufRaw*>(buffer);
        if (!in_buf->audio_data || in_buf->audio_bytes == 0) return kInvalidArg;

        std::vector<float> decoded_f32;
        {
            std::lock_guard<std::mutex> lk(v->xma2_mu);
            if (!v->xma2_decoder) {
                v->xma2_decoder = std::make_unique<scarlettx::audio::Xma2StreamDecoder>(
                    v->xma2_channels, v->xma2_rate);
            }
            if (!v->xma2_decoder->DecodeBlock(
                    in_buf->audio_data, in_buf->audio_bytes, decoded_f32, in_buf->play_length)) {
                return kInvalidArg;
            }
            std::vector<int16_t> pcm16(decoded_f32.size());
            for (size_t i = 0; i < decoded_f32.size(); ++i) {
                const float clamped = std::clamp(decoded_f32[i], -1.0f, 1.0f);
                pcm16[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
            }
            while (v->xma2_pcm_ring.size() >= 64u) {
                v->xma2_pcm_ring.pop_front();
            }
            v->xma2_pcm_ring.push_back(std::move(pcm16));
            XAudio2BufRaw host_buf{};
            host_buf.flags = in_buf->flags;
            host_buf.audio_bytes = static_cast<uint32_t>(v->xma2_pcm_ring.back().size() * sizeof(int16_t));
            host_buf.audio_data = reinterpret_cast<const uint8_t*>(v->xma2_pcm_ring.back().data());
            host_buf.context = in_buf->context;
            return VCall<int32_t, const void*, const void*>(v->host, 21, &host_buf, nullptr);
        }
    }
    const int32_t hr = VCall<int32_t, const void*, const void*>(v->host, 21, buffer, buffer_wma);
    if (buffer) {
        // XAUDIO2_BUFFER: Flags, AudioBytes, ..., LoopCount at +32.
        uint32_t flags, bytes, loops;
        std::memcpy(&flags, buffer, 4);
        std::memcpy(&bytes, static_cast<const uint8_t*>(buffer) + 4, 4);
        std::memcpy(&loops, static_cast<const uint8_t*>(buffer) + 32, 4);
        XA2_TRACE(8, "source voice %p SubmitSourceBuffer(flags=0x%x, bytes=%u, loops=%u, wma=%p) -> 0x%08x",
                  static_cast<void*>(v), flags, bytes, loops, buffer_wma, static_cast<uint32_t>(hr));
    }
    return hr;
}

void WINAPI SourceGetState(VoiceProxy* v, void* state, uint32_t flags) {
    VCall<void, void*, uint32_t>(v->host, 25, state, flags);
    if (state) {
        // XAUDIO2_VOICE_STATE: current buffer context, BuffersQueued, SamplesPlayed.
        uint32_t queued;
        uint64_t played;
        std::memcpy(&queued, static_cast<const uint8_t*>(state) + 8, 4);
        std::memcpy(&played, static_cast<const uint8_t*>(state) + 12, 8);
        XA2_TRACE(6, "source voice %p GetState(flags=0x%x): %u queued, %llu samples played",
                  static_cast<void*>(v), flags, queued, static_cast<unsigned long long>(played));
    }
}

#define XA2_FWD(slot, ...) reinterpret_cast<void*>(&VoiceForward<slot, __VA_ARGS__>)
#define XA2_VOICE_COMMON                                                              \
    XA2_FWD(0, void, void*),                        /* GetVoiceDetails */             \
    reinterpret_cast<void*>(&VoiceSetOutputVoices),                                   \
    reinterpret_cast<void*>(&VoiceSetEffectChain),                                    \
    reinterpret_cast<void*>(&VoiceEnableEffect),                                      \
    reinterpret_cast<void*>(&VoiceDisableEffect),                                     \
    reinterpret_cast<void*>(&VoiceGetEffectState),                                    \
    reinterpret_cast<void*>(&VoiceSetEffectParameters),                               \
    reinterpret_cast<void*>(&VoiceGetEffectParameters),                               \
    XA2_FWD(8, int32_t, const void*, uint32_t),     /* SetFilterParameters */         \
    XA2_FWD(9, void, void*),                        /* GetFilterParameters */         \
    reinterpret_cast<void*>(&VoiceSetOutputFilterParameters),                         \
    reinterpret_cast<void*>(&VoiceGetOutputFilterParameters),                         \
    XA2_FWD(12, int32_t, float, uint32_t),          /* SetVolume */                   \
    XA2_FWD(13, void, float*),                      /* GetVolume */                   \
    XA2_FWD(14, int32_t, uint32_t, const float*, uint32_t), /* SetChannelVolumes */   \
    XA2_FWD(15, void, uint32_t, float*),            /* GetChannelVolumes */           \
    reinterpret_cast<void*>(&VoiceSetOutputMatrix),                                   \
    reinterpret_cast<void*>(&VoiceGetOutputMatrix),                                   \
    reinterpret_cast<void*>(&VoiceDestroy)

void* const kSourceVoiceVtbl[29] = {
    XA2_VOICE_COMMON,
    reinterpret_cast<void*>(&SourceStart),
    XA2_FWD(20, int32_t, uint32_t, uint32_t),       // Stop
    reinterpret_cast<void*>(&SourceSubmitBuffer),
    XA2_FWD(22, int32_t),                           // FlushSourceBuffers
    XA2_FWD(23, int32_t),                           // Discontinuity
    XA2_FWD(24, int32_t, uint32_t),                 // ExitLoop
    reinterpret_cast<void*>(&SourceGetState),
    XA2_FWD(26, int32_t, float, uint32_t),          // SetFrequencyRatio
    XA2_FWD(27, void, float*),                      // GetFrequencyRatio
    XA2_FWD(28, int32_t, uint32_t),                 // SetSourceSampleRate
};
void* const kSubmixVoiceVtbl[19] = {XA2_VOICE_COMMON};
void* const kMasteringVoiceVtbl[20] = {
    XA2_VOICE_COMMON,
    XA2_FWD(19, int32_t, uint32_t*),                // GetChannelMask
};

// ---- engine (IXAudio2) -------------------------------------------------------

int32_t WINAPI EngineQueryInterface(EngineProxy* e, const Guid* riid, void** out) {
    if (!out) return kPointer;
    *out = nullptr;
    if (!riid || !(SameGuid(*riid, kIidUnknown) || SameGuid(*riid, kIidXAudio2))) return kNoInterface;
    e->refs.fetch_add(1, std::memory_order_relaxed);
    *out = e;
    return kOk;
}

uint32_t WINAPI EngineAddRef(EngineProxy* e) { return e->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

uint32_t WINAPI EngineRelease(EngineProxy* e) {
    const uint32_t remaining = e->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        XA2_TRACE(8, "IXAudio2 %p released", static_cast<void*>(e));
        for (CallbackBridge* bridge : e->callbacks) VCall<void, void*>(e->host, 4, bridge);
        VCall<uint32_t>(e->host, 2);
        for (void* retired : e->retired_hosts) VCall<uint32_t>(retired, 2);
        // Releasing the engine destroyed the voices the title left behind.
        for (VoiceProxy* voice : e->voices) {
            delete voice->callback;
            delete voice;
        }
        for (CallbackBridge* bridge : e->callbacks) delete bridge;
        delete e;
    }
    return remaining;
}

int32_t WINAPI EngineRegisterForCallbacks(EngineProxy* e, void* guest_callback) {
    if (!guest_callback) return kInvalidArg;
    auto* bridge = new CallbackBridge{kEngineCallbackVtbl, guest_callback};
    const int32_t hr = VCall<int32_t, void*>(e->host, 3, bridge);
    XA2_TRACE(8, "IXAudio2::RegisterForCallbacks(%p) -> 0x%08x", guest_callback, static_cast<uint32_t>(hr));
    if (hr < 0) {
        delete bridge;
        return hr;
    }
    std::lock_guard<std::mutex> lock(e->mu);
    e->callbacks.push_back(bridge);
    return hr;
}

void WINAPI EngineUnregisterForCallbacks(EngineProxy* e, void* guest_callback) {
    CallbackBridge* bridge = nullptr;
    {
        std::lock_guard<std::mutex> lock(e->mu);
        for (auto it = e->callbacks.begin(); it != e->callbacks.end(); ++it) {
            if ((*it)->guest == guest_callback) {
                bridge = *it;
                e->callbacks.erase(it);
                break;
            }
        }
    }
    if (!bridge) return;
    VCall<void, void*>(e->host, 4, bridge);  // returns once the callback is no longer in use
    delete bridge;
}

// Wraps a host voice, or destroys it when there is nothing to hand it to.
int32_t FinishVoice(EngineProxy* e, int32_t hr, void* host_voice, VoiceKind kind, void* const* vtbl,
                    CallbackBridge* bridge,
                    void** out) {
    if (hr < 0 || !host_voice) {
        delete bridge;
        return hr < 0 ? hr : static_cast<int32_t>(0x80004005u);
    }
    auto* voice = new VoiceProxy{vtbl, host_voice, kind, bridge, e};
    {
        std::lock_guard<std::mutex> lock(e->mu);
        e->voices.push_back(voice);
    }
    *out = voice;
    return hr;
}

int32_t WINAPI EngineCreateSourceVoice(EngineProxy* e, void** out, const void* format, uint32_t flags,
                                       float max_frequency_ratio, void* guest_callback,
                                       const VoiceSends* sends, const EffectChain* chain) {
    if (!out) return kInvalidArg;
    *out = nullptr;
    if (!format) return kInvalidArg;
    VoiceSends host_sends{};
    std::vector<SendDescriptor> storage;
    if (sends && !TranslateSends(sends, &host_sends, &storage)) return kInvalidArg;
    EffectChain host_chain{};
    std::vector<EffectDescriptor> fx_storage;
    std::vector<EffectSlotState> fx_emulated;
    const bool has_chain = chain && chain->effect_count != 0;
    if (has_chain && !ValidateAndCopyEffectChain(chain, &host_chain, &fx_storage, &fx_emulated)) {
        return kInvalidArg;
    }
    const auto* w = static_cast<const uint8_t*>(format);
    uint16_t tag, channels, bits;
    uint32_t rate;
    std::memcpy(&tag, w, 2);
    std::memcpy(&channels, w + 2, 2);
    std::memcpy(&rate, w + 4, 4);
    std::memcpy(&bits, w + 14, 2);

    #pragma pack(push, 1)
    struct HostPcmWaveFormatEx {
        uint16_t tag;
        uint16_t channels;
        uint32_t rate;
        uint32_t avg_bytes;
        uint16_t block_align;
        uint16_t bits;
        uint16_t cb_size;
    };
    #pragma pack(pop)
    const bool is_xma2 = (tag == 0x0166u);
    HostPcmWaveFormatEx pcm_fmt{};
    const void* host_format = format;
    if (is_xma2) {
        const uint16_t safe_ch = (channels >= 1u && channels <= 8u) ? channels : 2u;
        const uint32_t safe_rate = rate > 0u ? rate : 48000u;
        pcm_fmt.tag = 1u;  // WAVE_FORMAT_PCM (PC XAudio2 rejects 0x0166 XMA2 natively)
        pcm_fmt.channels = safe_ch;
        pcm_fmt.rate = safe_rate;
        pcm_fmt.bits = 16u;
        pcm_fmt.block_align = static_cast<uint16_t>(safe_ch * 2u);
        pcm_fmt.avg_bytes = safe_rate * pcm_fmt.block_align;
        pcm_fmt.cb_size = 0u;
        host_format = &pcm_fmt;
    }

    CallbackBridge* bridge = guest_callback ? new CallbackBridge{kVoiceCallbackVtbl, guest_callback} : nullptr;
    void* host_voice = nullptr;
    bool host_fx_ok = false;
    int32_t hr = static_cast<int32_t>(0x80004005u);
    if (has_chain && !e->silent) {
        hr = VCall<int32_t, void**, const void*, uint32_t, float, void*, const VoiceSends*, const EffectChain*>(
            e->host, 5, &host_voice, host_format, flags, max_frequency_ratio, bridge, sends ? &host_sends : nullptr,
            &host_chain);
        host_fx_ok = (hr >= 0 && host_voice != nullptr);
    }
    if (!host_fx_ok) {
        hr = VCall<int32_t, void**, const void*, uint32_t, float, void*, const VoiceSends*, const void*>(
            e->host, 5, &host_voice, host_format, flags, max_frequency_ratio, bridge, sends ? &host_sends : nullptr,
            nullptr);
    }
    XA2_TRACE(32, "IXAudio2::CreateSourceVoice(tag=0x%x, ch=%u, rate=%u, bits=%u, flags=0x%x, ratio=%g, "
                  "callback=%p, sends=%u, fx=%u) -> 0x%08x",
              tag, channels, rate, bits, flags, static_cast<double>(max_frequency_ratio), guest_callback,
              sends ? sends->send_count : 0u, has_chain ? host_chain.effect_count : 0u, static_cast<uint32_t>(hr));
    const int32_t fin = FinishVoice(e, hr, host_voice, VoiceKind::kSource, kSourceVoiceVtbl, bridge, out);
    if (fin >= 0 && *out) {
        auto* vp = static_cast<VoiceProxy*>(*out);
        if (has_chain) {
            vp->host_effects_active = host_fx_ok;
            vp->emulated_effects = std::move(fx_emulated);
        }
        if (is_xma2) {
            vp->is_xma2_voice = true;
            vp->xma2_channels = pcm_fmt.channels;
            vp->xma2_rate = pcm_fmt.rate;
            vp->xma2_decoder = std::make_unique<scarlettx::audio::Xma2StreamDecoder>(
                pcm_fmt.channels, pcm_fmt.rate);
        }
    }
    return fin;
}

int32_t WINAPI EngineCreateSubmixVoice(EngineProxy* e, void** out, uint32_t channels, uint32_t rate,
                                       uint32_t flags, uint32_t stage, const VoiceSends* sends,
                                       const EffectChain* chain) {
    if (!out) return kInvalidArg;
    *out = nullptr;
    VoiceSends host_sends{};
    std::vector<SendDescriptor> storage;
    if (sends && !TranslateSends(sends, &host_sends, &storage)) return kInvalidArg;
    EffectChain host_chain{};
    std::vector<EffectDescriptor> fx_storage;
    std::vector<EffectSlotState> fx_emulated;
    const bool has_chain = chain && chain->effect_count != 0;
    if (has_chain && !ValidateAndCopyEffectChain(chain, &host_chain, &fx_storage, &fx_emulated)) {
        return kInvalidArg;
    }
    void* host_voice = nullptr;
    bool host_fx_ok = false;
    int32_t hr = static_cast<int32_t>(0x80004005u);
    if (has_chain && !e->silent) {
        hr = VCall<int32_t, void**, uint32_t, uint32_t, uint32_t, uint32_t, const VoiceSends*, const EffectChain*>(
            e->host, 6, &host_voice, channels, rate, flags, stage, sends ? &host_sends : nullptr, &host_chain);
        host_fx_ok = (hr >= 0 && host_voice != nullptr);
    }
    if (!host_fx_ok) {
        hr = VCall<int32_t, void**, uint32_t, uint32_t, uint32_t, uint32_t, const VoiceSends*, const void*>(
            e->host, 6, &host_voice, channels, rate, flags, stage, sends ? &host_sends : nullptr, nullptr);
    }
    XA2_TRACE(16, "IXAudio2::CreateSubmixVoice(ch=%u, rate=%u, flags=0x%x, stage=%u, fx=%u) -> 0x%08x", channels, rate,
              flags, stage, has_chain ? host_chain.effect_count : 0u, static_cast<uint32_t>(hr));
    const int32_t fin = FinishVoice(e, hr, host_voice, VoiceKind::kSubmix, kSubmixVoiceVtbl, nullptr, out);
    if (fin >= 0 && *out && has_chain) {
        auto* vp = static_cast<VoiceProxy*>(*out);
        vp->host_effects_active = host_fx_ok;
        vp->emulated_effects = std::move(fx_emulated);
    }
    return fin;
}

int32_t WINAPI EngineCreateMasteringVoice(EngineProxy* e, void** out, uint32_t channels, uint32_t rate,
                                          uint32_t flags, const wchar_t* device_id, const EffectChain* chain,
                                          uint32_t category) {
    if (!out) return kInvalidArg;
    *out = nullptr;
    if (chain && chain->effect_count != 0) {
        XA2_TRACE(8, "IXAudio2::CreateMasteringVoice: %u effects not passed to the host", chain->effect_count);
    }
    // A console device id means nothing here: always the host's default device.
    void* host_voice = nullptr;
    int32_t hr = VCall<int32_t, void**, uint32_t, uint32_t, uint32_t, const wchar_t*, const void*, uint32_t>(
        e->host, 7, &host_voice, channels, rate, flags, nullptr, nullptr, category);
    if (hr < 0 && !IsCallError(hr) && !e->silent) {
        // No usable audio device: keep the title's audio code running, silently.
        if (void* silent_host = ScarlettXCreateNullXAudio2()) {
            std::fprintf(stderr,
                         "[ScarlettX][xaudio2_9] CreateMasteringVoice failed on the host (0x%08x): "
                         "no audio device, using silent output\n", static_cast<uint32_t>(hr));
            std::lock_guard<std::mutex> lock(e->mu);
            void* old_host = e->host;
            for (CallbackBridge* bridge : e->callbacks) {
                VCall<void, void*>(old_host, 4, bridge);
                VCall<int32_t, void*>(silent_host, 3, bridge);
            }
            e->retired_hosts.push_back(old_host);
            e->host = silent_host;
            e->silent = true;
            hr = VCall<int32_t, void**, uint32_t, uint32_t, uint32_t, const wchar_t*, const void*, uint32_t>(
                silent_host, 7, &host_voice, channels, rate, flags, nullptr, nullptr, category);
        }
    }
    XA2_TRACE(8, "IXAudio2::CreateMasteringVoice(ch=%u, rate=%u, flags=0x%x, device=%p, category=%u) -> 0x%08x",
              channels, rate, flags, static_cast<const void*>(device_id), category, static_cast<uint32_t>(hr));
    return FinishVoice(e, hr, host_voice, VoiceKind::kMastering, kMasteringVoiceVtbl, nullptr, out);
}

template <int Slot, typename Ret, typename... Args>
Ret WINAPI EngineForward(EngineProxy* e, Args... args) {
    XA2_TRACE(1, "IXAudio2 slot %d first used", Slot);
    return VCall<Ret, Args...>(e->host, Slot, args...);
}

void* const kEngineVtbl[13] = {
    reinterpret_cast<void*>(&EngineQueryInterface),
    reinterpret_cast<void*>(&EngineAddRef),
    reinterpret_cast<void*>(&EngineRelease),
    reinterpret_cast<void*>(&EngineRegisterForCallbacks),
    reinterpret_cast<void*>(&EngineUnregisterForCallbacks),
    reinterpret_cast<void*>(&EngineCreateSourceVoice),
    reinterpret_cast<void*>(&EngineCreateSubmixVoice),
    reinterpret_cast<void*>(&EngineCreateMasteringVoice),
    reinterpret_cast<void*>(&EngineForward<8, int32_t>),                    // StartEngine
    reinterpret_cast<void*>(&EngineForward<9, void>),                       // StopEngine
    reinterpret_cast<void*>(&EngineForward<10, int32_t, uint32_t>),         // CommitChanges
    reinterpret_cast<void*>(&EngineForward<11, void, void*>),               // GetPerformanceData
    reinterpret_cast<void*>(&EngineForward<12, void, const void*, void*>),  // SetDebugConfiguration
};

}  // namespace

extern "C" int32_t ScarlettXGuestXAudio2Create(void** out, uint32_t flags, uint32_t processor) {
    if (!out) return kInvalidArg;
    *out = nullptr;
    static const HMODULE host_dll = []() -> HMODULE {
        wchar_t dir[MAX_PATH] = {};
        if (GetSystemDirectoryW(dir, MAX_PATH) == 0) return nullptr;
        return LoadLibraryW((std::wstring(dir) + L"\\XAudio2_9.dll").c_str());
    }();
    using CreateFn = int32_t (WINAPI*)(void**, uint32_t, uint32_t);
    const auto create = host_dll
        ? reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(host_dll, "XAudio2Create")))
        : nullptr;
    // The host engine opens its device through COM. A console title never
    // initializes COM, so keep the multithreaded apartment alive for it.
    static const bool mta_held = [] {
        using IncrementFn = int32_t (WINAPI*)(void**);
        const HMODULE combase = LoadLibraryW(L"combase.dll");
        const auto increment = combase
            ? reinterpret_cast<IncrementFn>(reinterpret_cast<void*>(GetProcAddress(combase, "CoIncrementMTAUsage")))
            : nullptr;
        void* cookie = nullptr;
        return increment && increment(&cookie) >= 0;
    }();
    (void)mta_held;
    const char* force_silent = std::getenv("SCARLETTX_XAUDIO2_SILENT");
    void* host = nullptr;
    bool silent = false;
    int32_t hr = static_cast<int32_t>(0x80004005u);
    if (create && !(force_silent && force_silent[0] == '1')) {
        // The console's processor mask does not describe this machine: 0 = default.
        hr = create(&host, flags, 0);
    }
    if (hr < 0 || !host) {
        host = ScarlettXCreateNullXAudio2();
        silent = true;
        if (!host) return hr < 0 ? hr : static_cast<int32_t>(0x80004005u);
        hr = kOk;
    }
    XA2_TRACE(8, "XAudio2Create(flags=0x%x, processor=0x%x) -> 0x%08x%s", flags, processor,
              static_cast<uint32_t>(hr), silent ? " (silent output)" : "");
    auto* engine = new EngineProxy;
    engine->vtbl = kEngineVtbl;
    engine->host = host;
    engine->silent = silent;
    *out = engine;
    return hr;
}

#else  // !_WIN32

extern "C" int32_t ScarlettXGuestXAudio2Create(void** out, uint32_t, uint32_t) {
    if (out) *out = nullptr;
    return static_cast<int32_t>(0x80004001u);
}

#endif
