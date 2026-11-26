#define SCARLETTX_BUILDING_XSAPI 1
#include "scarlettx/services/xsapi_abi.h"

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
    XblContextHandle context, uint32_t /*titleId*/, void* /*asyncBlock*/) {
    if (!context) return kInvalidArg;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblAchievementsGetAchievementsForTitleIdResult(
    void* /*asyncBlock*/, size_t bufferSize, void* buffer, XblAchievementsResultHandle* resultHandle, size_t* bufferUsed) {
    if (resultHandle) *resultHandle = reinterpret_cast<XblAchievementsResultHandle>(0x1000);
    if (bufferUsed) *bufferUsed = sizeof(uint32_t);
    if (buffer && bufferSize >= sizeof(uint32_t)) {
        *static_cast<uint32_t*>(buffer) = 0; // 0 achievements unlocked in offline mode
    }
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblAchievementsUpdateAchievementAsync(
    XblContextHandle context, const char* achievementId, uint32_t /*percentComplete*/, void* /*asyncBlock*/) {
    if (!context || !achievementId) return kInvalidArg;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblAchievementsUpdateAchievementResult(void* /*asyncBlock*/) {
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblProfileGetUserProfileAsync(
    XblContextHandle context, uint64_t /*xboxUserId*/, void* /*asyncBlock*/) {
    if (!context) return kInvalidArg;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblProfileGetUserProfileResult(
    void* /*asyncBlock*/, size_t bufferSize, void* buffer, XblUserProfile* userProfile, size_t* bufferUsed) {
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
    XblContextHandle context, int32_t /*isUserActive*/, const char* richPresenceString, void* /*asyncBlock*/) {
    if (!context || !richPresenceString) return kInvalidArg;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblPresenceSetPresenceResult(void* /*asyncBlock*/) {
    return kSuccess;
}

}  // extern "C"
