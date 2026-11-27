#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#define SLICE_EXPORT __declspec(dllexport)
#else
#define SLICE_EXPORT
#endif

#include "scarlettx/gdk/xgameruntime_abi.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/vulkan_backend_abi.h"
#include "scarlettx/gpu/xs2_abi.h"
#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/kernel/kernelx_abi.h"
#include "scarlettx/net/httpclient_abi.h"
#include "scarlettx/services/xsapi_abi.h"

namespace {

void AppendLe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void PumpQueue(XTaskQueueHandle queue, XAsyncBlock* async) {
    while (XAsyncGetStatus(async, false) == XGR_E_PENDING) {
        XTaskQueueDispatch(queue, XTaskQueuePort_Completion, 20);
    }
}

void SCARLETTX_GDK_CALLBACK OnRtaWsMessage(HCWebSocketHandle, const char* msg, void* ctx) {
    if (ctx && msg) {
        *static_cast<std::string*>(ctx) = msg;
    }
}

}  // namespace

extern "C" SLICE_EXPORT int ScarlettGuestMain() {
    if (XGameRuntimeInitialize() != XGR_S_OK) return 1;

    XTaskQueueHandle queue = nullptr;
    if (XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_Manual, &queue) != XGR_S_OK) {
        return 2;
    }

    // 1. 2 MB Unified Memory Large-Page Allocation + Physical Aliasing Ring Churn (kernelx.dll)
    void* ring_base = nullptr;
    uint64_t ring_gpu_va = 0;
    if (KernelXAllocateMemory(2u * 1024u * 1024u, KERNELX_PAGE_2MB, KERNELX_MEM_GPU_VISIBLE,
                              &ring_base, &ring_gpu_va) != 0 ||
        !ring_base || (reinterpret_cast<uintptr_t>(ring_base) & (2u * 1024u * 1024u - 1u)) != 0) {
        std::fprintf(stderr, "[openworld_slice] 2 MB aligned KernelXAllocateMemory failed\n");
        return 10;
    }

    void* alias_view = nullptr;
    if (KernelXCreateMemoryAlias(ring_base, 0, 256u * 1024u, &alias_view) != 0 || !alias_view || alias_view == ring_base) {
        std::fprintf(stderr, "[openworld_slice] KernelXCreateMemoryAlias failed\n");
        return 11;
    }

    // Write world sector magic via base VA and read back coherently through physical alias VA!
    static_cast<uint32_t*>(ring_base)[0] = 0x4F50574Cu;  // 'OPWL'
    if (static_cast<const uint32_t*>(alias_view)[0] != 0x4F50574Cu) {
        std::fprintf(stderr, "[openworld_slice] Physical memory alias coherency failed\n");
        return 12;
    }

    // 2. Concurrent Multi-Queue DirectStorage Open-World Streaming into Aliased Unified Memory
    const char* stream_file = std::getenv("SCARLETTX_OPENWORLD_STREAM_PAK");
    if (!stream_file || !*stream_file) {
        std::fprintf(stderr, "[openworld_slice] Missing SCARLETTX_OPENWORLD_STREAM_PAK\n");
        return 20;
    }
    uint64_t ds_factory = 0, ds_file = 0, q_geom = 0, q_tex = 0;
    if (DStorageGetFactory(&ds_factory) != 0 ||
        DStorageOpenFile(ds_factory, stream_file, &ds_file) != 0) {
        return 21;
    }
    uint64_t comp_sz = 0;
    DStorageGetFileSize(ds_file, &comp_sz);

    DSTORAGE_QUEUE_DESC qd_hi{};
    qd_hi.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd_hi.Capacity = 32;
    qd_hi.Priority = DSTORAGE_PRIORITY_HIGH;
    qd_hi.Name = "OpenWorld_WorldGeom";
    DSTORAGE_QUEUE_DESC qd_nm{};
    qd_nm.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd_nm.Capacity = 32;
    qd_nm.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd_nm.Name = "OpenWorld_MipTex";
    if (DStorageCreateQueue(ds_factory, &qd_hi, &q_geom) != 0 ||
        DStorageCreateQueue(ds_factory, &qd_nm, &q_tex) != 0) {
        return 22;
    }

    DSTORAGE_REQUEST req_geom{};
    req_geom.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req_geom.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req_geom.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    req_geom.SourceFileHandle = ds_file;
    req_geom.SourceFileOffset = 0;
    req_geom.SourceSize = static_cast<uint32_t>(comp_sz);
    req_geom.DestinationMemory = static_cast<uint8_t*>(ring_base) + 4096;
    req_geom.DestinationSize = 4096;
    req_geom.UncompressedSize = 4096;
    req_geom.Name = "SectorGeomChunk";

    DSTORAGE_REQUEST req_tex = req_geom;
    req_tex.DestinationMemory = static_cast<uint8_t*>(alias_view) + 16384;
    req_tex.Name = "SectorTexChunk";

    if (DStorageEnqueueRequest(q_geom, &req_geom) != 0 ||
        DStorageEnqueueRequest(q_tex, &req_tex) != 0 ||
        DStorageSubmit(q_geom) != 0 ||
        DStorageSubmit(q_tex) != 0 ||
        DStorageWaitForIdle(q_geom, 5000) != 0 ||
        DStorageWaitForIdle(q_tex, 5000) != 0) {
        std::fprintf(stderr, "[openworld_slice] Concurrent multi-queue DirectStorage streaming failed\n");
        return 23;
    }
    // Verify data streamed into alias_view+16384 is immediately visible at ring_base+16384!
    if (std::memcmp(static_cast<const uint8_t*>(ring_base) + 4096,
                    static_cast<const uint8_t*>(ring_base) + 16384, 4096) != 0) {
        std::fprintf(stderr, "[openworld_slice] Aliased DirectStorage destination mismatch\n");
        return 24;
    }

    // 3. Wave32 RDNA2 XS2 -> SPIR-V 1.5 Lifting + Live Vulkan 1.3 GPU Compute Culling + Timeline Semaphore
    const uint32_t wave32_words[] = {
        0x06040000u,               // v_add_f32 v2, v0, v0
        0xE2000000u, 0x00000200u,  // buffer_store_dword v2, v0
        0xBF800000u,               // s_endpgm
    };
    std::vector<uint8_t> xs2_wave32 = {'X', 'S', '2', '\0'};
    AppendLe32(xs2_wave32, 1u);
    AppendLe32(xs2_wave32, 3u);
    AppendLe32(xs2_wave32, static_cast<uint32_t>(sizeof(wave32_words)));
    xs2_wave32.insert(xs2_wave32.end(), reinterpret_cast<const uint8_t*>(wave32_words),
                      reinterpret_cast<const uint8_t*>(wave32_words) + sizeof(wave32_words));

    XS2_TRANSLATION_OPTIONS lift_opts{0x00060005u, 2u, 1u, 1u};
    void* spirv_ptr = nullptr;
    size_t spirv_bytes = 0;
    if (Xs2ShaderLiftToSpirv(xs2_wave32.data(), xs2_wave32.size(), &lift_opts, &spirv_ptr, &spirv_bytes) != 0 ||
        !spirv_ptr || spirv_bytes < 64) {
        std::fprintf(stderr, "[openworld_slice] Xs2ShaderLiftToSpirv failed\n");
        return 30;
    }

    VULKAN_XS_INIT_DESC vk_init{};
    VULKAN_XS_BACKEND_HANDLE vk_backend = nullptr;
    if (VulkanXsCreateBackend(&vk_init, &vk_backend) != 0 || !vk_backend) return 31;
    VULKAN_XS_CAPABILITIES vk_caps{};
    VulkanXsQueryCapabilities(vk_backend, &vk_caps);
    const bool has_hw_vulkan = (std::strstr(vk_caps.deviceName, "ScarlettEdge Vulkan (") != nullptr);

    float ssbo_cull[64] = {};
    uint64_t timeline_val = 0;
    VULKAN_XS_COMPUTE_DISPATCH_DESC vk_disp{};
    vk_disp.spirvBytecode = spirv_ptr;
    vk_disp.spirvSize = spirv_bytes;
    vk_disp.groupCountX = 1;
    vk_disp.groupCountY = 1;
    vk_disp.groupCountZ = 1;
    vk_disp.inoutSsboData = ssbo_cull;
    vk_disp.ssboByteSize = sizeof(ssbo_cull);
    vk_disp.outTimelineValue = &timeline_val;
    if (VulkanXsDispatchComputeSpirv(vk_backend, &vk_disp) != 0) {
        return 32;
    }
    if (has_hw_vulkan && (timeline_val == 0 || ssbo_cull[5] != 10.0f)) {
        std::fprintf(stderr, "[openworld_slice] Live Vulkan 1.3 XS2 compute dispatch failed (ssbo[5]=%f, tl=%llu)\n",
                     ssbo_cull[5], static_cast<unsigned long long>(timeline_val));
        return 32;
    }
    Xs2ShaderFreeBytecode(spirv_ptr);
    VulkanXsDestroyBackend(vk_backend);

    // On headless CI hosts without a Vulkan ICD, execute the same Wave32 XS2 compute shader via D3D12 WARP!
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dp{1, 0, 0, 0, 0, 1};
    uint64_t d3d_dev = 0;
    if (D3D12XboxCreateSyntheticDevice(&dp, &d3d_dev) != 0) return 50;
    if (!has_hw_vulkan) {
        D3D12XBOX_ROOT_SIGNATURE_DESC cs_rs_desc{};
        cs_rs_desc.NumUavs = 1;
        uint64_t cs_rs = 0, cs_pso = 0;
        D3D12XBOX_COMPUTE_PSO_DESC cs_pso_desc{};
        if (D3D12XboxCreateRootSignature(d3d_dev, &cs_rs_desc, &cs_rs) != 0) return 33;
        cs_pso_desc.RootSignatureHandle = cs_rs;
        cs_pso_desc.CS = {xs2_wave32.data(), xs2_wave32.size()};
        if (D3D12XboxCreateComputePipelineState(d3d_dev, &cs_pso_desc, &cs_pso) != 0 ||
            D3D12XboxDispatchCompute(d3d_dev, cs_pso, 1, 1, 1, nullptr, 0, ssbo_cull, sizeof(ssbo_cull)) != 0 ||
            ssbo_cull[5] != 10.0f) {
            std::fprintf(stderr, "[openworld_slice] D3D12 WARP fallback XS2 compute dispatch failed\n");
            return 34;
        }
        timeline_val = 1;
        D3D12XboxDestroyPipelineState(d3d_dev, cs_pso);
        D3D12XboxDestroyRootSignature(d3d_dev, cs_rs);
    }

    // 4. Persistent XblTitleStorage + HCWebSocket Xbox Live RTA Session
    if (XblInitialize("openworld-slice-scid") != 0) return 40;
    XblContextHandle xbl_ctx = nullptr;
    if (XblContextCreateHandle(0xCAFEBABE0001ull, &xbl_ctx) != 0 || !xbl_ctx) return 41;

    const char* mode_env = std::getenv("SCARLETTX_OPENWORLD_VERIFY_PERSIST");
    const bool verify_persist_only = (mode_env && mode_env[0] == '1');
    const std::string expected_json = "{\"world\":\"OpenWorldTest\",\"wantedLevel\":3,\"x\":412.5}";

    if (!verify_persist_only) {
        XAsyncBlock up_ab{};
        up_ab.queue = queue;
        if (XblTitleStorageUploadBlobAsync(
                xbl_ctx, "saves/openworld_checkpoint.json", XblTitleStorageType_TrustedPlatformStorage,
                reinterpret_cast<const uint8_t*>(expected_json.data()), expected_json.size(), &up_ab) != 0) {
            return 42;
        }
        PumpQueue(queue, &up_ab);
        if (XblTitleStorageUploadBlobResult(&up_ab) != 0) return 43;
    }

    uint8_t dl_buf[256] = {};
    XAsyncBlock dl_ab{};
    dl_ab.queue = queue;
    if (XblTitleStorageDownloadBlobAsync(
            xbl_ctx, "saves/openworld_checkpoint.json", XblTitleStorageType_TrustedPlatformStorage,
            dl_buf, sizeof(dl_buf), &dl_ab) != 0) {
        return 44;
    }
    PumpQueue(queue, &dl_ab);
    size_t dl_bytes = 0;
    if (XblTitleStorageDownloadBlobResult(&dl_ab, &dl_bytes) != 0 ||
        std::string(reinterpret_cast<const char*>(dl_buf), dl_bytes) != expected_json) {
        std::fprintf(stderr, "[openworld_slice] Persistent XblTitleStorage verification failed\n");
        return 45;
    }

    HCInitialize(nullptr);
    HCWebSocketRegisterMockReply("wss://rta.xboxlive.com", "[0,1,\"OPENWORLD_SESSION_READY\"]");
    std::string ws_reply;
    HCWebSocketHandle ws = nullptr;
    if (HCWebSocketCreate(&ws, &OnRtaWsMessage, nullptr, nullptr, &ws_reply) != 0 || !ws) return 46;
    XAsyncBlock ws_conn_ab{queue, nullptr, nullptr, {}};
    HCWebSocketConnectAsync("wss://rta.xboxlive.com/connect", "rta.xboxlive.com.V2", ws, &ws_conn_ab);
    PumpQueue(queue, &ws_conn_ab);
    XAsyncBlock ws_send_ab{queue, nullptr, nullptr, {}};
    HCWebSocketSendMessageAsync(ws, "[1,1,\"https://sessiondirectory.xboxlive.com\"]", &ws_send_ab);
    PumpQueue(queue, &ws_send_ab);
    HCWebSocketDisconnect(ws);
    HCWebSocketCloseHandle(ws);
    HCCleanup();
    if (ws_reply.find("OPENWORLD_SESSION_READY") == std::string::npos) {
        std::fprintf(stderr, "[openworld_slice] HCWebSocket RTA reply missing\n");
        return 47;
    }

    // 5. Open-World BVH Ray-Tracing + 60 FPS Pacing + Zero-TDR GPU Breadcrumb Stability Check
    SCARLETT_BVH_TRIANGLE world_tri{};
    world_tri.v0[0] = -2.0f; world_tri.v0[1] = -2.0f; world_tri.v0[2] = 5.0f;
    world_tri.v1[0] =  2.0f; world_tri.v1[1] = -2.0f; world_tri.v1[2] = 5.0f;
    world_tri.v2[0] =  0.0f; world_tri.v2[1] =  2.0f; world_tri.v2[2] = 5.0f;
    world_tri.color[0] = 0.9f; world_tri.color[1] = 0.4f; world_tri.color[2] = 0.2f;
    SCARLETT_BVH_BUILD_DESC bvh_bd{&world_tri, 1, D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE};
    uint64_t blas = 0, tlas = 0;
    if (XboxBvhBuildBlas(&bvh_bd, &blas) != 0 || XboxBvhBuildTlas(&blas, 1, &tlas) != 0) return 51;

    for (uint32_t f = 0; f < 5; ++f) {
        uint64_t tok = 0;
        D3D12XboxWaitFrameEventX(d3d_dev, &tok);
        D3D12XboxWriteBreadcrumb(d3d_dev, 10 + f, 0x3000 + f, "OpenWorld_Wave32Cull");
        SCARLETT_RAY_DESC ray{{0.0f, 0.0f, 0.0f}, 0.01f, {0.0f, 0.0f, 1.0f}, 100.0f};
        SCARLETT_RAY_HIT hit{};
        if (XboxBvhTraceSingleRay(tlas, &ray, &hit) != 0 || hit.Hit != 1u) return 52;
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{256, 144, tok};
        D3D12XboxPresentX(d3d_dev, &plane);
    }
    D3D12XboxWriteBreadcrumb(d3d_dev, 99, 0x7777, "OpenWorld_Present60Hz");

    D3D12XBOX_GPU_STABILITY_REPORT stab{};
    if (D3D12XboxGetGpuStabilityReport(d3d_dev, &stab) != 0 ||
        stab.DeviceRemovedReason != 0 ||
        stab.P99FrameTimeUs == 0 ||
        std::strcmp(stab.LastBreadcrumbTag, "OpenWorld_Present60Hz") != 0) {
        return 53;
    }

    XboxBvhDestroy(tlas);
    XboxBvhDestroy(blas);
    D3D12XboxDestroyDevice(d3d_dev);
    XblContextCloseHandle(xbl_ctx);
    XblCleanup();
    DStorageDestroyQueue(q_tex);
    DStorageDestroyQueue(q_geom);
    DStorageCloseFile(ds_file);
    DStorageDestroyFactory(ds_factory);
    KernelXFreeMemoryAlias(alias_view);
    KernelXFreeMemory(ring_base);
    XTaskQueueTerminate(queue, true, nullptr, nullptr);
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    std::printf("[openworld_slice] Open-world slice completed cleanly (vk_tl=%llu, p99_us=%llu)\n",
                static_cast<unsigned long long>(timeline_val),
                static_cast<unsigned long long>(stab.P99FrameTimeUs));
    return 0;
}

int main() {
    return ScarlettGuestMain();
}
