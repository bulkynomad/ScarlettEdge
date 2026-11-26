#include "scarlettx/gpu/vulkan_backend_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    std::cout << "[test_vulkan_backend] Testing Hardware DXR & Vulkan Backend...\n";

    // 1. Backend lifecycle
    VULKAN_XS_INIT_DESC init_desc{};
    init_desc.enableRayTracing = 1;
    init_desc.enableValidation = 0;

    VULKAN_XS_BACKEND_HANDLE backend = nullptr;
    SX_CHECK(VulkanXsCreateBackend(&init_desc, &backend) == 0);
    SX_CHECK(backend != nullptr);

    // 2. Query capabilities
    VULKAN_XS_CAPABILITIES caps{};
    SX_CHECK(VulkanXsQueryCapabilities(backend, &caps) == 0);
    SX_CHECK(caps.hardwareRaytracingSupported == 1);
    SX_CHECK(caps.maxRayRecursionDepth == 31);
    SX_CHECK(std::strlen(caps.deviceName) > 0);

    // 3. RT Pipeline
    VULKAN_XS_RT_PIPELINE_DESC pipe_desc{};
    pipe_desc.maxRecursionDepth = 2;
    VULKAN_XS_RT_PIPELINE_HANDLE rt_pipe = nullptr;
    SX_CHECK(VulkanXsCreateRaytracingPipeline(backend, &pipe_desc, &rt_pipe) == 0);
    SX_CHECK(rt_pipe != nullptr);

    // 4. Acceleration structures (BLAS / TLAS)
    VULKAN_XS_AS_BUILD_DESC blas_desc{};
    blas_desc.type = 0; // BLAS
    blas_desc.triangleCount = 1200;
    VULKAN_XS_AS_HANDLE blas = nullptr;
    SX_CHECK(VulkanXsBuildAccelerationStructure(backend, &blas_desc, &blas) == 0);
    SX_CHECK(blas != nullptr);

    blas_desc.triangleCount = 1250;
    SX_CHECK(VulkanXsUpdateAccelerationStructure(backend, blas, &blas_desc) == 0);

    VULKAN_XS_AS_BUILD_DESC tlas_desc{};
    tlas_desc.type = 1; // TLAS
    tlas_desc.geometryCount = 1;
    VULKAN_XS_AS_HANDLE tlas = nullptr;
    SX_CHECK(VulkanXsBuildAccelerationStructure(backend, &tlas_desc, &tlas) == 0);
    SX_CHECK(tlas != nullptr);

    // 5. Trace rays
    VULKAN_XS_TRACE_RAYS_DESC trace_desc{};
    trace_desc.width = 1920;
    trace_desc.height = 1080;
    trace_desc.depth = 1;
    trace_desc.pipeline = rt_pipe;
    trace_desc.topLevelAs = tlas;
    SX_CHECK(VulkanXsTraceRays(backend, &trace_desc) == 0);

    // 6. Layout conversion & Swapchain
    void* descriptor_layout = nullptr;
    const uint32_t dummy_root_sig[] = {1, 2, 3, 4};
    SX_CHECK(VulkanXsTranslateXboxRootSignatureToVulkanLayout(dummy_root_sig, sizeof(dummy_root_sig), &descriptor_layout) == 0);
    SX_CHECK(descriptor_layout != nullptr);

    VULKAN_XS_SWAPCHAIN_HANDLE swapchain = nullptr;
    SX_CHECK(VulkanXsCreateSwapchain(backend, reinterpret_cast<void*>(0x1234), 1920, 1080, &swapchain) == 0);
    SX_CHECK(swapchain != nullptr);
    SX_CHECK(VulkanXsPresentFrame(backend, swapchain, 1) == 0);

    // 7. Backend statistics
    VULKAN_XS_STATS stats{};
    SX_CHECK(VulkanXsGetBackendStatistics(backend, &stats) == 0);
    SX_CHECK(stats.asBuilds == 2);
    SX_CHECK(stats.framesPresented == 1);
    SX_CHECK(stats.raysTraced == 1920ull * 1080ull);

    // 8. Cleanup
    SX_CHECK(VulkanXsDestroySwapchain(backend, swapchain) == 0);
    SX_CHECK(VulkanXsDestroyAccelerationStructure(backend, tlas) == 0);
    SX_CHECK(VulkanXsDestroyAccelerationStructure(backend, blas) == 0);
    SX_CHECK(VulkanXsDestroyRaytracingPipeline(backend, rt_pipe) == 0);
    SX_CHECK(VulkanXsDestroyBackend(backend) == 0);

    std::cout << "[test_vulkan_backend] All 14 VulkanXs APIs verified successfully!\n";
    return 0;
}
