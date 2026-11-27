// Track 4H: d3d12_xs.dll Xbox COM Proxy Torture-Test (Hardware GPU + WARP Fallback)
// Exercises XboxD3D12DeviceProxy / CommandQueueProxy / GraphicsCommandListProxy / ResourceProxy:
//   - D3D12XboxCreateDevice (SDK 0x20006)
//   - D3D12_HEAP_FLAG_ALLOW_DISPLAY (0x80) sanitization
//   - CreateHeap + CreatePlacedResource + GetGPUVirtualAddress
//   - CreateRootSignature + CreateGraphicsPipelineState + CreateCommandSignature + ExecuteIndirect
//   - ResourceBarrier proxy unwrapping + ExecuteCommandLists + Fence sync
//   - QueueProxyPresentX (slot 30) -> FlightRecorder::RecordFrameTimeUs live pacing telemetry

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#endif

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::cerr << "[test_d3d12_xs_proxy_rtx] FAILED at line " << line << ": " << expr << "\n";
        ++g_failures;
    }
}

#define CHECK(x) Check((x), #x, __LINE__)

}  // namespace

int main() {
#if !defined(_WIN32)
    std::cout << "[test_d3d12_xs_proxy_rtx] SKIPPED (non-Windows)\n";
    return 0;
#else
    namespace fs = std::filesystem;
    const fs::path flight_dir = fs::temp_directory_path() / "scarlettx_test_d3d12_xs_proxy_rtx";
    std::error_code ec;
    fs::remove_all(flight_dir, ec);
    fs::create_directories(flight_dir, ec);

    scarlettx::FlightRecorderOptions fropts;
    fropts.output_dir = flight_dir.string();
    fropts.hang_timeout_ms = 0;
    scarlettx::FlightRecorder::Instance().Enable(fropts);

    // Activate Xbox COM Proxy mode (RealTitleMode) + headless swapchain mode
    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    _putenv_s("SCARLETTX_HEADLESS", "1");

    // 1. Create Xbox D3D12 Device Proxy (SDK 0x20006); prefer HW GPU, fall back to WARP on headless CI
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
    if (!dev) {
        scarlettx::FlightRecorder::Instance().Disable();
        return 1;
    }

    // 2. CommandQueue, Allocator, Fence
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue != nullptr);

    ID3D12CommandAllocator* alloc = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) && alloc != nullptr);

    ID3D12Fence* fence = nullptr;
    CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence != nullptr);

    // 3. Committed RT with Xbox D3D12_HEAP_FLAG_ALLOW_DISPLAY (0x80) — must be stripped cleanly
    D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = 128;
    rtDesc.Height = 128;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12Resource* rt = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommittedResource(
              &defaultHeap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &rtDesc,
              D3D12_RESOURCE_STATE_COMMON, &clearVal, IID_PPV_ARGS(&rt))) &&
          rt != nullptr);

    // 4. CreateHeap + CreatePlacedResource + GetGPUVirtualAddress
    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = 65536;
    heapDesc.Properties = D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_UPLOAD};
    heapDesc.Alignment = 65536;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ID3D12Heap* uploadHeap = nullptr;
    CHECK(SUCCEEDED(dev->CreateHeap(&heapDesc, IID_PPV_ARGS(&uploadHeap))) && uploadHeap != nullptr);

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = 256;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* indirectBuf = nullptr;
    CHECK(SUCCEEDED(dev->CreatePlacedResource(
              uploadHeap, 0, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
              IID_PPV_ARGS(&indirectBuf))) &&
          indirectBuf != nullptr);
    if (indirectBuf) {
        CHECK(indirectBuf->GetGPUVirtualAddress() != 0);
        void* mapped = nullptr;
        CHECK(SUCCEEDED(indirectBuf->Map(0, nullptr, &mapped)) && mapped != nullptr);
        if (mapped) {
            D3D12_DRAW_ARGUMENTS args{3, 1, 0, 0};
            std::memcpy(mapped, &args, sizeof(args));
            indirectBuf->Unmap(0, nullptr);
        }
    }

    // 5. RootSignature + HLSL VS/PS PSO + CommandSignature (ExecuteIndirect)
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob)));
    ID3D12RootSignature* rootSig = nullptr;
    if (rsBlob) {
        CHECK(SUCCEEDED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                 IID_PPV_ARGS(&rootSig))) &&
              rootSig != nullptr);
        rsBlob->Release();
    }
    if (errBlob) errBlob->Release();

    const char* vsSrc =
        "float4 main(uint vid : SV_VertexID) : SV_Position {"
        "  float2 p[3] = { float2(0,0.5), float2(0.5,-0.5), float2(-0.5,-0.5) };"
        "  return float4(p[vid % 3], 0, 1);"
        "}";
    const char* psSrc = "float4 main() : SV_Target0 { return float4(0.2, 0.8, 0.4, 1.0); }";
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    CHECK(SUCCEEDED(D3DCompile(vsSrc, std::strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr)));
    CHECK(SUCCEEDED(D3DCompile(psSrc, std::strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig;
    psoDesc.VS = {vsBlob ? vsBlob->GetBufferPointer() : nullptr, vsBlob ? vsBlob->GetBufferSize() : 0};
    psoDesc.PS = {psBlob ? psBlob->GetBufferPointer() : nullptr, psBlob ? psBlob->GetBufferSize() : 0};
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    CHECK(SUCCEEDED(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))) && pso != nullptr);
    if (vsBlob) vsBlob->Release();
    if (psBlob) psBlob->Release();

    D3D12_INDIRECT_ARGUMENT_DESC argDesc{};
    argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC csDesc{};
    csDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    csDesc.NumArgumentDescs = 1;
    csDesc.pArgumentDescs = &argDesc;
    ID3D12CommandSignature* cmdSig = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandSignature(&csDesc, nullptr, IID_PPV_ARGS(&cmdSig))) && cmdSig != nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap))) && rtvHeap != nullptr);
    if (rtvHeap && rt) {
        dev->CreateRenderTargetView(rt, nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    ID3D12GraphicsCommandList* cmd = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso, IID_PPV_ARGS(&cmd))) &&
          cmd != nullptr);

    // 6. Execute 10 proxy frames with ResourceBarrier + ExecuteIndirect + PresentX slot 30
    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (cmd && queue && alloc && fence && rt && rtvHeap && rootSig && pso && cmdSig && indirectBuf) {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_VIEWPORT vp{0, 0, 128, 128, 0, 1};
        const D3D12_RECT sc{0, 0, 128, 128};

        // SDK 0x20004+ (0x20006) Queue proxy shifts slots >= 14 by +2:
        //   slot 16 = Signal, slot 32 = PresentX
        using PfnQueueSignal = HRESULT(__stdcall*)(void*, void*, UINT64);
        using PfnPresentX = intptr_t(__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t);
        auto** qvtbl = *reinterpret_cast<void***>(queue);
        auto queueSignal = reinterpret_cast<PfnQueueSignal>(qvtbl[16]);
        auto presentX = reinterpret_cast<PfnPresentX>(qvtbl[32]);

        // Xbox d3d12_xs shifts GraphicsCommandList slots >= 28 by +4:
        //   slot 34 = SetGraphicsRootSignature
        //   slot 50 = OMSetRenderTargets
        //   slot 52 = ClearRenderTargetView
        //   slot 63 = ExecuteIndirect
        auto** cvtbl = *reinterpret_cast<void***>(cmd);
        using PfnSetGfxRootSig = void(__stdcall*)(void*, void*);
        using PfnOmSetRT = void(__stdcall*)(void*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
        using PfnClearRtv = void(__stdcall*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*);
        using PfnExecIndirect = void(__stdcall*)(void*, void*, UINT, void*, UINT64, void*, UINT64);
        auto setGfxRootSig = reinterpret_cast<PfnSetGfxRootSig>(cvtbl[34]);
        auto omSetRT = reinterpret_cast<PfnOmSetRT>(cvtbl[50]);
        auto clearRtv = reinterpret_cast<PfnClearRtv>(cvtbl[52]);
        auto execIndirect = reinterpret_cast<PfnExecIndirect>(cvtbl[63]);

        for (UINT64 frame = 1; frame <= 10; ++frame) {
            if (frame > 1) {
                CHECK(SUCCEEDED(alloc->Reset()));
                CHECK(SUCCEEDED(cmd->Reset(alloc, pso)));
            }
            D3D12_RESOURCE_BARRIER b1{};
            b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b1.Transition.pResource = rt;
            b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b1.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmd->ResourceBarrier(1, &b1);

            setGfxRootSig(cmd, rootSig);
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &sc);
            omSetRT(cmd, 1, &rtvH, FALSE, nullptr);
            const float cc[4] = {0.04f, 0.08f, 0.14f, 1.0f};
            clearRtv(cmd, rtvH, cc, 0, nullptr);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            execIndirect(cmd, cmdSig, 1, indirectBuf, 0, nullptr, 0);

            D3D12_RESOURCE_BARRIER b2 = b1;
            b2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            cmd->ResourceBarrier(1, &b2);
            CHECK(SUCCEEDED(cmd->Close()));

            ID3D12CommandList* lists[] = {cmd};
            queue->ExecuteCommandLists(1, lists);
            CHECK(SUCCEEDED(queueSignal(queue, fence, frame)));
            if (fence->GetCompletedValue() < frame) {
                CHECK(SUCCEEDED(fence->SetEventOnCompletion(frame, fenceEvt)));
                WaitForSingleObject(fenceEvt, 5000);
            }
            // Drive PresentX slot 32 so FlightRecorder records live frame-pacing deltas
            if (presentX) {
                presentX(queue, 0, 0, 0);
            }
        }
        CHECK(fence->GetCompletedValue() == 10);
    }

    const auto fp = scarlettx::FlightRecorder::Instance().GetFramePacingSummary();
    CHECK(fp.frames_recorded >= 9);
    scarlettx::FlightRecorder::Instance().Disable();

    if (fenceEvt) CloseHandle(fenceEvt);
    if (cmd) cmd->Release();
    if (rtvHeap) rtvHeap->Release();
    if (cmdSig) cmdSig->Release();
    if (pso) pso->Release();
    if (rootSig) rootSig->Release();
    if (indirectBuf) indirectBuf->Release();
    if (uploadHeap) uploadHeap->Release();
    if (rt) rt->Release();
    if (fence) fence->Release();
    if (alloc) alloc->Release();
    if (queue) queue->Release();
    dev->Release();
    fs::remove_all(flight_dir, ec);

    if (g_failures != 0) {
        std::cerr << "[test_d3d12_xs_proxy_rtx] " << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "[test_d3d12_xs_proxy_rtx] PASS (10/10 d3d12_xs proxy frames, "
              << fp.frames_recorded << " paced samples, p95=" << fp.p95_ms << " ms)\n";
    return 0;
#endif
}
