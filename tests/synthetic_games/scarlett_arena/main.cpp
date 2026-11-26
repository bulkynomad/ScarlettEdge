#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define ARENA_EXPORT __declspec(dllexport)
#else
#  define ARENA_EXPORT
#endif

#include "arena_assets.h"
#include "scarlettx/audio/xaudio2_abi.h"
#include "scarlettx/gdk/xgameruntime_abi.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/input/gameinput_abi.h"
#include "scarlettx/io/dstorage_abi.h"

namespace {

struct DStorageSignalCtx {
    uint64_t last_value = 0;
    uint32_t hits = 0;
};

void OnDStorageSignal(uint64_t value, void* context) {
    auto* ctx = static_cast<DStorageSignalCtx*>(context);
    if (ctx) {
        ctx->last_value = value;
        ctx->hits++;
    }
}

void AppendMovingBeacon(std::vector<SCARLETT_BVH_TRIANGLE>& tris,
                        float px,
                        float py,
                        float pz,
                        bool pulsed) {
    auto push_tri = [&](float x0, float y0, float z0,
                        float x1, float y1, float z1,
                        float x2, float y2, float z2,
                        float r, float g, float b, float refl) {
        SCARLETT_BVH_TRIANGLE t{};
        t.v0[0] = x0; t.v0[1] = y0; t.v0[2] = z0;
        t.v1[0] = x1; t.v1[1] = y1; t.v1[2] = z1;
        t.v2[0] = x2; t.v2[1] = y2; t.v2[2] = z2;
        t.color[0] = r; t.color[1] = g; t.color[2] = b;
        t.reflectivity = refl;
        tris.push_back(t);
    };

    const float s = pulsed ? 0.52f : 0.40f;
    const float h = pulsed ? 1.45f : 1.15f;
    const float r = pulsed ? 0.15f : 0.95f;
    const float g = pulsed ? 0.95f : 0.72f;
    const float b = pulsed ? 0.45f : 0.18f;

    // 4-sided crystal beacon centered at (px, py, pz)
    push_tri(px - s, py, pz - s, px + s, py, pz - s, px, py + h, pz, r, g, b, 0.35f);
    push_tri(px + s, py, pz - s, px + s, py, pz + s, px, py + h, pz, 0.20f, 0.75f, 0.95f, 0.35f);
    push_tri(px + s, py, pz + s, px - s, py, pz + s, px, py + h, pz, 0.85f, 0.28f, 0.85f, 0.35f);
    push_tri(px - s, py, pz + s, px - s, py, pz - s, px, py + h, pz, 0.95f, 0.40f, 0.22f, 0.35f);
}

}  // namespace

