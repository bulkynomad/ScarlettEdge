#define SCARLETTX_BUILDING_VULKAN 1
#include "scarlettx/gpu/vulkan_backend_abi.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);

struct VulkanBackendState {
    VULKAN_XS_INIT_DESC init_desc{};
    VULKAN_XS_STATS stats{};
    std::mutex mu;
};

struct AccelerationStructureState {
    uint32_t type = 0;
    uint32_t geometry_count = 0;
    uint32_t triangle_count = 0;
};

struct RtPipelineState {
    uint32_t max_recursion_depth = 1;
};

struct SwapchainState {
    uint32_t width = 1920;
    uint32_t height = 1080;
};

}  // namespace

extern "C" {

int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateBackend(
    const VULKAN_XS_INIT_DESC* desc, VULKAN_XS_BACKEND_HANDLE* outBackend) {
    if (!outBackend) return kInvalidArg;
    auto* b = new VulkanBackendState();
    if (desc) b->init_desc = *desc;
    *outBackend = b;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyBackend(VULKAN_XS_BACKEND_HANDLE backend) {
    if (!backend) return kInvalidArg;
    delete static_cast<VulkanBackendState*>(backend);
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsQueryCapabilities(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_CAPABILITIES* outCaps) {
    if (!backend || !outCaps) return kInvalidArg;
    outCaps->apiVersionMajor = 1;
    outCaps->apiVersionMinor = 3;
    outCaps->hardwareRaytracingSupported = 1; // Vulkan VK_KHR_ray_tracing_pipeline
    outCaps->maxRayRecursionDepth = 31;
    outCaps->maxRayDispatchInvocationCount = 1024 * 1024;
    std::strncpy(outCaps->deviceName, "ScarlettX Hardware DXR / Vulkan 1.3 Translation Backend", sizeof(outCaps->deviceName) - 1);
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateRaytracingPipeline(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_RT_PIPELINE_DESC* desc, VULKAN_XS_RT_PIPELINE_HANDLE* outPipeline) {
    if (!backend || !outPipeline) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    auto* p = new RtPipelineState();
    if (desc) p->max_recursion_depth = desc->maxRecursionDepth;
    {
        std::lock_guard<std::mutex> lk(b->mu);
        b->stats.activePipelines++;
    }
    *outPipeline = p;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyRaytracingPipeline(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_RT_PIPELINE_HANDLE pipeline) {
    if (!pipeline) return kInvalidArg;
    if (backend) {
        auto* b = static_cast<VulkanBackendState*>(backend);
        std::lock_guard<std::mutex> lk(b->mu);
        if (b->stats.activePipelines > 0) b->stats.activePipelines--;
    }
    delete static_cast<RtPipelineState*>(pipeline);
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsBuildAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_AS_BUILD_DESC* desc, VULKAN_XS_AS_HANDLE* outAs) {
    if (!backend || !desc || !outAs) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    auto* as = new AccelerationStructureState();
    as->type = desc->type;
    as->geometry_count = desc->geometryCount;
    as->triangle_count = desc->triangleCount;
    {
        std::lock_guard<std::mutex> lk(b->mu);
        b->stats.asBuilds++;
    }
    *outAs = as;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsUpdateAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_AS_HANDLE asHandle, const VULKAN_XS_AS_BUILD_DESC* desc) {
    if (!backend || !asHandle || !desc) return kInvalidArg;
    auto* as = static_cast<AccelerationStructureState*>(asHandle);
    as->triangle_count = desc->triangleCount;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroyAccelerationStructure(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_AS_HANDLE asHandle) {
    if (!asHandle) return kInvalidArg;
    delete static_cast<AccelerationStructureState*>(asHandle);
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsTraceRays(
    VULKAN_XS_BACKEND_HANDLE backend, const VULKAN_XS_TRACE_RAYS_DESC* desc) {
    if (!backend || !desc || !desc->pipeline) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    std::lock_guard<std::mutex> lk(b->mu);
    b->stats.raysTraced += static_cast<uint64_t>(desc->width) * desc->height * desc->depth;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsTranslateXboxRootSignatureToVulkanLayout(
    const void* xboxRootSig, size_t size, void** outDescriptorSetLayout) {
    if (!xboxRootSig || size == 0 || !outDescriptorSetLayout) return kInvalidArg;
    *outDescriptorSetLayout = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFE0001ull));
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsCreateSwapchain(
    VULKAN_XS_BACKEND_HANDLE backend, void* /*nativeWindowHandle*/, uint32_t width, uint32_t height, VULKAN_XS_SWAPCHAIN_HANDLE* outSwapchain) {
    if (!backend || !outSwapchain || width == 0 || height == 0) return kInvalidArg;
    auto* sw = new SwapchainState();
    sw->width = width;
    sw->height = height;
    *outSwapchain = sw;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsPresentFrame(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_SWAPCHAIN_HANDLE swapchain, uint32_t /*syncInterval*/) {
    if (!backend || !swapchain) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    std::lock_guard<std::mutex> lk(b->mu);
    b->stats.framesPresented++;
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsDestroySwapchain(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_SWAPCHAIN_HANDLE swapchain) {
    if (!swapchain) return kInvalidArg;
    delete static_cast<SwapchainState*>(swapchain);
    return kSuccess;
}

int32_t SCARLETTX_VULKAN_CALL VulkanXsGetBackendStatistics(
    VULKAN_XS_BACKEND_HANDLE backend, VULKAN_XS_STATS* outStats) {
    if (!backend || !outStats) return kInvalidArg;
    auto* b = static_cast<VulkanBackendState*>(backend);
    std::lock_guard<std::mutex> lk(b->mu);
    *outStats = b->stats;
    return kSuccess;
}

}  // extern "C"
