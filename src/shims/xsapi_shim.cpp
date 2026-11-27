#define SCARLETTX_BUILDING_XSAPI 1
#include "scarlettx/services/xsapi_abi.h"
#include "scarlettx/gdk/xgameruntime_abi.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);

struct XblContext {
    uint64_t xuid = 0x0009000000000001ull;
    std::string gamertag = "ScarlettPlayer";
    std::string scid = "00000000-0000-0000-0000-000000000000";
};

struct XblMultiplayerSession {
    std::string template_name;
    std::vector<uint64_t> members;
};

std::mutex g_mu;
std::string g_scid;

XGameResult SCARLETTX_GDK_CALLBACK XsapiImmediateProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    switch (op) {
        case XAsyncOp_Begin:
            return XAsyncSchedule(data->async, 0);
        case XAsyncOp_DoWork: {
            const size_t result_sz = reinterpret_cast<size_t>(data->context);
            XAsyncComplete(data->async, XGR_S_OK, result_sz);
            return XGR_S_OK;
        }
        case XAsyncOp_GetResult:
            if (data->buffer && data->bufferSize > 0) {
                std::memset(data->buffer, 0, data->bufferSize);
            }
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

int32_t StartXsapiAsync(void* asyncBlock, const char* name, size_t resultBytes) {
    if (!asyncBlock) return kSuccess;
    auto* async = static_cast<XAsyncBlock*>(asyncBlock);
    return XAsyncBegin(
        async,
        reinterpret_cast<void*>(resultBytes),
        reinterpret_cast<const void*>(&XsapiImmediateProvider),
        name,
        &XsapiImmediateProvider);
}

int32_t FinishXsapiAsync(void* asyncBlock) {
    if (!asyncBlock) return kSuccess;
    auto* async = static_cast<XAsyncBlock*>(asyncBlock);
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return hr;
    size_t sz = 0;
    if (XAsyncGetResultSize(async, &sz) == XGR_S_OK && sz > 0) {
        std::vector<uint8_t> tmp(sz, 0);
        size_t used = 0;
        (void)XAsyncGetResult(
            async,
            reinterpret_cast<const void*>(&XsapiImmediateProvider),
            tmp.size(),
            tmp.data(),
            &used);
    } else {
        size_t used = 0;
        (void)XAsyncGetResult(
            async,
            reinterpret_cast<const void*>(&XsapiImmediateProvider),
            0,
            nullptr,
            &used);
    }
    return kSuccess;
}

}  // namespace

