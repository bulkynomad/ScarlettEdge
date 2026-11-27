#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "scarlettx/gpu/d3d12_xs_abi.h"

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_d3d12_xs_abi] FAILED: " #cond                   \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define SX_CHECK_EQ(a, b)                                                       \
    do {                                                                        \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b)) {                                                      \
            std::cerr << "[test_d3d12_xs_abi] FAILED: " #a " == " #b            \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ \
                      << "\n";                                                  \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#endif

namespace {

SCARLETT_BVH_TRIANGLE MakeTri(float z, float r, float g, float b) {
    SCARLETT_BVH_TRIANGLE t{};
    t.v0[0] = -1.0f; t.v0[1] = -1.0f; t.v0[2] = z;
    t.v1[0] =  1.0f; t.v1[1] = -1.0f; t.v1[2] = z;
    t.v2[0] =  0.0f; t.v2[1] =  1.0f; t.v2[2] = z;
    t.color[0] = r; t.color[1] = g; t.color[2] = b;
    t.reflectivity = 0.25f;
    return t;
}

}  // namespace

int main() {
    // 1. Direct PC D3D12 creation via D3D12XboxCreateDevice + root signature serialization
#if defined(_WIN32)
    {
        D3D12XboxSetProcessDebugFlags(D3D12XBOX_PROCESS_DEBUG_FLAG_NONE);
        SX_CHECK_EQ(D3D12XboxGetProcessDebugFlags(), 0u);

        // What a title passes before it creates its device.
        D3D12XS_VIRTUAL_MEMORY_CONFIG vm{};
        vm.Option = 0xFFFFFFFFu;
        vm.Result0 = vm.Result1 = ~0ull;
        SX_CHECK(D3D12ConfigureVirtualMemoryX(&vm) >= 0);

        void* deserializer = nullptr;
        const IID iid_deserializer = __uuidof(ID3D12RootSignatureDeserializer);
        SX_CHECK(D3D12XsCreateVersionedRootSignatureDeserializer(nullptr, 0, &iid_deserializer, &deserializer) < 0);
        SX_CHECK(vm.Result0 == 0 && vm.Result1 == 0);
        SX_CHECK_EQ(D3D12ConfigureVirtualMemoryX(nullptr), static_cast<int32_t>(0x80070057u));
        vm.Flags = 2;
        SX_CHECK_EQ(D3D12ConfigureVirtualMemoryX(&vm), static_cast<int32_t>(0x80070057u));
        vm.Flags = 1;
        vm.SizeIn4MbUnits = 34;
        SX_CHECK_EQ(D3D12ConfigureVirtualMemoryX(&vm), static_cast<int32_t>(0x80070057u));

        // The interface id is a data export; a title's import slot holds its address.
        const void* iid = reinterpret_cast<const void*>(
            GetProcAddress(GetModuleHandleA("d3d12_xs.dll"), "IID_ID3D12Device"));
        SX_CHECK(iid != nullptr);
        SX_CHECK(std::memcmp(iid, &__uuidof(ID3D12Device), 16) == 0);

        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1;
        params.PreferWarp = 1;
        ID3D12Device* raw_dev = nullptr;
        const int32_t hr = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device),
                                                 reinterpret_cast<void**>(&raw_dev));
        SX_CHECK(hr >= 0);
        SX_CHECK(raw_dev != nullptr);

        // Verify we can create a real ID3D12CommandQueue on the returned PC D3D12 device
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        SX_CHECK(SUCCEEDED(raw_dev->CreateCommandQueue(&qdesc, __uuidof(ID3D12CommandQueue),
                                                       reinterpret_cast<void**>(&queue))));
        SX_CHECK(queue != nullptr);

        // Serialize + deserialize a root signature through d3d12_xs.dll
        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err_blob = nullptr;
        SX_CHECK(D3D12XsSerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                               reinterpret_cast<void**>(&blob),
                                               reinterpret_cast<void**>(&err_blob)) >= 0);
        SX_CHECK(blob != nullptr);
        SX_CHECK(blob->GetBufferSize() > 0);

        ID3D12RootSignatureDeserializer* deser = nullptr;
        SX_CHECK(D3D12XsCreateRootSignatureDeserializer(
                     blob->GetBufferPointer(), blob->GetBufferSize(),
                     &__uuidof(ID3D12RootSignatureDeserializer),
                     reinterpret_cast<void**>(&deser)) >= 0);
        SX_CHECK(deser != nullptr);
        SX_CHECK(deser->GetRootSignatureDesc() != nullptr);

        deser->Release();
        blob->Release();
        if (err_blob) err_blob->Release();
        queue->Release();
        raw_dev->Release();
    }
