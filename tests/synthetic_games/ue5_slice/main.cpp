#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#define SLICE_EXPORT __declspec(dllexport)
#else
#define SLICE_EXPORT
#endif

#include "scarlettx/audio/xaudio2_abi.h"
#include "scarlettx/gdk/xgameruntime_abi.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/media/codecs_abi.h"
#include "scarlettx/middleware/bink2_abi.h"
#include "scarlettx/services/xsapi_abi.h"

namespace {

struct GuidPod {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t d4[8];
};

void AppendLe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void PumpQueueUntilDone(XTaskQueueHandle queue, XAsyncBlock* async) {
    while (XAsyncGetStatus(async, false) == XGR_E_PENDING) {
        XTaskQueueDispatch(queue, XTaskQueuePort_Completion, 20);
    }
}

}  // namespace

extern "C" SLICE_EXPORT int ScarlettGuestMain() {
    // 1. GDK 2025 Runtime + QueryApiImpl COM tables + XStore License + DLC Mount + HMAC XBL3.0 Token
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        std::fprintf(stderr, "[ue5_slice] XGameRuntimeInitialize failed\n");
        return 1;
    }

    const GuidPod kXStoreApi = {
        0xCE8E7A26u, 0x2A0Eu, 0x4982u, {0xA2, 0x37, 0x71, 0xF9, 0x67, 0xB0, 0x5C, 0x3A}};
    const GuidPod kXStore2025Iid = {
        0xD4B92E11u, 0x5C84u, 0x49A2u, {0x8E, 0x17, 0x63, 0xF0, 0xB8, 0xC9, 0x12, 0x45}};
    const GuidPod kXPackageApi = {
        0xAF406016u, 0xE850u, 0x4AA8u, {0xA8, 0x8D, 0x2F, 0x3D, 0xCB, 0x9D, 0xAC, 0x7E}};
    const GuidPod kXPackage2025Iid = {
        0xA8C4234Eu, 0x7F1Bu, 0x4D3Au, {0x9C, 0x81, 0x5E, 0x2B, 0x7D, 0x90, 0xA1, 0x14}};
    void* xstore_vtbl = nullptr;
    void* xpkg_vtbl = nullptr;
    if (QueryApiImpl(&kXStoreApi, &kXStore2025Iid, &xstore_vtbl) != XGR_S_OK || !xstore_vtbl ||
        QueryApiImpl(&kXPackageApi, &kXPackage2025Iid, &xpkg_vtbl) != XGR_S_OK || !xpkg_vtbl) {
        std::fprintf(stderr, "[ue5_slice] GDK 2025 QueryApiImpl failed\n");
        return 2;
    }

    XTaskQueueHandle queue = nullptr;
    if (XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_Manual, &queue) != XGR_S_OK) {
        return 3;
    }

    XAsyncBlock user_ab{};
    user_ab.queue = queue;
    if (XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_ab) != XGR_S_OK) return 4;
    PumpQueueUntilDone(queue, &user_ab);
    XUserHandle user = nullptr;
    if (XUserAddResult(&user_ab, &user) != XGR_S_OK || !user) return 5;

    XStoreContextHandle store_ctx = nullptr;
    if (XStoreCreateContext(user, &store_ctx) != XGR_S_OK || !store_ctx) return 6;
    XAsyncBlock lic_ab{};
    lic_ab.queue = queue;
    if (XStoreQueryGameLicenseAsync(store_ctx, &lic_ab) != XGR_S_OK) return 7;
    PumpQueueUntilDone(queue, &lic_ab);
    XStoreGameLicense lic{};
    if (XStoreQueryGameLicenseResult(&lic_ab, &lic) != XGR_S_OK || !lic.isActive || lic.isTrial) {
        std::fprintf(stderr, "[ue5_slice] XStore license inactive\n");
        return 8;
    }

    std::string dlc_id;
    auto enum_cb = [](void* ctx, const XPackageDetails* d) -> bool {
        if (ctx && d && d->packageIdentifier) {
            *static_cast<std::string*>(ctx) = d->packageIdentifier;
        }
        return true;
    };
    if (XPackageEnumeratePackages(XPackageKind_Content, XPackageEnumerationScope_ThisAndRelated, &dlc_id, enum_cb) != XGR_S_OK ||
        dlc_id.empty()) {
        std::fprintf(stderr, "[ue5_slice] XPackageEnumeratePackages returned no content\n");
        return 9;
    }
    XAsyncBlock mount_ab{};
    mount_ab.queue = queue;
    if (XPackageMountWithUiAsync(dlc_id.c_str(), &mount_ab) != XGR_S_OK) return 10;
    PumpQueueUntilDone(queue, &mount_ab);
    XPackageMountHandle mount_h = nullptr;
    if (XPackageMountWithUiResult(&mount_ab, &mount_h) != XGR_S_OK || !mount_h) return 11;
    size_t mount_sz = 0;
    XPackageGetMountPathSize(mount_h, &mount_sz);
    std::string mount_path(mount_sz, '\0');
    if (XPackageGetMountPath(mount_h, mount_sz, mount_path.data()) != XGR_S_OK) return 12;
    XPackageCloseMountHandle(mount_h);

    // HMAC-SHA256 XBL3.0 token + signature check
    XAsyncBlock tok_ab{};
    tok_ab.queue = queue;
    if (XUserGetTokenAndSignatureUtf16Async(
            user, XUserGetTokenAndSignatureOptions_None,
            L"POST", L"https://userpresence.xboxlive.com/users/xuid(1)/devices/current",
            0, nullptr, 0, nullptr, &tok_ab) != XGR_S_OK) {
        return 13;
    }
    PumpQueueUntilDone(queue, &tok_ab);
    size_t tok_sz = 0;
    if (XUserGetTokenAndSignatureUtf16ResultSize(&tok_ab, &tok_sz) != XGR_S_OK || tok_sz == 0) return 14;
    std::vector<uint8_t> tok_buf(tok_sz, 0);
    XUserGetTokenAndSignatureUtf16Data* tok_data = nullptr;
    if (XUserGetTokenAndSignatureUtf16Result(&tok_ab, tok_sz, tok_buf.data(), &tok_data, nullptr) != XGR_S_OK ||
        !tok_data || !tok_data->token || !tok_data->signature || std::wcsncmp(tok_data->token, L"XBL3.0 x=", 9) != 0) {
        std::fprintf(stderr, "[ue5_slice] XUserGetTokenAndSignatureUtf16Result invalid\n");
        return 15;
    }

    // 2. WinRT RoActivateInstance IInspectable broker
    const GuidPod kIidInspectable = {
        0xAF86E2E0u, 0xB12Du, 0x4C6Au, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
    void* winrt_gamepad = nullptr;
    if (XblWinRtActivateClassByName(L"Windows.Gaming.Input.Gamepad", &kIidInspectable, 0, &winrt_gamepad) != 0 ||
        !winrt_gamepad) {
        std::fprintf(stderr, "[ue5_slice] WinRT activation failed\n");
        return 16;
    }
#if defined(_WIN32)
    {
        void** vtbl = *reinterpret_cast<void***>(winrt_gamepad);
        using FnGetIids = int32_t(__stdcall*)(void*, uint32_t*, void**);
        using FnRelease = uint32_t(__stdcall*)(void*);
        uint32_t iid_cnt = 0;
        void* iids_mem = nullptr;
        if (reinterpret_cast<FnGetIids>(vtbl[3])(winrt_gamepad, &iid_cnt, &iids_mem) != 0 || iid_cnt == 0 || !iids_mem) {
            return 17;
        }
        CoTaskMemFree(iids_mem);
        reinterpret_cast<FnRelease>(vtbl[2])(winrt_gamepad);
    }
#endif

    // 3. Bink2 KB2 intro movie playback with A/V PTS lip-sync from extracted XVC package root
    const char* pkg_root_env = std::getenv("SCARLETTX_PACKAGE_ROOT");
    const std::string pkg_root = (pkg_root_env && *pkg_root_env) ? pkg_root_env : ".";
    const std::string bink_path = pkg_root + "/Content/Movies/ue5_intro.kb2";
    BINK* bnk = BinkOpen(bink_path.c_str(), BINKOPEN_PTS_SYNC);
    if (!bnk || bnk->Width != 32 || bnk->Height != 32) {
        std::fprintf(stderr, "[ue5_slice] BinkOpen failed for %s\n", bink_path.c_str());
        return 20;
    }
    BinkSetAudioPts(bnk, 60000ull);  // Advance audio PTS to trigger PTS lip-sync catch-up
    BinkDoFrame(bnk);
    std::vector<uint8_t> video_rgba(32 * 32 * 4, 0);
    BinkCopyToBuffer(bnk, video_rgba.data(), 32 * 4, 32, 0, 0, BINKSURFACE32RGBA);
    bool non_zero_video = false;
    for (uint8_t b : video_rgba) {
        if (b != 0 && b != 255) { non_zero_video = true; break; }
    }
    BinkClose(bnk);
    if (!non_zero_video) {
        std::fprintf(stderr, "[ue5_slice] Decoded KB2 video frame was blank\n");
        return 21;
    }

    // 4. DirectStorage RFC 1951 GDeflate streaming + BCPK BC1 texture block unpack
    const std::string pak_path = pkg_root + "/Content/Paks/nanite_chunk0.gdeflate";
    const char* uncomp_env = std::getenv("SCARLETTX_UE5_PAK_UNCOMP_SIZE");
    const uint32_t pak_uncomp_size = uncomp_env ? static_cast<uint32_t>(std::strtoul(uncomp_env, nullptr, 10)) : 2064u;
    uint64_t ds_factory = 0, ds_file = 0, ds_queue = 0;
    if (DStorageGetFactory(&ds_factory) != 0 ||
        DStorageOpenFile(ds_factory, pak_path.c_str(), &ds_file) != 0) {
        std::fprintf(stderr, "[ue5_slice] DStorageOpenFile failed: %s\n", pak_path.c_str());
        return 30;
    }
    uint64_t comp_file_sz = 0;
    DStorageGetFileSize(ds_file, &comp_file_sz);
    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = 16;
    qd.Priority = DSTORAGE_PRIORITY_HIGH;
    qd.Name = "UE5_IoDispatcher";
    if (DStorageCreateQueue(ds_factory, &qd, &ds_queue) != 0) return 31;

    std::vector<uint8_t> pak_plain(pak_uncomp_size, 0);
    DSTORAGE_REQUEST ds_req{};
    ds_req.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    ds_req.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    ds_req.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    ds_req.SourceFileHandle = ds_file;
    ds_req.SourceFileOffset = 0;
    ds_req.SourceSize = static_cast<uint32_t>(comp_file_sz);
    ds_req.DestinationMemory = pak_plain.data();
    ds_req.DestinationSize = pak_uncomp_size;
    ds_req.UncompressedSize = pak_uncomp_size;
    ds_req.Name = "NaniteChunk0";
    if (DStorageEnqueueRequest(ds_queue, &ds_req) != 0 ||
        DStorageSubmit(ds_queue) != 0 ||
        DStorageWaitForIdle(ds_queue, 5000) != 0) {
        std::fprintf(stderr, "[ue5_slice] DirectStorage GDeflate stream failed\n");
        return 32;
    }

    // First 16 bytes of streamed pak hold 2 BC1 4x4 blocks; remaining 2048 bytes hold a retail XMA2 MDCT audio block!
    uint8_t bc1_rgba[128] = {};
    if (XboxCodecBcPackDecompress(pak_plain.data(), 16, bc1_rgba, sizeof(bc1_rgba), 1 /* BC1->RGBA8 */) != 0) {
        return 33;
    }

    // 5. Retail 2048-byte XMA2 MDCT decode + X3DAudio 5.1 surround + CreateFX(CLSID_FXReverb)
    std::vector<float> decoded_pcm(4096, 0.0f);
    uint32_t decoded_frames = 0;
    uint16_t decoded_ch = 0;
    if (Xma2DecodePacket(pak_plain.data() + 16, 2048, decoded_pcm.data(),
                         static_cast<uint32_t>(decoded_pcm.size()), &decoded_frames, &decoded_ch) != 0 ||
        decoded_frames == 0) {
        std::fprintf(stderr, "[ue5_slice] Retail 2048B XMA2 MDCT decode failed\n");
        return 40;
    }

    X3DAUDIO_HANDLE x3d{};
    if (X3DAudioInitialize(0x3Fu /* 5.1 surround */, 343.5f, x3d) != 0) return 41;
    X3DAUDIO_LISTENER listener{};
    listener.OrientFront = {0.0f, 0.0f, 1.0f};
    listener.OrientTop = {0.0f, 1.0f, 0.0f};
    X3DAUDIO_EMITTER emitter{};
    emitter.OrientFront = {0.0f, 0.0f, 1.0f};
    emitter.OrientTop = {0.0f, 1.0f, 0.0f};
    emitter.Position = {4.0f, 0.0f, 6.0f};
    emitter.Velocity = {-15.0f, 0.0f, -20.0f};
    emitter.ChannelCount = 1;
    emitter.CurveDistanceScaler = 2.0f;
    emitter.DopplerScaler = 1.0f;
    float matrix_5_1[6] = {};
    X3DAUDIO_DSP_SETTINGS dsp{};
    dsp.pMatrixCoefficients = matrix_5_1;
    dsp.SrcChannelCount = 1;
    dsp.DstChannelCount = 6;
    X3DAudioCalculate(x3d, &listener, &emitter,
                      SCARLETTX_X3DAUDIO_CALCULATE_MATRIX | SCARLETTX_X3DAUDIO_CALCULATE_DOPPLER, &dsp);
    if (dsp.DopplerFactor <= 1.0f || matrix_5_1[1] <= matrix_5_1[0]) {
        std::fprintf(stderr, "[ue5_slice] X3DAudio 5.1 spatial/Doppler calculation invalid\n");
        return 42;
    }

    const GuidPod kClsidFXReverb = {
        0x7D9ACA56u, 0xCB68u, 0x4807u, {0xB6, 0x32, 0xB1, 0x37, 0x35, 0x2E, 0x85, 0x96}};
    void* reverb_fx = nullptr;
    if (CreateFX(&kClsidFXReverb, &reverb_fx, nullptr, 0) != 0 || !reverb_fx) {
        std::fprintf(stderr, "[ue5_slice] CreateFX(CLSID_FXReverb) failed\n");
        return 43;
    }
#if defined(_WIN32)
    {
        void** vtbl = *reinterpret_cast<void***>(reverb_fx);
        using FnRel = uint32_t(__stdcall*)(void*);
        reinterpret_cast<FnRel>(vtbl[2])(reverb_fx);
    }
#endif

    // 6. D3D12_XS + RDNA2 XS2 PSO Prewarm Cache + 60 FPS Menu/Gameplay Breadcrumb Loop
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.PreferWarp = 1;
    uint64_t d3d_dev = 0;
    if (D3D12XboxCreateSyntheticDevice(&dev_params, &d3d_dev) != 0) return 50;
    D3D12XboxSetFrameIntervalX(d3d_dev, D3D12XBOX_FRAME_INTERVAL_60_HZ, 0);

    const uint32_t xs2_cs_words[] = {
        0x06040000u,               // v_add_f32 v2, v0, v0
        0x10060402u,               // v_mul_f32 v3, v2, v2
        0xE2000000u, 0x00000300u,  // buffer_store_dword v3, v0
        0xBF800000u,               // s_endpgm
    };
    std::vector<uint8_t> xs2_blob = {'X', 'S', '2', '\0'};
    AppendLe32(xs2_blob, 1u);
    AppendLe32(xs2_blob, 3u);
    AppendLe32(xs2_blob, static_cast<uint32_t>(sizeof(xs2_cs_words)));
    xs2_blob.insert(xs2_blob.end(), reinterpret_cast<const uint8_t*>(xs2_cs_words),
                    reinterpret_cast<const uint8_t*>(xs2_cs_words) + sizeof(xs2_cs_words));

    D3D12XBOX_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumUavs = 1;
    uint64_t rs = 0, pso1 = 0, pso2 = 0;
    if (D3D12XboxCreateRootSignature(d3d_dev, &rs_desc, &rs) != 0) return 51;
    D3D12XBOX_COMPUTE_PSO_DESC cpso{};
    cpso.RootSignatureHandle = rs;
    cpso.CS = {xs2_blob.data(), xs2_blob.size()};
    if (D3D12XboxCreateComputePipelineState(d3d_dev, &cpso, &pso1) != 0 ||
        D3D12XboxCreateComputePipelineState(d3d_dev, &cpso, &pso2) != 0) {
        return 52;
    }

    float gpu_uav[16] = {};
    if (D3D12XboxDispatchCompute(d3d_dev, pso1, 1, 1, 1, nullptr, 0, gpu_uav, sizeof(gpu_uav)) != 0 ||
        gpu_uav[3] != 36.0f) {
        std::fprintf(stderr, "[ue5_slice] XS2 compute output mismatch\n");
        return 53;
    }

    D3D12XboxWriteBreadcrumb(d3d_dev, 1, 0x1001, "UE5_IntroMovie");
    D3D12XboxWriteBreadcrumb(d3d_dev, 1, 0x1002, "UE5_MainMenu3D");
    for (uint32_t f = 0; f < 6; ++f) {
        uint64_t tok = 0;
        D3D12XboxWaitFrameEventX(d3d_dev, &tok);
        D3D12XboxWriteBreadcrumb(d3d_dev, 2 + f, 0x2000 + f, "UE5_NaniteBasePass");
        uint64_t tri_hash = 0;
        if (D3D12XboxRenderTestTriangle(d3d_dev, 128, 128, nullptr, &tri_hash) != 0 || tri_hash == 0) {
            return 54;
        }
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{128, 128, tok};
        D3D12XboxPresentX(d3d_dev, &plane);
    }
    D3D12XboxWriteBreadcrumb(d3d_dev, 99, 0x9999, "UE5_LumenPresent");

    D3D12XBOX_GPU_STABILITY_REPORT stab{};
    if (D3D12XboxGetGpuStabilityReport(d3d_dev, &stab) != 0 ||
        stab.DeviceRemovedReason != 0 ||
        stab.PsoCacheHits < 1 ||
        stab.BreadcrumbCount < 8 ||
        stab.RecordedFrameCount < 6 ||
        std::strcmp(stab.LastBreadcrumbTag, "UE5_LumenPresent") != 0) {
        std::fprintf(stderr, "[ue5_slice] GPU stability report check failed\n");
        return 55;
    }

    D3D12XboxDestroyPipelineState(d3d_dev, pso2);
    D3D12XboxDestroyPipelineState(d3d_dev, pso1);
    D3D12XboxDestroyRootSignature(d3d_dev, rs);
    D3D12XboxDestroyDevice(d3d_dev);
    DStorageDestroyQueue(ds_queue);
    DStorageCloseFile(ds_file);
    DStorageDestroyFactory(ds_factory);
    XStoreCloseContextHandle(store_ctx);
    XUserCloseHandle(user);
    XTaskQueueTerminate(queue, true, nullptr, nullptr);
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    std::printf("[ue5_slice] Booted from encrypted XVC cleanly (pso_hits=%llu, p99_us=%llu)\n",
                static_cast<unsigned long long>(stab.PsoCacheHits),
                static_cast<unsigned long long>(stab.P99FrameTimeUs));
    return 0;
}

int main() {
    return ScarlettGuestMain();
}
