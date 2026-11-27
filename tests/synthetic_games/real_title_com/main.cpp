// Synthetic Real-Title Xbox Series X|S Executable (mainCRTStartup, NO ScarlettGuestMain export!).
// Exercises the exact SCARLETTX_REAL_TITLE=1 guest execution path with zero synthetic KernelX* imports:
//  - Clean GetCommandLineW (no host flags or --volume-key leak)
//  - C++ exception unwinding (.pdata)
//  - QueryApiImpl COM tables (XStore Product +0x20 inAppOfferToken ABI)
//  - XMemVirtualAlloc(MEM_GRAPHICS) unified CPU/GPU virtual memory (passed directly as Root CBV GPU VA)
//  - d3d12_xs.dll!D3D12XboxCreateDevice (SDK 0x20004) COM proxy:
//      * Valid retail QueryInterface (IUnknown, ID3D12Device, IDXGIDevice -> GetAdapter)
//      * D3D12SerializeRootSignature + CreateRootSignature (root CBV at b0)
//      * RTV ID3D12DescriptorHeap (MSVC C++ sret GetCPUDescriptorHandleForHeapStart)
//      * 256x256 ALLOW_DISPLAY (0x80) render target + multi-frame CommandList + FlushPipelineX
//        + ExecuteCommandLists + Fence::SetEventOnCompletion + WaitFrameEventX + Queue::PresentX loop
//  - xcrdapi.dll!DStorageGetFactory COM queue reading G:\MicrosoftGame.Config with dirty padding bytes,
//    synchronized via IDStorageQueueX::EnqueueSignal on the D3D12 fence (zero Sleep loops)
//  - xaudio2_9.dll!XAudio2Create COM voice + custom CLSID CreateFX (FallbackXapo GetRegistrationProperties)
//  - GameInputCreate COM IGameInput (retail IID QueryInterface + CreateDispatcher)
//  - HCHttpCallPerformAsync + XAsyncGetStatus(S_OK)
//  - Standard CRT exit -> ExitProcess(0) caught cleanly in-process by PeMapper!

#include "scarlettx/audio/xaudio2_abi.h"
#include "scarlettx/gdk/xgameruntime_abi.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/input/gameinput_abi.h"
#include "scarlettx/net/httpclient_abi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include <d3d12.h>
#include <windows.h>

extern "C" int32_t WINAPI DStorageGetFactory(const void* riid, void** out);

namespace {

#define RT_REQUIRE(cond)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "[real_title_com] CHECK FAILED at line %d: %s\n", __LINE__,  \
                         #cond);                                                              \
            return 100 + (__LINE__ % 100);                                                    \
        }                                                                                     \
    } while (0)

struct RawGuid {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t d4[8];
};

template <typename... Args>
intptr_t XboxVCall(void* obj, int slot, Args... args) {
    void** vtbl = *reinterpret_cast<void***>(obj);
    using Fn = intptr_t(SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtbl[slot])(obj, args...);
}

// Xbox d3d12_xs SDK 0x20004 COM vtable slot indices
constexpr int kComRelease = 2;
constexpr int kDevSetFrameIntervalX = 78;
constexpr int kDevWaitFrameEventX = 80;

constexpr int kQueueExecuteCommandLists = 10;
constexpr int kQueueSignalV4 = 16;
constexpr int kQueuePresentXV4 = 32;

constexpr int kAllocReset = 8;

constexpr int kCmdListClose = 9;
constexpr int kCmdListReset = 10;
constexpr int kCmdListResourceBarrier = 26;
constexpr int kCmdListSetGraphicsRootSignature = 34;
constexpr int kCmdListSetGraphicsRootCBV = 42;
constexpr int kCmdListOMSetRenderTargets = 50;
constexpr int kCmdListClearRenderTargetView = 52;
constexpr int kCmdListFlushPipelineXV4 = 69;

constexpr int kFenceGetCompletedValue = 8;
constexpr int kFenceSetEventOnCompletion = 9;

uint32_t ReadTargetFrameCount() {
    char buf[32] = {};
    const DWORD n = GetEnvironmentVariableA("SCARLETTX_MAX_FRAMES", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        const unsigned long parsed = std::strtoul(buf, nullptr, 10);
        if (parsed >= 1ul && parsed <= 1000ul) {
            return static_cast<uint32_t>(parsed);
        }
    }
    return 3u;
}

