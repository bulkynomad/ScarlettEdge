// Drives the guest-facing XAudio2 objects the way a title does: through raw
// vtable slots, with callback objects implemented here.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

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

}  // namespace

int main() {
    if (RunSilentQueue() != 0) return 1;
    if (RunEngine(false) != 0) return 1;  // host output if there is a device, else the fallback
    if (RunEngine(true) != 0) return 1;   // silent output
    std::cout << "[test_xaudio2_guest] All guest-facing XAudio2 checks passed.\n";
    return 0;
}

#else

int main() { return 0; }

#endif
