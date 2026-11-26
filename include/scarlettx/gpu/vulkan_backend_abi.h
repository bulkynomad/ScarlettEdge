#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_VULKAN)
#    define SCARLETTX_VULKAN_API __declspec(dllexport)
#  else
#    define SCARLETTX_VULKAN_API __declspec(dllimport)
#  endif
#  define SCARLETTX_VULKAN_CALL __stdcall
#else
#  define SCARLETTX_VULKAN_API __attribute__((visibility("default")))
#  define SCARLETTX_VULKAN_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* VULKAN_XS_BACKEND_HANDLE;
typedef void* VULKAN_XS_RT_PIPELINE_HANDLE;
typedef void* VULKAN_XS_AS_HANDLE;
typedef void* VULKAN_XS_SWAPCHAIN_HANDLE;

typedef struct VULKAN_XS_INIT_DESC {
    uint32_t enableValidation;
    uint32_t enableRayTracing;
    uint32_t enableDescriptorIndexing;
    const char* preferredDeviceName;
} VULKAN_XS_INIT_DESC;

typedef struct VULKAN_XS_CAPABILITIES {
    uint32_t apiVersionMajor;
    uint32_t apiVersionMinor;
    uint32_t hardwareRaytracingSupported;
    uint32_t maxRayRecursionDepth;
    uint32_t maxRayDispatchInvocationCount;
    char deviceName[256];
} VULKAN_XS_CAPABILITIES;

typedef struct VULKAN_XS_RT_PIPELINE_DESC {
    uint32_t maxRecursionDepth;
    uint32_t shaderStageCount;
    const void* raygenSpirv;
    size_t raygenSize;
    const void* missSpirv;
    size_t missSize;
    const void* closestHitSpirv;
    size_t closestHitSize;
} VULKAN_XS_RT_PIPELINE_DESC;

typedef struct VULKAN_XS_AS_BUILD_DESC {
    uint32_t type;                // 0 = BottomLevel (BLAS), 1 = TopLevel (TLAS)
    uint32_t geometryCount;
    uint32_t triangleCount;
    uint64_t vertexBufferGpuVa;
    uint64_t indexBufferGpuVa;
} VULKAN_XS_AS_BUILD_DESC;

typedef struct VULKAN_XS_TRACE_RAYS_DESC {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    VULKAN_XS_RT_PIPELINE_HANDLE pipeline;
    VULKAN_XS_AS_HANDLE topLevelAs;
} VULKAN_XS_TRACE_RAYS_DESC;

typedef struct VULKAN_XS_STATS {
    uint64_t framesPresented;
    uint64_t raysTraced;
    uint64_t asBuilds;
    uint32_t activePipelines;
} VULKAN_XS_STATS;

// Backend lifecycle & capabilities
SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateBackend(
    const VULKAN_XS_INIT_DESC* desc, VULKAN_XS_BACKEND_HANDLE* outBackend);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyBackend(
    VULKAN_XS_BACKEND_HANDLE backend);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsQueryCapabilities(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_CAPABILITIES* outCaps);

// Hardware DXR Tier 1.1 Acceleration Structures & Pipelines
SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateRaytracingPipeline(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_RT_PIPELINE_DESC* desc, VULKAN_XS_RT_PIPELINE_HANDLE* outPipeline);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyRaytracingPipeline(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_RT_PIPELINE_HANDLE pipeline);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsBuildAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_AS_BUILD_DESC* desc, VULKAN_XS_AS_HANDLE* outAs);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsUpdateAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_AS_HANDLE asHandle, const VULKAN_XS_AS_BUILD_DESC* desc);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_AS_HANDLE asHandle);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsTraceRays(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_TRACE_RAYS_DESC* desc);

// Layout conversion & Presentation
SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsTranslateXboxRootSignatureToVulkanLayout(
    const void* xboxRootSig, size_t size, void** outDescriptorSetLayout);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateSwapchain(
    VULKAN_XS_BACKEND_HANDLE backend, void* nativeWindowHandle, uint32_t width, uint32_t height, VULKAN_XS_SWAPCHAIN_HANDLE* outSwapchain);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsPresentFrame(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_SWAPCHAIN_HANDLE swapchain, uint32_t syncInterval);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroySwapchain(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_SWAPCHAIN_HANDLE swapchain);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsGetBackendStatistics(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_STATS* outStats);

#ifdef __cplusplus
}
#endif
