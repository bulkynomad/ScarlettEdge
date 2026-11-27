#pragma once

#include <cstdint>

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_BINK2)
#define SCARLETTX_BINK2_API __declspec(dllexport)
#else
#define SCARLETTX_BINK2_API __declspec(dllimport)
#endif
#else
#define SCARLETTX_BINK2_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BINK {
    uint32_t Width;
    uint32_t Height;
    uint32_t Frames;
    uint32_t FrameNum;       // 1-based current frame
    uint32_t LastFrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t ReadError;
    uint32_t OpenFlags;
    uint32_t Paused;
    uint32_t SoundOn;
    int32_t Volume;
    uint64_t InternalId;
} BINK;

typedef struct BINKSUMMARY {
    uint32_t Width;
    uint32_t Height;
    uint32_t TotalTime;
    uint32_t FileFrameRate;
    uint32_t FileFrameRateDiv;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t TotalOpenTime;
    uint32_t TotalFrames;
    uint32_t TotalPlayedFrames;
    uint32_t SkippedFrames;
} BINKSUMMARY;

// Surface pixel format & PTS lip-sync flags matching Bink2 conventions
enum {
    BINKSURFACE32RGBA = 6,
    BINKSURFACE32BGRA = 3,
    BINKSURFACE24 = 1,
    BINKOPEN_PTS_SYNC = 0x00010000u,
    BINKCOPYALL = 0x80000000u
};

typedef struct BINKREALTIME {
    uint32_t FrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t Frames;
    uint32_t FramesTime;
    uint32_t FramesVideoDecompTime;
    uint32_t FramesAudioDecompTime;
    uint32_t ReadBufferSize;
    uint32_t ReadBufferUsed;
    uint32_t FramesDataRate;
} BINKREALTIME;

SCARLETTX_BINK2_API BINK* BinkOpen(const char* name, uint32_t flags);
SCARLETTX_BINK2_API void BinkClose(BINK* bnk);
SCARLETTX_BINK2_API int32_t BinkDoFrame(BINK* bnk);
SCARLETTX_BINK2_API void BinkNextFrame(BINK* bnk);
SCARLETTX_BINK2_API int32_t BinkWait(BINK* bnk);
SCARLETTX_BINK2_API int32_t BinkCopyToBuffer(
    BINK* bnk,
    void* dest,
    int32_t dest_pitch,
    uint32_t dest_height,
    uint32_t dest_x,
    uint32_t dest_y,
    uint32_t flags);
SCARLETTX_BINK2_API int32_t BinkPause(BINK* bnk, int32_t pause);
SCARLETTX_BINK2_API void BinkGoto(BINK* bnk, uint32_t frame, int32_t flags);
SCARLETTX_BINK2_API void BinkSetSoundOnOff(BINK* bnk, int32_t onoff);
SCARLETTX_BINK2_API void BinkSetVolume(BINK* bnk, uint32_t track_id, int32_t volume);
SCARLETTX_BINK2_API void BinkGetSummary(BINK* bnk, BINKSUMMARY* sum);
SCARLETTX_BINK2_API const char* BinkGetError(void);
SCARLETTX_BINK2_API void BinkSetSoundTrack(BINK* bnk, uint32_t total_tracks);
SCARLETTX_BINK2_API void BinkSetVideoOnOff(BINK* bnk, int32_t onoff);
SCARLETTX_BINK2_API void BinkSetFrameRate(BINK* bnk, uint32_t rate, uint32_t scale);
SCARLETTX_BINK2_API void BinkGetFrameRate(BINK* bnk, uint32_t* rate, uint32_t* scale);
SCARLETTX_BINK2_API int32_t BinkGetRects(BINK* bnk, uint32_t* rect_count, void* rects);
SCARLETTX_BINK2_API void BinkGetRealtime(BINK* bnk, void* realtime, uint32_t flags);
SCARLETTX_BINK2_API int32_t BinkGetTrackType(BINK* bnk, uint32_t track_index, uint32_t* track_type);
SCARLETTX_BINK2_API int32_t BinkService(BINK* bnk);
SCARLETTX_BINK2_API void BinkSetMixBins(BINK* bnk, uint32_t track_id, const uint32_t* mix_bins, uint32_t count);
SCARLETTX_BINK2_API void BinkSetMixBinVolumes(BINK* bnk, uint32_t track_id, const float* volumes, uint32_t count);
SCARLETTX_BINK2_API void BinkSetMemory(void* alloc_func, void* free_func);
SCARLETTX_BINK2_API void* BinkOpenMiles(void* miles_driver);
SCARLETTX_BINK2_API void BinkSetAudioPts(BINK* bnk, uint64_t audio_pts_us);
SCARLETTX_BINK2_API int32_t BinkGetPtsState(
    BINK* bnk,
    uint64_t* out_video_pts_us,
    uint64_t* out_audio_pts_us,
    int64_t* out_av_drift_us);

#ifdef __cplusplus
}
#endif
