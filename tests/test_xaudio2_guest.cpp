// Drives the guest-facing XAudio2 objects the way a title does: through raw
// vtable slots, with callback objects implemented here.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#include "scarlettx/audio/acphal_abi.h"
#include "scarlettx/audio/xaudio2_abi.h"

#if defined(_WIN32)
#include <windows.h>

#define SX_CHECK(...)                                                           \
    do {                                                                        \
        if (!(__VA_ARGS__)) {                                                   \
            std::cerr << "[test_xaudio2_guest] FAILED: " #__VA_ARGS__           \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

namespace {

template <typename Ret, typename... Args>
Ret VCall(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = Ret (WINAPI*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

#pragma pack(push, 1)
struct SendDescriptor { uint32_t flags; void* voice; };
struct VoiceSends { uint32_t count; SendDescriptor* sends; };
struct Buffer {
    uint32_t flags, audio_bytes;
    const uint8_t* audio_data;
    uint32_t play_begin, play_length, loop_begin, loop_length, loop_count;
    void* context;
};
struct VoiceState { void* context; uint32_t queued; uint64_t samples_played; };
struct VoiceDetails { uint32_t creation_flags, active_flags, channels, rate; };
struct WaveFormat {
    uint16_t tag, channels;
    uint32_t rate, bytes_per_second;
    uint16_t block_align, bits, extra;
};
#pragma pack(pop)

struct Guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
const Guid kIidUnknown = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const Guid kIidOther = {0x12345678, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};

// The title's IXAudio2VoiceCallback.
struct VoiceCallback {
    void* const* vtbl = nullptr;
    std::atomic<int> pass_start{0}, pass_end{0}, stream_end{0}, buffer_start{0}, buffer_end{0}, loop_end{0};
    std::atomic<void*> last_context{nullptr};
    HANDLE wake = nullptr;

    explicit VoiceCallback(void* const* v) : vtbl(v), wake(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~VoiceCallback() {
        if (wake) CloseHandle(wake);
    }
    VoiceCallback(const VoiceCallback&) = delete;
    VoiceCallback& operator=(const VoiceCallback&) = delete;
};
void WINAPI OnPassStart(VoiceCallback* c, uint32_t) { ++c->pass_start; if (c->wake) SetEvent(c->wake); }
void WINAPI OnPassEnd(VoiceCallback* c) { ++c->pass_end; if (c->wake) SetEvent(c->wake); }
void WINAPI OnStreamEnd(VoiceCallback* c) { ++c->stream_end; if (c->wake) SetEvent(c->wake); }
void WINAPI OnBufferStart(VoiceCallback* c, void* ctx) { c->last_context = ctx; ++c->buffer_start; if (c->wake) SetEvent(c->wake); }
void WINAPI OnBufferEnd(VoiceCallback* c, void* ctx) { c->last_context = ctx; ++c->buffer_end; if (c->wake) SetEvent(c->wake); }
void WINAPI OnLoopEnd(VoiceCallback* c, void*) { ++c->loop_end; if (c->wake) SetEvent(c->wake); }
void WINAPI OnVoiceError(VoiceCallback*, void*, int32_t) {}
void* const kVoiceCallbackVtbl[] = {
    reinterpret_cast<void*>(&OnPassStart), reinterpret_cast<void*>(&OnPassEnd),
    reinterpret_cast<void*>(&OnStreamEnd), reinterpret_cast<void*>(&OnBufferStart),
    reinterpret_cast<void*>(&OnBufferEnd), reinterpret_cast<void*>(&OnLoopEnd),
    reinterpret_cast<void*>(&OnVoiceError),
};

// The title's IXAudio2EngineCallback.
struct EngineCallback {
    void* const* vtbl = nullptr;
    std::atomic<int> pass_start{0}, pass_end{0};
    HANDLE wake = nullptr;

    explicit EngineCallback(void* const* v) : vtbl(v), wake(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~EngineCallback() {
        if (wake) CloseHandle(wake);
    }
    EngineCallback(const EngineCallback&) = delete;
    EngineCallback& operator=(const EngineCallback&) = delete;
};
void WINAPI OnEnginePassStart(EngineCallback* c) { ++c->pass_start; if (c->wake) SetEvent(c->wake); }
void WINAPI OnEnginePassEnd(EngineCallback* c) { ++c->pass_end; if (c->wake) SetEvent(c->wake); }
void WINAPI OnCriticalError(EngineCallback*, int32_t) {}
void* const kEngineCallbackVtbl[] = {
    reinterpret_cast<void*>(&OnEnginePassStart), reinterpret_cast<void*>(&OnEnginePassEnd),
    reinterpret_cast<void*>(&OnCriticalError),
};

template <typename Pred>
bool WaitForEvent(HANDLE wake, Pred pred, DWORD timeout_ms = 5000) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!pred()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        if (!wake || WaitForSingleObject(wake, static_cast<DWORD>(deadline - now)) == WAIT_TIMEOUT) {
            break;
        }
    }
    return pred();
}

int RunEngine(bool force_silent) {
    _putenv_s("SCARLETTX_XAUDIO2_SILENT", force_silent ? "1" : "0");

    SX_CHECK(XAudio2Create(nullptr, 0, 0) < 0);
    void* engine = nullptr;
    SX_CHECK(XAudio2Create(&engine, 0, 0) == 0);
    SX_CHECK(engine != nullptr);

    // IUnknown.
    void* unknown = nullptr;
    SX_CHECK(VCall<int32_t, const Guid*, void**>(engine, 0, &kIidUnknown, &unknown) == 0);
    SX_CHECK(unknown == engine);
    SX_CHECK(VCall<uint32_t>(engine, 2) == 1);
    void* other = engine;
    SX_CHECK(VCall<int32_t, const Guid*, void**>(engine, 0, &kIidOther, &other) == static_cast<int32_t>(0x80004002u));
    SX_CHECK(other == nullptr);

    // Registered before there is an output: must survive the switch to silent output.
    EngineCallback engine_cb{kEngineCallbackVtbl};
    SX_CHECK(VCall<int32_t, void*>(engine, 3, &engine_cb) == 0);

    void* mastering = nullptr;
    SX_CHECK(VCall<int32_t, void**, uint32_t, uint32_t, uint32_t, const wchar_t*, const void*, uint32_t>(
                 engine, 7, &mastering, 0u, 0u, 0u, nullptr, nullptr, 6u) == 0);
    SX_CHECK(mastering != nullptr);
    VoiceDetails details{};
    VCall<void, VoiceDetails*>(mastering, 0, &details);
    SX_CHECK(details.channels > 0 && details.rate > 0);
    uint32_t mask = 0;
    SX_CHECK(VCall<int32_t, uint32_t*>(mastering, 19, &mask) == 0);
    SX_CHECK(mask != 0);

    void* submix = nullptr;
    SX_CHECK(VCall<int32_t, void**, uint32_t, uint32_t, uint32_t, uint32_t, const VoiceSends*, const void*>(
                 engine, 6, &submix, 2u, 48000u, 0u, 0u, nullptr, nullptr) == 0);
    SX_CHECK(submix != nullptr);

    const WaveFormat format = {1, 2, 48000, 48000 * 4, 4, 16, 0};  // PCM16 stereo
    VoiceCallback voice_cb{kVoiceCallbackVtbl};

    // A send list may only name voices handed out by the engine.
    void* fake_vtbl[1] = {nullptr};
    void* fake_voice = fake_vtbl;
    SendDescriptor bad_send = {0, &fake_voice};
    VoiceSends bad_sends = {1, &bad_send};
    void* source = nullptr;
    SX_CHECK(VCall<int32_t, void**, const WaveFormat*, uint32_t, float, void*, const VoiceSends*, const void*>(
                 engine, 5, &source, &format, 0u, 2.0f, &voice_cb, &bad_sends, nullptr) ==
             static_cast<int32_t>(0x80070057u));
    SX_CHECK(source == nullptr);

    SendDescriptor send = {0, submix};
    VoiceSends sends = {1, &send};
    SX_CHECK(VCall<int32_t, void**, const WaveFormat*, uint32_t, float, void*, const VoiceSends*, const void*>(
                 engine, 5, &source, &format, 0u, 2.0f, &voice_cb, &sends, nullptr) == 0);
    SX_CHECK(source != nullptr);
    SX_CHECK(source != submix && source != mastering);

    VCall<void, VoiceDetails*>(source, 0, &details);
    SX_CHECK(details.channels == 2 && details.rate == 48000);
    SX_CHECK(VCall<int32_t, float, uint32_t>(source, 12, 0.5f, 0u) == 0);
    float volume = 0.0f;
    VCall<void, float*>(source, 13, &volume);
    SX_CHECK(std::fabs(volume - 0.5f) < 1e-5f);
    SX_CHECK(VCall<int32_t, float, uint32_t>(source, 26, 1.0f, 0u) == 0);
    SX_CHECK(VCall<int32_t, const VoiceSends*>(source, 1, &bad_sends) == static_cast<int32_t>(0x80070057u));
    SX_CHECK(VCall<int32_t, const VoiceSends*>(source, 1, &sends) == 0);

    // Effect chain DSP state & parameter persistence test (works on both host fallback and silent mode)
    {
        #pragma pack(push, 1)
        struct FxDesc { void* effect; int32_t initial_state; uint32_t output_channels; };
        struct FxChain { uint32_t count; FxDesc* descriptors; };
        #pragma pack(pop)
        // Minimal valid COM IUnknown stub so host XAudio2 can safely call QueryInterface
        struct MockXapo {
            void* const* vtbl;
            static int32_t WINAPI QI(void*, const Guid*, void** out) {
                if (out) *out = nullptr;
                return static_cast<int32_t>(0x80004002u);  // E_NOINTERFACE
            }
            static uint32_t WINAPI AddRef(void*) { return 1; }
            static uint32_t WINAPI Release(void*) { return 1; }
        };
        static void* const kMockXapoVtbl[3] = {
            reinterpret_cast<void*>(&MockXapo::QI),
            reinterpret_cast<void*>(&MockXapo::AddRef),
            reinterpret_cast<void*>(&MockXapo::Release),
        };
        MockXapo dummy_xapo{kMockXapoVtbl};
        FxDesc fx_desc = {&dummy_xapo, 1, 2};
        FxChain fx_chain = {1, &fx_desc};
        SX_CHECK(VCall<int32_t, const FxChain*>(submix, 2, &fx_chain) == 0);
        int32_t enabled = 0;
        VCall<void, uint32_t, int32_t*>(submix, 5, 0u, &enabled);
        SX_CHECK(enabled == 1);
        SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(submix, 4, 0u, 0u) == 0);  // DisableEffect
        VCall<void, uint32_t, int32_t*>(submix, 5, 0u, &enabled);
        SX_CHECK(enabled == 0);
        SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(submix, 3, 0u, 0u) == 0);  // EnableEffect
        VCall<void, uint32_t, int32_t*>(submix, 5, 0u, &enabled);
        SX_CHECK(enabled == 1);
        const float reverb_params[2] = {0.75f, 1.5f};
        float readback_params[2] = {0.0f, 0.0f};
        SX_CHECK(VCall<int32_t, uint32_t, const void*, uint32_t, uint32_t>(
                     submix, 6, 0u, reverb_params, sizeof(reverb_params), 0u) == 0);
        SX_CHECK(VCall<int32_t, uint32_t, void*, uint32_t>(
                     submix, 7, 0u, readback_params, sizeof(readback_params)) == 0);
        SX_CHECK(std::fabs(readback_params[0] - 0.75f) < 1e-5f);
        SX_CHECK(std::fabs(readback_params[1] - 1.5f) < 1e-5f);
        // Clear effect chain
        SX_CHECK(VCall<int32_t, const FxChain*>(submix, 2, nullptr) == 0);
    }

    // 100 ms of silence, twice: the second buffer ends the stream.
    const std::vector<uint8_t> pcm(4800 * 4, 0);
    Buffer buffer{};
    buffer.audio_bytes = static_cast<uint32_t>(pcm.size());
    buffer.audio_data = pcm.data();
    buffer.context = reinterpret_cast<void*>(uintptr_t{0x1111});
    SX_CHECK(VCall<int32_t, const Buffer*, const void*>(source, 21, nullptr, nullptr) < 0);
    SX_CHECK(VCall<int32_t, const Buffer*, const void*>(source, 21, &buffer, nullptr) == 0);
    VoiceState state{};
    VCall<void, VoiceState*, uint32_t>(source, 25, &state, 0u);
    SX_CHECK(state.queued == 1);
    SX_CHECK(voice_cb.buffer_start == 0);  // not started yet

    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(source, 19, 0u, 0u) == 0);
    SX_CHECK(WaitForEvent(voice_cb.wake, [&] { return voice_cb.buffer_end == 1; }));
    SX_CHECK(voice_cb.buffer_start == 1);
    SX_CHECK(voice_cb.last_context.load() == buffer.context);
    SX_CHECK(voice_cb.stream_end == 0);
    VCall<void, VoiceState*, uint32_t>(source, 25, &state, 0u);
    SX_CHECK(state.queued == 0);
    if (force_silent) SX_CHECK(state.samples_played == 4800);

    buffer.flags = 0x40;  // XAUDIO2_END_OF_STREAM
    buffer.context = reinterpret_cast<void*>(uintptr_t{0x2222});
    SX_CHECK(VCall<int32_t, const Buffer*, const void*>(source, 21, &buffer, nullptr) == 0);
    SX_CHECK(WaitForEvent(voice_cb.wake, [&] { return voice_cb.stream_end == 1; }));
    SX_CHECK(voice_cb.buffer_start == 2 && voice_cb.buffer_end == 2);
    SX_CHECK(voice_cb.last_context.load() == buffer.context);
    SX_CHECK(voice_cb.pass_start > 0 && voice_cb.pass_end > 0);
    VCall<void, VoiceState*, uint32_t>(source, 25, &state, 0u);
    SX_CHECK(state.queued == 0);
    if (force_silent) SX_CHECK(state.samples_played == 0);  // counted per stream

    SX_CHECK(WaitForEvent(engine_cb.wake, [&] { return engine_cb.pass_start > 0 && engine_cb.pass_end > 0; }));
    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(source, 20, 0u, 0u) == 0);

    // No callback may arrive once DestroyVoice / UnregisterForCallbacks return.
    // Observe two subsequent mixing passes via a separate EngineCallback event wait.
    EngineCallback observer_cb{kEngineCallbackVtbl};
    SX_CHECK(VCall<int32_t, void*>(engine, 3, &observer_cb) == 0);
    VCall<void>(source, 18);
    VCall<void, void*>(engine, 4, &engine_cb);
    const int voice_passes = voice_cb.pass_start;
    const int engine_passes = engine_cb.pass_start;
    observer_cb.pass_end.store(0);
    SX_CHECK(WaitForEvent(observer_cb.wake, [&] { return observer_cb.pass_end.load() >= 2; }));
    VCall<void, void*>(engine, 4, &observer_cb);
    SX_CHECK(voice_cb.pass_start == voice_passes);
    SX_CHECK(engine_cb.pass_start == engine_passes);

    VCall<void>(submix, 18);

    // Retail 52-byte XMA2WAVEFORMATEX (tag = 0x0166) guest voice -> automatic IMDCT-to-PCM16 bridge!
    {
        #pragma pack(push, 1)
        struct GuestXma2Fmt {
            uint16_t tag;
            uint16_t channels;
            uint32_t rate;
            uint32_t avg_bytes;
            uint16_t block_align;
            uint16_t bits;
            uint16_t cb_size;
            uint16_t num_streams;
            uint32_t channel_mask;
            uint32_t samples_encoded;
            uint32_t bytes_per_block;
            uint32_t play_begin, play_length, loop_begin, loop_length;
            uint8_t loop_count, encoder_ver;
            uint16_t block_count;
        };
        #pragma pack(pop)
        static_assert(sizeof(GuestXma2Fmt) == 52, "XMA2WAVEFORMATEX size");

        GuestXma2Fmt xfmt{};
        xfmt.tag = 0x0166u;
        xfmt.channels = 2;
        xfmt.rate = 48000;
        xfmt.bits = 16;
        xfmt.cb_size = 34;
        xfmt.num_streams = 1;
        xfmt.bytes_per_block = 2048;
        xfmt.encoder_ver = 4;
        xfmt.block_count = 1;

        VoiceCallback xma_cb{kVoiceCallbackVtbl};
        void* xma_voice = nullptr;
        SX_CHECK(VCall<int32_t, void**, const void*, uint32_t, float, void*, const VoiceSends*, const void*>(
                     engine, 5, &xma_voice, &xfmt, 0u, 2.0f, &xma_cb, nullptr, nullptr) == 0);
        SX_CHECK(xma_voice != nullptr);

        std::vector<float> test_pcm(256 * 2, 0.25f);
        uint32_t xma_bytes = 0;
        (void)Xma2EncodeMdctBlock(test_pcm.data(), 256, 2, nullptr, 0, &xma_bytes);
        std::vector<uint8_t> xma_pkt(xma_bytes, 0);
        SX_CHECK(Xma2EncodeMdctBlock(test_pcm.data(), 256, 2, xma_pkt.data(), xma_bytes, &xma_bytes) == 0);

        Buffer xbuf{};
        xbuf.flags = 0x40;  // END_OF_STREAM
        xbuf.audio_bytes = xma_bytes;
        xbuf.audio_data = xma_pkt.data();
        xbuf.context = reinterpret_cast<void*>(uintptr_t{0x5CA2});
        SX_CHECK(VCall<int32_t, const Buffer*, const void*>(xma_voice, 21, &xbuf, nullptr) == 0);
        SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(xma_voice, 19, 0u, 0u) == 0);
        SX_CHECK(WaitForEvent(xma_cb.wake, [&] { return xma_cb.buffer_end == 1; }));
        SX_CHECK(xma_cb.last_context.load() == xbuf.context);
        VCall<void>(xma_voice, 18);
    }

    VCall<void>(mastering, 18);
    SX_CHECK(VCall<uint32_t>(engine, 2) == 0);
    return 0;
}

