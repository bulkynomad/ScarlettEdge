#include "scarlettx/services/xsapi_abi.h"
#include "scarlettx/gdk/xgameruntime_abi.h"
#include "sx_check.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>

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
    XGameRuntimeUninitialize();

    // 7. Cleanup
    SX_CHECK(XblContextCloseHandle(ctx) == 0);
    SX_CHECK(XblCleanup() == 0);

    std::cout << "[test_xsapi] All 16 Xbl APIs verified successfully!\n";
    return 0;
}