#endif

    // 2. Scarlett handle-based device, native device extraction, resource allocation, frame pipeline
    uint64_t dev = 0;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    SX_CHECK_EQ(D3D12XboxCreateSyntheticDevice(&params, &dev), 0);
    SX_CHECK(dev != 0);

    // Null & invalid handle checks
    SX_CHECK(D3D12XboxQueryCapabilities(dev, nullptr) != 0);
    SX_CHECK(D3D12XboxQueryRaytracingCapabilities(0xDEADBEEFull, nullptr) != 0);

    D3D12XBOX_RAYTRACING_CAPABILITIES rt_caps{};
    SX_CHECK_EQ(D3D12XboxQueryRaytracingCapabilities(dev, &rt_caps), 0);
    SX_CHECK(rt_caps.ActiveBackend == D3D12XBOX_RT_BACKEND_HARDWARE_DXR ||
             rt_caps.ActiveBackend == D3D12XBOX_RT_BACKEND_SOFTWARE_BVH);
    SX_CHECK(rt_caps.MaxRecursionDepth == 31);
    SX_CHECK(std::strlen(rt_caps.AdapterName) > 0);

    // Resource allocation info via real ID3D12Device::GetResourceAllocationInfo
    uint64_t alloc_size = 0;
    uint64_t alloc_align = 0;
    SX_CHECK_EQ(D3D12XboxGetResourceAllocationInfo(dev, 1920, 1080, 28 /* R8G8B8A8_UNORM */,
                                                   &alloc_size, &alloc_align), 0);
    SX_CHECK(alloc_size >= 1920ull * 1080ull * 4ull);
    SX_CHECK(alloc_align >= 65536ull);

    // Frame interval, schedule frame event, WaitFrameEventXEx, PresentX, stats, Suspend/Resume
    SX_CHECK_EQ(D3D12XboxSetFrameIntervalX(dev, D3D12XBOX_FRAME_INTERVAL_120_HZ, 0), 0);

#if defined(_WIN32)
    HANDLE frame_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SX_CHECK(frame_ev != nullptr);
    SX_CHECK_EQ(D3D12XboxScheduleFrameEventX(dev, D3D12XBOX_FRAME_EVENT_ORIGIN, 0, frame_ev), 0);
    SX_CHECK_EQ(WaitForSingleObject(frame_ev, 1000), WAIT_OBJECT_0);
    CloseHandle(frame_ev);

    ID3D12Device* native_dev = nullptr;
    SX_CHECK_EQ(D3D12XboxGetNativeDevice(dev, reinterpret_cast<void**>(&native_dev)), 0);
    SX_CHECK(native_dev != nullptr);

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    SX_CHECK(SUCCEEDED(native_dev->CreateCommandQueue(&qdesc, __uuidof(ID3D12CommandQueue),
                                                      reinterpret_cast<void**>(&queue))));
#endif

    D3D12XBOX_FRAME_PIPELINE_TOKEN tok1 = 0;
    SX_CHECK_EQ(D3D12XboxWaitFrameEventXEx(dev, D3D12XBOX_FRAME_EVENT_ORIGIN, 100, &tok1), 0);
    SX_CHECK_EQ(tok1, 1ull);

    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = tok1;
    plane.Width = 1280;
    plane.Height = 720;

    D3D12XBOX_PRESENT_PARAMETERS present_params{};
    present_params.Token = tok1;
    present_params.PlaneCount = 1;
    present_params.Flags = D3D12XBOX_PRESENT_FLAG_IMMEDIATE;
    present_params.Planes = &plane;
#if defined(_WIN32)
    present_params.CommandQueue = queue;
