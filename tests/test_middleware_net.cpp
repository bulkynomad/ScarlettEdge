#include "scarlettx/middleware/bink2_abi.h"
#include "scarlettx/middleware/ffx_abi.h"
#include "scarlettx/net/httpclient_abi.h"
#include "scarlettx/net/xcurl_abi.h"
#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_middleware_net] FAILED: " #cond                 \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

size_t CurlWriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* dst = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    dst->append(ptr, total);
    return total;
}

}  // namespace

int main() {
    // 1. Bink2Scarlett.dll
    {
        BINK* bnk = BinkOpen("intro_scarlett.bk2", 0);
        SX_CHECK(bnk != nullptr);
        SX_CHECK(bnk->Width == 320 && bnk->Height == 180);
        SX_CHECK(BinkWait(bnk) == 0);
        SX_CHECK(BinkDoFrame(bnk) == 0);

        std::vector<uint8_t> rgba(static_cast<size_t>(bnk->Width) * bnk->Height * 4ull, 0);
        SX_CHECK(BinkCopyToBuffer(
                     bnk, rgba.data(), static_cast<int32_t>(bnk->Width * 4u),
                     bnk->Height, 0, 0, BINKSURFACE32RGBA) == 0);
        SX_CHECK(rgba[3] == 255);

        BinkNextFrame(bnk);
        SX_CHECK(bnk->FrameNum == 2);
        SX_CHECK(BinkDoFrame(bnk) == 0);
        BinkGoto(bnk, 10, 0);
        SX_CHECK(bnk->FrameNum == 10);
        BinkSetSoundOnOff(bnk, 0);
        BinkSetVolume(bnk, 0, 16384);
        SX_CHECK(BinkPause(bnk, 1) == 1);
        SX_CHECK(BinkWait(bnk) == 1);

        BINKSUMMARY sum{};
        BinkGetSummary(bnk, &sum);
        SX_CHECK(sum.Width == 320 && sum.Height == 180);
        SX_CHECK(sum.TotalPlayedFrames == 2);

        BinkSetFrameRate(bnk, 60, 1);
        uint32_t r = 0, sc = 0;
        BinkGetFrameRate(bnk, &r, &sc);
        SX_CHECK(r == 60 && sc == 1);
        SX_CHECK(BinkService(bnk) == 0);
        uint32_t track_type = 999;
        SX_CHECK(BinkGetTrackType(bnk, 0, &track_type) == 0 && track_type == 0);
        uint32_t rects = 999;
        SX_CHECK(BinkGetRects(bnk, &rects, nullptr) == 0 && rects == 0);
        BinkSetVideoOnOff(bnk, 1);
        BinkSetSoundTrack(bnk, 1);
        BinkSetMemory(nullptr, nullptr);
        SX_CHECK(BinkOpenMiles(reinterpret_cast<void*>(0x1234)) == reinterpret_cast<void*>(0x1234));

        BinkClose(bnk);

        // 1b. Real KB2 container with frame-offset table + YCbCr decode + A/V PTS lip-sync frame catch-up
        int pts_bnk_tag = 0;
        const auto kb2_path = std::filesystem::temp_directory_path() /
                              ("scarlett_pts_test_" +
                               std::to_string(reinterpret_cast<uintptr_t>(&pts_bnk_tag)) + ".bk2");
        {
            std::vector<uint8_t> kb2(256, 0);
            kb2[0] = 'K'; kb2[1] = 'B'; kb2[2] = '2'; kb2[3] = 'i';
            auto wr32 = [&](size_t off, uint32_t v) { std::memcpy(kb2.data() + off, &v, 4); };
            wr32(8, 10u);    // 10 frames
            wr32(20, 64u);   // 64x64
            wr32(24, 64u);
            wr32(28, 30u);   // 30 FPS (33,333 us/frame)
            wr32(32, 1u);
            // Frame offset table at +44 (11 entries)
            for (uint32_t fi = 0; fi <= 10u; ++fi) {
                wr32(44u + fi * 4u, (100u + fi * 12u) | (fi == 0 ? 1u : 0u));
            }
            for (size_t k = 100u; k < kb2.size(); ++k) {
                kb2[k] = static_cast<uint8_t>((k * 37u) & 0xFFu);
            }
            std::ofstream(kb2_path, std::ios::binary).write(
                reinterpret_cast<const char*>(kb2.data()), static_cast<std::streamsize>(kb2.size()));
        }
        BINK* pts_bnk = BinkOpen(kb2_path.string().c_str(), BINKOPEN_PTS_SYNC);
        SX_CHECK(pts_bnk != nullptr && pts_bnk->Width == 64 && pts_bnk->Frames == 10);
        // Advance audio clock to 110,000 us (~3.3 frames @ 30 FPS) -> BinkDoFrame must skip lagging frames to lip-sync!
        BinkSetAudioPts(pts_bnk, 110000ull);
        SX_CHECK(BinkDoFrame(pts_bnk) == 1);  // 1 = skipped lagging frames to catch up with audio PTS
        uint64_t v_pts = 0, a_pts = 0;
        int64_t drift = 0;
        SX_CHECK(BinkGetPtsState(pts_bnk, &v_pts, &a_pts, &drift) == 0);
        SX_CHECK(a_pts == 110000ull && std::llabs(drift) <= 50000ll);
        BINKSUMMARY pts_sum{};
        BinkGetSummary(pts_bnk, &pts_sum);
        SX_CHECK(pts_sum.SkippedFrames >= 2u);
        BINKREALTIME rt{};
        BinkGetRealtime(pts_bnk, &rt, 0);
        SX_CHECK(rt.FrameRate == 30u && rt.ReadBufferSize > 0u);
        BinkClose(pts_bnk);
        std::error_code rm_ec;
        std::filesystem::remove(kb2_path, rm_ec);
    }

    // 2. amd_fidelityfx_loader_xbox.dll -> amd_fidelityfx_upscaler_xbox.dll
    {
        ffxQueryDescGetProviderVersion ver{};
        ver.header.type = FFX_API_DESC_TYPE_GET_PROVIDER_VERSION;
        SX_CHECK(ffxQuery(nullptr, &ver.header) == FFX_API_RETURN_OK);
        SX_CHECK(ver.versionId != 0 && ver.versionName != nullptr);

        uint32_t rw = 0, rh = 0;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode res_q{};
        res_q.header.type = FFX_API_DESC_TYPE_UPSCALE_GET_RENDERRESOLUTION;
        res_q.displayWidth = 3840;
        res_q.displayHeight = 2160;
        res_q.qualityMode = FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;  // 2.0x
        res_q.pOutRenderWidth = &rw;
        res_q.pOutRenderHeight = &rh;
        SX_CHECK(ffxQuery(nullptr, &res_q.header) == FFX_API_RETURN_OK);
        SX_CHECK(rw == 1920 && rh == 1080);

        int32_t phases = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount jp{};
        jp.header.type = FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT;
        jp.renderWidth = 1920;
        jp.displayWidth = 3840;
        jp.pOutPhaseCount = &phases;
        SX_CHECK(ffxQuery(nullptr, &jp.header) == FFX_API_RETURN_OK);
        SX_CHECK(phases == 32);

        float jx = 0.0f, jy = 0.0f;
        ffxQueryDescUpscaleGetJitterOffset jo{};
        jo.header.type = FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET;
        jo.index = 0;
        jo.phaseCount = phases;
        jo.pOutX = &jx;
        jo.pOutY = &jy;
        SX_CHECK(ffxQuery(nullptr, &jo.header) == FFX_API_RETURN_OK);
        SX_CHECK(std::fabs(jx) <= 0.5f && std::fabs(jy) <= 0.5f);

        ffxContext ctx = nullptr;
        ffxCreateContextDescUpscale cdesc{};
        cdesc.header.type = FFX_API_DESC_TYPE_UPSCALE_CREATE;
        cdesc.maxRenderWidth = 64;
        cdesc.maxRenderHeight = 64;
        cdesc.maxUpscaleWidth = 128;
        cdesc.maxUpscaleHeight = 128;
        SX_CHECK(ffxCreateContext(&ctx, &cdesc.header, nullptr) == FFX_API_RETURN_OK);
        SX_CHECK(ctx != nullptr);

        ffxConfigureDescUpscale cfg{};
        cfg.header.type = FFX_API_DESC_TYPE_UPSCALE_CONFIGURE;
        cfg.sharpness = 0.75f;
        cfg.enableSharpening = 1;
        SX_CHECK(ffxConfigure(&ctx, &cfg.header) == FFX_API_RETURN_OK);

        std::vector<uint8_t> src(32 * 32 * 4, 0);
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 32; ++x) {
                const size_t i = (static_cast<size_t>(y) * 32 + x) * 4;
                src[i + 0] = static_cast<uint8_t>(x * 8);
                src[i + 1] = static_cast<uint8_t>(y * 8);
                src[i + 2] = 200;
                src[i + 3] = 255;
            }
        }
        std::vector<uint8_t> dst(64 * 64 * 4, 0);
        uint64_t hash = 0;
        ffxDispatchDescUpscale dd{};
        dd.header.type = FFX_API_DESC_TYPE_UPSCALE_DISPATCH;
        dd.colorRgba8 = src.data();
        dd.outputRgba8 = dst.data();
        dd.renderWidth = 32;
        dd.renderHeight = 32;
        dd.upscaleWidth = 64;
        dd.upscaleHeight = 64;
        dd.outFrameHash = &hash;
        SX_CHECK(ffxDispatch(&ctx, &dd.header) == FFX_API_RETURN_OK);
        SX_CHECK(hash != 0);
        SX_CHECK(dst[2] == 200 && dst[3] == 255);
        SX_CHECK(ffxDestroyContext(&ctx, nullptr) == FFX_API_RETURN_OK);
    }

    // 3. libHttpClient.GDK.dll
    {
        SX_CHECK(HCInitialize(nullptr) == SX_S_OK);
        SX_CHECK(HCIsInitialized() == 1);
        const char* hc_ver = nullptr;
        SX_CHECK(HCGetLibVersion(&hc_ver) == SX_S_OK && hc_ver != nullptr);

        HCHttpCallClearMocks();
        const std::string mock_json = "{\"entitled\":true,\"title\":\"ScarlettX\"}";
        SX_CHECK(HCHttpCallRegisterMock(
                     "GET", "https://title.mgt.xboxlive.com/", 200,
                     reinterpret_cast<const uint8_t*>(mock_json.data()),
                     static_cast<uint32_t>(mock_json.size()),
                     "application/json") == SX_S_OK);

        HCCallHandle call = nullptr;
        SX_CHECK(HCHttpCallCreate(&call) == SX_S_OK);
        SX_CHECK(HCHttpCallRequestSetUrl(call, "GET", "https://title.mgt.xboxlive.com/config") == SX_S_OK);
        SX_CHECK(HCHttpCallRequestSetTimeout(call, 45) == SX_S_OK);
        const char* q_method = nullptr;
        const char* q_url = nullptr;
        SX_CHECK(HCHttpCallRequestGetUrl(call, &q_method, &q_url) == SX_S_OK);
        SX_CHECK(q_method && std::string(q_method) == "GET");
        SX_CHECK(q_url && std::string(q_url) == "https://title.mgt.xboxlive.com/config");
        uint32_t q_timeout = 0;
        SX_CHECK(HCHttpCallRequestGetTimeout(call, &q_timeout) == SX_S_OK);
        SX_CHECK(q_timeout == 45);
        SX_CHECK(HCHttpCallRequestSetHeader(call, "Accept", "application/json", 1) == SX_S_OK);

        bool completed = false;
        XAsyncBlock async{};
        async.context = &completed;
        async.callback = [](XAsyncBlock* ab) {
            *static_cast<bool*>(ab->context) = true;
        };
        SX_CHECK(HCHttpCallPerformAsync(call, &async) == SX_S_OK);
        SX_CHECK(completed);

        uint32_t status = 0;
        SX_CHECK(HCHttpCallResponseGetStatusCode(call, &status) == SX_S_OK);
        SX_CHECK(status == 200);
        const char* resp_str = nullptr;
        SX_CHECK(HCHttpCallResponseGetResponseString(call, &resp_str) == SX_S_OK);
        SX_CHECK(std::string(resp_str) == mock_json);
        const char* ct = nullptr;
        SX_CHECK(HCHttpCallResponseGetHeader(call, "Content-Type", &ct) == SX_S_OK);
        SX_CHECK(ct && std::string(ct) == "application/json");
        SX_CHECK(HCHttpCallCloseHandle(call) == SX_S_OK);

        // 3b. HCWebSocket* (Xbox Live RTA & PlayFab WebSocket transport)
        struct WsCtx {
            std::string last_msg;
            std::vector<uint8_t> last_bin;
            uint32_t close_code = 0;
        } ws_ctx;
        HCWebSocketHandle ws = nullptr;
        SX_CHECK(HCWebSocketCreate(
                     &ws,
                     [](HCWebSocketHandle, const char* msg, void* c) {
                         static_cast<WsCtx*>(c)->last_msg = msg ? msg : "";
                     },
                     [](HCWebSocketHandle, const uint8_t* b, uint32_t n, void* c) {
                         static_cast<WsCtx*>(c)->last_bin.assign(b, b + n);
                     },
                     [](HCWebSocketHandle, uint32_t code, void* c) {
                         static_cast<WsCtx*>(c)->close_code = code;
                     },
                     &ws_ctx) == SX_S_OK);
        SX_CHECK(ws != nullptr);
        SX_CHECK(HCWebSocketSetHeader(ws, "Authorization", "XBL3.0 x=1;jwt") == SX_S_OK);
        SX_CHECK(HCWebSocketConnectAsync("wss://rta.xboxlive.com/connect", "xboxlive-rta", ws, nullptr) == SX_S_OK);
        SX_CHECK(HCWebSocketSendMessageAsync(ws, "[1,1,\"https://sessiondirectory.xboxlive.com\"]", nullptr) == SX_S_OK);
        SX_CHECK(ws_ctx.last_msg.find("Subscribed") != std::string::npos);

        const uint8_t bin_ping[4] = {0x50, 0x46, 0x01, 0x02};
        SX_CHECK(HCWebSocketSendBinaryMessageAsync(ws, bin_ping, sizeof(bin_ping), nullptr) == SX_S_OK);
        SX_CHECK(ws_ctx.last_bin.size() == 4 && ws_ctx.last_bin[0] == 0x50);
        SX_CHECK(HCWebSocketDisconnect(ws) == SX_S_OK);
        SX_CHECK(ws_ctx.close_code == 1000u);
        SX_CHECK(HCWebSocketCloseHandle(ws) == SX_S_OK);

        HCCleanup();
    }

    // 4. XCurl.dll
    {
        SX_CHECK(curl_global_init(0) == CURLE_OK);
        SX_CHECK(curl_version() != nullptr);
        xcurl_clear_mocks();
        const std::string payload = "{\"telemetry\":\"accepted\"}";
        SX_CHECK(xcurl_register_mock_response(
                     "https://services.scarlettx.local/", 200,
                     "application/json", payload.data(), payload.size()) == CURLE_OK);

        CURL* curl = curl_easy_init();
        SX_CHECK(curl != nullptr);
        curl_slist* hdrs = curl_slist_append(nullptr, "X-Client: ScarlettX");
        std::string body_out;
        SX_CHECK(curl_easy_setopt(curl, CURLOPT_URL, "https://services.scarlettx.local/ping") == CURLE_OK);
        SX_CHECK(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs) == CURLE_OK);
        SX_CHECK(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWriteToString) == CURLE_OK);
        SX_CHECK(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_out) == CURLE_OK);
        SX_CHECK(curl_easy_perform(curl) == CURLE_OK);

        long code = 0;
        int64_t dl = 0;
        SX_CHECK(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK);
        SX_CHECK(curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dl) == CURLE_OK);
        SX_CHECK(code == 200);
        SX_CHECK(body_out == payload);
        SX_CHECK(dl == static_cast<int64_t>(payload.size()));

        // Multi interface, driven the way a title's update loop does it.
        CURLM* multi = curl_multi_init();
        SX_CHECK(multi != nullptr);
        body_out.clear();
        SX_CHECK(curl_multi_add_handle(multi, curl) == CURLM_OK);
        SX_CHECK(curl_multi_add_handle(multi, curl) == CURLM_ADDED_ALREADY);
        SX_CHECK(curl_multi_add_handle(multi, &body_out) == CURLM_BAD_EASY_HANDLE);
        int numfds = -1;
        SX_CHECK(curl_multi_poll(multi, nullptr, 0, 1000, &numfds) == CURLM_OK);
        SX_CHECK(numfds == 0);
        int running = -1;
        SX_CHECK(curl_multi_perform(multi, &running) == CURLM_OK);
        SX_CHECK(running == 0);
        SX_CHECK(body_out == payload);
        int left = -1;
        CURLMsg* msg = curl_multi_info_read(multi, &left);
        SX_CHECK(msg != nullptr && left == 0);
        SX_CHECK(msg->msg == CURLMSG_DONE && msg->easy_handle == curl);
        SX_CHECK(curl_multi_remove_handle(multi, curl) == CURLM_OK);
        SX_CHECK(msg->data.result == CURLE_OK);
        SX_CHECK(curl_multi_info_read(multi, &left) == nullptr && left == 0);
        SX_CHECK(curl_multi_poll(multi, nullptr, 0, 0, &numfds) == CURLM_OK);

        // A handle removed before it ran produces no message.
        SX_CHECK(curl_multi_add_handle(multi, curl) == CURLM_OK);
        SX_CHECK(curl_multi_remove_handle(multi, curl) == CURLM_OK);
        SX_CHECK(curl_multi_perform(multi, &running) == CURLM_OK);
        SX_CHECK(curl_multi_info_read(multi, &left) == nullptr);
        SX_CHECK(curl_multi_cleanup(multi) == CURLM_OK);
        SX_CHECK(curl_multi_cleanup(multi) == CURLM_BAD_HANDLE);
        SX_CHECK(curl_multi_perform(multi, &running) == CURLM_BAD_HANDLE);

        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
    }

    std::cout << "[PASS] test_middleware_net (Bink2Scarlett, FidelityFX, libHttpClient.GDK, XCurl)\n";
    return 0;
}