extern "C" {

int32_t SCARLETTX_XSAPI_CALL XblInitialize(const char* scid) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_scid = scid ? scid : "00000000-0000-0000-0000-000000000000";
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblCleanup(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_scid.clear();
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblContextCreateHandle(uint64_t xuid, XblContextHandle* outContext) {
    if (!outContext) return kInvalidArg;
    auto* ctx = new XblContext();
    ctx->xuid = xuid ? xuid : 0x0009000000000001ull;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ctx->scid = g_scid;
    }
    *outContext = ctx;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblContextCloseHandle(XblContextHandle context) {
    if (!context) return kInvalidArg;
    delete static_cast<XblContext*>(context);
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblContextGetXboxUserId(XblContextHandle context, uint64_t* outXboxUserId) {
    if (!context || !outXboxUserId) return kInvalidArg;
    *outXboxUserId = static_cast<XblContext*>(context)->xuid;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblContextGetGamertag(XblContextHandle context, char* outGamertag, size_t bufferSize) {
    if (!context || !outGamertag || bufferSize == 0) return kInvalidArg;
    auto* ctx = static_cast<XblContext*>(context);
    std::strncpy(outGamertag, ctx->gamertag.c_str(), bufferSize - 1);
    outGamertag[bufferSize - 1] = '\0';
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle context, uint32_t /*titleId*/, void* asyncBlock) {
    if (!context) return kInvalidArg;
    return StartXsapiAsync(asyncBlock, "XblAchievementsGetAchievementsForTitleIdAsync", sizeof(uint32_t));
}

int32_t SCARLETTX_XSAPI_CALL XblAchievementsGetAchievementsForTitleIdResult(
    void* asyncBlock, size_t bufferSize, void* buffer, XblAchievementsResultHandle* resultHandle, size_t* bufferUsed) {
    const int32_t hr = FinishXsapiAsync(asyncBlock);
    if (hr < 0) return hr;
    if (resultHandle) *resultHandle = reinterpret_cast<XblAchievementsResultHandle>(0x1000);
    if (bufferUsed) *bufferUsed = sizeof(uint32_t);
    if (buffer && bufferSize >= sizeof(uint32_t)) {
        *static_cast<uint32_t*>(buffer) = 0; // 0 achievements unlocked in offline mode
    }
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblAchievementsUpdateAchievementAsync(
    XblContextHandle context, const char* achievementId, uint32_t /*percentComplete*/, void* asyncBlock) {
    if (!context || !achievementId) return kInvalidArg;
    return StartXsapiAsync(asyncBlock, "XblAchievementsUpdateAchievementAsync", 0);
}

int32_t SCARLETTX_XSAPI_CALL XblAchievementsUpdateAchievementResult(void* asyncBlock) {
    return FinishXsapiAsync(asyncBlock);
}

int32_t SCARLETTX_XSAPI_CALL XblProfileGetUserProfileAsync(
    XblContextHandle context, uint64_t /*xboxUserId*/, void* asyncBlock) {
    if (!context) return kInvalidArg;
    return StartXsapiAsync(asyncBlock, "XblProfileGetUserProfileAsync", sizeof(XblUserProfile));
}

int32_t SCARLETTX_XSAPI_CALL XblProfileGetUserProfileResult(
    void* asyncBlock, size_t bufferSize, void* buffer, XblUserProfile* userProfile, size_t* bufferUsed) {
    (void)bufferSize;
    (void)buffer;
    const int32_t hr = FinishXsapiAsync(asyncBlock);
    if (hr < 0) return hr;
    if (userProfile) {
        userProfile->xboxUserId = 0x0009000000000001ull;
        std::strncpy(userProfile->gamertag, "ScarlettPlayer", sizeof(userProfile->gamertag) - 1);
        std::strncpy(userProfile->modernGamertag, "ScarlettPlayer", sizeof(userProfile->modernGamertag) - 1);
        std::strncpy(userProfile->modernGamertagSuffix, "#1234", sizeof(userProfile->modernGamertagSuffix) - 1);
        std::strncpy(userProfile->uniqueModernGamertag, "ScarlettPlayer#1234", sizeof(userProfile->uniqueModernGamertag) - 1);
        std::strncpy(userProfile->gamerscore, "1000", sizeof(userProfile->gamerscore) - 1);
    }
    if (bufferUsed) *bufferUsed = sizeof(XblUserProfile);
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblMultiplayerCreateSession(
    XblContextHandle context, const char* sessionTemplateName, XblMultiplayerSessionHandle* outSession) {
    if (!context || !sessionTemplateName || !outSession) return kInvalidArg;
    auto* sess = new XblMultiplayerSession();
    sess->template_name = sessionTemplateName;
    sess->members.push_back(static_cast<XblContext*>(context)->xuid);
    *outSession = sess;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblMultiplayerLeaveSession(XblMultiplayerSessionHandle session) {
    if (!session) return kInvalidArg;
    delete static_cast<XblMultiplayerSession*>(session);
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblPresenceSetPresenceAsync(
    XblContextHandle context, int32_t /*isUserActive*/, const char* richPresenceString, void* asyncBlock) {
    if (!context || !richPresenceString) return kInvalidArg;
    return StartXsapiAsync(asyncBlock, "XblPresenceSetPresenceAsync", 0);
}

int32_t SCARLETTX_XSAPI_CALL XblPresenceSetPresenceResult(void* asyncBlock) {
    return FinishXsapiAsync(asyncBlock);
}

}  // extern "C"