// Queue behaviour of the silent output: loop regions, Discontinuity, flush.
int RunSilentQueue() {
    _putenv_s("SCARLETTX_XAUDIO2_SILENT", "1");
    void* engine = nullptr;
    SX_CHECK(XAudio2Create(&engine, 0, 0) == 0);
    void* mastering = nullptr;
    SX_CHECK(VCall<int32_t, void**, uint32_t, uint32_t, uint32_t, const wchar_t*, const void*, uint32_t>(
                 engine, 7, &mastering, 2u, 48000u, 0u, nullptr, nullptr, 6u) == 0);
    const WaveFormat format = {1, 2, 48000, 48000 * 4, 4, 16, 0};
    VoiceCallback cb{kVoiceCallbackVtbl};
    void* source = nullptr;
    SX_CHECK(VCall<int32_t, void**, const WaveFormat*, uint32_t, float, void*, const VoiceSends*, const void*>(
                 engine, 5, &source, &format, 0u, 2.0f, &cb, nullptr, nullptr) == 0);
    const std::vector<uint8_t> pcm(48000 * 4, 0);  // one second
    VoiceState state{};

    // 4800 frames, of which 2400..3600 play once more: 6000 frames in total.
    Buffer buffer{};
    buffer.audio_bytes = 4800 * 4;
    buffer.audio_data = pcm.data();
    buffer.loop_begin = 2400;
    buffer.loop_length = 1200;
    buffer.loop_count = 1;
    SX_CHECK(VCall<int32_t, const Buffer*, const void*>(source, 21, &buffer, nullptr) == 0);
    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(source, 19, 0u, 0u) == 0);
    SX_CHECK(WaitForEvent(cb.wake, [&] { return cb.buffer_end == 1; }));
    SX_CHECK(cb.loop_end == 1);
    VCall<void, VoiceState*, uint32_t>(source, 25, &state, 0u);
    SX_CHECK(state.samples_played == 6000);

    // Discontinuity: the last queued buffer ends the stream.
    buffer = Buffer{};
    buffer.audio_bytes = 480 * 4;
    buffer.audio_data = pcm.data();
    SX_CHECK(VCall<int32_t, const Buffer*, const void*>(source, 21, &buffer, nullptr) == 0);
    SX_CHECK(VCall<int32_t>(source, 23) == 0);
    SX_CHECK(WaitForEvent(cb.wake, [&] { return cb.stream_end == 1; }));
    SX_CHECK(cb.buffer_end == 2);

    // Flush keeps the playing buffer of a started voice, and nothing once stopped.
    buffer.audio_bytes = static_cast<uint32_t>(pcm.size());
    SX_CHECK(VCall<int32_t, const Buffer*, const void*>(source, 21, &buffer, nullptr) == 0);
    SX_CHECK(VCall<int32_t, const Buffer*, const void*>(source, 21, &buffer, nullptr) == 0);
    SX_CHECK(WaitForEvent(cb.wake, [&] { return cb.buffer_start == 3; }));
    SX_CHECK(VCall<int32_t>(source, 22) == 0);
    VCall<void, VoiceState*, uint32_t>(source, 25, &state, 0u);
    SX_CHECK(state.queued == 1);
    SX_CHECK(cb.buffer_end == 3);
    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(source, 20, 0u, 0u) == 0);
    SX_CHECK(VCall<int32_t>(source, 22) == 0);
    VCall<void, VoiceState*, uint32_t>(source, 25, &state, 0u);
    SX_CHECK(state.queued == 0);
    SX_CHECK(cb.buffer_end == 4);

    // The engine is released with its voices still alive.
    SX_CHECK(VCall<uint32_t>(engine, 2) == 0);
    return 0;
}

