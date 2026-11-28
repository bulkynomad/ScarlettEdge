// Gate M3 Verification Suite: Real-Title Xbox D3D12 COM Synchronization,
// Upload Ring, FlushPipelineX Barriers, True ID3D12Heap Aliasing, Strict QI,
// Dynamic PresentX Resize, VSYNC QPC Pacing & D3D12 Debug Layer Cleanliness.

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "sx_check.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d12.h>
#  include <d3d12sdklayers.h>
#  include <d3dcompiler.h>
#  include <dxgi1_6.h>
#  include <intrin.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

// Xbox SDK 0x20006 Device vtable slots
constexpr int kQi = 0;
constexpr int kRelease = 2;
constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommandAllocator = 9;
constexpr int kDevCreateGraphicsPipelineState = 10;
constexpr int kDevCreateComputePipelineState = 11;
constexpr int kDevCreateCommandList = 12;
constexpr int kDevCreateDescriptorHeap = 14;
constexpr int kDevGetDescriptorHandleIncrementSize = 15;
constexpr int kDevCreateRootSignature = 16;
constexpr int kDevCreateShaderResourceView = 18;
constexpr int kDevCreateUnorderedAccessView = 19;
constexpr int kDevCreateRenderTargetView = 20;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kDevCreateHeap = 28;
constexpr int kDevCreatePlacedResource = 29;
constexpr int kDevCreateFence = 36;
constexpr int kDevCreateCommandSignature = 41;
constexpr int kDevGetAdapterLuid = 44;
constexpr int kDevCreatePlacedResourceX = 74;
constexpr int kDevSetFrameIntervalX = 78;
constexpr int kDevWaitFrameEventX = 80;

// Xbox SDK 0x20006 CommandQueue vtable slots (+2 shift at >= 14)
constexpr int kQueueExecuteCommandLists = 10;
constexpr int kQueueSignalV4 = 16;
constexpr int kQueuePresentXV4 = 32;

// Xbox SDK 0x20006 GraphicsCommandList vtable slots (+4 shift at >= 28, +2 shift at >= 59)
constexpr int kListClose = 9;
constexpr int kListReset = 10;
constexpr int kListDrawInstanced = 12;
constexpr int kListDispatch = 14;
constexpr int kListCopyBufferRegion = 15;
constexpr int kListCopyTextureRegion = 16;
constexpr int kListIASetPrimitiveTopology = 20;
constexpr int kListRSSetViewports = 21;
constexpr int kListRSSetScissorRects = 22;
constexpr int kListSetPipelineState = 25;
constexpr int kListResourceBarrier = 26;
constexpr int kListSetDescriptorHeaps = 32;
constexpr int kListSetComputeRootSignature = 33;
constexpr int kListSetGraphicsRootSignature = 34;
constexpr int kListSetComputeRootDescriptorTable = 35;
constexpr int kListSetGraphicsRootConstantBufferView = 42;
constexpr int kListSetComputeRootShaderResourceView = 43;
constexpr int kListSetComputeRootUnorderedAccessView = 45;
constexpr int kListOMSetRenderTargets = 50;
constexpr int kListClearRenderTargetView = 52;
constexpr int kListExecuteIndirectV4 = 65;
constexpr int kListFlushPipelineXV4 = 69;

// Resource / Fence / Heap / Allocator vtable slots
constexpr int kAllocReset = 8;
constexpr int kResMap = 8;
constexpr int kResUnmap = 9;
constexpr int kResGetGPUVirtualAddress = 11;
constexpr int kDescHeapGetCPUStart = 9;
constexpr int kDescHeapGetGPUStart = 10;
constexpr int kFenceGetCompletedValue = 8;
constexpr int kFenceSetEventOnCompletion = 9;

void WaitFence(void* fence, uint64_t val, HANDLE ev) {
    if (static_cast<uint64_t>(Call(fence, kFenceGetCompletedValue)) < val) {
        SX_REQUIRE(Call(fence, kFenceSetEventOnCompletion,
                        static_cast<uintptr_t>(val),
                        reinterpret_cast<uintptr_t>(ev)) == 0);
        SX_REQUIRE(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    }
}

ID3DBlob* CompileHlsl(const char* src, const char* entry, const char* target) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(
        src, std::strlen(src), nullptr, nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            std::fprintf(stderr, "HLSL compile error (%s): %s\n",
                         target, static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        SX_REQUIRE(false);
    }
    if (err) err->Release();
    return blob;
}

HWND FindOwnPresentWindow() {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExA(nullptr, hwnd, "ScarlettXPresentWindowClass", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return hwnd;
    }
    return nullptr;
}

// Acquire the underlying host singleton ID3D12InfoQueue via adapter LUID without
// bypassing the Xbox COM proxy's strict QueryInterface boundary.
ID3D12InfoQueue* TryAcquireHostInfoQueueForAdapterLuid(LUID adapter_luid) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory) {
        return nullptr;
    }
    IDXGIAdapter1* adapter = nullptr;
    HRESULT hr = factory->EnumAdapterByLuid(adapter_luid, IID_PPV_ARGS(&adapter));
    factory->Release();
    if (FAILED(hr) || !adapter) return nullptr;

    HMODULE sys_d3d12 = nullptr;
    wchar_t sys_dir[MAX_PATH] = {};
    if (GetSystemDirectoryW(sys_dir, MAX_PATH) > 0) {
        std::wstring p = std::wstring(sys_dir) + L"\\d3d12.dll";
        sys_d3d12 = LoadLibraryW(p.c_str());
    }
    ID3D12Device* host_dev = nullptr;
    if (sys_d3d12) {
        using PfnCreate = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
        auto fn = reinterpret_cast<PfnCreate>(GetProcAddress(sys_d3d12, "D3D12CreateDevice"));
        if (fn) {
            (void)fn(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&host_dev));
        }
    }
    adapter->Release();
    if (!host_dev) return nullptr;

    ID3D12InfoQueue* iq = nullptr;
    (void)host_dev->QueryInterface(IID_PPV_ARGS(&iq));
    host_dev->Release();
    return iq;
}

