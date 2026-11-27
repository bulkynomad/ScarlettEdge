// Synthetic Real-Title Xbox Series X|S Executable (mainCRTStartup, NO ScarlettGuestMain export!).
// Exercises the exact SCARLETTX_REAL_TITLE=1 guest execution path:
//  - Clean GetCommandLineW (no host flags or --volume-key leak)
//  - C++ exception unwinding (.pdata)
//  - QueryApiImpl COM tables (XUser, XGameSave, XStore Product +0x20 inAppOfferToken ABI)
//  - XMemVirtualAlloc(MEM_GRAPHICS) -> KernelXMapVirtualToGpu
//  - d3d12_xs.dll!D3D12XboxCreateDevice COM proxy (InfoQueue E_NOINTERFACE, sret DescHeap, unified CBV VA)
//  - xcrdapi.dll!DStorageGetFactory COM queue with dirty destination_pad bytes + G:\ VFS read
//  - xaudio2_9.dll!XAudio2Create COM voice + custom CLSID CreateFX (FallbackXapo GetRegistrationProperties)
//  - GameInputCreate COM IGameInput (QueryInterface validation + CreateDispatcher)
//  - HCHttpCallPerformAsync + XAsyncGetStatus(S_OK)
//  - Standard CRT exit -> ExitProcess(0) caught cleanly in-process by PeMapper!

