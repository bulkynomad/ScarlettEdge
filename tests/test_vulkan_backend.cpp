#include "scarlettx/gpu/vulkan_backend_abi.h"
#include "scarlettx/gpu/xs2_abi.h"
#include "sx_check.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

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

    // 3. RT Pipeline (with real minimal valid SPIR-V 1.5 shader module to exercise host vulkan-1.dll)
    const uint32_t minimal_spirv[] = {
        0x07230203u, 0x00010500u, 0x53585332u, 6u, 0u,
        (2u << 16) | 17u, 1u,                              // OpCapability Shader
        (3u << 16) | 14u, 0u, 1u,                          // OpMemoryModel Logical GLSL450
        (5u << 16) | 15u, 5u, 3u, 0x6E69616Du, 0u,         // OpEntryPoint GLCompute %3 "main"
        (6u << 16) | 16u, 3u, 17u, 1u, 1u, 1u,             // OpExecutionMode %3 LocalSize 1 1 1
        (2u << 16) | 19u, 1u,                              // OpTypeVoid %1
        (3u << 16) | 33u, 2u, 1u,                          // OpTypeFunction %2 %1
        (5u << 16) | 54u, 1u, 3u, 0u, 2u,                  // OpFunction
        (2u << 16) | 248u, 4u,                             // OpLabel %4
        (1u << 16) | 253u,                                 // OpReturn
        (1u << 16) | 56u                                   // OpFunctionEnd
    };
    VULKAN_XS_RT_PIPELINE_DESC pipe_desc{};
    pipe_desc.maxRecursionDepth = 2;
    pipe_desc.shaderStageCount = 1;
    pipe_desc.raygenSpirv = minimal_spirv;
    pipe_desc.raygenSize = sizeof(minimal_spirv);
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

    uint64_t blas_va = 0;
    SX_CHECK(VulkanXsGetAccelerationStructureDeviceAddress(backend, blas, &blas_va) == 0);
    SX_CHECK(blas_va != 0);

    VULKAN_XS_AS_BUILD_DESC tlas_desc{};
    tlas_desc.type = 1; // TLAS
    tlas_desc.geometryCount = 1;
    tlas_desc.vertexBufferGpuVa = blas_va;
    VULKAN_XS_AS_HANDLE tlas = nullptr;
    SX_CHECK(VulkanXsBuildAccelerationStructure(backend, &tlas_desc, &tlas) == 0);
    SX_CHECK(tlas != nullptr);

    uint64_t tlas_va = 0;
    SX_CHECK(VulkanXsGetAccelerationStructureDeviceAddress(backend, tlas, &tlas_va) == 0);
    SX_CHECK(tlas_va != 0 && tlas_va != blas_va);

    // 5. Shader Binding Table (SBT) & Trace rays
    VULKAN_XS_SBT_INFO sbt{};
    SX_CHECK(VulkanXsBuildShaderBindingTable(backend, rt_pipe, &sbt) == 0);
    SX_CHECK(sbt.raygen.deviceAddress != 0);
    SX_CHECK(sbt.raygen.stride == sbt.raygen.size && sbt.raygen.stride >= 32u);
    SX_CHECK((sbt.raygen.deviceAddress % sbt.baseAlignment) == 0);
    SX_CHECK((sbt.miss.deviceAddress % sbt.baseAlignment) == 0);
    SX_CHECK((sbt.hit.deviceAddress % sbt.baseAlignment) == 0);

    VULKAN_XS_TRACE_RAYS_DESC trace_desc{};
    trace_desc.width = 1920;
    trace_desc.height = 1080;
    trace_desc.depth = 1;
    trace_desc.pipeline = rt_pipe;
    trace_desc.topLevelAs = tlas;
    SX_CHECK(VulkanXsTraceRays(backend, &trace_desc) == 0);

    // 5b. Hardware SPV_KHR_ray_tracing pipeline (RayGenerationKHR + MissKHR + ClosestHitKHR) + SBT + TraceRays
    {
        auto make_rt_spirv = [](uint32_t execution_model) {
            // Valid SPIR-V 1.4+ module with OpCapability RayTracingKHR (4479) + OpExtension "SPV_KHR_ray_tracing"
            return std::vector<uint32_t>{
                0x07230203u, 0x00010400u, 0x53585332u, 6u, 0u,
                (2u << 16) | 17u, 4479u,  // OpCapability RayTracingKHR
                (6u << 16) | 10u,         // OpExtension "SPV_KHR_ray_tracing"
                0x5F565053u, 0x5F52484Bu, 0x5F796172u, 0x63617274u, 0x00676E69u,
                (3u << 16) | 14u, 0u, 1u, // OpMemoryModel Logical GLSL450
                (5u << 16) | 15u, execution_model, 3u, 0x6E69616Du, 0u,  // OpEntryPoint <model> %3 "main"
                (2u << 16) | 19u, 1u,     // OpTypeVoid %1
                (3u << 16) | 33u, 2u, 1u, // OpTypeFunction %2 %1
                (5u << 16) | 54u, 1u, 3u, 0u, 2u, // OpFunction
                (2u << 16) | 248u, 4u,    // OpLabel %4
                (1u << 16) | 253u,        // OpReturn
                (1u << 16) | 56u          // OpFunctionEnd
            };
        };
        const auto rgen_spv = make_rt_spirv(5313u);  // RayGenerationKHR
        const auto miss_spv = make_rt_spirv(5315u);  // MissKHR
        const auto chit_spv = make_rt_spirv(5316u);  // ClosestHitKHR

        VULKAN_XS_RT_PIPELINE_DESC khr_desc{};
        khr_desc.maxRecursionDepth = 1;
        khr_desc.shaderStageCount = 3;
        khr_desc.raygenSpirv = rgen_spv.data();
        khr_desc.raygenSize = rgen_spv.size() * sizeof(uint32_t);
        khr_desc.missSpirv = miss_spv.data();
        khr_desc.missSize = miss_spv.size() * sizeof(uint32_t);
        khr_desc.closestHitSpirv = chit_spv.data();
        khr_desc.closestHitSize = chit_spv.size() * sizeof(uint32_t);

        VULKAN_XS_RT_PIPELINE_HANDLE khr_pipe = nullptr;
        SX_CHECK(VulkanXsCreateRaytracingPipeline(backend, &khr_desc, &khr_pipe) == 0 && khr_pipe != nullptr);

        VULKAN_XS_SBT_INFO khr_sbt{};
        SX_CHECK(VulkanXsBuildShaderBindingTable(backend, khr_pipe, &khr_sbt) == 0);
        SX_CHECK((khr_sbt.raygen.deviceAddress % khr_sbt.baseAlignment) == 0);
        SX_CHECK((khr_sbt.miss.deviceAddress % khr_sbt.baseAlignment) == 0);
        SX_CHECK((khr_sbt.hit.deviceAddress % khr_sbt.baseAlignment) == 0);
        std::cout << "[test_vulkan_backend] KHR RT SBT built (hardwareBacked=" << khr_sbt.hardwareBacked
                  << ", handleSize=" << khr_sbt.handleSize << ", baseAlign=" << khr_sbt.baseAlignment
                  << ", blasVA=0x" << std::hex << blas_va << ", tlasVA=0x" << tlas_va << std::dec << ")\n";

        VULKAN_XS_TRACE_RAYS_DESC khr_trace{};
        khr_trace.width = 64;
        khr_trace.height = 64;
        khr_trace.depth = 1;
        khr_trace.pipeline = khr_pipe;
        khr_trace.topLevelAs = tlas;
        SX_CHECK(VulkanXsTraceRays(backend, &khr_trace) == 0);
        SX_CHECK(VulkanXsDestroyRaytracingPipeline(backend, khr_pipe) == 0);
    }

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
    SX_CHECK(stats.raysTraced == 1920ull * 1080ull + 64ull * 64ull);

    // 8. End-to-End Scarlett RDNA2 (XS2) -> SPIR-V 1.5 -> Live Vulkan 1.3 GPU Compute + Timeline Semaphore!
    {
        // RDNA2 Compute Kernel:
        //   v0 starts at float(GlobalInvocationId.x)
        //   s_load_dwordx4 s[4:7], s[0:1], 0x0 -> v4..v7 = uboVec4.xyzw (sdata = 4 -> 4<<6 = 0x100)
        //   v_fma_f32 v2, v0, v4, v5           -> v2 = tid.x * ubo.x + ubo.y (w1 = (5<<18)|(4<<9)|0 = 0x00140800)
        //   buffer_store_dword v2, v0          -> ssbo[tid.x] = v2
        //   s_endpgm
        const uint32_t rdna2_vk_kernel[] = {
            0xF4000100u, 0x00000000u,
            0xD4000002u, 0x00140800u,
            0xE2000000u, 0x00000200u,
            0xBF800000u,
        };
        XS2_TRANSLATION_OPTIONS spv_opts{};
        spv_opts.targetShaderModel = 0x00060005u;
        spv_opts.enableWave32 = 1;
        void* spv_code = nullptr;
        size_t spv_size = 0;
        SX_CHECK(Xs2ShaderLiftToSpirv(rdna2_vk_kernel, sizeof(rdna2_vk_kernel), &spv_opts, &spv_code, &spv_size) == 0);
        SX_CHECK(spv_code != nullptr && spv_size >= 40);

        const float ubo_params[4] = {3.5f, 12.0f, 0.0f, 0.0f};
        float ssbo_data[16] = {};
        uint64_t timeline_val = 0;

        VULKAN_XS_COMPUTE_DISPATCH_DESC disp{};
        disp.spirvBytecode = spv_code;
        disp.spirvSize = spv_size;
        disp.groupCountX = 2;  // 2 * LocalSize(8) = 16 invocations
        disp.groupCountY = 1;
        disp.groupCountZ = 1;
        disp.uboVec4 = ubo_params;
        disp.inoutSsboData = ssbo_data;
        disp.ssboByteSize = sizeof(ssbo_data);
        disp.outTimelineValue = &timeline_val;

        SX_CHECK(VulkanXsDispatchComputeSpirv(backend, &disp) == 0);
        if (std::strstr(caps.deviceName, "ScarlettEdge Vulkan (") != nullptr) {
            SX_CHECK(timeline_val >= 1ull);
            for (uint32_t i = 0; i < 16; ++i) {
                const float expected = static_cast<float>(i) * 3.5f + 12.0f;
                SX_CHECK(std::fabs(ssbo_data[i] - expected) < 1e-4f);
            }
            std::cout << "[test_vulkan_backend] Verified live GPU execution on " << caps.deviceName
                      << " (timeline=" << timeline_val << ", ssbo[15]=" << ssbo_data[15] << ")\n";
        }
        Xs2ShaderFreeBytecode(spv_code);

#if defined(_WIN32)
        // Verify real Win32 HWND VkSurfaceKHR creation path
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"SXVkSurf", WS_POPUP, 0, 0, 64, 64,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd) {
            VULKAN_XS_SWAPCHAIN_HANDLE w32_sw = nullptr;
            SX_CHECK(VulkanXsCreateSwapchain(backend, hwnd, 64, 64, &w32_sw) == 0);
            SX_CHECK(w32_sw != nullptr);
            SX_CHECK(VulkanXsDestroySwapchain(backend, w32_sw) == 0);
            DestroyWindow(hwnd);
        }
#endif
    }

    // 9. Cleanup
    SX_CHECK(VulkanXsDestroySwapchain(backend, swapchain) == 0);
    SX_CHECK(VulkanXsDestroyAccelerationStructure(backend, tlas) == 0);
    SX_CHECK(VulkanXsDestroyAccelerationStructure(backend, blas) == 0);
    SX_CHECK(VulkanXsDestroyRaytracingPipeline(backend, rt_pipe) == 0);
    SX_CHECK(VulkanXsDestroyBackend(backend) == 0);

    std::cout << "[test_vulkan_backend] All 17 VulkanXs APIs verified successfully!\n";
    return 0;
}