uint32_t ReadPreferWarpFlag() {
    char buf[16] = {};
    const DWORD n = GetEnvironmentVariableA("SCARLETTX_PREFER_WARP", buf, sizeof(buf));
    if (n > 0 && buf[0] == '0') {
        return 0u;
    }
    return 1u;
}

}  // namespace

int main() {
    // 1. Verify GetCommandLineW hook hides host "boot" / "--volume-key"
    const wchar_t* cmd_w = GetCommandLineW();
    RT_REQUIRE(cmd_w != nullptr);
    const std::wstring cmd_str(cmd_w);
    RT_REQUIRE(cmd_str.find(L"--volume-key") == std::wstring::npos);

    // 2. Verify .pdata exception table unwinding works cleanly in-process
    bool caught_cpp_exception = false;
    try {
        throw std::runtime_error("scarlett_pdata_unwind_ok");
    } catch (const std::runtime_error& e) {
        caught_cpp_exception = (std::strcmp(e.what(), "scarlett_pdata_unwind_ok") == 0);
    }
    RT_REQUIRE(caught_cpp_exception);

    // 3. Initialize GDK & test QueryApiImpl COM tables (including XStoreProduct +0x20 ABI!)
    RT_REQUIRE(XGameRuntimeInitialize() == 0);

    static constexpr RawGuid kXStoreProviderGuid = {
        0xCE8E7A26u, 0x2A0Eu, 0x4982u, {0xA2u, 0x37u, 0x71u, 0xF9u, 0x67u, 0xB0u, 0x5Cu, 0x3Au}};
    static constexpr RawGuid kIUnknownGuid = {
        0x00000000u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};
    void* store_tbl = nullptr;
    RT_REQUIRE(QueryApiImpl(&kXStoreProviderGuid, &kXStoreProviderGuid, &store_tbl) == 0 &&
               store_tbl != nullptr);
    {
        XStoreContextHandle sctx = nullptr;
        RT_REQUIRE(XStoreCreateContext(nullptr, &sctx) == 0 && sctx != nullptr);
        XAsyncBlock q_ab{};
        RT_REQUIRE(XStoreQueryEntitledProductsAsync(sctx, 0xFFu, 16u, &q_ab) == 0);
        RT_REQUIRE(XAsyncGetStatus(&q_ab, true) == 0);
        XStoreProductQueryHandle qh = nullptr;
        RT_REQUIRE(XStoreQueryEntitledProductsResult(&q_ab, &qh) == 0 && qh != nullptr);
        bool enum_called = false;
        RT_REQUIRE(XStoreEnumerateProductsQuery(
                       qh, &enum_called,
                       [](const XStoreProduct* prod, void* ctx) -> bool {
                           if (!prod || !prod->storeId || !prod->inAppOfferToken || !prod->linkUri) {
                               return false;
                           }
                           *static_cast<bool*>(ctx) = (std::strlen(prod->storeId) > 0);
                           return true;
                       }) == 0);
        char dlc_env_buf[16] = {};
        if (GetEnvironmentVariableA("SCARLETTX_DLC_ROOT", dlc_env_buf, sizeof(dlc_env_buf)) > 0 &&
            dlc_env_buf[0] != '\0') {
            RT_REQUIRE(enum_called);
        }
        XStoreCloseProductsQueryHandle(qh);
        XStoreCloseContextHandle(sctx);
        using FnTblRel = uint32_t(WINAPI*)(void*);
        reinterpret_cast<FnTblRel>((*reinterpret_cast<void* const* const*>(store_tbl))[2])(store_tbl);
    }

    // 4. Unified Memory: XMemVirtualAlloc with MEM_GRAPHICS (0x04000000) — no synthetic KernelX* imports!
    constexpr uint32_t kXboxMemGraphics = 0x04000000u;
    void* umem = XMemVirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT | kXboxMemGraphics,
                                  0, PAGE_READWRITE);
    RT_REQUIRE(umem != nullptr);
    float* cb_floats = static_cast<float*>(umem);
    const D3D12_GPU_VIRTUAL_ADDRESS umem_gpu_va = reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(umem);

    // 5. d3d12_xs Real-Title COM Device, Queue, CommandList, Fence & DirectStorage Fence Sync
    D3D12XBOX_CREATE_DEVICE_PARAMETERS d3d_params{};
    d3d_params.Version = 0x20004u;
    d3d_params.PreferWarp = ReadPreferWarpFlag();
    ID3D12Device* dev = nullptr;
    const GUID iid_dev = __uuidof(ID3D12Device);
    if (D3D12XboxCreateDevice(nullptr, &d3d_params, &iid_dev, reinterpret_cast<void**>(&dev)) < 0 || !dev) {
        d3d_params.PreferWarp = 1u;
        RT_REQUIRE(D3D12XboxCreateDevice(nullptr, &d3d_params, &iid_dev,
                                         reinterpret_cast<void**>(&dev)) == 0 &&
                   dev != nullptr);
    }

    // Retail COM QueryInterface checks (IUnknown + IDXGIDevice -> GetAdapter)
    {
        IUnknown* unk = nullptr;
        RT_REQUIRE(SUCCEEDED(dev->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&unk))) && unk != nullptr);
        unk->Release();

        static constexpr GUID kIidIDXGIDevice = {
            0x54EC77FAu, 0x1377u, 0x44E6u, {0x8Cu, 0x32u, 0x88u, 0xFDu, 0x5Fu, 0x44u, 0xC8u, 0x4Cu}};
        void* dxgi_dev = nullptr;
        RT_REQUIRE(SUCCEEDED(dev->QueryInterface(kIidIDXGIDevice, &dxgi_dev)) && dxgi_dev != nullptr);
        void* dxgi_adapter = nullptr;
        RT_REQUIRE(XboxVCall(dxgi_dev, 7 /* IDXGIDevice::GetAdapter */, &dxgi_adapter) == 0 && dxgi_adapter != nullptr);
        XboxVCall(dxgi_adapter, kComRelease);
        XboxVCall(dxgi_dev, kComRelease);
    }

    ID3D12Fence* fence = nullptr;
    RT_REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence != nullptr);
    HANDLE fence_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    RT_REQUIRE(fence_ev != nullptr);
    uint64_t fence_val = 0;

    auto wait_for_fence = [&](uint64_t target) -> bool {
        if (static_cast<uint64_t>(XboxVCall(fence, kFenceGetCompletedValue)) < target) {
            if (XboxVCall(fence, kFenceSetEventOnCompletion, static_cast<uintptr_t>(target),
                          reinterpret_cast<uintptr_t>(fence_ev)) != 0) {
                return false;
            }
            if (WaitForSingleObject(fence_ev, 5000) != WAIT_OBJECT_0) {
                return false;
            }
        }
        return static_cast<uint64_t>(XboxVCall(fence, kFenceGetCompletedValue)) >= target;
    };

    // 6. xcrdapi.dll COM DirectStorage reading G:\MicrosoftGame.Config with dirty padding at +0x12..+0x17,
    //    synchronized via IDStorageQueueX::EnqueueSignal on our D3D12 fence (zero Sleep!)
    void* ds_fac = nullptr;
    RT_REQUIRE(DStorageGetFactory(&kIUnknownGuid, &ds_fac) == 0 && ds_fac != nullptr);
    {
        auto* fvtbl = *reinterpret_cast<void* const* const*>(ds_fac);
        struct DsQueueDesc {
            uint64_t source_type;
            uint16_t capacity;
            int8_t priority;
            const char* name;
        } qd{0, 128, 0, "RealTitleQueue"};
        using FnCreateQ = int32_t(WINAPI*)(void*, const DsQueueDesc*, const void*, void**);
        using FnOpenF = int32_t(WINAPI*)(void*, const wchar_t*, const void*, void**);
        using FnRel = uint32_t(WINAPI*)(void*);
        void* ds_q = nullptr;
        void* ds_file = nullptr;
        RT_REQUIRE(reinterpret_cast<FnCreateQ>(fvtbl[3])(ds_fac, &qd, &kIUnknownGuid, &ds_q) == 0 && ds_q);
        RT_REQUIRE(reinterpret_cast<FnOpenF>(fvtbl[4])(
                       ds_fac, L"G:\\MicrosoftGame.Config", &kIUnknownGuid, &ds_file) == 0 &&
                   ds_file);

        char read_buf[64]{};
        struct DsReq {
            uint64_t options;
            void* destination;
            uint16_t destination_page_offset;
            uint16_t dirty_pad[3];
            uint32_t destination_size;
            uint32_t reserved;
            void* source;
            uint64_t file_offset;
            uint32_t source_size;
            uint32_t intermediate_size;
            uint64_t cancellation_tag;
            const char* name;
        } req{};
        req.options = 0;
        req.destination = read_buf;
        req.destination_page_offset = 0;
        req.dirty_pad[0] = 0xDEADu;
        req.dirty_pad[1] = 0xBEEFu;
        req.dirty_pad[2] = 0xCAFEu;
        req.destination_size = 38;
        req.source = ds_file;
        req.file_offset = 0;
        req.source_size = 38;
        req.name = "ReadManifestHead";

        void* ds_st = nullptr;
        using FnCreateSt = int32_t(WINAPI*)(void*, uint32_t, const char*, const void*, void**);
        RT_REQUIRE(reinterpret_cast<FnCreateSt>(fvtbl[5])(ds_fac, 4, "St", &kIUnknownGuid, &ds_st) == 0);

        auto* qvtbl = *reinterpret_cast<void* const* const*>(ds_q);
        using FnEnqReq = void(WINAPI*)(void*, const DsReq*);
        using FnEnqSt = void(WINAPI*)(void*, void*, uint32_t);
        using FnEnqSig = void(WINAPI*)(void*, void*, uint64_t);
        using FnSubmit = void(WINAPI*)(void*);
        const uint64_t ds_fence_target = ++fence_val;
        reinterpret_cast<FnEnqReq>(qvtbl[3])(ds_q, &req);
        reinterpret_cast<FnEnqSt>(qvtbl[4])(ds_q, ds_st, 0);
        reinterpret_cast<FnEnqSig>(qvtbl[5])(ds_q, fence, ds_fence_target);
        reinterpret_cast<FnSubmit>(qvtbl[6])(ds_q);

        RT_REQUIRE(wait_for_fence(ds_fence_target));

        auto* svtbl = *reinterpret_cast<void* const* const*>(ds_st);
        using FnIsComp = int32_t(WINAPI*)(void*, uint32_t);
        using FnGetHr = int32_t(WINAPI*)(void*, uint32_t);
        RT_REQUIRE(reinterpret_cast<FnIsComp>(svtbl[3])(ds_st, 0) == 1);
        RT_REQUIRE(reinterpret_cast<FnGetHr>(svtbl[4])(ds_st, 0) == 0);
        RT_REQUIRE(std::memcmp(read_buf, "<?xml", 5) == 0);

        reinterpret_cast<FnRel>(svtbl[2])(ds_st);
        reinterpret_cast<FnRel>((*reinterpret_cast<void* const* const*>(ds_file))[2])(ds_file);
        reinterpret_cast<FnRel>(qvtbl[2])(ds_q);
        reinterpret_cast<FnRel>(fvtbl[2])(ds_fac);
    }

    // 7. Real-Title D3D12X COM CommandList + Root CBV (MEM_GRAPHICS) + PresentX Frame Loop
    {
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        RT_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))) && queue != nullptr);

        ID3D12CommandAllocator* cmd_alloc = nullptr;
        RT_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_alloc))) &&
                   cmd_alloc != nullptr);

        ID3D12GraphicsCommandList* cmd_list = nullptr;
        RT_REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc, nullptr,
                                                    IID_PPV_ARGS(&cmd_list))) &&
                   cmd_list != nullptr);

        // Serialize & create a Root Signature with 1 Root CBV at register(b0) via retail d3d12_xs export
        D3D12_ROOT_PARAMETER root_param{};
        root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_param.Descriptor.ShaderRegister = 0;
        root_param.Descriptor.RegisterSpace = 0;
        root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 1;
        rs_desc.pParameters = &root_param;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* rs_blob = nullptr;
        RT_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rs_blob, nullptr)) &&
                   rs_blob != nullptr);
        ID3D12RootSignature* root_sig = nullptr;
        RT_REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                      IID_PPV_ARGS(&root_sig))) &&
                   root_sig != nullptr);
        rs_blob->Release();

        // RTV descriptor heap (exercising MSVC C++ sret ABI on GetCPUDescriptorHandleForHeapStart)
        D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
        rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_hd.NumDescriptors = 1;
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        RT_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))) && rtv_heap != nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        RT_REQUIRE(rtv_cpu.ptr != 0);

        // 256x256 RGBA8 displayable backbuffer with Xbox ALLOW_DISPLAY (0x80) heap flag
        D3D12_HEAP_PROPERTIES rt_hp{};
        rt_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = 256;
        rt_desc.Height = 256;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        ID3D12Resource* render_target = nullptr;
        const HRESULT rt_hr = dev->CreateCommittedResource(
            &rt_hp, D3D12_HEAP_FLAG_ALLOW_DISPLAY,
            &rt_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&render_target));
        if (FAILED(rt_hr) || !render_target) {
            std::fprintf(stderr, "[real_title_com] CreateCommittedResource failed: hr=0x%08X\n",
                         static_cast<unsigned>(rt_hr));
        }
        RT_REQUIRE(SUCCEEDED(rt_hr) && render_target != nullptr);
        dev->CreateRenderTargetView(render_target, nullptr, rtv_cpu);

        // Set 1 ms frame pacing interval for fast deterministic CI execution
        RT_REQUIRE(XboxVCall(dev, kDevSetFrameIntervalX, static_cast<void*>(nullptr),
                             uintptr_t{1000}, uintptr_t{2}, uintptr_t{0}) == 0);

        const uint32_t total_frames = ReadTargetFrameCount();
        uint64_t last_frame_token = 0;

        for (uint32_t frame_idx = 0; frame_idx < total_frames; ++frame_idx) {
            uint64_t frame_token = 0;
            RT_REQUIRE(XboxVCall(dev, kDevWaitFrameEventX, uintptr_t{0}, uintptr_t{INFINITE},
                                 static_cast<void*>(nullptr), uintptr_t{0}, &frame_token) == 0);
            RT_REQUIRE(frame_token > last_frame_token);
            last_frame_token = frame_token;

            if (frame_idx > 0) {
                RT_REQUIRE(XboxVCall(cmd_alloc, kAllocReset) == 0);
                RT_REQUIRE(XboxVCall(cmd_list, kCmdListReset, cmd_alloc, static_cast<void*>(nullptr)) == 0);
            }

            // Update unified MEM_GRAPHICS constant buffer for this frame
            cb_floats[0] = 0.15f + 0.20f * static_cast<float>(frame_idx);
            cb_floats[1] = 0.35f;
            cb_floats[2] = 0.65f;
            cb_floats[3] = 1.0f;

            D3D12_RESOURCE_BARRIER to_rt{};
            to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_rt.Transition.pResource = render_target;
            to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            XboxVCall(cmd_list, kCmdListResourceBarrier, uintptr_t{1}, &to_rt);

            XboxVCall(cmd_list, kCmdListSetGraphicsRootSignature, root_sig);
            XboxVCall(cmd_list, kCmdListSetGraphicsRootCBV, uintptr_t{0}, static_cast<uintptr_t>(umem_gpu_va));
            XboxVCall(cmd_list, kCmdListOMSetRenderTargets, uintptr_t{1}, &rtv_cpu, uintptr_t{0},
                      static_cast<void*>(nullptr));

            const float clear_rgba[4] = {cb_floats[0], cb_floats[1], cb_floats[2], cb_floats[3]};
            XboxVCall(cmd_list, kCmdListClearRenderTargetView, rtv_cpu.ptr, clear_rgba, uintptr_t{0},
                      static_cast<void*>(nullptr));
            RT_REQUIRE(XboxVCall(cmd_list, kCmdListFlushPipelineXV4, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}) == 0);

            D3D12_RESOURCE_BARRIER to_present = to_rt;
            to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            XboxVCall(cmd_list, kCmdListResourceBarrier, uintptr_t{1}, &to_present);
            RT_REQUIRE(XboxVCall(cmd_list, kCmdListClose) == 0);

            void* submit_lists[1] = {cmd_list};
            XboxVCall(queue, kQueueExecuteCommandLists, uintptr_t{1}, submit_lists);

            const uint64_t ftarget = ++fence_val;
            RT_REQUIRE(XboxVCall(queue, kQueueSignalV4, fence, static_cast<uintptr_t>(ftarget)) == 0);
            RT_REQUIRE(wait_for_fence(ftarget));

            uint64_t plane[8] = {};
            plane[1] = 1;
            plane[2] = reinterpret_cast<uint64_t>(render_target);
            RT_REQUIRE(XboxVCall(queue, kQueuePresentXV4, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        }

        render_target->Release();
        rtv_heap->Release();
        root_sig->Release();
        XboxVCall(cmd_list, kComRelease);
        XboxVCall(cmd_alloc, kComRelease);
        XboxVCall(queue, kComRelease);
    }

    CloseHandle(fence_ev);
    XboxVCall(fence, kComRelease);
    dev->Release();

    // 8. XAudio2Create + custom CLSID CreateFX (FallbackXapo GetRegistrationProperties)
    void* xa_eng = nullptr;
    RT_REQUIRE(XAudio2Create(&xa_eng, 0, 0) == 0 && xa_eng != nullptr);
    {
        static constexpr RawGuid kCustomXboxXapoClsid = {
            0x99887766u, 0x5544u, 0x3322u, {0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u}};
        void* fx = nullptr;
        RT_REQUIRE(CreateFX(&kCustomXboxXapoClsid, &fx, nullptr, 0) == 0 && fx != nullptr);
        auto* fx_vtbl = *reinterpret_cast<void* const* const*>(fx);
        void* reg_props = nullptr;
        using FnGetRegProps = int32_t(WINAPI*)(void*, void**);
        RT_REQUIRE(reinterpret_cast<FnGetRegProps>(fx_vtbl[3])(fx, &reg_props) == 0 && reg_props != nullptr);
        CoTaskMemFree(reg_props);
        using FnRel = uint32_t(WINAPI*)(void*);
        reinterpret_cast<FnRel>(fx_vtbl[2])(fx);
        reinterpret_cast<FnRel>((*reinterpret_cast<void* const* const*>(xa_eng))[2])(xa_eng);
    }

    // 9. GameInput COM QueryInterface (retail IUnknown + IGameInput IIDs) & CreateDispatcher
    void* gi = nullptr;
    RT_REQUIRE(GameInputCreate(reinterpret_cast<uint64_t*>(&gi)) == 0 && gi != nullptr);
    {
        auto* gi_vtbl = *reinterpret_cast<void* const* const*>(gi);
        static constexpr RawGuid kIidIGameInputV0 = {
            0x11BE2A7Eu, 0x4254u, 0x445Au, {0x9Cu, 0x09u, 0xFFu, 0xC4u, 0x0Fu, 0x00u, 0x69u, 0x18u}};
        using FnQI = int32_t(WINAPI*)(void*, const void*, void**);
        using FnDisp = int32_t(WINAPI*)(void*, void**);
        using FnRel = uint32_t(WINAPI*)(void*);
        void* qi_out = nullptr;
        RT_REQUIRE(reinterpret_cast<FnQI>(gi_vtbl[0])(gi, &kIidIGameInputV0, &qi_out) == 0 && qi_out == gi);
        reinterpret_cast<FnRel>(gi_vtbl[2])(qi_out);
        void* disp = nullptr;
        RT_REQUIRE(reinterpret_cast<FnDisp>(gi_vtbl[15])(gi, &disp) == 0 && disp != nullptr);
        reinterpret_cast<FnRel>((*reinterpret_cast<void* const* const*>(disp))[2])(disp);
        reinterpret_cast<FnRel>(gi_vtbl[2])(gi);
    }

    // 10. libHttpClient PerformAsync + XAsyncGetStatus(S_OK)
    RT_REQUIRE(HCInitialize(nullptr) == 0);
    {
        HCCallHandle call = nullptr;
        RT_REQUIRE(HCHttpCallCreate(&call) == 0 && call != nullptr);
        RT_REQUIRE(HCHttpCallRequestSetUrl(call, "GET", "https://title.xboxlive.com/ping") == 0);
        XAsyncBlock hc_ab{};
        RT_REQUIRE(HCHttpCallPerformAsync(call, &hc_ab) == 0);
        RT_REQUIRE(XAsyncGetStatus(&hc_ab, true) == 0);
        HCHttpCallCloseHandle(call);
    }
    HCCleanup();

    XMemVirtualFree(umem, 0, MEM_RELEASE);
    XGameRuntimeUninitialize();

    std::printf("[real_title_com] Real-title COM PresentX loop & CRT exit verified cleanly!\n");
    return 0;
}