#endif
    SX_CHECK_EQ(D3D12XboxPresentXParams(dev, &present_params), 0);

    D3D12XBOX_FRAME_STATISTICS stats{};
    SX_CHECK_EQ(D3D12XboxGetFrameStatisticsX(dev, &stats), 0);
    SX_CHECK_EQ(stats.PresentedFrameCount, 1ull);
    SX_CHECK_EQ(stats.LastPresentedToken, 1ull);
    SX_CHECK_EQ(stats.LastIssuedToken, 1ull);
    SX_CHECK_EQ(stats.IntervalHz, 120u);
    SX_CHECK_EQ(stats.Suspended, 0u);

    // Suspend rejects PresentX until resumed
    SX_CHECK_EQ(D3D12XboxSuspendX(dev, 0), 0);
    SX_CHECK(D3D12XboxPresentXParams(dev, &present_params) != 0);
    SX_CHECK_EQ(D3D12XboxResumeX(dev), 0);
    SX_CHECK_EQ(D3D12XboxPresentXParams(dev, &present_params), 0);

    // GPU stability breadcrumbs + QPC frame-time percentile profiler
    SX_CHECK_EQ(D3D12XboxWriteBreadcrumb(dev, 0x1001ull, 0xCAFE0001ull, "MenuPassBegin"), 0);
    SX_CHECK_EQ(D3D12XboxWriteBreadcrumb(dev, 0x1001ull, 0xCAFE0002ull, "SceneOpaquePass"), 0);
    SX_CHECK_EQ(D3D12XboxRecordFrameTimestamp(dev, 16500ull), 0);
    SX_CHECK_EQ(D3D12XboxRecordFrameTimestamp(dev, 16667ull), 0);
    SX_CHECK_EQ(D3D12XboxRecordFrameTimestamp(dev, 17100ull), 0);

    D3D12XBOX_GPU_STABILITY_REPORT stab{};
    SX_CHECK_EQ(D3D12XboxGetGpuStabilityReport(dev, &stab), 0);
    SX_CHECK_EQ(stab.DeviceRemovedReason, 0);
    SX_CHECK(stab.BreadcrumbCount >= 2u);
    SX_CHECK_EQ(stab.LastBreadcrumbValue, 0xCAFE0002ull);
    SX_CHECK_EQ(std::string(stab.LastBreadcrumbTag), std::string("SceneOpaquePass"));
    SX_CHECK(stab.RecordedFrameCount >= 3ull);
    SX_CHECK(stab.MinFrameTimeUs > 0ull);
    SX_CHECK(stab.P99FrameTimeUs >= stab.AvgFrameTimeUs);
    SX_CHECK(stab.MaxFrameTimeUs >= stab.P99FrameTimeUs);

#if defined(_WIN32)
    queue->Release();
    native_dev->Release();
#endif

    // 3. DXR Acceleration Structure translation via d3d12_xs.dll
    D3D12XBOX_RT_AS_INPUTS pre_in{};
    pre_in.Type = D3D12XBOX_AS_TYPE_BOTTOM_LEVEL;
    pre_in.Flags = D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE | D3D12XBOX_AS_BUILD_FLAG_PREFER_FAST_TRACE;
    pre_in.NumDescs = 2;
    D3D12XBOX_RT_AS_PREBUILD_INFO pre_info{};
    SX_CHECK_EQ(D3D12XboxGetRaytracingAccelerationStructurePrebuildInfo(dev, &pre_in, &pre_info), 0);
    SX_CHECK(pre_info.ResultDataMaxSizeInBytes > 0);
    SX_CHECK(pre_info.ScratchDataSizeInBytes > 0);
    SX_CHECK(pre_info.UpdateScratchDataSizeInBytes > 0);

    SCARLETT_BVH_TRIANGLE tris[2] = {
        MakeTri(2.0f, 1.0f, 0.2f, 0.2f),
        MakeTri(4.0f, 0.2f, 0.4f, 1.0f),
    };
    D3D12XBOX_RT_AS_BUILD_DESC blas_desc{};
    blas_desc.Inputs = pre_in;
    blas_desc.Triangles = tris;
    uint64_t dxr_blas = 0;
    SX_CHECK_EQ(D3D12XboxBuildRaytracingAccelerationStructure(dev, &blas_desc, &dxr_blas), 0);
    SX_CHECK(dxr_blas != 0);

    D3D12XBOX_RT_AS_BUILD_DESC tlas_desc{};
    tlas_desc.Inputs.Type = D3D12XBOX_AS_TYPE_TOP_LEVEL;
    tlas_desc.Inputs.Flags = D3D12XBOX_AS_BUILD_FLAG_PREFER_FAST_TRACE;
    tlas_desc.Inputs.NumDescs = 1;
    tlas_desc.BlasHandles = &dxr_blas;
    uint64_t dxr_tlas = 0;
    SX_CHECK_EQ(D3D12XboxBuildRaytracingAccelerationStructure(dev, &tlas_desc, &dxr_tlas), 0);
    SX_CHECK(dxr_tlas != 0);

    SX_CHECK_EQ(D3D12XboxDestroyRaytracingAccelerationStructure(dev, dxr_tlas), 0);
    SX_CHECK_EQ(D3D12XboxDestroyRaytracingAccelerationStructure(dev, dxr_blas), 0);
    SX_CHECK_EQ(D3D12XboxDestroyRaytracingAccelerationStructure(dev, dxr_blas) != 0, true);