// Concurrent realtime SubmitSourceBuffer + FlushSourceBuffers churn on both XMA2 (0x0166)
// and PCM16 voices while actively playing (zero Sleep(), pure Win32 event synchronization).
struct ChurnCallback {
    static constexpr uintptr_t kContextTag = 0x5CA20000ull;
    static constexpr size_t kMaxBuffers = 512;

    void* const* vtbl = nullptr;
    std::atomic<uint32_t> buffer_starts{0};
    std::atomic<uint32_t> buffer_ends{0};
    std::atomic<bool> invalid_context{false};
    std::array<std::atomic<uint32_t>, kMaxBuffers> end_seen{};
    HANDLE wake = nullptr;

    explicit ChurnCallback(void* const* v)
        : vtbl(v), wake(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        for (auto& c : end_seen) c.store(0u, std::memory_order_relaxed);
    }
    ~ChurnCallback() {
        if (wake) CloseHandle(wake);
    }
    ChurnCallback(const ChurnCallback&) = delete;
    ChurnCallback& operator=(const ChurnCallback&) = delete;

    static uint32_t DecodeId(void* ctx, bool* ok) {
        const uintptr_t raw = reinterpret_cast<uintptr_t>(ctx);
        if ((raw & ~uintptr_t{0xFFFFu}) != kContextTag) {
            *ok = false;
            return 0u;
        }
        const uint32_t id = static_cast<uint32_t>(raw & 0xFFFFu);
        if (id == 0u || id >= kMaxBuffers) {
            *ok = false;
            return 0u;
        }
        *ok = true;
        return id;
    }
};

