// Next-Gen d3d12_xs.dll Xbox COM Proxy Torture-Test (Hardware RTX + WARP Fallback)
// Scaffolded with local AI (qwen2.5-coder:14b) and refined for exact Xbox SDK 0x20006 ABI:
//   - Enhanced Barriers (ID3D12GraphicsCommandList7::Barrier, Xbox slot 90) proxy unwrapping
//   - Mesh Shader Tier query + DispatchMesh (Xbox slot 76) vtable validation
//   - Hardware DXR 1.1 Tier query + SetPipelineState1 (slot 70) / DispatchRays (slot 71)
//     + D3D12XboxGetRaytracingAccelerationStructurePrebuildInfo

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "scarlettx/gpu/d3d12_xs_abi.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d12.h>
#endif

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::cerr << "[test_d3d12_xs_nextgen_rtx] FAILED at line " << line << ": " << expr << "\n";
        ++g_failures;
    }
}

#define CHECK(x) Check((x), #x, __LINE__)

}  // namespace

int main() {
#if !defined(_WIN32)
    std::cout << "[test_d3d12_xs_nextgen_rtx] SKIPPED (non-Windows)\n";
    return 0;
#else
    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    _putenv_s("SCARLETTX_HEADLESS", "1");

    // 1. Create Xbox D3D12 Device Proxy (SDK 0x20006); prefer HW RTX, fallback to WARP on headless CI
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 0x20006u;
    params.PreferWarp = 0;
    ID3D12Device* dev = nullptr;
    int32_t hr = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device),
                                       reinterpret_cast<void**>(&dev));
    if (hr < 0 || !dev) {
        params.PreferWarp = 1;
        hr = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device),
                                   reinterpret_cast<void**>(&dev));
    }
    CHECK(hr >= 0 && dev != nullptr);
    if (!dev) return 1;

    // Query feature tiers through the Xbox device proxy
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    const HRESULT hr5 = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
    CHECK(SUCCEEDED(hr5));

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
    const HRESULT hr7 = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7));
    CHECK(SUCCEEDED(hr7));

    bool enhanced_barriers = false;
#if defined(__ID3D12GraphicsCommandList7_INTERFACE_DEFINED__)
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 opt12{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &opt12, sizeof(opt12)))) {
        enhanced_barriers = (opt12.EnhancedBarriersSupported != FALSE);
    }
#endif

    // Create Queue, Allocator, CommandList, Fence, and Buffer proxy
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue != nullptr);

    ID3D12CommandAllocator* alloc = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) && alloc != nullptr);

    ID3D12GraphicsCommandList* cmd = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))) && cmd != nullptr);

    ID3D12Fence* fence = nullptr;
    CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence != nullptr);

    D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = 4096;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommittedResource(
              &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf))) &&
          buf != nullptr);

    // 2. Test 1: Enhanced Barriers (CL7::Barrier, Xbox slot 90) proxy unwrapping on GPU
    if (cmd && queue && fence && buf) {
        auto** cvtbl = *reinterpret_cast<void***>(cmd);
        auto** qvtbl = *reinterpret_cast<void***>(queue);
        CHECK(cvtbl[70] != nullptr);  // SetPipelineState1
        CHECK(cvtbl[71] != nullptr);  // DispatchRays
        CHECK(cvtbl[76] != nullptr);  // DispatchMesh
        CHECK(cvtbl[90] != nullptr);  // CL7::Barrier

#if defined(__ID3D12GraphicsCommandList7_INTERFACE_DEFINED__)
        if (enhanced_barriers) {
            D3D12_BUFFER_BARRIER bb{};
            bb.SyncBefore = D3D12_BARRIER_SYNC_ALL;
            bb.SyncAfter = D3D12_BARRIER_SYNC_ALL;
            bb.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
            bb.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
            bb.pResource = buf;  // XboxD3D12ResourceProxy* — must be unwrapped by CmdListBarrier!
            bb.Offset = 0;
            bb.Size = UINT64_MAX;

            D3D12_BARRIER_GROUP grp{};
            grp.Type = D3D12_BARRIER_TYPE_BUFFER;
            grp.NumBarriers = 1;
            grp.pBufferBarriers = &bb;

            using PfnBarrier = intptr_t(__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t);
            auto barrier_fn = reinterpret_cast<PfnBarrier>(cvtbl[90]);
            barrier_fn(cmd, 1, reinterpret_cast<uintptr_t>(&grp), 0);
        }
#endif
        CHECK(SUCCEEDED(cmd->Close()));
        ID3D12CommandList* lists[] = {cmd};
        queue->ExecuteCommandLists(1, lists);

        using PfnQueueSignal = HRESULT(__stdcall*)(void*, void*, UINT64);
        auto queueSignal = reinterpret_cast<PfnQueueSignal>(qvtbl[16]);
        CHECK(SUCCEEDED(queueSignal(queue, fence, 1)));

        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(evt != nullptr);
        if (evt) {
            if (fence->GetCompletedValue() < 1) {
                CHECK(SUCCEEDED(fence->SetEventOnCompletion(1, evt)));
                WaitForSingleObject(evt, 5000);
            }
            CloseHandle(evt);
        }
        CHECK(fence->GetCompletedValue() == 1);
    }

    // 3. Test 2: DXR 1.1 PrebuildInfo through d3d12_xs.dll
    {
        uint64_t xs_dev = 0;
        CHECK(D3D12XboxCreateSyntheticDevice(&params, &xs_dev) == 0 && xs_dev != 0);
        if (xs_dev != 0) {
            D3D12XBOX_RT_AS_INPUTS inputs{};
            inputs.Type = 1;  // BLAS
            inputs.NumDescs = 128;
            D3D12XBOX_RT_AS_PREBUILD_INFO prebuild{};
            CHECK(D3D12XboxGetRaytracingAccelerationStructurePrebuildInfo(xs_dev, &inputs, &prebuild) == 0);
            CHECK(prebuild.ResultDataMaxSizeInBytes > 0 && prebuild.ScratchDataSizeInBytes > 0);
            D3D12XboxDestroyDevice(xs_dev);
        }
    }

    if (buf) buf->Release();
    if (fence) fence->Release();
    if (cmd) cmd->Release();
    if (alloc) alloc->Release();
    if (queue) queue->Release();
    dev->Release();

    if (g_failures != 0) {
        std::cerr << "[test_d3d12_xs_nextgen_rtx] " << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "[test_d3d12_xs_nextgen_rtx] PASS (DXR Tier=" << static_cast<int>(opt5.RaytracingTier)
              << ", MeshTier=" << static_cast<int>(opt7.MeshShaderTier)
              << ", EnhancedBarriers=" << (enhanced_barriers ? "yes" : "no") << ")\n";
    return 0;
#endif
}