#if defined(_WIN32)
    // 3b. D3D12.2 Ultimate: VRS, Tiled (Reserved) Resources, ExecuteIndirect GPU Counts, & Work Graphs!
    {
        // (1) Variable Rate Shading (VRS) query & command-list state
        D3D12XBOX_VRS_CAPABILITIES vrs_caps{};
        SX_CHECK_EQ(D3D12XboxQueryVrsCapabilities(dev, &vrs_caps), 0);
        SX_CHECK(vrs_caps.ShadingRateImageTileSize >= 8u);
        const uint32_t combiners[2] = {0u /* PASSTHROUGH */, 1u /* OVERRIDE */};
        SX_CHECK_EQ(D3D12XboxSetShadingRate(dev, 0x5u /* D3D12_SHADING_RATE_2X2 */, combiners), 0);

        // (2) Tiled (Reserved) Resource creation + 64 KB sparse tile mapping + GPU R/W round-trip
        uint64_t reserved_res = 0;
        SX_CHECK_EQ(D3D12XboxCreateReservedResource(dev, 131072ull /* 2 x 64 KB tiles */, &reserved_res), 0);
        SX_CHECK(reserved_res != 0);

        std::vector<uint32_t> tile1_in(1024);
        for (size_t i = 0; i < tile1_in.size(); ++i) tile1_in[i] = static_cast<uint32_t>(0xA50000u + i * 7u);
        std::vector<uint32_t> tile1_out(1024, 0u);
        SX_CHECK_EQ(D3D12XboxUpdateTileMappings(
                        dev, reserved_res, 1u /* map tile #1 at +64 KB */, 1u,
                        tile1_in.data(), tile1_in.size() * sizeof(uint32_t),
                        tile1_out.data(), tile1_out.size() * sizeof(uint32_t)),
                    0);
        SX_CHECK(tile1_out == tile1_in);
        SX_CHECK_EQ(D3D12XboxDestroyReservedResource(dev, reserved_res), 0);

        // (3) ExecuteIndirect with GPU Count Buffer:
        //     4 indirect commands submitted (max=4), GPU count buffer = 2 -> only first 2 dispatches execute!
        auto compile_cs = [](const char* src) {
            ID3DBlob* cs = nullptr;
            ID3DBlob* err = nullptr;
            const HRESULT hr = D3DCompile(src, std::strlen(src), "d3d12_2_test.hlsl",
                                          nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
            if (err) err->Release();
            std::vector<uint8_t> bytes;
            if (SUCCEEDED(hr) && cs) {
                const auto* p = static_cast<const uint8_t*>(cs->GetBufferPointer());
                bytes.assign(p, p + cs->GetBufferSize());
                cs->Release();
            }
            return bytes;
        };

        const auto indirect_cs = compile_cs(R"(
RWByteAddressBuffer g_Out : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 gid : SV_GroupID) {
    uint prev = 0;
    g_Out.InterlockedAdd(0, 1u, prev);
    g_Out.InterlockedAdd(4, gid.x + 1u, prev);
}
)");
        SX_CHECK(!indirect_cs.empty());

        D3D12XBOX_ROOT_SIGNATURE_DESC irs_desc{};
        irs_desc.NumUavs = 1;
        uint64_t irs = 0;
        SX_CHECK_EQ(D3D12XboxCreateRootSignature(dev, &irs_desc, &irs), 0);

        D3D12XBOX_COMPUTE_PSO_DESC ipso_desc{};
        ipso_desc.RootSignatureHandle = irs;
        ipso_desc.CS = {indirect_cs.data(), indirect_cs.size()};
        uint64_t ipso = 0;
        SX_CHECK_EQ(D3D12XboxCreateComputePipelineState(dev, &ipso_desc, &ipso), 0);

        // 4 candidate indirect dispatches: GroupCountX = {3, 5, 100, 200}; GPU count buffer = 2 -> 3 + 5 = 8 groups!
        const uint32_t triplets[12] = {
            3u, 1u, 1u,
            5u, 1u, 1u,
            100u, 1u, 1u,
            200u, 1u, 1u,
        };
        uint32_t indirect_uav[4] = {0u, 0u, 0xCAFEBABEu, 0u};
        SX_CHECK_EQ(D3D12XboxExecuteIndirectWithCount(
                        dev, ipso, 4u /* max_command_count */, triplets, 2u /* gpu_count_buffer_value */,
                        indirect_uav, sizeof(indirect_uav)),
                    0);
        SX_CHECK_EQ(indirect_uav[0], 8u);  // 3 + 5 = 8 total groups (commands 2 & 3 skipped by GPU count buffer!)
        SX_CHECK_EQ(indirect_uav[2], 0xCAFEBABEu);
        SX_CHECK_EQ(D3D12XboxDestroyPipelineState(dev, ipso), 0);
        SX_CHECK_EQ(D3D12XboxDestroyRootSignature(dev, irs), 0);

        // (4) GPU Producer -> Consumer Work Graph (zero CPU readback between Producer and Consumer!)
        const auto producer_cs = compile_cs(R"(
cbuffer Constants : register(b0) { uint g_RecordCount; uint g_BaseVal; uint g_P2; uint g_P3; };
RWByteAddressBuffer g_DispatchArgs : register(u0);
RWByteAddressBuffer g_Records      : register(u1);
RWByteAddressBuffer g_UnusedOut    : register(u2);
[numthreads(1, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x == 0u) {
        g_DispatchArgs.Store3(0, uint3(g_RecordCount, 1u, 1u));
        for (uint i = 0u; i < g_RecordCount; ++i) {
            g_Records.Store(i * 4u, g_BaseVal + i * 10u);
        }
    }
}
)");
        const auto consumer_cs = compile_cs(R"(
cbuffer Constants : register(b0) { uint g_RecordCount; uint g_BaseVal; uint g_Mul; uint g_P3; };
RWByteAddressBuffer g_DispatchArgs : register(u0);
RWByteAddressBuffer g_Records      : register(u1);
RWByteAddressBuffer g_FinalOut     : register(u2);
[numthreads(1, 1, 1)]
void main(uint3 gid : SV_GroupID) {
    uint rec = g_Records.Load(gid.x * 4u);
    g_FinalOut.Store(gid.x * 4u, rec * g_Mul + 1u);
}
)");
        SX_CHECK(!producer_cs.empty() && !consumer_cs.empty());

        D3D12XBOX_WORK_GRAPH_DESC wg_desc{};
        wg_desc.ProducerCS = {producer_cs.data(), producer_cs.size()};
        wg_desc.ConsumerCS = {consumer_cs.data(), consumer_cs.size()};
        wg_desc.MaxNodeRecords = 16;
        wg_desc.RecordStrideBytes = 4;
        uint64_t wg = 0;
        SX_CHECK_EQ(D3D12XboxCreateWorkGraph(dev, &wg_desc, &wg), 0);
        SX_CHECK(wg != 0);

        const uint32_t wg_consts[4] = {6u /* 6 consumer groups emitted by Producer */, 100u, 3u, 0u};
        uint32_t wg_out[8] = {};
        SX_CHECK_EQ(D3D12XboxDispatchWorkGraph(dev, wg, 1u, wg_consts, 4u, wg_out, sizeof(wg_out)), 0);
        for (uint32_t i = 0; i < 6u; ++i) {
            SX_CHECK_EQ(wg_out[i], (100u + i * 10u) * 3u + 1u);
        }
        SX_CHECK_EQ(wg_out[6], 0u);  // Unlaunched consumer slot stays 0!
        SX_CHECK_EQ(D3D12XboxDestroyWorkGraph(dev, wg), 0);
    }