void WINAPI ChurnPassStart(ChurnCallback*, uint32_t) {}
void WINAPI ChurnPassEnd(ChurnCallback*) {}
void WINAPI ChurnStreamEnd(ChurnCallback* c) {
    if (c->wake) SetEvent(c->wake);
}
void WINAPI ChurnBufferStart(ChurnCallback* c, void* ctx) {
    bool ok = false;
    (void)ChurnCallback::DecodeId(ctx, &ok);
    if (!ok) c->invalid_context.store(true, std::memory_order_release);
    c->buffer_starts.fetch_add(1u, std::memory_order_acq_rel);
    if (c->wake) SetEvent(c->wake);
}
void WINAPI ChurnBufferEnd(ChurnCallback* c, void* ctx) {
    bool ok = false;
    const uint32_t id = ChurnCallback::DecodeId(ctx, &ok);
    if (!ok || c->end_seen[id].fetch_add(1u, std::memory_order_acq_rel) != 0u) {
        c->invalid_context.store(true, std::memory_order_release);
    }
    c->buffer_ends.fetch_add(1u, std::memory_order_acq_rel);
    if (c->wake) SetEvent(c->wake);
}
void WINAPI ChurnLoopEnd(ChurnCallback* c, void* ctx) {
    bool ok = false;
    (void)ChurnCallback::DecodeId(ctx, &ok);
    if (!ok) c->invalid_context.store(true, std::memory_order_release);
    if (c->wake) SetEvent(c->wake);
}
void WINAPI ChurnVoiceError(ChurnCallback* c, void* ctx, int32_t) {
    bool ok = false;
    (void)ChurnCallback::DecodeId(ctx, &ok);
    if (!ok) c->invalid_context.store(true, std::memory_order_release);
}
void* const kChurnCallbackVtbl[] = {
    reinterpret_cast<void*>(&ChurnPassStart),
    reinterpret_cast<void*>(&ChurnPassEnd),
    reinterpret_cast<void*>(&ChurnStreamEnd),
    reinterpret_cast<void*>(&ChurnBufferStart),
    reinterpret_cast<void*>(&ChurnBufferEnd),
    reinterpret_cast<void*>(&ChurnLoopEnd),
    reinterpret_cast<void*>(&ChurnVoiceError),
};

