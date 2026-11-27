#include "scarlettx/services/xsapi_abi.h"
#include "scarlettx/gdk/xgameruntime_abi.h"
#include "sx_check.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#include <objbase.h>
#include <windows.h>
#endif

namespace {
void SCARLETTX_GDK_CALLBACK OnXsapiComplete(XAsyncBlock* block) {
    if (block && block->context) {
        static_cast<std::atomic<int>*>(block->context)->fetch_add(1);
    }
}
}  // namespace

int main() {
    std::cout << "[test_xsapi] Testing Live Xbox Services (XSAPI/WinRT)...\n";

    // 1. Initialize and create context
    SX_CHECK(XblInitialize("00000000-0000-0000-0000-000000000001") == 0);

    XblContextHandle ctx = nullptr;
    SX_CHECK(XblContextCreateHandle(0x0009000000000042ull, &ctx) == 0);
    SX_CHECK(ctx != nullptr);

    // 2. Query user ID and Gamertag
    uint64_t xuid = 0;
    SX_CHECK(XblContextGetXboxUserId(ctx, &xuid) == 0);
    SX_CHECK(xuid == 0x0009000000000042ull);

    char gamertag[64]{};
    SX_CHECK(XblContextGetGamertag(ctx, gamertag, sizeof(gamertag)) == 0);
    SX_CHECK(std::strlen(gamertag) > 0);

    // 3. Achievements
    SX_CHECK(XblAchievementsGetAchievementsForTitleIdAsync(ctx, 0x1234, nullptr) == 0);
    uint32_t ach_buf = 0;
    XblAchievementsResultHandle ach_res = nullptr;
    size_t ach_used = 0;
    SX_CHECK(XblAchievementsGetAchievementsForTitleIdResult(nullptr, sizeof(ach_buf), &ach_buf, &ach_res, &ach_used) == 0);
    SX_CHECK(XblAchievementsUpdateAchievementAsync(ctx, "ACH_001", 100, nullptr) == 0);
    SX_CHECK(XblAchievementsUpdateAchievementResult(nullptr) == 0);

    // 4. User profile
    SX_CHECK(XblProfileGetUserProfileAsync(ctx, xuid, nullptr) == 0);
    XblUserProfile profile{};
    size_t prof_used = 0;
    SX_CHECK(XblProfileGetUserProfileResult(nullptr, sizeof(profile), nullptr, &profile, &prof_used) == 0);
    SX_CHECK(profile.xboxUserId == 0x0009000000000001ull);
    SX_CHECK(std::strlen(profile.gamertag) > 0);

    // 5. Multiplayer session
    XblMultiplayerSessionHandle sess = nullptr;
    SX_CHECK(XblMultiplayerCreateSession(ctx, "DeathmatchTemplate", &sess) == 0);
    SX_CHECK(sess != nullptr);
    SX_CHECK(XblMultiplayerLeaveSession(sess) == 0);

    // 6. Rich presence (both nullptr and live XAsyncBlock completion callback)
    SX_CHECK(XblPresenceSetPresenceAsync(ctx, 1, "Exploring Night City", nullptr) == 0);
    SX_CHECK(XblPresenceSetPresenceResult(nullptr) == 0);

    SX_CHECK(XGameRuntimeInitialize() == 0);
    std::atomic<int> cb_fired{0};
    XAsyncBlock async{};
    async.context = &cb_fired;
    async.callback = &OnXsapiComplete;
    SX_CHECK(XblProfileGetUserProfileAsync(ctx, xuid, &async) == 0);
    XblUserProfile async_profile{};
    size_t async_used = 0;
    SX_CHECK(XblProfileGetUserProfileResult(&async, sizeof(async_profile), nullptr, &async_profile, &async_used) == 0);
    SX_CHECK(async_profile.xboxUserId == 0x0009000000000001ull);

    // 6b. Persistent disk-backed XblTitleStorage (Upload -> Download -> Verify -> Delete)
    {
        const auto ts_root = std::filesystem::temp_directory_path() / "scarlettx_test_title_storage";
        std::error_code ec;
        std::filesystem::remove_all(ts_root, ec);
#if defined(_WIN32)
        _putenv_s("SCARLETTX_SAVE_ROOT", ts_root.string().c_str());
#endif
        const std::string cloud_cfg = "{\"sensitivity\":1.25,\"hdr_nits\":1000}";
        XAsyncBlock up_ab{};
        SX_CHECK(XblTitleStorageUploadBlobAsync(
                     ctx, "settings/player_prefs.json", XblTitleStorageType_TrustedPlatformStorage,
                     reinterpret_cast<const uint8_t*>(cloud_cfg.data()), cloud_cfg.size(), &up_ab) == 0);
        SX_CHECK(XblTitleStorageUploadBlobResult(&up_ab) == 0);

        std::vector<uint8_t> dl_buf(128, 0);
        XAsyncBlock dl_ab{};
        SX_CHECK(XblTitleStorageDownloadBlobAsync(
                     ctx, "settings/player_prefs.json", XblTitleStorageType_TrustedPlatformStorage,
                     dl_buf.data(), dl_buf.size(), &dl_ab) == 0);
        size_t dl_bytes = 0;
        SX_CHECK(XblTitleStorageDownloadBlobResult(&dl_ab, &dl_bytes) == 0);
        SX_CHECK(dl_bytes == cloud_cfg.size());
        SX_CHECK(std::memcmp(dl_buf.data(), cloud_cfg.data(), cloud_cfg.size()) == 0);

        XAsyncBlock del_ab{};
        SX_CHECK(XblTitleStorageDeleteBlobAsync(
                     ctx, "settings/player_prefs.json", XblTitleStorageType_TrustedPlatformStorage, &del_ab) == 0);
        SX_CHECK(XblTitleStorageDeleteBlobResult(&del_ab) == 0);
        SX_CHECK(XblTitleStorageDownloadBlobAsync(
                     ctx, "settings/player_prefs.json", XblTitleStorageType_TrustedPlatformStorage,
                     dl_buf.data(), dl_buf.size(), nullptr) != 0);
        std::filesystem::remove_all(ts_root, ec);
#if defined(_WIN32)
        _putenv_s("SCARLETTX_SAVE_ROOT", "");
#endif
    }

    // 6c. WinRT IInspectable / IActivationFactory Xbox Game Core Broker
    {
        void* fac = nullptr;
        SX_CHECK(XblWinRtActivateClassByName(
                     L"Windows.Gaming.XboxLive.Storage.GameSaveProvider", nullptr, 1, &fac) == 0);
        SX_CHECK(fac != nullptr);
        auto* vtbl = *reinterpret_cast<void* const* const*>(fac);
        using FnGetIids = int32_t(__stdcall*)(void*, uint32_t*, void**);
        using FnGetTrust = int32_t(__stdcall*)(void*, int32_t*);
        using FnActivate = int32_t(__stdcall*)(void*, void**);
        using FnRelease = uint32_t(__stdcall*)(void*);

        uint32_t iid_cnt = 0;
        void* iids_mem = nullptr;
        SX_CHECK(reinterpret_cast<FnGetIids>(vtbl[3])(fac, &iid_cnt, &iids_mem) == 0);
        SX_CHECK(iid_cnt >= 1 && iids_mem != nullptr);
#if defined(_WIN32)
        CoTaskMemFree(iids_mem);
#else
        std::free(iids_mem);
#endif
        int32_t trust = -1;
        SX_CHECK(reinterpret_cast<FnGetTrust>(vtbl[5])(fac, &trust) == 0 && trust == 0);

        void* inst = nullptr;
        SX_CHECK(reinterpret_cast<FnActivate>(vtbl[6])(fac, &inst) == 0 && inst != nullptr);
        auto* inst_vtbl = *reinterpret_cast<void* const* const*>(inst);
        SX_CHECK(reinterpret_cast<FnRelease>(inst_vtbl[2])(inst) == 0u);
        SX_CHECK(reinterpret_cast<FnRelease>(vtbl[2])(fac) == 0u);

        // Non-Xbox runtime class returns REGDB_E_CLASSNOTREG so GuestRoActivateInstance delegates to combase.dll
        void* desktop_obj = nullptr;
        SX_CHECK(XblWinRtActivateClassByName(L"Windows.Foundation.Uri", nullptr, 1, &desktop_obj) != 0);
        SX_CHECK(desktop_obj == nullptr);
    }

    XGameRuntimeUninitialize();

    // 7. Cleanup
    SX_CHECK(XblContextCloseHandle(ctx) == 0);
    SX_CHECK(XblCleanup() == 0);

    std::cout << "[test_xsapi] All 16 Xbl APIs verified successfully!\n";
    return 0;
}