size_t DrainAndCountInfoQueueErrors(ID3D12InfoQueue* iq, const char* phase_label) {
    if (!iq) return 0;
    size_t bad = 0;
    const UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(iq->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<uint8_t> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(iq->GetMessage(i, msg, &len))) {
            if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "[InfoQueue][%s][sev=%d][id=%d] %s\n",
                             phase_label, static_cast<int>(msg->Severity),
                             static_cast<int>(msg->ID),
                             msg->pDescription ? msg->pDescription : "");
                ++bad;
            }
        }
    }
    iq->ClearStoredMessages();
    return bad;
}

}  // namespace
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if !defined(_WIN32)
    std::printf("test_d3d12_xs_com_sync: SKIPPED (non-Windows)\n");
    return 0;
#else
    sx_test::ScopedTempDir temp_dir("sx_d3d12_com_sync");
    scarlettx::FlightRecorderOptions fropts{};
    fropts.output_dir = temp_dir.path().string();
    fropts.hang_timeout_ms = 0;
    fropts.install_veh = false;
    scarlettx::FlightRecorder::Instance().Enable(fropts);

    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    _putenv_s("SCARLETTX_D3D12_DEBUG", "1");
    _putenv_s("SCARLETTX_HEADLESS", "1");
    _putenv_s("SCARLETTX_PRESENT_OFFSCREEN", "1");
    _putenv_s("SCARLETTX_FCAP", "0");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 0x20006u;
    params.PreferWarp = 0;
    void* device = nullptr;
    const IID iid_dev = __uuidof(ID3D12Device);
    if (D3D12XboxCreateDevice(nullptr, &params, &iid_dev, &device) < 0 || !device) {
        params.PreferWarp = 1;
        SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &iid_dev, &device) >= 0 && device != nullptr);
    }

    LUID adapter_luid{};
    Call(device, kDevGetAdapterLuid, &adapter_luid);
    ID3D12InfoQueue* info_queue = TryAcquireHostInfoQueueForAdapterLuid(adapter_luid);
    if (info_queue) {
        info_queue->ClearStoredMessages();
    }
    std::printf("test_d3d12_xs_com_sync: D3D12 Debug Layer InfoQueue=%s\n",
                info_queue ? "ACTIVE" : "unavailable (SDKLayers not installed)");

    void* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(device, kDevCreateCommandQueue, &qd, static_cast<void*>(nullptr), &queue) == 0 && queue);

    void* allocs[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        SX_REQUIRE(Call(device, kDevCreateCommandAllocator,
                        uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                        static_cast<void*>(nullptr), &allocs[i]) == 0 && allocs[i]);
    }

    void* cmd = nullptr;
    SX_REQUIRE(Call(device, kDevCreateCommandList, uintptr_t{0},
                    uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT}, allocs[0],
                    static_cast<void*>(nullptr), static_cast<void*>(nullptr), &cmd) == 0 && cmd);
    SX_REQUIRE(Call(cmd, kListClose) == 0);

    void* fence = nullptr;
    SX_REQUIRE(Call(device, kDevCreateFence, uintptr_t{0}, uintptr_t{0},
                    static_cast<void*>(nullptr), &fence) == 0 && fence);
    HANDLE fence_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SX_REQUIRE(fence_ev != nullptr);
    uint64_t next_fence = 0;

    // =========================================================================
    // 1. Strict QueryInterface Verification (+ FlightRecorder ComQiMiss)
    // =========================================================================
    {
        const IID unknown_iid = {0xDEADBEEFu, 0xCAFEu, 0x4BADu, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22}};
        const IID iq_iid = __uuidof(ID3D12InfoQueue);
        void* out = reinterpret_cast<void*>(0x1);
        SX_REQUIRE(Call(device, kQi, &unknown_iid, &out) == static_cast<intptr_t>(E_NOINTERFACE) && out == nullptr);
        SX_REQUIRE(Call(device, kQi, &iq_iid, &out) == static_cast<intptr_t>(E_NOINTERFACE) && out == nullptr);
        SX_REQUIRE(Call(cmd, kQi, &unknown_iid, &out) == static_cast<intptr_t>(E_NOINTERFACE) && out == nullptr);
        SX_REQUIRE(Call(queue, kQi, &unknown_iid, &out) == static_cast<intptr_t>(E_NOINTERFACE) && out == nullptr);

        const auto stats = scarlettx::FlightRecorder::Instance().GetStats();
        SX_REQUIRE(stats.unique_com_qi_misses >= 3u);

        // Known D3D12 / DXGI IIDs succeed cleanly.
        const IID iid_dev5 = __uuidof(ID3D12Device5);
        const IID iid_cl4 = __uuidof(ID3D12GraphicsCommandList4);
        SX_REQUIRE(Call(device, kQi, &iid_dev5, &out) == 0 && out == device);
        Call(device, kRelease);
        SX_REQUIRE(Call(cmd, kQi, &iid_cl4, &out) == 0 && out == cmd);
        Call(cmd, kRelease);
    }

    // =========================================================================
    // 2. 100 Multi-Draw In-Place CBV Frames (Upload Ring & Slab Reclamation)
    // =========================================================================
    {
        constexpr uint32_t kNumFrames = 100;
        constexpr uint32_t kDrawsPerFrame = 4;
        constexpr uint32_t kRtWidth = 16;   // 4 quads * 4 px wide
        constexpr uint32_t kRtHeight = kNumFrames;

        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp.Descriptor.ShaderRegister = 0;
        rp.Descriptor.RegisterSpace = 0;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &rp;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ID3DBlob* rs_blob = nullptr;
        SX_REQUIRE(D3D12XsSerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                 reinterpret_cast<void**>(&rs_blob), nullptr) >= 0 && rs_blob);
        void* gfx_rs = nullptr;
        const IID iid_rs = __uuidof(ID3D12RootSignature);
        SX_REQUIRE(Call(device, kDevCreateRootSignature, uintptr_t{0},
                        rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), &iid_rs, &gfx_rs) == 0 && gfx_rs);
        rs_blob->Release();

        const char* vs_hlsl =
            "cbuffer DrawCb : register(b0) { float4 uColor; float4 uRect; };\n"
            "float4 main(uint vid : SV_VertexID) : SV_Position {\n"
            "    float2 uv = float2((vid << 1) & 2, vid & 2);\n"
            "    float2 pos = lerp(uRect.xy, uRect.zw, uv);\n"
            "    return float4(pos, 0.0, 1.0);\n"
            "}\n";
        const char* ps_hlsl =
            "cbuffer DrawCb : register(b0) { float4 uColor; float4 uRect; };\n"
            "float4 main() : SV_Target0 { return uColor; }\n";

        ID3DBlob* vs = CompileHlsl(vs_hlsl, "main", "vs_5_0");
        ID3DBlob* ps = CompileHlsl(ps_hlsl, "main", "ps_5_0");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gpso{};
        gpso.pRootSignature = static_cast<ID3D12RootSignature*>(gfx_rs);
        gpso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        gpso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        gpso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gpso.SampleMask = UINT_MAX;
        gpso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gpso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gpso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gpso.NumRenderTargets = 1;
        gpso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpso.SampleDesc.Count = 1;

        void* gfx_pso = nullptr;
        SX_REQUIRE(Call(device, kDevCreateGraphicsPipelineState, &gpso,
                        static_cast<void*>(nullptr), &gfx_pso) == 0 && gfx_pso);
        vs->Release();
        ps->Release();

        D3D12_HEAP_PROPERTIES def_hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_HEAP_PROPERTIES up_hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_HEAP_PROPERTIES rb_hp{D3D12_HEAP_TYPE_READBACK};

        D3D12_RESOURCE_DESC rtd{};
        rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rtd.Width = kRtWidth;
        rtd.Height = kRtHeight;
        rtd.DepthOrArraySize = 1;
        rtd.MipLevels = 1;
        rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtd.SampleDesc.Count = 1;
        rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        void* rt = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &def_hp, uintptr_t{0}, &rtd,
                        uintptr_t{D3D12_RESOURCE_STATE_RENDER_TARGET}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &rt) == 0 && rt);

        D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
        rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_hd.NumDescriptors = 1;
        void* rtv_heap = nullptr;
        SX_REQUIRE(Call(device, kDevCreateDescriptorHeap, &rtv_hd,
                        static_cast<void*>(nullptr), &rtv_heap) == 0 && rtv_heap);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_h{};
        Call(rtv_heap, kDescHeapGetCPUStart, &rtv_h);
        Call(device, kDevCreateRenderTargetView, rt, static_cast<void*>(nullptr), rtv_h.ptr);

        // Single 256B UPLOAD CBV mutated in-place + single raw VirtualAlloc guest CBV mutated in-place
        D3D12_RESOURCE_DESC cb_desc{};
        cb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cb_desc.Width = 256;
        cb_desc.Height = 1;
        cb_desc.DepthOrArraySize = 1;
        cb_desc.MipLevels = 1;
        cb_desc.SampleDesc.Count = 1;
        cb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* upload_cbv = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &up_hp, uintptr_t{0}, &cb_desc,
                        uintptr_t{D3D12_RESOURCE_STATE_GENERIC_READ}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &upload_cbv) == 0 && upload_cbv);

        struct alignas(16) DrawConsts {
            float color[4];
            float rect[4];
            uint32_t pad[56];
        };
        DrawConsts* mapped_cb = nullptr;
        SX_REQUIRE(Call(upload_cbv, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &mapped_cb) == 0 && mapped_cb);
        const auto upload_cbv_va = static_cast<uint64_t>(Call(upload_cbv, kResGetGPUVirtualAddress));

        auto* raw_guest_cb = static_cast<DrawConsts*>(
            VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        SX_REQUIRE(raw_guest_cb != nullptr);

        // Also exercise transient oversized guest upload slab (> 1 MiB) on frame 0 to verify reclamation
        void* transient_burst = VirtualAlloc(nullptr, 1536 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        SX_REQUIRE(transient_burst != nullptr);

        uint64_t slot_fences[2] = {0, 0};
        for (uint32_t f = 0; f < kNumFrames; ++f) {
            const int slot = static_cast<int>(f & 1u);
            if (slot_fences[slot] != 0) {
                WaitFence(fence, slot_fences[slot], fence_ev);
            }
            SX_REQUIRE(Call(allocs[slot], kAllocReset) == 0);
            SX_REQUIRE(Call(cmd, kListReset, allocs[slot], gfx_pso) == 0);

            if (f == 0) {
                const float zero_rgba[4] = {0.f, 0.f, 0.f, 0.f};
                using PfnClearRtv = void(SCARLETTX_GDK_CALLBACK*)(void*, uintptr_t, const float*, uint32_t, const void*);
                auto clear_fn = reinterpret_cast<PfnClearRtv>((*reinterpret_cast<void***>(cmd))[kListClearRenderTargetView]);
                clear_fn(cmd, rtv_h.ptr, zero_rgba, 0, nullptr);
            }

            const D3D12_VIEWPORT vp{0.0f, static_cast<float>(f), static_cast<float>(kRtWidth), 1.0f, 0.0f, 1.0f};
            const D3D12_RECT sc{0, static_cast<LONG>(f), static_cast<LONG>(kRtWidth), static_cast<LONG>(f + 1)};
            Call(cmd, kListSetGraphicsRootSignature, gfx_rs);
            Call(cmd, kListRSSetViewports, uintptr_t{1}, &vp);
            Call(cmd, kListRSSetScissorRects, uintptr_t{1}, &sc);
            Call(cmd, kListOMSetRenderTargets, uintptr_t{1}, &rtv_h, uintptr_t{0}, static_cast<void*>(nullptr));
            Call(cmd, kListIASetPrimitiveTopology, uintptr_t{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST});

            for (uint32_t d = 0; d < kDrawsPerFrame; ++d) {
                const uint8_t r_expect = static_cast<uint8_t>((f * 3u + d * 17u + 11u) & 0xFFu);
                const uint8_t g_expect = static_cast<uint8_t>((f * 7u + d * 53u + 29u) & 0xFFu);
                const uint8_t b_expect = static_cast<uint8_t>((d * 61u + 43u) & 0xFFu);

                DrawConsts dc{};
                dc.color[0] = static_cast<float>(r_expect) / 255.0f;
                dc.color[1] = static_cast<float>(g_expect) / 255.0f;
                dc.color[2] = static_cast<float>(b_expect) / 255.0f;
                dc.color[3] = 1.0f;
                const float x0 = -1.0f + 0.5f * static_cast<float>(d);
                const float x1 = x0 + 0.5f;
                dc.rect[0] = x0;
                dc.rect[1] = -1.0f;
                dc.rect[2] = x1;
                dc.rect[3] = 1.0f;

                if (f == 5 && d == 0) {
                    auto* burst_dc = static_cast<DrawConsts*>(transient_burst);
                    *burst_dc = dc;
                    Call(cmd, kListSetGraphicsRootConstantBufferView, uintptr_t{0},
                         reinterpret_cast<uintptr_t>(burst_dc));
                } else if (d < 2u) {
                    // Overwrite mapped UPLOAD CBV in-place mid-command-list and across frames in flight!
                    *mapped_cb = dc;
                    Call(cmd, kListSetGraphicsRootConstantBufferView, uintptr_t{0},
                         static_cast<uintptr_t>(upload_cbv_va));
                } else {
                    // Overwrite raw unified guest CPU CBV in-place mid-command-list!
                    *raw_guest_cb = dc;
                    Call(cmd, kListSetGraphicsRootConstantBufferView, uintptr_t{0},
                         reinterpret_cast<uintptr_t>(raw_guest_cb));
                }
                Call(cmd, kListDrawInstanced, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});
            }

            SX_REQUIRE(Call(cmd, kListClose) == 0);
            void* lists[] = {cmd};
            Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
            const uint64_t fv = ++next_fence;
            SX_REQUIRE(Call(queue, kQueueSignalV4, fence, static_cast<uintptr_t>(fv)) == 0);
            slot_fences[slot] = fv;
        }

        WaitFence(fence, next_fence, fence_ev);

        // Read back the 16x100 RT and verify all 400 per-draw regions preserved distinct CBV snapshots!
        constexpr uint32_t kRowPitch = 256;
        D3D12_RESOURCE_DESC rbd = cb_desc;
        rbd.Width = static_cast<UINT64>(kRowPitch) * kRtHeight;
        void* rb = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &rb_hp, uintptr_t{0}, &rbd,
                        uintptr_t{D3D12_RESOURCE_STATE_COPY_DEST}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &rb) == 0 && rb);

        SX_REQUIRE(Call(allocs[0], kAllocReset) == 0);
        SX_REQUIRE(Call(cmd, kListReset, allocs[0], static_cast<void*>(nullptr)) == 0);
        D3D12_RESOURCE_BARRIER tr{};
        tr.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tr.Transition.pResource = static_cast<ID3D12Resource*>(rt);
        tr.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tr.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        tr.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        Call(cmd, kListResourceBarrier, uintptr_t{1}, &tr);

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = static_cast<ID3D12Resource*>(rb);
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst_loc.PlacedFootprint.Footprint.Width = kRtWidth;
        dst_loc.PlacedFootprint.Footprint.Height = kRtHeight;
        dst_loc.PlacedFootprint.Footprint.Depth = 1;
        dst_loc.PlacedFootprint.Footprint.RowPitch = kRowPitch;

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = static_cast<ID3D12Resource*>(rt);
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;
        Call(cmd, kListCopyTextureRegion, &dst_loc, uintptr_t{0}, uintptr_t{0}, uintptr_t{0},
             &src_loc, static_cast<void*>(nullptr));
        SX_REQUIRE(Call(cmd, kListClose) == 0);

        void* lists[] = {cmd};
        Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
        const uint64_t rb_fv = ++next_fence;
        SX_REQUIRE(Call(queue, kQueueSignalV4, fence, static_cast<uintptr_t>(rb_fv)) == 0);
        WaitFence(fence, rb_fv, fence_ev);

        const uint8_t* pixels = nullptr;
        SX_REQUIRE(Call(rb, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &pixels) == 0 && pixels);
        for (uint32_t f = 0; f < kNumFrames; ++f) {
            const uint8_t* row = pixels + static_cast<size_t>(f) * kRowPitch;
            for (uint32_t d = 0; d < kDrawsPerFrame; ++d) {
                const uint32_t sample_x = d * 4u + 2u;
                const uint8_t r_got = row[sample_x * 4u + 0u];
                const uint8_t g_got = row[sample_x * 4u + 1u];
                const uint8_t b_got = row[sample_x * 4u + 2u];
                const uint8_t r_exp = static_cast<uint8_t>((f * 3u + d * 17u + 11u) & 0xFFu);
                const uint8_t g_exp = static_cast<uint8_t>((f * 7u + d * 53u + 29u) & 0xFFu);
                const uint8_t b_exp = static_cast<uint8_t>((d * 61u + 43u) & 0xFFu);
                SX_REQUIRE(std::abs(static_cast<int>(r_got) - static_cast<int>(r_exp)) <= 1);
                SX_REQUIRE(std::abs(static_cast<int>(g_got) - static_cast<int>(g_exp)) <= 1);
                SX_REQUIRE(std::abs(static_cast<int>(b_got) - static_cast<int>(b_exp)) <= 1);
            }
        }
        Call(rb, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));
        Call(upload_cbv, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

        VirtualFree(transient_burst, 0, MEM_RELEASE);
        VirtualFree(raw_guest_cb, 0, MEM_RELEASE);
        for (void* o : {rb, upload_cbv, rtv_heap, rt, gfx_pso, gfx_rs}) Call(o, kRelease);

        SX_REQUIRE(DrainAndCountInfoQueueErrors(info_queue, "MultiDrawCBV100") == 0);
        std::printf("test_d3d12_xs_com_sync: 100 multi-draw in-place CBV frames OK\n");
    }

    // =========================================================================
    // 3. UAV -> FlushPipelineX -> ExecuteIndirect (IndirectArgs) + SRV
    //    (Zero explicit ResourceBarrier calls between producer and consumer!)
    // =========================================================================
    {
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors = 1;
        uav_range.BaseShaderRegister = 1;  // u1 via descriptor table
        uav_range.RegisterSpace = 0;
        uav_range.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER cs_rps[3]{};
        // param 0: Root UAV u0
        cs_rps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        cs_rps[0].Descriptor.ShaderRegister = 0;
        cs_rps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // param 1: Root SRV t0
        cs_rps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        cs_rps[1].Descriptor.ShaderRegister = 0;
        cs_rps[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // param 2: Descriptor Table UAV u1
        cs_rps[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cs_rps[2].DescriptorTable.NumDescriptorRanges = 1;
        cs_rps[2].DescriptorTable.pDescriptorRanges = &uav_range;
        cs_rps[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC cs_rsd{};
        cs_rsd.NumParameters = 3;
        cs_rsd.pParameters = cs_rps;
        ID3DBlob* rs_blob = nullptr;
        SX_REQUIRE(D3D12XsSerializeRootSignature(&cs_rsd, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                 reinterpret_cast<void**>(&rs_blob), nullptr) >= 0 && rs_blob);
        void* cs_rs = nullptr;
        const IID iid_rs = __uuidof(ID3D12RootSignature);
        SX_REQUIRE(Call(device, kDevCreateRootSignature, uintptr_t{0},
                        rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), &iid_rs, &cs_rs) == 0 && cs_rs);
        rs_blob->Release();

        // Pass 1 CS: writes DispatchArgs {3, 1, 1} to u0 (root UAV) and 12 payload dwords to u1 (table UAV).
        const char* producer_cs_hlsl =
            "RWByteAddressBuffer uArgs : register(u0);\n"
            "RWByteAddressBuffer uPayload : register(u1);\n"
            "[numthreads(12, 1, 1)]\n"
            "void main(uint tid : SV_DispatchThreadID) {\n"
            "    if (tid == 0) {\n"
            "        uArgs.Store3(0, uint3(3u, 1u, 1u));\n"
            "    }\n"
            "    uPayload.Store(tid * 4u, 0x1000u + tid * 7u);\n"
            "}\n";

        // Pass 2 CS: launched by ExecuteIndirect(uArgs), reads t0 (payload SRV) and writes u0 (final output).
        const char* consumer_cs_hlsl =
            "ByteAddressBuffer tPayload : register(t0);\n"
            "RWByteAddressBuffer uOut : register(u0);\n"
            "[numthreads(4, 1, 1)]\n"
            "void main(uint tid : SV_DispatchThreadID) {\n"
            "    uint v = tPayload.Load(tid * 4u);\n"
            "    uOut.Store(tid * 4u, v ^ 0xA5A50000u);\n"
            "}\n";

        ID3DBlob* prod_cs = CompileHlsl(producer_cs_hlsl, "main", "cs_5_0");
        ID3DBlob* cons_cs = CompileHlsl(consumer_cs_hlsl, "main", "cs_5_0");

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
        cpso.pRootSignature = static_cast<ID3D12RootSignature*>(cs_rs);
        cpso.CS = {prod_cs->GetBufferPointer(), prod_cs->GetBufferSize()};
        void* prod_pso = nullptr;
        SX_REQUIRE(Call(device, kDevCreateComputePipelineState, &cpso,
                        static_cast<void*>(nullptr), &prod_pso) == 0 && prod_pso);
        cpso.CS = {cons_cs->GetBufferPointer(), cons_cs->GetBufferSize()};
        void* cons_pso = nullptr;
        SX_REQUIRE(Call(device, kDevCreateComputePipelineState, &cpso,
                        static_cast<void*>(nullptr), &cons_pso) == 0 && cons_pso);
        prod_cs->Release();
        cons_cs->Release();

        D3D12_INDIRECT_ARGUMENT_DESC iarg{};
        iarg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC csd{};
        csd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        csd.NumArgumentDescs = 1;
        csd.pArgumentDescs = &iarg;
        void* dispatch_sig = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommandSignature, &csd, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &dispatch_sig) == 0 && dispatch_sig);

        D3D12_HEAP_PROPERTIES def_hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_HEAP_PROPERTIES rb_hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC uav_bd{};
        uav_bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uav_bd.Width = 256;
        uav_bd.Height = 1;
        uav_bd.DepthOrArraySize = 1;
        uav_bd.MipLevels = 1;
        uav_bd.SampleDesc.Count = 1;
        uav_bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uav_bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        void* args_buf = nullptr;
        void* payload_buf = nullptr;
        void* out_buf = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &def_hp, uintptr_t{0}, &uav_bd,
                        uintptr_t{D3D12_RESOURCE_STATE_COMMON}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &args_buf) == 0 && args_buf);
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &def_hp, uintptr_t{0}, &uav_bd,
                        uintptr_t{D3D12_RESOURCE_STATE_COMMON}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &payload_buf) == 0 && payload_buf);
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &def_hp, uintptr_t{0}, &uav_bd,
                        uintptr_t{D3D12_RESOURCE_STATE_COMMON}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &out_buf) == 0 && out_buf);

        D3D12_RESOURCE_DESC rbd = uav_bd;
        rbd.Flags = D3D12_RESOURCE_FLAG_NONE;
        void* rb_buf = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &rb_hp, uintptr_t{0}, &rbd,
                        uintptr_t{D3D12_RESOURCE_STATE_COPY_DEST}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &rb_buf) == 0 && rb_buf);

        D3D12_DESCRIPTOR_HEAP_DESC srv_uav_hd{};
        srv_uav_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_uav_hd.NumDescriptors = 4;
        srv_uav_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        void* srv_uav_heap = nullptr;
        SX_REQUIRE(Call(device, kDevCreateDescriptorHeap, &srv_uav_hd,
                        static_cast<void*>(nullptr), &srv_uav_heap) == 0 && srv_uav_heap);

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_h{};
        Call(srv_uav_heap, kDescHeapGetCPUStart, &cpu_h);
        Call(srv_uav_heap, kDescHeapGetGPUStart, &gpu_h);

        D3D12_UNORDERED_ACCESS_VIEW_DESC raw_uav{};
        raw_uav.Format = DXGI_FORMAT_R32_TYPELESS;
        raw_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        raw_uav.Buffer.NumElements = 64;
        raw_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        Call(device, kDevCreateUnorderedAccessView, payload_buf, static_cast<void*>(nullptr),
             &raw_uav, cpu_h.ptr);

        const auto args_va = static_cast<uint64_t>(Call(args_buf, kResGetGPUVirtualAddress));
        const auto payload_va = static_cast<uint64_t>(Call(payload_buf, kResGetGPUVirtualAddress));
        const auto out_va = static_cast<uint64_t>(Call(out_buf, kResGetGPUVirtualAddress));

        SX_REQUIRE(Call(allocs[0], kAllocReset) == 0);
        SX_REQUIRE(Call(cmd, kListReset, allocs[0], prod_pso) == 0);
        void* heaps[] = {srv_uav_heap};
        Call(cmd, kListSetDescriptorHeaps, uintptr_t{1}, static_cast<void**>(heaps));
        Call(cmd, kListSetComputeRootSignature, cs_rs);
        Call(cmd, kListSetComputeRootUnorderedAccessView, uintptr_t{0}, static_cast<uintptr_t>(args_va));
        Call(cmd, kListSetComputeRootDescriptorTable, uintptr_t{2}, static_cast<uintptr_t>(gpu_h.ptr));
        Call(cmd, kListDispatch, uintptr_t{1}, uintptr_t{1}, uintptr_t{1});

        // ONLY FlushPipelineX (no ResourceBarrier!) between UAV write and IndirectArgs + SRV read!
        SX_REQUIRE(Call(cmd, kListFlushPipelineXV4, uintptr_t{0xFFFFFFFFu}, uintptr_t{0}, uintptr_t{0}) == 0);

        Call(cmd, kListSetPipelineState, cons_pso);
        Call(cmd, kListSetComputeRootUnorderedAccessView, uintptr_t{0}, static_cast<uintptr_t>(out_va));
        Call(cmd, kListSetComputeRootShaderResourceView, uintptr_t{1}, static_cast<uintptr_t>(payload_va));
        Call(cmd, kListExecuteIndirectV4, dispatch_sig, uintptr_t{1}, args_buf, uintptr_t{0},
             static_cast<void*>(nullptr), uintptr_t{0});

        // FlushPipelineX again before CopyBufferRegion readback!
        SX_REQUIRE(Call(cmd, kListFlushPipelineXV4, uintptr_t{0xFFFFFFFFu}, uintptr_t{0}, uintptr_t{0}) == 0);
        Call(cmd, kListCopyBufferRegion, rb_buf, uintptr_t{0}, out_buf, uintptr_t{0}, uintptr_t{48});
        SX_REQUIRE(Call(cmd, kListClose) == 0);

        void* lists[] = {cmd};
        Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
        const uint64_t fv = ++next_fence;
        SX_REQUIRE(Call(queue, kQueueSignalV4, fence, static_cast<uintptr_t>(fv)) == 0);
        WaitFence(fence, fv, fence_ev);

        const uint32_t* words = nullptr;
        SX_REQUIRE(Call(rb_buf, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &words) == 0 && words);
        for (uint32_t i = 0; i < 12; ++i) {
            SX_REQUIRE(words[i] == ((0x1000u + i * 7u) ^ 0xA5A50000u));
        }
        Call(rb_buf, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

        for (void* o : {srv_uav_heap, rb_buf, out_buf, payload_buf, args_buf, dispatch_sig, cons_pso, prod_pso, cs_rs}) {
            Call(o, kRelease);
        }

        SX_REQUIRE(DrainAndCountInfoQueueErrors(info_queue, "FlushPipelineX_UAV_Indirect_SRV") == 0);
        std::printf("test_d3d12_xs_com_sync: UAV -> FlushPipelineX -> IndirectArgs/SRV OK\n");
    }

    // =========================================================================
    // 4. True ID3D12Heap Placed-Resource Aliasing (CreatePlacedResource & CreatePlacedResourceX)
    // =========================================================================
    {
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = 256 * 1024;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        hd.Flags = static_cast<D3D12_HEAP_FLAGS>(0x84);  // Xbox title heap flags sanitized to Tier 2 shared heap

        const IID iid_heap = __uuidof(ID3D12Heap);
        void* heap = nullptr;
        SX_REQUIRE(Call(device, kDevCreateHeap, &hd, &iid_heap, &heap) == 0 && heap != nullptr);

        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 65536;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        void* placed_a = nullptr;
        void* placed_b = nullptr;
        // Place TWO distinct resource proxies at the EXACT SAME byte offset (0x10000) in `heap`!
        SX_REQUIRE(Call(device, kDevCreatePlacedResource, heap, uintptr_t{0x10000}, &bd,
                        uintptr_t{D3D12_RESOURCE_STATE_COMMON}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &placed_a) == 0 && placed_a);
        SX_REQUIRE(Call(device, kDevCreatePlacedResource, heap, uintptr_t{0x10000}, &bd,
                        uintptr_t{D3D12_RESOURCE_STATE_COMMON}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &placed_b) == 0 && placed_b);

        // Guest releases the heap wrapper early; placed resources keep underlying ID3D12Heap alive.
        SX_REQUIRE(Call(heap, kRelease) == 0);

        // Verify physical GPU VA identity on the shared ID3D12Heap!
        SX_REQUIRE(Call(placed_a, kResGetGPUVirtualAddress) == Call(placed_b, kResGetGPUVirtualAddress));

        D3D12_HEAP_PROPERTIES up_hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_HEAP_PROPERTIES rb_hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC small_bd = bd;
        small_bd.Width = 256;
        void* up_buf = nullptr;
        void* rb_buf = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &up_hp, uintptr_t{0}, &small_bd,
                        uintptr_t{D3D12_RESOURCE_STATE_GENERIC_READ}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &up_buf) == 0 && up_buf);
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, &rb_hp, uintptr_t{0}, &small_bd,
                        uintptr_t{D3D12_RESOURCE_STATE_COPY_DEST}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &rb_buf) == 0 && rb_buf);

        uint32_t* up_words = nullptr;
        SX_REQUIRE(Call(up_buf, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &up_words) == 0 && up_words);
        for (uint32_t i = 0; i < 64; ++i) up_words[i] = 0xCAFE0000u + i * 13u;
        Call(up_buf, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

        // Copy into placed_a ONLY, emit FlushPipelineX (global Aliasing barrier), read back from placed_b!
        SX_REQUIRE(Call(allocs[0], kAllocReset) == 0);
        SX_REQUIRE(Call(cmd, kListReset, allocs[0], static_cast<void*>(nullptr)) == 0);
        Call(cmd, kListCopyBufferRegion, placed_a, uintptr_t{0}, up_buf, uintptr_t{0}, uintptr_t{256});
        SX_REQUIRE(Call(cmd, kListFlushPipelineXV4, uintptr_t{0xFFFFFFFFu}, uintptr_t{0}, uintptr_t{0}) == 0);
        Call(cmd, kListCopyBufferRegion, rb_buf, uintptr_t{0}, placed_b, uintptr_t{0}, uintptr_t{256});
        SX_REQUIRE(Call(cmd, kListClose) == 0);

        void* lists[] = {cmd};
        Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
        const uint64_t fv = ++next_fence;
        SX_REQUIRE(Call(queue, kQueueSignalV4, fence, static_cast<uintptr_t>(fv)) == 0);
        WaitFence(fence, fv, fence_ev);

        const uint32_t* rb_words = nullptr;
        SX_REQUIRE(Call(rb_buf, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &rb_words) == 0 && rb_words);
        for (uint32_t i = 0; i < 64; ++i) {
            SX_REQUIRE(rb_words[i] == (0xCAFE0000u + i * 13u));
        }
        Call(rb_buf, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

        // Also verify CreatePlacedResourceX overlapping GPU aliasing at exact guest virtual address!
        void* guest_arena = VirtualAlloc(nullptr, 256 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        SX_REQUIRE(guest_arena != nullptr);
        const uintptr_t aligned_va =
            (reinterpret_cast<uintptr_t>(guest_arena) + 0xFFFFu) & ~static_cast<uintptr_t>(0xFFFFu);
        D3D12_RESOURCE_DESC px_bd = bd;
        px_bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        void* px_a = nullptr;
        void* px_b = nullptr;
        SX_REQUIRE(Call(device, kDevCreatePlacedResourceX, aligned_va, &px_bd,
                        uintptr_t{D3D12_RESOURCE_STATE_COMMON}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &px_a) == 0 && px_a);
        SX_REQUIRE(Call(device, kDevCreatePlacedResourceX, aligned_va, &px_bd,
                        uintptr_t{D3D12_RESOURCE_STATE_COMMON}, static_cast<void*>(nullptr),
                        static_cast<void*>(nullptr), &px_b) == 0 && px_b);

        for (uint32_t i = 0; i < 64; ++i) {
            reinterpret_cast<uint32_t*>(aligned_va)[i] = 0x55AA0000u + i * 3u;
        }

        // Write new GPU data into px_a (overwriting initial staged bytes) and read back from px_b!
        SX_REQUIRE(Call(allocs[0], kAllocReset) == 0);
        SX_REQUIRE(Call(cmd, kListReset, allocs[0], static_cast<void*>(nullptr)) == 0);
        Call(cmd, kListCopyBufferRegion, px_a, uintptr_t{0}, up_buf, uintptr_t{0}, uintptr_t{256});
        SX_REQUIRE(Call(cmd, kListFlushPipelineXV4, uintptr_t{0xFFFFFFFFu}, uintptr_t{0}, uintptr_t{0}) == 0);
        Call(cmd, kListCopyBufferRegion, rb_buf, uintptr_t{0}, px_b, uintptr_t{0}, uintptr_t{256});
        SX_REQUIRE(Call(cmd, kListClose) == 0);

        Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
        const uint64_t fv2 = ++next_fence;
        SX_REQUIRE(Call(queue, kQueueSignalV4, fence, static_cast<uintptr_t>(fv2)) == 0);
        WaitFence(fence, fv2, fence_ev);

        SX_REQUIRE(Call(rb_buf, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &rb_words) == 0 && rb_words);
        for (uint32_t i = 0; i < 64; ++i) {
            SX_REQUIRE(rb_words[i] == (0xCAFE0000u + i * 13u));
        }
        Call(rb_buf, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

        Call(px_b, kRelease);
        Call(px_a, kRelease);
        VirtualFree(guest_arena, 0, MEM_RELEASE);
        for (void* o : {rb_buf, up_buf, placed_b, placed_a}) Call(o, kRelease);

        SX_REQUIRE(DrainAndCountInfoQueueErrors(info_queue, "PlacedHeapAliasing") == 0);
        std::printf("test_d3d12_xs_com_sync: True ID3D12Heap placed-resource aliasing OK\n");
    }

    // =========================================================================
    // 5. Dynamic Swapchain Resize (1280x720 -> 1920x1080 + HDR10 Format Resize)
    //    & 6. WaitFrameEventX(VSYNC) Pacing (< 5% CPU, Zero Double-Throttle)
    // =========================================================================
    {
        _putenv_s("SCARLETTX_HEADLESS", "0");

        D3D12_HEAP_PROPERTIES def_hp{D3D12_HEAP_TYPE_DEFAULT};
        auto make_display_tex = [&](UINT w, UINT h, DXGI_FORMAT fmt) -> void* {
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = w;
            d.Height = h;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = fmt;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            void* tex = nullptr;
            SX_REQUIRE(Call(device, kDevCreateCommittedResource, &def_hp, uintptr_t{0}, &d,
                            uintptr_t{D3D12_RESOURCE_STATE_COMMON}, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr), &tex) == 0 && tex);
            return tex;
        };

        void* tex_720p = make_display_tex(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM);
        void* tex_1080p = make_display_tex(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
        void* tex_1080p_hdr10 = make_display_tex(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM);

        uint64_t plane[8] = {};
        plane[1] = 1;

        // Present 1280x720 -> creates swapchain at 1280x720
        plane[2] = reinterpret_cast<uint64_t>(tex_720p);
        SX_REQUIRE(Call(queue, kQueuePresentXV4, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        const HWND hwnd = FindOwnPresentWindow();
        SX_REQUIRE(hwnd != nullptr);

        // Present 1920x1080 -> dynamically calls IDXGISwapChain::ResizeBuffers(0, 1920, 1080, ...)
        plane[2] = reinterpret_cast<uint64_t>(tex_1080p);
        SX_REQUIRE(Call(queue, kQueuePresentXV4, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        SX_REQUIRE(DrainAndCountInfoQueueErrors(info_queue, "SwapChainResize_720p_to_1080p") == 0);

        // Present 1920x1080 R10G10B10A2_UNORM -> dynamically resizes swapchain format
        plane[2] = reinterpret_cast<uint64_t>(tex_1080p_hdr10);
        SX_REQUIRE(Call(queue, kQueuePresentXV4, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        SX_REQUIRE(DrainAndCountInfoQueueErrors(info_queue, "SwapChainResize_HDR10_Format") == 0);

        // Switch back to 1080p RGBA8 (completing format ResizeBuffers before timing)
        // and test steady-state WaitFrameEventX(VSYNC) cadence + CPU usage + zero double-throttle!
        plane[2] = reinterpret_cast<uint64_t>(tex_1080p);
        SX_REQUIRE(Call(queue, kQueuePresentXV4, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        SX_REQUIRE(DrainAndCountInfoQueueErrors(info_queue, "SwapChainResize_BackTo_RGBA8") == 0);

        _putenv_s("SCARLETTX_FCAP", "");
        SX_REQUIRE(Call(device, kDevSetFrameIntervalX, static_cast<void*>(nullptr),
                        uintptr_t{16667}, uintptr_t{2}, uintptr_t{0}) == 0);

        HANDLE vsync_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        SX_REQUIRE(vsync_ev != nullptr);
        uint64_t tok = 0;
        // Warm up 1 VSYNC interval to align to QPC VBlank grid
        SX_REQUIRE(Call(device, kDevWaitFrameEventX, uintptr_t{D3D12XBOX_FRAME_EVENT_VSYNC},
                        uintptr_t{INFINITE}, reinterpret_cast<uintptr_t>(vsync_ev),
                        uintptr_t{0}, &tok) == 0);
        SX_REQUIRE(WaitForSingleObject(vsync_ev, 0) == WAIT_OBJECT_0);

        // Use hardware cycle counters (QueryThreadCycleTime vs invariant TSC) rather than
        // GetProcessTimes/GetThreadTimes, whose 15.625 ms Windows tick quantum causes a
        // single 64 Hz tick over a 167 ms window to quantize to 9.31% (15.625 / 167.9).
        ULONG64 wait_cycles_accum = 0;
        ULONG64 loop_cycles_0 = 0, loop_cycles_1 = 0;
        SX_REQUIRE(QueryThreadCycleTime(GetCurrentThread(), &loop_cycles_0) != 0);
        const uint64_t tsc_0 = __rdtsc();
        const auto wall_t0 = std::chrono::steady_clock::now();

        constexpr int kVSyncFrames = 10;
        uint64_t prev_tok = tok;
        for (int i = 0; i < kVSyncFrames; ++i) {
            ULONG64 wc0 = 0, wc1 = 0;
            (void)QueryThreadCycleTime(GetCurrentThread(), &wc0);
            SX_REQUIRE(Call(device, kDevWaitFrameEventX, uintptr_t{D3D12XBOX_FRAME_EVENT_VSYNC},
                            uintptr_t{INFINITE}, reinterpret_cast<uintptr_t>(vsync_ev),
                            uintptr_t{0}, &tok) == 0);
            (void)QueryThreadCycleTime(GetCurrentThread(), &wc1);
            if (wc1 >= wc0) wait_cycles_accum += (wc1 - wc0);
            SX_REQUIRE(tok > prev_tok);
            prev_tok = tok;
            // Also call windowed PresentX every frame to prove ZERO double-throttling!
            SX_REQUIRE(Call(queue, kQueuePresentXV4, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        }

        const double wall_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - wall_t0)
                                   .count();
        const uint64_t tsc_1 = __rdtsc();
        SX_REQUIRE(QueryThreadCycleTime(GetCurrentThread(), &loop_cycles_1) != 0);
        CloseHandle(vsync_ev);

        const uint64_t tsc_delta = (tsc_1 > tsc_0) ? (tsc_1 - tsc_0) : 1ull;
        const double wait_cpu_pct = 100.0 * static_cast<double>(wait_cycles_accum) / static_cast<double>(tsc_delta);
        const double loop_cpu_pct = 100.0 * static_cast<double>(loop_cycles_1 - loop_cycles_0) / static_cast<double>(tsc_delta);

        std::printf("test_d3d12_xs_com_sync: %d VSYNC+PresentX frames in %.1f ms (wait_cpu=%.3f%%, thread_cpu=%.2f%%)\n",
                    kVSyncFrames, wall_ms, wait_cpu_pct, loop_cpu_pct);

        // 10 frames at 60 Hz = ~166.7 ms. Double-throttling would exceed ~320 ms.
        SX_REQUIRE(wall_ms >= 130.0 && wall_ms <= 275.0);
        SX_REQUIRE(wait_cpu_pct < 2.0);
        SX_REQUIRE(loop_cpu_pct < 25.0);

        const uint64_t present_drain_fv = ++next_fence;
        SX_REQUIRE(Call(queue, kQueueSignalV4, fence, static_cast<uintptr_t>(present_drain_fv)) == 0);
        WaitFence(fence, present_drain_fv, fence_ev);

        Call(tex_1080p_hdr10, kRelease);
        Call(tex_1080p, kRelease);
        Call(tex_720p, kRelease);

        SX_REQUIRE(DrainAndCountInfoQueueErrors(info_queue, "VSyncAndSwapChain") == 0);
    }

    // Cleanup all COM proxies and assert 0 final InfoQueue errors
    CloseHandle(fence_ev);
    Call(fence, kRelease);
    Call(cmd, kRelease);
    Call(allocs[0], kRelease);
    Call(allocs[1], kRelease);
    Call(queue, kRelease);
    Call(device, kRelease);

    if (info_queue) {
        SX_REQUIRE(DrainAndCountInfoQueueErrors(info_queue, "Shutdown") == 0);
        info_queue->Release();
    }

    scarlettx::FlightRecorder::Instance().Disable();
    std::printf("test_d3d12_xs_com_sync: ALL GATE M3 CHECKS PASSED\n");
    return 0;
#endif
}