int RunRealtimeFlushChurn(bool force_silent) {
    _putenv_s("SCARLETTX_XAUDIO2_SILENT", force_silent ? "1" : "0");

    void* engine = nullptr;
    SX_CHECK(XAudio2Create(&engine, 0, 0) == 0 && engine != nullptr);

    void* mastering = nullptr;
    SX_CHECK(VCall<int32_t, void**, uint32_t, uint32_t, uint32_t, const wchar_t*, const void*, uint32_t>(
                 engine, 7, &mastering, 2u, 48000u, 0u, nullptr, nullptr, 6u) == 0);

    #pragma pack(push, 1)
    struct GuestXma2Fmt {
        uint16_t tag;
        uint16_t channels;
        uint32_t rate;
        uint32_t avg_bytes;
        uint16_t block_align;
        uint16_t bits;
        uint16_t cb_size;
        uint16_t num_streams;
        uint32_t channel_mask;
        uint32_t samples_encoded;
        uint32_t bytes_per_block;
        uint32_t play_begin, play_length, loop_begin, loop_length;
        uint8_t loop_count, encoder_ver;
        uint16_t block_count;
    };
    #pragma pack(pop)

    GuestXma2Fmt xfmt{};
    xfmt.tag = 0x0166u;
    xfmt.channels = 2;
    xfmt.rate = 48000;
    xfmt.bits = 16;
    xfmt.cb_size = 34;
    xfmt.num_streams = 1;
    xfmt.bytes_per_block = 2048;
    xfmt.encoder_ver = 4;
    xfmt.block_count = 1;

    const WaveFormat pfmt = {1, 2, 48000, 48000 * 4, 4, 16, 0};

    ChurnCallback xma_cb{kChurnCallbackVtbl};
    ChurnCallback pcm_cb{kChurnCallbackVtbl};
    void* xma_voice = nullptr;
    void* pcm_voice = nullptr;
    void* xma_nocb_voice = nullptr;

    SX_CHECK(VCall<int32_t, void**, const void*, uint32_t, float, void*, const VoiceSends*, const void*>(
                 engine, 5, &xma_voice, &xfmt, 0u, 2.0f, &xma_cb, nullptr, nullptr) == 0);
    SX_CHECK(VCall<int32_t, void**, const WaveFormat*, uint32_t, float, void*, const VoiceSends*, const void*>(
                 engine, 5, &pcm_voice, &pfmt, 0u, 2.0f, &pcm_cb, nullptr, nullptr) == 0);
    // Also exercise an XMA2 voice without a guest callback to verify internal slot retirement.
    SX_CHECK(VCall<int32_t, void**, const void*, uint32_t, float, void*, const VoiceSends*, const void*>(
                 engine, 5, &xma_nocb_voice, &xfmt, 0u, 2.0f, nullptr, nullptr, nullptr) == 0);

    // ~42.6 ms (2048 stereo frames at 48 kHz) per buffer so the front buffer remains actively
    // playing on the mixing thread while subsequent queued buffers are flushed!
    constexpr uint32_t kBufFrames = 2048;
    std::vector<float> src_wave(kBufFrames * 2);
    for (uint32_t i = 0; i < kBufFrames; ++i) {
        const float t = static_cast<float>(i) / 48000.0f;
        src_wave[i * 2 + 0] = 0.25f * std::sin(2.0f * 3.14159265f * 330.0f * t);
        src_wave[i * 2 + 1] = 0.25f * std::cos(2.0f * 3.14159265f * 440.0f * t);
    }
    uint32_t xma_bytes = 0;
    (void)Xma2EncodeMdctBlock(src_wave.data(), kBufFrames, 2, nullptr, 0, &xma_bytes);
    std::vector<uint8_t> xma_pkt(xma_bytes, 0);
    SX_CHECK(Xma2EncodeMdctBlock(src_wave.data(), kBufFrames, 2, xma_pkt.data(), xma_bytes, &xma_bytes) == 0);
    std::vector<int16_t> pcm_pkt(kBufFrames * 2, 1024);

    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(xma_voice, 19, 0u, 0u) == 0);
    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(pcm_voice, 19, 0u, 0u) == 0);
    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(xma_nocb_voice, 19, 0u, 0u) == 0);

    HANDLE req_flush_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE ack_flush_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SX_CHECK(req_flush_ev != nullptr && ack_flush_ev != nullptr);

    std::atomic<bool> stop_worker{false};
    std::atomic<bool> worker_failed{false};

    // Dedicated flusher thread racing FlushSourceBuffers + GetState against realtime playback & submission.
    std::thread flusher([&] {
        for (;;) {
            if (WaitForSingleObject(req_flush_ev, 5000) != WAIT_OBJECT_0) {
                worker_failed.store(true, std::memory_order_release);
                break;
            }
            if (stop_worker.load(std::memory_order_acquire)) break;

            VoiceState st{};
            VCall<void, VoiceState*, uint32_t>(xma_voice, 25, &st, 0x0100u);
            if (st.context != nullptr) {
                bool ok = false;
                (void)ChurnCallback::DecodeId(st.context, &ok);
                if (!ok) worker_failed.store(true, std::memory_order_release);
            }
            if (VCall<int32_t>(xma_voice, 22) < 0 ||
                VCall<int32_t>(pcm_voice, 22) < 0 ||
                VCall<int32_t>(xma_nocb_voice, 22) < 0) {
                worker_failed.store(true, std::memory_order_release);
            }
            VCall<void, VoiceState*, uint32_t>(pcm_voice, 25, &st, 0x0100u);
            if (st.context != nullptr) {
                bool ok = false;
                (void)ChurnCallback::DecodeId(st.context, &ok);
                if (!ok) worker_failed.store(true, std::memory_order_release);
            }
            SetEvent(ack_flush_ev);
        }
    });

    constexpr uint32_t kRounds = 20u;
    constexpr uint32_t kBatchSize = 6u;
    uint32_t xma_submitted = 0u;
    uint32_t pcm_submitted = 0u;

    for (uint32_t round = 0; round < kRounds; ++round) {
        for (uint32_t j = 0; j < kBatchSize; ++j) {
            ++xma_submitted;
            Buffer xb{};
            xb.audio_bytes = xma_bytes;
            xb.audio_data = xma_pkt.data();
            xb.context = reinterpret_cast<void*>(ChurnCallback::kContextTag | xma_submitted);
            SX_CHECK(VCall<int32_t, const Buffer*, const void*>(xma_voice, 21, &xb, nullptr) == 0);
            SX_CHECK(VCall<int32_t, const Buffer*, const void*>(xma_nocb_voice, 21, &xb, nullptr) == 0);

            ++pcm_submitted;
            Buffer pb{};
            pb.audio_bytes = static_cast<uint32_t>(pcm_pkt.size() * sizeof(int16_t));
            pb.audio_data = reinterpret_cast<const uint8_t*>(pcm_pkt.data());
            pb.context = reinterpret_cast<void*>(ChurnCallback::kContextTag | pcm_submitted);
            SX_CHECK(VCall<int32_t, const Buffer*, const void*>(pcm_voice, 21, &pb, nullptr) == 0);
        }

        if (round == 0u) {
            // Ensure realtime mixing has actively started reading the front buffer before flushing!
            SX_CHECK(WaitForEvent(xma_cb.wake, [&] { return xma_cb.buffer_starts.load() >= 1u; }));
            SX_CHECK(WaitForEvent(pcm_cb.wake, [&] { return pcm_cb.buffer_starts.load() >= 1u; }));
        }

        SetEvent(req_flush_ev);
        // Submit one more buffer concurrently with the in-flight FlushSourceBuffers call!
        {
            ++xma_submitted;
            Buffer xb{};
            xb.audio_bytes = xma_bytes;
            xb.audio_data = xma_pkt.data();
            xb.context = reinterpret_cast<void*>(ChurnCallback::kContextTag | xma_submitted);
            SX_CHECK(VCall<int32_t, const Buffer*, const void*>(xma_voice, 21, &xb, nullptr) == 0);

            ++pcm_submitted;
            Buffer pb{};
            pb.audio_bytes = static_cast<uint32_t>(pcm_pkt.size() * sizeof(int16_t));
            pb.audio_data = reinterpret_cast<const uint8_t*>(pcm_pkt.data());
            pb.context = reinterpret_cast<void*>(ChurnCallback::kContextTag | pcm_submitted);
            SX_CHECK(VCall<int32_t, const Buffer*, const void*>(pcm_voice, 21, &pb, nullptr) == 0);
        }
        SX_CHECK(WaitForSingleObject(ack_flush_ev, 5000) == WAIT_OBJECT_0);
        // Wait until flushed buffers have drained down so queue never exceeds XAUDIO2_MAX_QUEUED_BUFFERS (64).
        SX_CHECK(WaitForEvent(xma_cb.wake, [&] { return xma_submitted - xma_cb.buffer_ends.load() <= 8u; }));
        SX_CHECK(WaitForEvent(pcm_cb.wake, [&] { return pcm_submitted - pcm_cb.buffer_ends.load() <= 8u; }));
    }

    stop_worker.store(true, std::memory_order_release);
    SetEvent(req_flush_ev);
    flusher.join();
    CloseHandle(req_flush_ev);
    CloseHandle(ack_flush_ev);
    SX_CHECK(!worker_failed.load(std::memory_order_acquire));

    // Stop and flush remaining active front buffers; every submitted buffer must fire OnBufferEnd exactly once.
    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(xma_voice, 20, 0u, 0u) == 0);
    SX_CHECK(VCall<int32_t>(xma_voice, 22) == 0);
    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(pcm_voice, 20, 0u, 0u) == 0);
    SX_CHECK(VCall<int32_t>(pcm_voice, 22) == 0);
    SX_CHECK(VCall<int32_t, uint32_t, uint32_t>(xma_nocb_voice, 20, 0u, 0u) == 0);
    SX_CHECK(VCall<int32_t>(xma_nocb_voice, 22) == 0);

    SX_CHECK(WaitForEvent(xma_cb.wake, [&] { return xma_cb.buffer_ends.load() == xma_submitted; }));
    SX_CHECK(WaitForEvent(pcm_cb.wake, [&] { return pcm_cb.buffer_ends.load() == pcm_submitted; }));
    SX_CHECK(!xma_cb.invalid_context.load(std::memory_order_acquire));
    SX_CHECK(!pcm_cb.invalid_context.load(std::memory_order_acquire));

    for (uint32_t id = 1u; id <= xma_submitted; ++id) {
        SX_CHECK(xma_cb.end_seen[id].load(std::memory_order_acquire) == 1u);
    }
    for (uint32_t id = 1u; id <= pcm_submitted; ++id) {
        SX_CHECK(pcm_cb.end_seen[id].load(std::memory_order_acquire) == 1u);
    }

    VoiceState final_state{};
    VCall<void, VoiceState*, uint32_t>(xma_voice, 25, &final_state, 0u);
    SX_CHECK(final_state.queued == 0u && final_state.context == nullptr);
    VCall<void, VoiceState*, uint32_t>(pcm_voice, 25, &final_state, 0u);
    SX_CHECK(final_state.queued == 0u && final_state.context == nullptr);

    VCall<void>(xma_nocb_voice, 18);
    VCall<void>(pcm_voice, 18);
    VCall<void>(xma_voice, 18);
    VCall<void>(mastering, 18);
    SX_CHECK(VCall<uint32_t>(engine, 2) == 0u);
    return 0;
}