#endif

    SX_CHECK_EQ(D3D12XboxDestroyDevice(dev), 0);

    // 4. XboxBvhBuilder.dll extended APIs (prebuild, build, update, stats, single-ray trace)
    D3D12XBOX_RT_AS_PREBUILD_INFO bvh_pre{};
    SX_CHECK_EQ(XboxBvhGetPrebuildInfo(2, D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE, &bvh_pre), 0);
    SX_CHECK(bvh_pre.ResultDataMaxSizeInBytes >= 256);
    SX_CHECK(bvh_pre.UpdateScratchDataSizeInBytes >= 256);

    SCARLETT_BVH_BUILD_DESC bdesc{};
    bdesc.Triangles = tris;
    bdesc.TriangleCount = 2;
    bdesc.BuildFlags = D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE;
    uint64_t blas = 0;
    SX_CHECK_EQ(XboxBvhBuildBlas(&bdesc, &blas), 0);

    SCARLETT_BVH_STATS bstats{};
    SX_CHECK_EQ(XboxBvhGetStats(blas, &bstats), 0);
    SX_CHECK_EQ(bstats.TriangleCount, 2u);
    SX_CHECK(bstats.NodeCount >= 1u);

    // Update BLAS geometry (move first triangle closer to z = 1.5)
    tris[0] = MakeTri(1.5f, 0.2f, 1.0f, 0.3f);
    SX_CHECK_EQ(XboxBvhUpdateBlas(blas, &bdesc), 0);

    uint64_t tlas = 0;
    SX_CHECK_EQ(XboxBvhBuildTlas(&blas, 1, &tlas), 0);

    SCARLETT_RAY_DESC ray{};
    ray.Origin[0] = 0.0f; ray.Origin[1] = 0.0f; ray.Origin[2] = 0.0f;
    ray.Direction[0] = 0.0f; ray.Direction[1] = 0.0f; ray.Direction[2] = 1.0f;
    ray.TMin = 0.001f;
    ray.TMax = 100.0f;
    SCARLETT_RAY_HIT hit{};
    SX_CHECK_EQ(XboxBvhTraceSingleRay(tlas, &ray, &hit), 0);
    SX_CHECK_EQ(hit.Hit, 1u);
    SX_CHECK(std::fabs(hit.T - 1.5f) < 0.01f);

    // Test XboxBvhGetPostbuildInfo and XboxBvhCopy
    uint64_t postbuild_size = 0;
    SX_CHECK_EQ(XboxBvhGetPostbuildInfo(blas, 0, &postbuild_size), 0);
    SX_CHECK(postbuild_size > 0);

    uint64_t copied_blas = 0;
    SX_CHECK_EQ(XboxBvhCopy(blas, &copied_blas), 0);
    SX_CHECK(copied_blas != 0 && copied_blas != blas);
    SX_CHECK_EQ(XboxBvhDestroy(copied_blas), 0);

    // Ray pointing away should miss
    ray.Direction[2] = -1.0f;
    SCARLETT_RAY_HIT miss{};
    SX_CHECK_EQ(XboxBvhTraceSingleRay(tlas, &ray, &miss), 0);
    SX_CHECK_EQ(miss.Hit, 0u);

    SX_CHECK_EQ(XboxBvhDestroy(tlas), 0);
    SX_CHECK_EQ(XboxBvhDestroy(blas), 0);

    std::cout << "[PASS] test_d3d12_xs_abi (adapter=" << rt_caps.AdapterName
              << ", hw_dxr_tier=" << rt_caps.HardwareRaytracingTier
              << ", active_rt_backend="
              << (rt_caps.ActiveBackend == D3D12XBOX_RT_BACKEND_HARDWARE_DXR ? "DXR" : "SoftwareBVH")
              << ")\n";
    return 0;
}