extern "C" ARENA_EXPORT int ScarlettGuestMain() {
    // -----------------------------------------------------------------------
    // 1. GDK Runtime (xgameruntime.dll): Init, TaskQueue, XUser, XGameSave
    // -----------------------------------------------------------------------
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        std::fprintf(stderr, "[scarlett_arena] XGameRuntimeInitialize failed\n");
        return 1;
    }

    const uint32_t dev_type = XSystemGetDeviceType();
    if (dev_type == 0) {
        std::fprintf(stderr, "[scarlett_arena] Invalid XSystemGetDeviceType\n");
        return 2;
    }

    XTaskQueueHandle queue = nullptr;
    if (XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool,
                         XTaskQueueDispatchMode_Manual,
                         &queue) != XGR_S_OK) {
        std::fprintf(stderr, "[scarlett_arena] XTaskQueueCreate failed\n");
        return 3;
    }

    XAsyncBlock user_async{};
    user_async.queue = queue;
    if (XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_async) != XGR_S_OK) {
        std::fprintf(stderr, "[scarlett_arena] XUserAddAsync failed\n");
        return 4;
    }
    while (XAsyncGetStatus(&user_async, false) == XGR_E_PENDING) {
        XTaskQueueDispatch(queue, XTaskQueuePort_Completion, 50);
    }
    XUserHandle user = nullptr;
    if (XUserAddResult(&user_async, &user) != XGR_S_OK || !user) {
        std::fprintf(stderr, "[scarlett_arena] XUserAddResult failed\n");
        return 5;
    }

    XGameSaveProviderHandle save_provider = nullptr;
    if (XGameSaveInitializeProvider(user, "scarlett-arena-scid", false, &save_provider) != XGR_S_OK) {
        std::fprintf(stderr, "[scarlett_arena] XGameSaveInitializeProvider failed\n");
        return 6;
    }

    // -----------------------------------------------------------------------
    // 2. DirectStorage + XVD Asset Streaming (dstorage_xs.dll)
    // -----------------------------------------------------------------------
    const char* xvd_env = std::getenv("SCARLETTX_ARENA_XVD");
    const std::string xvd_path = (xvd_env && *xvd_env) ? xvd_env : "arena_assets.xvd";

    const char* uncomp_env = std::getenv("SCARLETTX_ARENA_BUNDLE_SIZE");
    const uint32_t expected_uncompressed =
        (uncomp_env && *uncomp_env) ? static_cast<uint32_t>(std::strtoul(uncomp_env, nullptr, 10)) : 0u;

    uint64_t ds_factory = 0;
    if (DStorageGetFactory(&ds_factory) != 0) {
        std::fprintf(stderr, "[scarlett_arena] DStorageGetFactory failed\n");
        return 10;
    }

    uint64_t xvd_file = 0;
    if (DStorageOpenXvdFile(ds_factory, xvd_path.c_str(), &xvd_file) != 0) {
        std::fprintf(stderr, "[scarlett_arena] DStorageOpenXvdFile failed: %s\n", xvd_path.c_str());
        return 11;
    }

    uint64_t xvd_user_size = 0;
    if (DStorageGetFileSize(xvd_file, &xvd_user_size) != 0 || xvd_user_size == 0) {
        std::fprintf(stderr, "[scarlett_arena] DStorageGetFileSize failed\n");
        return 12;
    }

    DSTORAGE_QUEUE_DESC ds_qdesc{};
    ds_qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    ds_qdesc.Capacity = 32;
    ds_qdesc.Priority = DSTORAGE_PRIORITY_HIGH;
    ds_qdesc.Name = "ArenaAssetQueue";

    uint64_t ds_queue = 0;
    if (DStorageCreateQueue(ds_factory, &ds_qdesc, &ds_queue) != 0) {
        std::fprintf(stderr, "[scarlett_arena] DStorageCreateQueue failed\n");
        return 13;
    }

    uint64_t status_arr = 0;
    if (DStorageCreateStatusArray(ds_factory, 4, "ArenaStatus", &status_arr) != 0) {
        std::fprintf(stderr, "[scarlett_arena] DStorageCreateStatusArray failed\n");
        return 14;
    }

    // Stream compressed bundle from XVD and decompress directly into memory.
    const uint32_t dest_cap = expected_uncompressed > 0 ? expected_uncompressed : 65536u;
    std::vector<uint8_t> bundle_bytes(dest_cap, 0);

    DSTORAGE_REQUEST req{};
    req.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_LZ_RLE;
    req.SourceFileHandle = xvd_file;
    req.SourceFileOffset = 0;
    req.SourceSize = static_cast<uint32_t>(xvd_user_size);
    req.DestinationMemory = bundle_bytes.data();
    req.DestinationSize = dest_cap;
    req.UncompressedSize = dest_cap;
    req.CancellationTag = 0xA4E4A001ull;
    req.Name = "StreamArenaAssets";

    uint64_t fence_val = 0;
    DStorageSignalCtx sig_ctx{};
    if (DStorageEnqueueRequest(ds_queue, &req) != 0 ||
        DStorageEnqueueStatus(ds_queue, status_arr, 0) != 0 ||
        DStorageEnqueueSignal(ds_queue, &fence_val, 42, OnDStorageSignal, &sig_ctx) != 0 ||
        DStorageSubmit(ds_queue) != 0 ||
        DStorageWaitForIdle(ds_queue, 5000) != 0) {
        std::fprintf(stderr, "[scarlett_arena] DirectStorage streaming failed\n");
        return 15;
    }

    if (!DStorageIsStatusComplete(status_arr, 0) ||
        DStorageGetStatusHResult(status_arr, 0) != 0 ||
        fence_val != 42 || sig_ctx.hits != 1) {
        std::fprintf(stderr, "[scarlett_arena] DirectStorage completion verification failed\n");
        return 16;
    }

    scarlett_arena::ArenaAssetHeader hdr{};
    std::memcpy(&hdr, bundle_bytes.data(), sizeof(hdr));
    if (hdr.magic != scarlett_arena::kArenaBundleMagic || hdr.triangle_count == 0 || hdr.xma2_bytes == 0) {
        std::fprintf(stderr, "[scarlett_arena] Invalid streamed arena bundle header\n");
        return 17;
    }

    const size_t tri_offset = sizeof(hdr);
    const size_t tri_bytes = hdr.triangle_count * sizeof(SCARLETT_BVH_TRIANGLE);
    const size_t xma_offset = tri_offset + tri_bytes;
    if (xma_offset + hdr.xma2_bytes > bundle_bytes.size()) {
        std::fprintf(stderr, "[scarlett_arena] Streamed arena bundle truncated\n");
        return 18;
    }

    std::vector<SCARLETT_BVH_TRIANGLE> static_tris(hdr.triangle_count);
    std::memcpy(static_tris.data(), bundle_bytes.data() + tri_offset, tri_bytes);

    std::vector<uint8_t> xma2_sfx(hdr.xma2_bytes);
    std::memcpy(xma2_sfx.data(), bundle_bytes.data() + xma_offset, hdr.xma2_bytes);

    // -----------------------------------------------------------------------
    // 3. D3D12_XS + XboxBvhBuilder (d3d12_xs.dll, XboxBvhBuilder.dll)
    // -----------------------------------------------------------------------
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.PreferWarp = 1;
    uint64_t d3d_dev = 0;
    if (D3D12XboxCreateSyntheticDevice(&dev_params, &d3d_dev) != 0) {
        std::fprintf(stderr, "[scarlett_arena] D3D12XboxCreateSyntheticDevice failed\n");
        return 20;
    }
    D3D12XboxSetFrameIntervalX(d3d_dev, 16667, 1);

    std::vector<SCARLETT_BVH_TRIANGLE> frame_tris = static_tris;
    AppendMovingBeacon(frame_tris, -0.8f, 0.25f, 0.1f, false);

    SCARLETT_BVH_BUILD_DESC bvh_desc{};
    bvh_desc.Triangles = frame_tris.data();
    bvh_desc.TriangleCount = static_cast<uint32_t>(frame_tris.size());

    uint64_t blas = 0;
    if (XboxBvhBuildBlas(&bvh_desc, &blas) != 0) {
        std::fprintf(stderr, "[scarlett_arena] XboxBvhBuildBlas failed\n");
        return 21;
    }
    uint64_t tlas = 0;
    if (XboxBvhBuildTlas(&blas, 1, &tlas) != 0) {
        std::fprintf(stderr, "[scarlett_arena] XboxBvhBuildTlas failed\n");
        return 22;
    }

    // -----------------------------------------------------------------------
    // 4. XAudio2 + XMA2 Audio Engine (xaudio2_9.dll)
    // -----------------------------------------------------------------------
    uint64_t audio_engine = 0;
    uint64_t master_voice = 0;
    uint64_t ambient_voice = 0;
    uint64_t sfx_voice = 0;
    if (XAudio2CreateEngine(0, &audio_engine) != 0 ||
        XAudio2CreateMasteringVoice(audio_engine, 2, 48000, &master_voice) != 0 ||
        XAudio2StartEngine(audio_engine) != 0) {
        std::fprintf(stderr, "[scarlett_arena] XAudio2 init failed\n");
        return 30;
    }

    XAUDIO2_WAVE_FORMAT ambient_fmt{};
    ambient_fmt.FormatTag = SCARLETTX_WAVE_FORMAT_IEEE_FLOAT;
    ambient_fmt.Channels = 1;
    ambient_fmt.SampleRate = 48000;
    ambient_fmt.BitsPerSample = 32;
    ambient_fmt.BlockAlign = 4;

    XAUDIO2_WAVE_FORMAT xma_fmt{};
    xma_fmt.FormatTag = SCARLETTX_WAVE_FORMAT_XMA2;
    xma_fmt.Channels = 1;
    xma_fmt.SampleRate = 48000;
    xma_fmt.BitsPerSample = 16;
    xma_fmt.BlockAlign = 2;

    if (XAudio2CreateSourceVoice(audio_engine, &ambient_fmt, 0, 2.0f, &ambient_voice) != 0 ||
        XAudio2CreateSourceVoice(audio_engine, &xma_fmt, 0, 2.0f, &sfx_voice) != 0) {
        std::fprintf(stderr, "[scarlett_arena] XAudio2CreateSourceVoice failed\n");
        return 31;
    }

    constexpr uint32_t kAudioFramesPerTick = 800;  // 16.67ms @ 48 kHz
    constexpr uint32_t kTotalFrames = 4;
    std::vector<float> ambient_pcm(kAudioFramesPerTick * kTotalFrames);
    for (size_t i = 0; i < ambient_pcm.size(); ++i) {
        ambient_pcm[i] = 0.22f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / 48000.0f);
    }
    XAUDIO2_BUFFER amb_buf{};
    amb_buf.AudioBytes = static_cast<uint32_t>(ambient_pcm.size() * sizeof(float));
    amb_buf.AudioData = reinterpret_cast<const uint8_t*>(ambient_pcm.data());
    XAudio2VoiceSetVolume(ambient_voice, 0.7f);
    XAudio2VoiceSubmitSourceBuffer(ambient_voice, &amb_buf);
    XAudio2VoiceStart(ambient_voice, 0);
    XAudio2VoiceSetVolume(sfx_voice, 0.9f);
    XAudio2VoiceStart(sfx_voice, 0);

    // -----------------------------------------------------------------------
    // 5. GameInput (GameInput.dll)
    // -----------------------------------------------------------------------
    uint64_t game_input = 0;
    if (GameInputCreate(&game_input) != 0) {
        std::fprintf(stderr, "[scarlett_arena] GameInputCreate failed\n");
        return 40;
    }
    uint64_t dev_ids[4] = {};
    uint32_t dev_count = 0;
    if (GameInputEnumerateDevices(game_input, GAMEINPUT_KIND_GAMEPAD, dev_ids, 4, &dev_count) != 0 ||
        dev_count == 0) {
        std::fprintf(stderr, "[scarlett_arena] No virtual GameInput gamepad found\n");
        return 41;
    }
    const uint64_t pad_id = dev_ids[0];

    // -----------------------------------------------------------------------
    // 6. Multi-frame integrated game loop
    // -----------------------------------------------------------------------
    const char* rt_dump_first = std::getenv("SCARLETTX_RT_DUMP");
    const char* rt_dump_last = std::getenv("SCARLETTX_ARENA_LAST_FRAME");
    const char* wav_dump = std::getenv("SCARLETTX_ARENA_WAV");

    float player_x = -0.8f;
    float player_z = 0.1f;
    uint32_t pulse_count = 0;
    uint64_t first_hash = 0;
    uint64_t last_hash = 0;
    std::vector<float> mixed_stereo;
    mixed_stereo.reserve(kTotalFrames * kAudioFramesPerTick * 2);

    const GAMEINPUT_GAMEPAD_STATE script[kTotalFrames] = {
        {GAMEINPUT_GAMEPAD_NONE, 0.0f, 0.0f, 0.25f, 0.10f, 0.0f, 0.0f},
        {GAMEINPUT_GAMEPAD_NONE, 0.0f, 0.2f, 0.65f, 0.15f, 0.1f, 0.0f},
        {GAMEINPUT_GAMEPAD_A,    0.0f, 0.8f, 0.85f, -0.10f, 0.2f, 0.0f},
        {GAMEINPUT_GAMEPAD_NONE, 0.0f, 0.1f, 0.50f, -0.20f, 0.1f, 0.0f},
    };

    for (uint32_t f = 0; f < kTotalFrames; ++f) {
        if (GameInputInjectGamepadState(game_input, pad_id, &script[f]) != 0) {
            return 42;
        }
        uint64_t reading = 0;
        GAMEINPUT_GAMEPAD_STATE state{};
        if (GameInputGetCurrentReading(game_input, GAMEINPUT_KIND_GAMEPAD, pad_id, &reading) != 0 ||
            GameInputGetGamepadState(game_input, reading, &state) != 0) {
            return 43;
        }

        player_x += state.LeftThumbstickX * 0.55f;
        player_z += state.LeftThumbstickY * 0.35f;
        const bool pulsed = (state.Buttons & GAMEINPUT_GAMEPAD_A) != 0;
        if (pulsed) {
            pulse_count++;
            GAMEINPUT_RUMBLE_PARAMS rumble{0.6f, 0.8f, 0.2f, state.RightTrigger};
            GameInputSetRumbleState(game_input, pad_id, &rumble);

            // Pan XMA2 SFX toward player_x and submit streamed XMA2 packet.
            const float pan_r = std::fmin(1.0f, std::fmax(0.0f, 0.5f + player_x * 0.35f));
            const float pan_l = 1.0f - pan_r;
            const float matrix[2] = {pan_l, pan_r};
            XAudio2VoiceSetOutputMatrix(sfx_voice, 1, 2, matrix);

            XAUDIO2_BUFFER sfx_buf{};
            sfx_buf.AudioBytes = static_cast<uint32_t>(xma2_sfx.size());
            sfx_buf.AudioData = xma2_sfx.data();
            XAudio2VoiceSubmitSourceBuffer(sfx_voice, &sfx_buf);
        }

        // Update dynamic BLAS + rebuild TLAS for the moved/pulsed beacon.
        frame_tris = static_tris;
        AppendMovingBeacon(frame_tris, player_x, 0.25f, player_z, pulsed);
        bvh_desc.Triangles = frame_tris.data();
        bvh_desc.TriangleCount = static_cast<uint32_t>(frame_tris.size());
        if (XboxBvhUpdateBlas(blas, &bvh_desc) != 0) {
            return 23;
        }
        XboxBvhDestroy(tlas);
        tlas = 0;
        if (XboxBvhBuildTlas(&blas, 1, &tlas) != 0) {
            return 24;
        }

        uint64_t token = 0;
        D3D12XboxWaitFrameEventX(d3d_dev, &token);

        SCARLETT_RT_DISPATCH_DESC rt_desc{};
        rt_desc.Width = 160;
        rt_desc.Height = 120;
        rt_desc.CameraPos[0] = player_x * 0.25f;
        rt_desc.CameraPos[1] = 1.15f;
        rt_desc.CameraPos[2] = -3.35f;
        rt_desc.CameraLookAt[0] = player_x * 0.5f;
        rt_desc.CameraLookAt[1] = 0.65f;
        rt_desc.CameraLookAt[2] = player_z;
        rt_desc.LightPos[0] = 1.4f;
        rt_desc.LightPos[1] = 2.8f;
        rt_desc.LightPos[2] = -1.2f;
        rt_desc.MaxBounces = 2;

        const char* dump = nullptr;
        if (f == 0 && rt_dump_first && *rt_dump_first) {
            dump = rt_dump_first;
        } else if (f == kTotalFrames - 1 && rt_dump_last && *rt_dump_last) {
            dump = rt_dump_last;
        }

        uint64_t frame_hash = 0;
        if (XboxBvhDispatchRays(tlas, &rt_desc, dump, &frame_hash) != 0) {
            return 25;
        }
        if (f == 0) {
            first_hash = frame_hash;
        }
        last_hash = frame_hash;

        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = token;
        plane.Width = rt_desc.Width;
        plane.Height = rt_desc.Height;
        D3D12XboxPresentX(d3d_dev, &plane);

        // Mix one frame's worth of stereo audio.
        std::vector<float> tick_pcm(kAudioFramesPerTick * 2, 0.0f);
        if (XAudio2RenderAudioFrames(audio_engine,
                                     kAudioFramesPerTick,
                                     tick_pcm.data(),
                                     static_cast<uint32_t>(tick_pcm.size())) != 0) {
            return 32;
        }
        mixed_stereo.insert(mixed_stereo.end(), tick_pcm.begin(), tick_pcm.end());

        std::fprintf(stdout,
                     "[scarlett_arena] Frame %u pos=(%.2f,%.2f) pulse=%d hash=0x%llx\n",
                     f,
                     player_x,
                     player_z,
                     pulsed ? 1 : 0,
                     static_cast<unsigned long long>(frame_hash));
    }

    if (first_hash == last_hash) {
        std::fprintf(stderr, "[scarlett_arena] Expected RT frame hash to change as player moved\n");
        return 26;
    }

    if (wav_dump && *wav_dump) {
        if (XAudio2WriteWavFile(wav_dump,
                                48000,
                                2,
                                mixed_stereo.data(),
                                kTotalFrames * kAudioFramesPerTick) != 0) {
            std::fprintf(stderr, "[scarlett_arena] XAudio2WriteWavFile failed\n");
            return 33;
        }
    }

    // -----------------------------------------------------------------------
    // 7. XGameSave persistence checkpoint (xgameruntime.dll)
    // -----------------------------------------------------------------------
    XGameSaveContainerHandle container = nullptr;
    if (XGameSaveCreateContainer(save_provider, "arena_checkpoint", &container) != XGR_S_OK) {
        return 50;
    }
    XGameSaveUpdateHandle update = nullptr;
    if (XGameSaveCreateUpdate(container, "Arena Checkpoint", &update) != XGR_S_OK) {
        return 51;
    }

    scarlett_arena::ArenaSaveState save{};
    save.magic = scarlett_arena::kArenaSaveMagic;
    save.frames_rendered = kTotalFrames;
    save.pulse_count = pulse_count;
    save.player_x = player_x;
    save.player_z = player_z;
    save.first_frame_hash = first_hash;
    save.last_frame_hash = last_hash;
    save.audio_frames_mixed = kTotalFrames * kAudioFramesPerTick;

    if (XGameSaveSubmitBlobWrite(update,
                                 "state.bin",
                                 reinterpret_cast<const uint8_t*>(&save),
                                 sizeof(save)) != XGR_S_OK ||
        XGameSaveSubmitUpdate(update) != XGR_S_OK) {
        return 52;
    }
    XGameSaveCloseUpdate(update);

    // Read back from XGameSave to verify round-trip persistence.
    const char* blob_names[1] = {"state.bin"};
    uint32_t blob_count = 1;
    std::vector<uint8_t> read_buf(sizeof(XGameSaveBlob) + sizeof(save) + 128, 0);
    auto* blobs = reinterpret_cast<XGameSaveBlob*>(read_buf.data());
    if (XGameSaveReadBlobData(container, blob_names, &blob_count, read_buf.size(), blobs) != XGR_S_OK ||
        blob_count != 1 || blobs[0].info.size != sizeof(save)) {
        std::fprintf(stderr, "[scarlett_arena] XGameSaveReadBlobData round-trip failed\n");
        return 53;
    }
    scarlett_arena::ArenaSaveState loaded{};
    std::memcpy(&loaded, blobs[0].data, sizeof(loaded));
    if (loaded.magic != scarlett_arena::kArenaSaveMagic ||
        loaded.frames_rendered != kTotalFrames ||
        loaded.pulse_count != 1 ||
        loaded.last_frame_hash != last_hash) {
        std::fprintf(stderr, "[scarlett_arena] XGameSave readback data mismatch\n");
        return 54;
    }

    // -----------------------------------------------------------------------
    // 8. Clean shutdown of all 6 HLE subsystems
    // -----------------------------------------------------------------------
    XGameSaveCloseContainer(container);
    XGameSaveCloseProvider(save_provider);
    GameInputDestroy(game_input);
    XAudio2DestroyVoice(sfx_voice);
    XAudio2DestroyVoice(ambient_voice);
    XAudio2DestroyVoice(master_voice);
    XAudio2DestroyEngine(audio_engine);
    XboxBvhDestroy(tlas);
    XboxBvhDestroy(blas);
    D3D12XboxDestroyDevice(d3d_dev);
    DStorageDestroyStatusArray(status_arr);
    DStorageDestroyQueue(ds_queue);
    DStorageCloseFile(xvd_file);
    DStorageDestroyFactory(ds_factory);
    XUserCloseHandle(user);
    XTaskQueueTerminate(queue, true, nullptr, nullptr);
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    std::fprintf(stdout, "[scarlett_arena] Full-stack Scarlett demo completed successfully!\n");
    std::fflush(stdout);
    return 0;
}

int main() {
    return ScarlettGuestMain();
}
