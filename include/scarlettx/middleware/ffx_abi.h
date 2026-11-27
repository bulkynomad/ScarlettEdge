#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_FFX_LOADER)
#define SCARLETTX_FFX_LOADER_API __declspec(dllexport)
#else
#define SCARLETTX_FFX_LOADER_API __declspec(dllimport)
#endif
#if defined(SCARLETTX_BUILDING_FFX_UPSCALER)
#define SCARLETTX_FFX_UPSCALER_API __declspec(dllexport)
#else
#define SCARLETTX_FFX_UPSCALER_API __declspec(dllimport)
#endif
#else
#define SCARLETTX_FFX_LOADER_API __attribute__((visibility("default")))
#define SCARLETTX_FFX_UPSCALER_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ffxReturnCode_t;
typedef void* ffxContext;

enum {
    FFX_API_RETURN_OK = 0,
    FFX_API_RETURN_ERROR = 1,
    FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,
    FFX_API_RETURN_ERROR_RUNTIME_ERROR = 3,
    FFX_API_RETURN_NO_PROVIDER = 4,
    FFX_API_RETURN_ERROR_MEMORY = 5,
    FFX_API_RETURN_ERROR_PARAMETER = 6
};

enum {
    FFX_API_DESC_TYPE_UPSCALE_CREATE = 0x00010000u,
    FFX_API_DESC_TYPE_UPSCALE_DISPATCH = 0x00010001u,
    FFX_API_DESC_TYPE_UPSCALE_GET_UPSCALERATIO = 0x00010002u,
    FFX_API_DESC_TYPE_UPSCALE_GET_RENDERRESOLUTION = 0x00010003u,
    FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT = 0x00010004u,
    FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET = 0x00010005u,
    FFX_API_DESC_TYPE_UPSCALE_CONFIGURE = 0x00010006u,
    FFX_API_DESC_TYPE_GET_PROVIDER_VERSION = 0x00000001u,

    // Official AMD FidelityFX SDK ffx_api / ffx_upscale descriptor IDs
    FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1 = 0x00000001u,
    FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 = 0x00000002u,
    FFX_API_QUERY_DESC_TYPE_GET_VERSIONS = 0x00000004u,
    FFX_API_DISPATCH_DESC_TYPE_UPSCALE = 0x00010004u
};

enum {
    FFX_UPSCALE_QUALITY_MODE_NATIVEAA = 0,
    FFX_UPSCALE_QUALITY_MODE_QUALITY = 1,          // 1.5x
    FFX_UPSCALE_QUALITY_MODE_BALANCED = 2,         // 1.7x
    FFX_UPSCALE_QUALITY_MODE_PERFORMANCE = 3,      // 2.0x
    FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE = 4 // 3.0x
};

typedef struct ffxApiHeader {
    uint64_t type;
    struct ffxApiHeader* pNext;
} ffxApiHeader;

typedef struct ffxCreateContextDescUpscale {
    ffxApiHeader header;
    uint32_t flags;
    uint32_t maxRenderWidth;
    uint32_t maxRenderHeight;
    uint32_t maxUpscaleWidth;
    uint32_t maxUpscaleHeight;
} ffxCreateContextDescUpscale;

typedef struct ffxDispatchDescUpscale {
    ffxApiHeader header;
    const uint8_t* colorRgba8;   // renderWidth * renderHeight * 4 (or commandList in official AMD ffx_api)
    uint8_t* outputRgba8;        // upscaleWidth * upscaleHeight * 4 (or color.resource in official AMD ffx_api)
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t upscaleWidth;
    uint32_t upscaleHeight;
    float jitterOffsetX;
    float jitterOffsetY;
    float sharpness;
    uint32_t reset;
    uint64_t* outFrameHash;
} ffxDispatchDescUpscale;

typedef struct ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode {
    ffxApiHeader header;
    uint32_t qualityMode;
    float* pOutUpscaleRatio;
} ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode;

typedef struct ffxQueryDescUpscaleGetRenderResolutionFromQualityMode {
    ffxApiHeader header;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t qualityMode;
    uint32_t* pOutRenderWidth;
    uint32_t* pOutRenderHeight;
} ffxQueryDescUpscaleGetRenderResolutionFromQualityMode;

typedef struct ffxQueryDescUpscaleGetJitterPhaseCount {
    ffxApiHeader header;
    int32_t renderWidth;
    int32_t displayWidth;
    int32_t* pOutPhaseCount;
} ffxQueryDescUpscaleGetJitterPhaseCount;

typedef struct ffxQueryDescUpscaleGetJitterOffset {
    ffxApiHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
} ffxQueryDescUpscaleGetJitterOffset;

typedef struct ffxConfigureDescUpscale {
    ffxApiHeader header;
    float sharpness;
    uint32_t enableSharpening;
} ffxConfigureDescUpscale;

typedef struct ffxQueryDescGetProviderVersion {
    ffxApiHeader header;
    uint64_t versionId;
    const char* versionName;
} ffxQueryDescGetProviderVersion;

typedef struct ffxQueryDescGetVersions {
    ffxApiHeader header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
} ffxQueryDescGetVersions;

// amd_fidelityfx_upscaler_xbox.dll provider exports
SCARLETTX_FFX_UPSCALER_API ffxReturnCode_t ffxUpscalerCreateContext(
    ffxContext* context,
    ffxApiHeader* desc);
SCARLETTX_FFX_UPSCALER_API ffxReturnCode_t ffxUpscalerDestroyContext(
    ffxContext* context);
SCARLETTX_FFX_UPSCALER_API ffxReturnCode_t ffxUpscalerConfigure(
    ffxContext* context,
    const ffxApiHeader* desc);
SCARLETTX_FFX_UPSCALER_API ffxReturnCode_t ffxUpscalerQuery(
    ffxContext* context,
    ffxApiHeader* desc);
SCARLETTX_FFX_UPSCALER_API ffxReturnCode_t ffxUpscalerDispatch(
    ffxContext* context,
    const ffxApiHeader* desc);

// amd_fidelityfx_loader_xbox.dll public entry points
SCARLETTX_FFX_LOADER_API ffxReturnCode_t ffxCreateContext(
    ffxContext* context,
    ffxApiHeader* desc,
    const void* memCb);
SCARLETTX_FFX_LOADER_API ffxReturnCode_t ffxDestroyContext(
    ffxContext* context,
    const void* memCb);
SCARLETTX_FFX_LOADER_API ffxReturnCode_t ffxConfigure(
    ffxContext* context,
    const ffxApiHeader* desc);
SCARLETTX_FFX_LOADER_API ffxReturnCode_t ffxQuery(
    ffxContext* context,
    ffxApiHeader* desc);
SCARLETTX_FFX_LOADER_API ffxReturnCode_t ffxDispatch(
    ffxContext* context,
    const ffxApiHeader* desc);

#ifdef __cplusplus
}
#endif