#include "scarlettx/audio/xaudio2_abi.h"
#include "scarlettx/gdk/xgameruntime_abi.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/input/gameinput_abi.h"
#include "scarlettx/kernel/kernelx_abi.h"
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
                           // Dereference inAppOfferToken (+0x20) & linkUri (+0x28) per GDK ABI!
                           if (!prod || !prod->storeId || !prod->inAppOfferToken || !prod->linkUri) {
                               return false;
                           }
                           *static_cast<bool*>(ctx) = (std::strlen(prod->storeId) > 0);
                           return true;
                       }) == 0);
        RT_REQUIRE(enum_called);
        XStoreCloseProductsQueryHandle(qh);
        XStoreCloseContextHandle(sctx);
        using FnTblRel = uint32_t(WINAPI*)(void*);
        reinterpret_cast<FnTblRel>((*reinterpret_cast<void* const* const*>(store_tbl))[2])(store_tbl);
    }

    // 4. Unified Memory: XMemVirtualAlloc with MEM_GRAPHICS (0x04000000) -> KernelX GPU VA!
    constexpr uint32_t kXboxMemGraphics = 0x04000000u;
    void* umem = XMemVirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT | kXboxMemGraphics,
                                  0, PAGE_READWRITE);
    RT_REQUIRE(umem != nullptr);
    float* cb_floats = static_cast<float*>(umem);
    cb_floats[0] = 1.0f;
    cb_floats[1] = 0.5f;
    cb_floats[2] = 0.25f;
    cb_floats[3] = 1.0f;
    uint64_t umem_gpu_va = 0;
    RT_REQUIRE(KernelXMapVirtualToGpu(umem, &umem_gpu_va) == 0 && umem_gpu_va != 0);

    // 5. d3d12_xs Real-Title COM Proxy
    D3D12XBOX_CREATE_DEVICE_PARAMETERS d3d_params{};
    d3d_params.Version = 0x20004u;
    d3d_params.PreferWarp = 1u;
    ID3D12Device* dev = nullptr;
    const GUID iid_dev = __uuidof(ID3D12Device);
    RT_REQUIRE(D3D12XboxCreateDevice(nullptr, &d3d_params, &iid_dev,
                                     reinterpret_cast<void**>(&dev)) == 0 &&
               dev != nullptr);

    // Querying ID3D12InfoQueue MUST return E_NOINTERFACE cleanly instead of returning `dev` and crashing!
    static constexpr GUID kIidInfoQueue = {
        0x0742A90Bu, 0xC387u, 0x483Fu, {0xB9u, 0x46u, 0x30u, 0xA7u, 0xE4u, 0xE6u, 0x14u, 0x58u}};
    void* info_q = reinterpret_cast<void*>(0x1);
    RT_REQUIRE(dev->QueryInterface(kIidInfoQueue, &info_q) < 0 && info_q == nullptr);

    // MSVC C++ sret ABI on ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart()
    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_hd.NumDescriptors = 2;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    RT_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))) && rtv_heap != nullptr);
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu_start = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    RT_REQUIRE(cpu_start.ptr != 0);

    rtv_heap->Release();
    dev->Release();

    // 6. xcrdapi.dll COM DirectStorage reading G:\MicrosoftGame.Config with dirty padding at +0x12..+0x17
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
        req.dirty_pad[0] = 0xDEADu;  // Non-zero uninitialized stack padding must NOT corrupt dst_ptr!
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
        using FnSubmit = void(WINAPI*)(void*);
        reinterpret_cast<FnEnqReq>(qvtbl[3])(ds_q, &req);
        reinterpret_cast<FnEnqSt>(qvtbl[4])(ds_q, ds_st, 0);
        reinterpret_cast<FnSubmit>(qvtbl[6])(ds_q);

        auto* svtbl = *reinterpret_cast<void* const* const*>(ds_st);
        using FnIsComp = int32_t(WINAPI*)(void*, uint32_t);
        using FnGetHr = int32_t(WINAPI*)(void*, uint32_t);
        for (int spin = 0; spin < 500 && !reinterpret_cast<FnIsComp>(svtbl[3])(ds_st, 0); ++spin) {
            Sleep(5);
        }
        RT_REQUIRE(reinterpret_cast<FnIsComp>(svtbl[3])(ds_st, 0) == 1);
        RT_REQUIRE(reinterpret_cast<FnGetHr>(svtbl[4])(ds_st, 0) == 0);
        RT_REQUIRE(std::memcmp(read_buf, "<?xml", 5) == 0);

        reinterpret_cast<FnRel>(svtbl[2])(ds_st);
        reinterpret_cast<FnRel>((*reinterpret_cast<void* const* const*>(ds_file))[2])(ds_file);
        reinterpret_cast<FnRel>(qvtbl[2])(ds_q);
        reinterpret_cast<FnRel>(fvtbl[2])(ds_fac);
    }

    // 7. XAudio2Create + custom CLSID CreateFX (FallbackXapo GetRegistrationProperties)
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

    // 8. GameInput COM QueryInterface & CreateDispatcher
    void* gi = nullptr;
    RT_REQUIRE(GameInputCreate(reinterpret_cast<uint64_t*>(&gi)) == 0 && gi != nullptr);
    {
        auto* gi_vtbl = *reinterpret_cast<void* const* const*>(gi);
        static constexpr RawGuid kIidIMarshal = {
            0x00000003u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};
        void* bad_qi = reinterpret_cast<void*>(0x1);
        using FnQI = int32_t(WINAPI*)(void*, const void*, void**);
        using FnDisp = int32_t(WINAPI*)(void*, void**);
        using FnRel = uint32_t(WINAPI*)(void*);
        RT_REQUIRE(reinterpret_cast<FnQI>(gi_vtbl[0])(gi, &kIidIMarshal, &bad_qi) < 0 && bad_qi == nullptr);
        void* disp = nullptr;
        RT_REQUIRE(reinterpret_cast<FnDisp>(gi_vtbl[15])(gi, &disp) == 0 && disp != nullptr);
        reinterpret_cast<FnRel>((*reinterpret_cast<void* const* const*>(disp))[2])(disp);
        reinterpret_cast<FnRel>(gi_vtbl[2])(gi);
    }

    // 9. libHttpClient PerformAsync + XAsyncGetStatus(S_OK)
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

    std::printf("[real_title_com] Real-title COM & CRT exit path verified cleanly!\n");
    return 0;  // Returns into real MSVC mainCRTStartup -> ExitProcess(0)!
}
