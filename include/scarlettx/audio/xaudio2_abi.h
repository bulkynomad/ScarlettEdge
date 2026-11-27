#pragma once

#include <cstdint>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_XAUDIO2)
#    define SCARLETTX_XAUDIO2_API __declspec(dllexport)
#  else
#    define SCARLETTX_XAUDIO2_API __declspec(dllimport)
#  endif
#else
#  define SCARLETTX_XAUDIO2_API __attribute__((visibility("default")))
#endif

extern "C" {

static constexpr uint16_t SCARLETTX_WAVE_FORMAT_PCM = 0x0001;
static constexpr uint16_t SCARLETTX_WAVE_FORMAT_IEEE_FLOAT = 0x0003;
static constexpr uint16_t SCARLETTX_WAVE_FORMAT_XMA2 = 0x0166;

struct XAUDIO2_WAVE_FORMAT {
    uint16_t FormatTag;
    uint16_t Channels;
    uint32_t SampleRate;
    uint16_t BitsPerSample;
    uint16_t BlockAlign;
};

struct XAUDIO2_BUFFER {
    uint32_t Flags;
    uint32_t AudioBytes;
    const uint8_t* AudioData;
    uint32_t PlayBegin;
    uint32_t PlayLength;
    uint32_t LoopBegin;
    uint32_t LoopLength;
    uint32_t LoopCount;
    void* Context;
};

struct XAUDIO2_VOICE_STATE {
    void* CurrentBufferContext;
    uint32_t BuffersQueued;
    uint64_t SamplesPlayed;
};

struct XAUDIO2_PERFORMANCE_DATA {
    uint64_t AudioCyclesSinceLastQuery;
    uint64_t TotalCyclesSinceLastQuery;
    uint32_t ActiveSourceVoiceCount;
    uint32_t TotalSourceVoiceCount;
    uint32_t ActiveSubmixVoiceCount;
    uint32_t GlitchesSinceEngineStarted;
    uint64_t TotalFramesMixed;
};

// Synthetic XMA2 packet header used by ScarlettX's XMA2->PCM bridge.
struct XMA2_PACKET_HEADER {
    uint32_t Magic;         // 'XMA2' (0x32414D58)
    uint16_t Channels;
    uint16_t SampleRateDiv100;  // e.g. 480 for 48000 Hz
    uint32_t SampleCount;   // PCM frames per channel
    int16_t InitialSample;
    int16_t StepSize;
};

// Verified retail XAudio2_9.dll ordinal #1 export (COM IXAudio2 creation).
SCARLETTX_XAUDIO2_API int32_t XAudio2Create(
    void** ppXAudio2,
    uint32_t flags,
    uint32_t xaudio2_processor);

SCARLETTX_XAUDIO2_API int32_t XAudio2CreateEngine(uint32_t flags, uint64_t* out_engine);
SCARLETTX_XAUDIO2_API int32_t XAudio2DestroyEngine(uint64_t engine);
SCARLETTX_XAUDIO2_API int32_t XAudio2StartEngine(uint64_t engine);
SCARLETTX_XAUDIO2_API int32_t XAudio2StopEngine(uint64_t engine);
SCARLETTX_XAUDIO2_API int32_t XAudio2GetPerformanceData(
    uint64_t engine,
    XAUDIO2_PERFORMANCE_DATA* out_data);

SCARLETTX_XAUDIO2_API int32_t XAudio2CreateMasteringVoice(
    uint64_t engine,
    uint32_t input_channels,
    uint32_t input_sample_rate,
    uint64_t* out_voice);
SCARLETTX_XAUDIO2_API int32_t XAudio2CreateSubmixVoice(
    uint64_t engine,
    uint32_t input_channels,
    uint32_t input_sample_rate,
    uint32_t processing_stage,
    uint64_t* out_voice);
SCARLETTX_XAUDIO2_API int32_t XAudio2CreateSourceVoice(
    uint64_t engine,
    const XAUDIO2_WAVE_FORMAT* format,
    uint32_t flags,
    float max_frequency_ratio,
    uint64_t* out_voice);
SCARLETTX_XAUDIO2_API int32_t XAudio2DestroyVoice(uint64_t voice);

SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceStart(uint64_t voice, uint32_t flags);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceStop(uint64_t voice, uint32_t flags);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceSubmitSourceBuffer(
    uint64_t voice,
    const XAUDIO2_BUFFER* buffer);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceFlushSourceBuffers(uint64_t voice);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceGetState(
    uint64_t voice,
    XAUDIO2_VOICE_STATE* out_state);

SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceSetVolume(uint64_t voice, float volume);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceGetVolume(uint64_t voice, float* out_volume);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceSetFrequencyRatio(uint64_t voice, float ratio);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceGetFrequencyRatio(uint64_t voice, float* out_ratio);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceSetOutputMatrix(
    uint64_t voice,
    uint32_t src_channels,
    uint32_t dst_channels,
    const float* level_matrix);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceGetOutputMatrix(
    uint64_t voice,
    uint32_t src_channels,
    uint32_t dst_channels,
    float* level_matrix);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceSetChannelVolumes(
    uint64_t voice,
    uint32_t channels,
    const float* volumes);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceGetChannelVolumes(
    uint64_t voice,
    uint32_t channels,
    float* out_volumes);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceEnableEffect(
    uint64_t voice,
    uint32_t effect_index);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceDisableEffect(
    uint64_t voice,
    uint32_t effect_index);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceDiscontinuity(
    uint64_t voice);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceExitLoop(
    uint64_t voice);
SCARLETTX_XAUDIO2_API int32_t XAudio2CommitChanges(
    uint64_t engine,
    uint32_t operation_set);
SCARLETTX_XAUDIO2_API int32_t XAudio2VoiceSetVolumeRamp(
    uint64_t voice,
    float target_volume,
    uint32_t ramp_duration_ms);

// Headless deterministic software mixer & WAV export
SCARLETTX_XAUDIO2_API int32_t XAudio2RenderAudioFrames(
    uint64_t engine,
    uint32_t frame_count,
    float* out_interleaved_pcm,
    uint32_t out_sample_capacity);
SCARLETTX_XAUDIO2_API int32_t XAudio2WriteWavFile(
    const char* path,
    uint32_t sample_rate,
    uint16_t channels,
    const float* interleaved_pcm,
    uint32_t frame_count);

// XMA2 packet encode/decode bridge (supports both 2048-byte retail XMA2 MDCT blocks and XMA2 test packets)
SCARLETTX_XAUDIO2_API int32_t Xma2DecodePacket(
    const uint8_t* packet_data,
    uint32_t packet_bytes,
    float* out_interleaved_pcm,
    uint32_t out_sample_capacity,
    uint32_t* out_frames_decoded,
    uint16_t* out_channels);

SCARLETTX_XAUDIO2_API int32_t Xma2EncodeMdctBlock(
    const float* interleaved_pcm,
    uint32_t frame_count,
    uint16_t channels,
    uint8_t* out_xma2_bytes,
    uint32_t max_out_bytes,
    uint32_t* out_bytes_written);

// ---------------------------------------------------------------------------
// X3DAudio & XAPOFX (CreateFX: CLSID_FXReverb, CLSID_FXEQ, CLSID_FXMasteringLimiter)
// ---------------------------------------------------------------------------
typedef uint8_t X3DAUDIO_HANDLE[20];

struct X3DAUDIO_VECTOR {
    float x;
    float y;
    float z;
};

struct X3DAUDIO_LISTENER {
    X3DAUDIO_VECTOR OrientFront;
    X3DAUDIO_VECTOR OrientTop;
    X3DAUDIO_VECTOR Position;
    X3DAUDIO_VECTOR Velocity;
    void* pCone;
};

struct X3DAUDIO_EMITTER {
    void* pCone;
    X3DAUDIO_VECTOR OrientFront;
    X3DAUDIO_VECTOR OrientTop;
    X3DAUDIO_VECTOR Position;
    X3DAUDIO_VECTOR Velocity;
    float InnerRadius;
    float InnerRadiusAngle;
    uint32_t ChannelCount;
    float ChannelRadius;
    float* pChannelAzimuths;
    void* pVolumeCurve;
    void* pLFECurve;
    void* pLPFDirectCurve;
    void* pLPFReverbCurve;
    void* pReverbCurve;
    float CurveDistanceScaler;
    float DopplerScaler;
};

struct X3DAUDIO_DSP_SETTINGS {
    float* pMatrixCoefficients;
    float* pDelayTimes;
    uint32_t SrcChannelCount;
    uint32_t DstChannelCount;
    float LPFDirectCoefficient;
    float LPFReverbCoefficient;
    float ReverbLevel;
    float DopplerFactor;
    float EmitterToListenerAngle;
    float EmitterToListenerDistance;
    float EmitterVelocityComponent;
    float ListenerVelocityComponent;
};

static constexpr uint32_t SCARLETTX_X3DAUDIO_CALCULATE_MATRIX = 0x00000001u;
static constexpr uint32_t SCARLETTX_X3DAUDIO_CALCULATE_DOPPLER = 0x00000020u;

SCARLETTX_XAUDIO2_API int32_t X3DAudioInitialize(
    uint32_t speaker_channel_mask,
    float speed_of_sound,
    X3DAUDIO_HANDLE instance);

SCARLETTX_XAUDIO2_API void X3DAudioCalculate(
    const X3DAUDIO_HANDLE instance,
    const X3DAUDIO_LISTENER* listener,
    const X3DAUDIO_EMITTER* emitter,
    uint32_t flags,
    X3DAUDIO_DSP_SETTINGS* dsp_settings);

SCARLETTX_XAUDIO2_API int32_t CreateFX(
    const void* clsid,
    void** out_effect,
    const void* init_data,
    uint32_t init_data_bytes);

}  // extern "C"
