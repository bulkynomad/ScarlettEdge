#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_CODECS)
#    define SCARLETTX_CODEC_API __declspec(dllexport)
#  else
#    define SCARLETTX_CODEC_API __declspec(dllimport)
#  endif
#  define SCARLETTX_CODEC_CALL __stdcall
#else
#  define SCARLETTX_CODEC_API __attribute__((visibility("default")))
#  define SCARLETTX_CODEC_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* XMA2_DECODER_HANDLE;
typedef void* BINK_VIDEO_HANDLE;

typedef struct XMA2_FORMAT {
    uint32_t sampleRate;
    uint32_t channelCount;
    uint32_t bitsPerSample;
    uint32_t blockCount;
} XMA2_FORMAT;

typedef struct BINK_VIDEO_INFO {
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    uint32_t fpsNumerator;
    uint32_t fpsDenominator;
    uint32_t hasAudio;
} BINK_VIDEO_INFO;

typedef struct CODEC_VERSION_INFO {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    char description[64];
} CODEC_VERSION_INFO;

// Hardware GDeflate & BCPack decompression
SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateDecompress(
    const void* srcCompressed, size_t compressedSize, void* dstDecompressed, size_t decompressedSize, size_t* bytesWritten);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompressRetail(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress32Lane(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress32LaneDynamic(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress32LaneStored(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecBcPackDecompress(
    const void* srcCompressed, size_t compressedSize, void* dstBlocks, size_t decompressedSize, uint32_t format);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecBcPackCompress(
    const void* srcBlocks, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten);

// Native SHAPE XMA2 audio decoding
SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecXma2CreateDecoder(
    const XMA2_FORMAT* format, XMA2_DECODER_HANDLE* outHandle);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecXma2DestroyDecoder(
    XMA2_DECODER_HANDLE handle);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecXma2SubmitData(
    XMA2_DECODER_HANDLE handle, const void* xmaData, uint32_t byteCount);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecXma2GetPcmSamples(
    XMA2_DECODER_HANDLE handle, void* outPcmBuffer, uint32_t maxSamples, uint32_t* samplesProduced);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecXma2Reset(
    XMA2_DECODER_HANDLE handle);

// Production Bink2 video & audio decode
SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoOpen(
    const void* fileMemory, size_t sizeBytes, uint32_t flags, BINK_VIDEO_HANDLE* outHandle);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoClose(
    BINK_VIDEO_HANDLE handle);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoDecodeFrame(
    BINK_VIDEO_HANDLE handle, void* outFrameBuffer, uint32_t pitch, uint32_t format);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoGetInfo(
    BINK_VIDEO_HANDLE handle, BINK_VIDEO_INFO* outInfo);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecBinkAudioExtractPcm(
    BINK_VIDEO_HANDLE handle, void* outPcm, uint32_t maxBytes, uint32_t* bytesProduced);

// Hardware acceleration capability & version
SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecHardwareAccelSupported(
    uint32_t codecId, int32_t* outSupported);

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecGetVersion(
    CODEC_VERSION_INFO* outVersion);

// Optional Windows Media Foundation (H.264/HEVC) cutscene decode probe
typedef struct MF_MEDIA_CAPABILITIES {
    int32_t mfPlatAvailable;
    int32_t mfReadWriteAvailable;
    int32_t h264Supported;
    int32_t hevcSupported;
    char backendName[64];
} MF_MEDIA_CAPABILITIES;

SCARLETTX_CODEC_API int32_t SCARLETTX_CODEC_CALL XboxCodecProbeMediaFoundation(
    MF_MEDIA_CAPABILITIES* outCaps);

#ifdef __cplusplus
}
#endif
