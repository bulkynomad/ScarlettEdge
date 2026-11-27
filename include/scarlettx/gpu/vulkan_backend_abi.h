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

typedef struct VULKAN_XS_SBT_REGION {
    uint64_t deviceAddress;
    uint64_t stride;
    uint64_t size;
} VULKAN_XS_SBT_REGION;

typedef struct VULKAN_XS_SBT_INFO {
    VULKAN_XS_SBT_REGION raygen;
    VULKAN_XS_SBT_REGION miss;
    VULKAN_XS_SBT_REGION hit;
    VULKAN_XS_SBT_REGION callable;
    uint32_t handleSize;
    uint32_t handleAlignment;
    uint32_t baseAlignment;
    uint32_t hardwareBacked;
} VULKAN_XS_SBT_INFO;

typedef struct VULKAN_XS_STATS {
    uint64_t framesPresented;
    uint64_t raysTraced;
    uint64_t asBuilds;
    uint32_t activePipelines;
} VULKAN_XS_STATS;

typedef struct VULKAN_XS_COMPUTE_DISPATCH_DESC {
    const void* spirvBytecode;
    size_t spirvSize;
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
    const float* uboVec4;         // optional 4 floats bound at Set 0, Binding 0
    void* inoutSsboData;          // host buffer uploaded before & read back after GPU execution (Set 0, Binding 2)
    size_t ssboByteSize;
    uint64_t* outTimelineValue;   // optional signaled Vulkan 1.2 Timeline Semaphore counter
} VULKAN_XS_COMPUTE_DISPATCH_DESC;

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

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsGetAccelerationStructureDeviceAddress(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_AS_HANDLE asHandle, uint64_t* outDeviceAddress);

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsBuildShaderBindingTable(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_RT_PIPELINE_HANDLE pipeline, VULKAN_XS_SBT_INFO* outSbt);

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

SCARLETTX_VULKAN_API int32_t SCARLETTX_VULKAN_CALL VulkanXsDispatchComputeSpirv(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_COMPUTE_DISPATCH_DESC* desc);

#ifdef __cplusplus
}
#endif