// Dynamically loads AcpHal.dll from SCARLETTX_BINARY_DIR and stresses ApuCreateHeap(1 MB)
// with > 200 ApuAlloc/ApuFree iterations (> 12 MB cumulative) plus fragmentation + coalescing.
int RunAcpHalHeapChurn() {
    const auto dll_path = std::filesystem::path(SCARLETTX_BINARY_DIR) / "AcpHal.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    SX_CHECK(mod != nullptr);

    struct ModGuard {
        HMODULE m;
        ~ModGuard() {
            if (m) FreeLibrary(m);
        }
    } guard{mod};

    using FnCreateHeap = int32_t(SCARLETTX_ACP_CALL*)(uint32_t, uint32_t);
    using FnAlloc = int32_t(SCARLETTX_ACP_CALL*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
    using FnFree = int32_t(SCARLETTX_ACP_CALL*)(void*);
    using FnGetState = int32_t(SCARLETTX_ACP_CALL*)(APU_HEAP_STATE*, uint32_t);
    using FnIsValid = int32_t(SCARLETTX_ACP_CALL*)(const void*, uint32_t);
    using FnMapPa = void*(SCARLETTX_ACP_CALL*)(uint32_t);
    using FnMapVa = uint32_t(SCARLETTX_ACP_CALL*)(const void*);
    using FnAllocShape = int32_t(SCARLETTX_ACP_CALL*)(ACP_SHAPE_CONTEXT_COUNTS*);
    using FnReleaseShape = int32_t(SCARLETTX_ACP_CALL*)(void);

    const auto pCreateHeap =
        reinterpret_cast<FnCreateHeap>(reinterpret_cast<void*>(GetProcAddress(mod, "ApuCreateHeap")));
    const auto pAlloc =
        reinterpret_cast<FnAlloc>(reinterpret_cast<void*>(GetProcAddress(mod, "ApuAlloc")));
    const auto pFree =
        reinterpret_cast<FnFree>(reinterpret_cast<void*>(GetProcAddress(mod, "ApuFree")));
    const auto pGetState =
        reinterpret_cast<FnGetState>(reinterpret_cast<void*>(GetProcAddress(mod, "ApuHeapGetState")));
    const auto pIsValid =
        reinterpret_cast<FnIsValid>(reinterpret_cast<void*>(GetProcAddress(mod, "ApuIsVirtualAddressValid")));
    const auto pMapPa =
        reinterpret_cast<FnMapPa>(reinterpret_cast<void*>(GetProcAddress(mod, "ApuMapApuAddress")));
    const auto pMapVa =
        reinterpret_cast<FnMapVa>(reinterpret_cast<void*>(GetProcAddress(mod, "ApuMapVirtualAddress")));
    const auto pAllocShape =
        reinterpret_cast<FnAllocShape>(reinterpret_cast<void*>(GetProcAddress(mod, "AcpHalAllocateShapeContexts")));
    const auto pReleaseShape =
        reinterpret_cast<FnReleaseShape>(reinterpret_cast<void*>(GetProcAddress(mod, "AcpHalReleaseShapeContexts")));

    SX_CHECK(pCreateHeap && pAlloc && pFree && pGetState && pIsValid && pMapPa && pMapVa &&
             pAllocShape && pReleaseShape);

    constexpr uint32_t kOneMb = 1024u * 1024u;
    SX_CHECK(pCreateHeap(kOneMb, 0u) == 0);

    APU_HEAP_STATE hstate{};
    SX_CHECK(pGetState(&hstate, 0u) == 0);
    SX_CHECK(hstate.bytesAllocated == 0u);
    SX_CHECK(hstate.bytesFree == kOneMb);
    SX_CHECK(hstate.maximumBlockSizeAvailable == kOneMb);
    SX_CHECK(hstate.allocationCount == 0u);

    struct LiveSlot {
        void* va = nullptr;
        uint32_t pa = 0;
        uint32_t size = 0;
    };
    constexpr size_t kWindow = 4;
    std::array<LiveSlot, kWindow> live{};

    const uint32_t kSizes[] = {65536u, 49152u, 98304u, 73728u, 81920u, 57344u};
    const uint32_t kAligns[] = {64u, 96u, 65536u, 192u, 4096u, 1000u, 32768u, 300u};

    uint64_t cumulative_bytes = 0;
    constexpr uint32_t kIterations = 240u;
    for (uint32_t i = 0; i < kIterations; ++i) {
        LiveSlot& slot = live[i % kWindow];
        if (slot.va != nullptr) {
            void* old_va = slot.va;
            const uint32_t old_pa = slot.pa;
            SX_CHECK(pFree(old_va) == 0);
            // Freed virtual and physical addresses must immediately stop validating and mapping!
            SX_CHECK(pIsValid(old_va, 1u) == 0);
            SX_CHECK(pMapVa(old_va) == 0u);
            SX_CHECK(pMapPa(old_pa) == nullptr);
            // Double-free must fail cleanly with E_INVALIDARG.
            SX_CHECK(pFree(old_va) < 0);
            slot = LiveSlot{};
        }

        const uint32_t req_size = kSizes[i % (sizeof(kSizes) / sizeof(kSizes[0]))];
        const uint32_t req_align = kAligns[i % (sizeof(kAligns) / sizeof(kAligns[0]))];
        const uint32_t exp_align = std::bit_ceil(std::max<uint32_t>(64u, req_align));

        void* va = nullptr;
        uint32_t pa = 0u;
        SX_CHECK(pAlloc(&va, &pa, req_size, req_align, 0u) == 0);
        SX_CHECK(va != nullptr && pa != 0u);
        SX_CHECK((reinterpret_cast<uintptr_t>(va) % exp_align) == 0u);
        SX_CHECK((pa % exp_align) == 0u);
        // Every physical span must stay inside the 1 MB [0x10000000, 0x10100000) heap!
        SX_CHECK(pa >= 0x10000000u && (pa - 0x10000000u) + req_size <= kOneMb);

        std::memset(va, static_cast<int>(i & 0xFFu), req_size);
        SX_CHECK(pIsValid(va, req_size) == 1);
        SX_CHECK(pIsValid(va, req_size + 1u) == 0);
        SX_CHECK(pIsValid(static_cast<const uint8_t*>(va) + req_size, 1u) == 0);
        SX_CHECK(pMapVa(va) == pa);
        SX_CHECK(pMapVa(static_cast<const uint8_t*>(va) + (req_size / 2u)) == pa + (req_size / 2u));
        SX_CHECK(pMapPa(pa) == va);
        SX_CHECK(pMapPa(pa + (req_size / 2u)) == static_cast<uint8_t*>(va) + (req_size / 2u));

        slot = LiveSlot{va, pa, req_size};
        cumulative_bytes += req_size;
    }

    // Verify > 200 iterations allocated > 12 MB across the 1 MB heap.
    SX_CHECK(cumulative_bytes > 12ull * 1024ull * 1024ull);

    for (LiveSlot& slot : live) {
        if (slot.va != nullptr) {
            void* old_va = slot.va;
            const uint32_t old_pa = slot.pa;
            SX_CHECK(pFree(old_va) == 0);
            SX_CHECK(pIsValid(old_va, 1u) == 0);
            SX_CHECK(pMapVa(old_va) == 0u);
            SX_CHECK(pMapPa(old_pa) == nullptr);
            slot = LiveSlot{};
        }
    }

    SX_CHECK(pGetState(&hstate, 0u) == 0);
    SX_CHECK(hstate.bytesAllocated == 0u);
    SX_CHECK(hstate.bytesFree == kOneMb);
    SX_CHECK(hstate.maximumBlockSizeAvailable == kOneMb);
    SX_CHECK(hstate.allocationCount == 0u);

    // Fragmentation + adjacent span coalescing proof:
    // Carve 4 x 256 KB blocks filling 100% of the 1 MB heap.
    constexpr uint32_t kQuarterMb = 256u * 1024u;
    void* b[4] = {};
    uint32_t b_pa[4] = {};
    for (int i = 0; i < 4; ++i) {
        SX_CHECK(pAlloc(&b[i], &b_pa[i], kQuarterMb, 4096u, 0u) == 0 && b[i] != nullptr);
    }
    void* extra = nullptr;
    SX_CHECK(pAlloc(&extra, nullptr, 64u, 64u, 0u) < 0 && extra == nullptr);

    // Free non-adjacent b[1] and b[3]: 512 KB free total, but max contiguous block is 256 KB!
    SX_CHECK(pFree(b[1]) == 0);
    SX_CHECK(pFree(b[3]) == 0);
    SX_CHECK(pGetState(&hstate, 0u) == 0);
    SX_CHECK(hstate.bytesFree == 2u * kQuarterMb);
    SX_CHECK(hstate.maximumBlockSizeAvailable == kQuarterMb);
    void* too_big_frag = nullptr;
    SX_CHECK(pAlloc(&too_big_frag, nullptr, 384u * 1024u, 4096u, 0u) < 0);

    // Free b[2] between b[1] and b[3]: b[1]+b[2]+b[3] must coalesce into a single 768 KB free block!
    SX_CHECK(pFree(b[2]) == 0);
    SX_CHECK(pGetState(&hstate, 0u) == 0);
    SX_CHECK(hstate.bytesFree == 3u * kQuarterMb);
    SX_CHECK(hstate.maximumBlockSizeAvailable == 3u * kQuarterMb);

    void* coalesced_700k = nullptr;
    uint32_t coalesced_pa = 0u;
    SX_CHECK(pAlloc(&coalesced_700k, &coalesced_pa, 700u * 1024u, 65536u, 0u) == 0 && coalesced_700k != nullptr);
    SX_CHECK((coalesced_pa % 65536u) == 0u);
    SX_CHECK(pFree(coalesced_700k) == 0);
    SX_CHECK(pFree(b[0]) == 0);

    // Full 1 MB contiguous allocation must now succeed at the base of the fully coalesced heap!
    void* full_mb = nullptr;
    uint32_t full_pa = 0u;
    SX_CHECK(pAlloc(&full_mb, &full_pa, kOneMb, 65536u, 0u) == 0 && full_mb != nullptr);
    SX_CHECK(full_pa == 0x10000000u);
    SX_CHECK(pFree(full_mb) == 0);

    // Verify AcpHalAllocateShapeContexts + AcpHalReleaseShapeContexts reclaim physical spans cleanly.
    ACP_SHAPE_CONTEXT_COUNTS shape{};
    shape.numSrcContexts = 16;
    shape.numEqCompContexts = 8;
    shape.numFiltVolContexts = 8;
    shape.numDmaContexts = 16;
    shape.numXmaContexts = 8;
    shape.numPcmContexts = 8;
    SX_CHECK(pAllocShape(&shape) == 0 && shape.srcContexts != nullptr);
    void* saved_src = shape.srcContexts;
    const uint32_t saved_src_pa = pMapVa(saved_src);
    SX_CHECK(saved_src_pa != 0u && pIsValid(saved_src, 32u) == 1);
    SX_CHECK(pReleaseShape() == 0);
    SX_CHECK(pIsValid(saved_src, 1u) == 0);
    SX_CHECK(pMapVa(saved_src) == 0u);
    SX_CHECK(pMapPa(saved_src_pa) == nullptr);

    SX_CHECK(pGetState(&hstate, 0u) == 0);
    SX_CHECK(hstate.bytesAllocated == 0u && hstate.maximumBlockSizeAvailable == kOneMb);

    // Restore default 64 MB capacity before unloading.
    SX_CHECK(pCreateHeap(64u * 1024u * 1024u, 0u) == 0);
    return 0;
}

}  // namespace

int main() {
    if (RunSilentQueue() != 0) return 1;
    if (RunEngine(false) != 0) return 1;  // host output if there is a device, else the fallback
    if (RunEngine(true) != 0) return 1;   // silent output
    if (RunRealtimeFlushChurn(true) != 0) return 1;
    if (RunRealtimeFlushChurn(false) != 0) return 1;
    if (RunAcpHalHeapChurn() != 0) return 1;
    std::cout << "[test_xaudio2_guest] All guest-facing XAudio2 + AcpHal APU reclamation checks passed.\n";
    return 0;
}

#else

int main() { return 0; }

#endif
