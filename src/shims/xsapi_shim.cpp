#define SCARLETTX_BUILDING_XSAPI 1
#include "scarlettx/services/xsapi_abi.h"
#include "scarlettx/gdk/xgameruntime_abi.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <objbase.h>
#include <windows.h>
#endif

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

bool IsReadableMemoryRange(const void* ptr, size_t len, bool* out_writable = nullptr) {
    if (!ptr || len == 0) return false;
#if defined(_WIN32)
    uintptr_t cur = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = cur + len;
    if (end < cur) return false;
    bool all_writable = true;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            return false;
        }
        const DWORD prot = mbi.Protect & 0xFFu;
        if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD) != 0) {
            return false;
        }
        const bool page_writable =
            prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
        if (!page_writable) {
            all_writable = false;
        }
        const uintptr_t region_end =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cur) {
            return false;
        }
        cur = region_end;
    }
    if (out_writable) {
        *out_writable = all_writable;
    }
    return true;
#else
    if (out_writable) *out_writable = true;
    return true;
#endif
}

bool IsPrintableScidChar(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.';
}

bool LooksLikeDirectScidString(const char* s, size_t* out_len = nullptr) {
    if (!s || !IsReadableMemoryRange(s, 1)) return false;
    size_t i = 0;
    for (; i < 128; ++i) {
        if (!IsReadableMemoryRange(s + i, 1)) return false;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\0') break;
        if (!IsPrintableScidChar(c)) return false;
    }
    if (i == 0 || i >= 128) return false;
    if (out_len) *out_len = i;
    return true;
}

bool LooksLikeUuidScid(const char* s) {
    size_t len = 0;
    if (!LooksLikeDirectScidString(s, &len) || len != 36) return false;
    return s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
}

std::string ExtractScidFromInitializeArg(const char* scid_or_args) {
    constexpr const char* kDefaultScid = "00000000-0000-0000-0000-000000000000";
    if (!scid_or_args) {
        return kDefaultScid;
    }
    // Check if caller passed a GDK XblInitArgs* ({ XTaskQueueHandle queue; const char* scid; }):
    // 8-byte aligned struct whose +8 field points to a valid ASCII SCID string.
    if ((reinterpret_cast<uintptr_t>(scid_or_args) & 0x7u) == 0) {
        bool writable_struct = false;
        if (IsReadableMemoryRange(scid_or_args, sizeof(XblInitArgs), &writable_struct)) {
            XblInitArgs candidate{};
            std::memcpy(&candidate, scid_or_args, sizeof(candidate));
            const bool queue_plausible =
                (candidate.queue == nullptr) || XSystemIsHandleValid(candidate.queue);
            if (candidate.scid != nullptr && queue_plausible) {
                if (candidate.queue != nullptr && XSystemIsHandleValid(candidate.queue) &&
                    LooksLikeDirectScidString(candidate.scid)) {
                    return candidate.scid;
                }
                if (!LooksLikeDirectScidString(scid_or_args) &&
                    LooksLikeDirectScidString(candidate.scid) &&
                    (writable_struct || LooksLikeUuidScid(candidate.scid))) {
                    return candidate.scid;
                }
            }
        }
    }
    if (LooksLikeDirectScidString(scid_or_args)) {
        return scid_or_args;
    }
    return kDefaultScid;
}

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
    const std::string resolved = ExtractScidFromInitializeArg(scid);
    std::lock_guard<std::mutex> lk(g_mu);
    g_scid = resolved;
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

namespace {

namespace fs = std::filesystem;

std::string SanitizeStorageToken(const char* raw) {
    if (!raw || !*raw) return "default";
    std::string out;
    for (const char* p = raw; *p; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            if (c == '.' && !out.empty() && out.back() == '.') continue;
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty() || out == ".") return "blob";
    return out;
}

fs::path ResolveTitleStoragePath(XblContext* ctx, uint32_t storageType, const char* blobPath) {
    const char* env_root = std::getenv("SCARLETTX_SAVE_ROOT");
    const char* env_ts = std::getenv("SCARLETTX_TITLE_STORAGE_DIR");
    fs::path base;
    if (env_root && *env_root) {
        base = fs::path(env_root) / "title_storage";
    } else if (env_ts && *env_ts) {
        base = fs::path(env_ts);
    } else {
        base = fs::temp_directory_path() / "scarlettx_saves" / "title_storage";
    }
    char xuid_hex[32]{};
    std::snprintf(xuid_hex, sizeof(xuid_hex), "%016llX",
                  static_cast<unsigned long long>(ctx ? ctx->xuid : 0ull));
    const std::string scid = SanitizeStorageToken(ctx ? ctx->scid.c_str() : "default_scid");
    return base / scid / std::to_string(storageType) /
           (storageType == XblTitleStorageType_GlobalStorage ? "global" : xuid_hex) /
           SanitizeStorageToken(blobPath);
}

struct TitleStorageDownloadTracker {
    std::mutex mu;
    std::unordered_map<void*, size_t> bytes_by_async;
    size_t last_sync_bytes = 0;
};

TitleStorageDownloadTracker& DlTracker() {
    static TitleStorageDownloadTracker t;
    return t;
}

// Clean-room WinRT IInspectable / IActivationFactory COM broker for Xbox Game Core classes
struct WinRtGuid {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t d4[8];
};

constexpr WinRtGuid kWinRtIUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr WinRtGuid kWinRtIInspectable = {0xAF86E2E0u, 0xB12Du, 0x4C6Au, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
constexpr WinRtGuid kWinRtIAgileObject = {0x94EA2B94u, 0xE9CCu, 0x49E0u, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
constexpr WinRtGuid kWinRtIActivationFactory = {0x00000035u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

bool WinRtGuidEquals(const void* a, const WinRtGuid& b) {
    return a && std::memcmp(a, &b, sizeof(WinRtGuid)) == 0;
}

bool IsZeroWinRtGuid(const WinRtGuid& g) {
    static constexpr WinRtGuid kZero{};
    return std::memcmp(&g, &kZero, sizeof(WinRtGuid)) == 0;
}

bool IsBaseWinRtIid(const WinRtGuid& g) {
    return WinRtGuidEquals(&g, kWinRtIUnknown) ||
           WinRtGuidEquals(&g, kWinRtIInspectable) ||
           WinRtGuidEquals(&g, kWinRtIAgileObject) ||
           WinRtGuidEquals(&g, kWinRtIActivationFactory);
}

WinRtGuid SynthesizeStaticsIidForClass(const wchar_t* cls) {
    // Official IGameSaveProviderStatics IID for Windows.Gaming.XboxLive.Storage.GameSaveProvider
    if (cls && std::wcscmp(cls, L"Windows.Gaming.XboxLive.Storage.GameSaveProvider") == 0) {
        return {0xD0A60E5Bu, 0x3107u, 0x445Au, {0x8B, 0x5C, 0xC5, 0xE0, 0x5D, 0x58, 0xE6, 0xD8}};
    }
    uint64_t h1 = 0xCBF29CE484222325ull;
    uint64_t h2 = 0x84222325CBF29CE4ull;
    for (const wchar_t* p = cls; p && *p; ++p) {
        h1 = (h1 ^ static_cast<uint64_t>(*p)) * 0x100000001B3ull;
        h2 = (h2 ^ (static_cast<uint64_t>(*p) + 0x9E3779B97F4A7C15ull)) * 0x100000001B3ull;
    }
    WinRtGuid g{};
    g.d1 = static_cast<uint32_t>(h1 & 0xFFFFFFFFu);
    g.d2 = static_cast<uint16_t>((h1 >> 32) & 0xFFFFu);
    g.d3 = static_cast<uint16_t>(((h1 >> 48) & 0x0FFFu) | 0x5000u);
    std::memcpy(g.d4, &h2, 8);
    g.d4[0] = static_cast<uint8_t>((g.d4[0] & 0x3Fu) | 0x80u);
    return g;
}

bool IsXboxGameCoreRuntimeClass(const wchar_t* cls) {
    if (!cls || !*cls) return false;
    // Note: "Windows.Gaming.Input." is intentionally NOT intercepted here so desktop
    // Windows combase.dll handles Windows.Gaming.Input.* natively!
    static constexpr const wchar_t* kPrefixes[] = {
        L"Windows.Gaming.XboxLive.",
        L"Windows.ApplicationModel.Store.",
        L"Windows.Media.Protection.",
        L"Windows.System.XboxLive",
        L"Xbox.Services.",
        L"Microsoft.Xbox.Services.",
    };
    for (const wchar_t* pre : kPrefixes) {
        const size_t n = std::wcslen(pre);
        if (std::wcsncmp(cls, pre, n) == 0) return true;
    }
    return false;
}

struct ShimWinRtObject {
    const void* const* vtbl;
    std::atomic<uint32_t> refs{1};
    std::wstring class_name;
    WinRtGuid custom_iid{};
    bool is_factory = false;
};

extern const void* const kShimWinRtFactoryVtbl[32];
extern const void* const kShimWinRtInstanceVtbl[32];

int32_t SCARLETTX_XSAPI_CALL WinRt_QueryInterface(void* self_ptr, const void* riid, void** ppv) {
    if (!ppv) return static_cast<int32_t>(0x80004003u);  // E_POINTER
    *ppv = nullptr;
    if (!self_ptr || !riid) return kInvalidArg;
    auto* self = static_cast<ShimWinRtObject*>(self_ptr);
    if (WinRtGuidEquals(riid, kWinRtIUnknown) ||
        WinRtGuidEquals(riid, kWinRtIInspectable) ||
        WinRtGuidEquals(riid, kWinRtIAgileObject) ||
        (self->is_factory && WinRtGuidEquals(riid, kWinRtIActivationFactory)) ||
        (!IsZeroWinRtGuid(self->custom_iid) && WinRtGuidEquals(riid, self->custom_iid))) {
        self->refs.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return kSuccess;
    }
    return static_cast<int32_t>(0x80004002u);  // E_NOINTERFACE
}

uint32_t SCARLETTX_XSAPI_CALL WinRt_AddRef(void* self_ptr) {
    if (!self_ptr) return 0;
    return static_cast<ShimWinRtObject*>(self_ptr)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_XSAPI_CALL WinRt_Release(void* self_ptr) {
    if (!self_ptr) return 0;
    auto* self = static_cast<ShimWinRtObject*>(self_ptr);
    const uint32_t rem = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

int32_t SCARLETTX_XSAPI_CALL WinRt_GetIids(void* self_ptr, uint32_t* iidCount, void** iids) {
    if (!iidCount || !iids) return static_cast<int32_t>(0x80004003u);
    *iidCount = 0;
    *iids = nullptr;
    if (!self_ptr) return kInvalidArg;
    auto* self = static_cast<ShimWinRtObject*>(self_ptr);
    // WinRT spec: exclude IUnknown, IInspectable, and IAgileObject from GetIids.
    WinRtGuid list[2]{};
    uint32_t count = 0;
    if (self->is_factory) {
        list[count++] = kWinRtIActivationFactory;
    }
    if (!IsZeroWinRtGuid(self->custom_iid) && !IsBaseWinRtIid(self->custom_iid)) {
        list[count++] = self->custom_iid;
    }
    if (count == 0) {
        return kSuccess;
    }
#if defined(_WIN32)
    auto* mem = static_cast<WinRtGuid*>(CoTaskMemAlloc(sizeof(WinRtGuid) * count));
#else
    auto* mem = static_cast<WinRtGuid*>(std::malloc(sizeof(WinRtGuid) * count));
#endif
    if (!mem) return static_cast<int32_t>(0x8007000Eu);
    std::memcpy(mem, list, sizeof(WinRtGuid) * count);
    *iidCount = count;
    *iids = mem;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL WinRt_GetRuntimeClassName(void* self_ptr, void** outHString) {
    if (!outHString) return static_cast<int32_t>(0x80004003u);
    *outHString = nullptr;
    if (!self_ptr) return kInvalidArg;
    auto* self = static_cast<ShimWinRtObject*>(self_ptr);
    if (self->is_factory) {
        return static_cast<int32_t>(0x8000000Eu);  // E_ILLEGAL_METHOD_CALL per WinRT IActivationFactory spec
    }
#if defined(_WIN32)
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    if (combase) {
        using FnCreateStr = int32_t(WINAPI*)(const wchar_t*, uint32_t, void**);
        auto create_str = reinterpret_cast<FnCreateStr>(reinterpret_cast<void*>(
            GetProcAddress(combase, "WindowsCreateString")));
        if (create_str) {
            return create_str(self->class_name.c_str(),
                              static_cast<uint32_t>(self->class_name.size()),
                              outHString);
        }
    }
#endif
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL WinRt_GetTrustLevel(void* /*self_ptr*/, int32_t* trustLevel) {
    if (!trustLevel) return static_cast<int32_t>(0x80004003u);
    *trustLevel = 0;  // BaseTrust
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL WinRt_ActivateInstance(void* self_ptr, void** instance) {
    if (!instance) return static_cast<int32_t>(0x80004003u);
    *instance = nullptr;
    if (!self_ptr) return kInvalidArg;
    auto* self = static_cast<ShimWinRtObject*>(self_ptr);
    if (!self->is_factory) {
        return static_cast<int32_t>(0x80004001u);  // E_NOTIMPL
    }
    auto* inst = new (std::nothrow) ShimWinRtObject();
    if (!inst) return static_cast<int32_t>(0x8007000Eu);
    inst->vtbl = kShimWinRtInstanceVtbl;
    inst->class_name = self->class_name;
    inst->custom_iid = WinRtGuid{};
    inst->is_factory = false;
    *instance = inst;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL WinRt_UnimplementedMethod(void* /*self_ptr*/) {
    // Never write to rdx ([in] arg 1); fail cleanly with E_NOTIMPL.
    return static_cast<int32_t>(0x80004001u);
}

#define SHIM_WINRT_STUB_SLOTS_7_TO_31 \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod), \
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod)

const void* const kShimWinRtFactoryVtbl[32] = {
    reinterpret_cast<const void*>(&WinRt_QueryInterface),       // 0 IUnknown::QueryInterface
    reinterpret_cast<const void*>(&WinRt_AddRef),               // 1 IUnknown::AddRef
    reinterpret_cast<const void*>(&WinRt_Release),              // 2 IUnknown::Release
    reinterpret_cast<const void*>(&WinRt_GetIids),              // 3 IInspectable::GetIids
    reinterpret_cast<const void*>(&WinRt_GetRuntimeClassName),  // 4 IInspectable::GetRuntimeClassName
    reinterpret_cast<const void*>(&WinRt_GetTrustLevel),        // 5 IInspectable::GetTrustLevel
    reinterpret_cast<const void*>(&WinRt_ActivateInstance),     // 6 IActivationFactory::ActivateInstance
    SHIM_WINRT_STUB_SLOTS_7_TO_31,
};

const void* const kShimWinRtInstanceVtbl[32] = {
    reinterpret_cast<const void*>(&WinRt_QueryInterface),       // 0 IUnknown::QueryInterface
    reinterpret_cast<const void*>(&WinRt_AddRef),               // 1 IUnknown::AddRef
    reinterpret_cast<const void*>(&WinRt_Release),              // 2 IUnknown::Release
    reinterpret_cast<const void*>(&WinRt_GetIids),              // 3 IInspectable::GetIids
    reinterpret_cast<const void*>(&WinRt_GetRuntimeClassName),  // 4 IInspectable::GetRuntimeClassName
    reinterpret_cast<const void*>(&WinRt_GetTrustLevel),        // 5 IInspectable::GetTrustLevel
    reinterpret_cast<const void*>(&WinRt_UnimplementedMethod),  // 6 Non-factory slot 6 -> E_NOTIMPL
    SHIM_WINRT_STUB_SLOTS_7_TO_31,
};

#undef SHIM_WINRT_STUB_SLOTS_7_TO_31

}  // namespace

extern "C" {

int32_t SCARLETTX_XSAPI_CALL XblTitleStorageUploadBlobAsync(
    XblContextHandle context,
    const char* blobPath,
    uint32_t storageType,
    const uint8_t* blobBuffer,
    size_t blobBufferSize,
    void* asyncBlock) {
    if (!context || !blobPath || !*blobPath || (!blobBuffer && blobBufferSize > 0)) {
        return kInvalidArg;
    }
    const fs::path path = ResolveTitleStoragePath(static_cast<XblContext*>(context), storageType, blobPath);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return static_cast<int32_t>(0x80004005u);
    if (blobBufferSize > 0) {
        ofs.write(reinterpret_cast<const char*>(blobBuffer), static_cast<std::streamsize>(blobBufferSize));
    }
    if (!ofs.good()) return static_cast<int32_t>(0x80004005u);
    return StartXsapiAsync(asyncBlock, "XblTitleStorageUploadBlobAsync", 0);
}

int32_t SCARLETTX_XSAPI_CALL XblTitleStorageUploadBlobResult(void* asyncBlock) {
    return FinishXsapiAsync(asyncBlock);
}

int32_t SCARLETTX_XSAPI_CALL XblTitleStorageDownloadBlobAsync(
    XblContextHandle context,
    const char* blobPath,
    uint32_t storageType,
    uint8_t* blobBuffer,
    size_t blobBufferSize,
    void* asyncBlock) {
    if (!context || !blobPath || !*blobPath || !blobBuffer || blobBufferSize == 0) {
        return kInvalidArg;
    }
    const fs::path path = ResolveTitleStoragePath(static_cast<XblContext*>(context), storageType, blobPath);
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return static_cast<int32_t>(0x80070490u);  // E_NOTFOUND
    const auto sz = static_cast<size_t>(std::max<std::streamoff>(0, ifs.tellg()));
    if (blobBufferSize < sz) return static_cast<int32_t>(0x8007007Au);  // E_NOT_SUFFICIENT_BUFFER
    ifs.seekg(0, std::ios::beg);
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(blobBuffer), static_cast<std::streamsize>(sz));
    }
    {
        auto& tr = DlTracker();
        std::lock_guard<std::mutex> lk(tr.mu);
        if (asyncBlock) tr.bytes_by_async[asyncBlock] = sz;
        else tr.last_sync_bytes = sz;
    }
    return StartXsapiAsync(asyncBlock, "XblTitleStorageDownloadBlobAsync", sizeof(size_t));
}

int32_t SCARLETTX_XSAPI_CALL XblTitleStorageDownloadBlobResult(
    void* asyncBlock,
    size_t* bytesDownloaded) {
    const int32_t hr = FinishXsapiAsync(asyncBlock);
    if (hr < 0) return hr;
    size_t n = 0;
    {
        auto& tr = DlTracker();
        std::lock_guard<std::mutex> lk(tr.mu);
        if (asyncBlock) {
            auto it = tr.bytes_by_async.find(asyncBlock);
            if (it != tr.bytes_by_async.end()) {
                n = it->second;
                tr.bytes_by_async.erase(it);
            }
        } else {
            n = tr.last_sync_bytes;
        }
    }
    if (bytesDownloaded) *bytesDownloaded = n;
    return kSuccess;
}

int32_t SCARLETTX_XSAPI_CALL XblTitleStorageDeleteBlobAsync(
    XblContextHandle context,
    const char* blobPath,
    uint32_t storageType,
    void* asyncBlock) {
    if (!context || !blobPath || !*blobPath) return kInvalidArg;
    const fs::path path = ResolveTitleStoragePath(static_cast<XblContext*>(context), storageType, blobPath);
    std::error_code ec;
    if (!fs::exists(path, ec)) return static_cast<int32_t>(0x80070490u);
    fs::remove(path, ec);
    return StartXsapiAsync(asyncBlock, "XblTitleStorageDeleteBlobAsync", 0);
}

int32_t SCARLETTX_XSAPI_CALL XblTitleStorageDeleteBlobResult(void* asyncBlock) {
    return FinishXsapiAsync(asyncBlock);
}

int32_t SCARLETTX_XSAPI_CALL XblWinRtActivateClassByName(
    const wchar_t* runtimeClassName,
    const void* riid,
    int32_t isFactory,
    void** outInspectable) {
    if (!outInspectable) return static_cast<int32_t>(0x80004003u);
    *outInspectable = nullptr;
    if (!runtimeClassName || !*runtimeClassName) return kInvalidArg;
    if (!IsXboxGameCoreRuntimeClass(runtimeClassName)) {
#if defined(_WIN32)
        // GuestRoActivateInstance passes (cls, nullptr, 0, out) and GuestRoGetActivationFactory
        // passes (cls, iid, 1, out); both receive REGDB_E_CLASSNOTREG so combase.dll handles
        // Windows.Gaming.Input.* natively. If a synthetic caller (e.g. ue5_slice) directly calls
        // XblWinRtActivateClassByName(L"Windows.Gaming.Input.*", &IID_IInspectable, 0, out),
        // forward directly to combase.dll's native RoGetActivationFactory!
        if (isFactory == 0 && riid != nullptr &&
            std::wcsncmp(runtimeClassName, L"Windows.Gaming.Input.", 21) == 0) {
            static HMODULE combase = LoadLibraryW(L"combase.dll");
            if (combase) {
                using FnRoInit = int32_t(WINAPI*)(int32_t);
                using FnCreateStr = int32_t(WINAPI*)(const wchar_t*, uint32_t, void**);
                using FnDeleteStr = int32_t(WINAPI*)(void*);
                using FnGetFac = int32_t(WINAPI*)(void*, const void*, void**);
                auto ro_init = reinterpret_cast<FnRoInit>(reinterpret_cast<void*>(
                    GetProcAddress(combase, "RoInitialize")));
                auto create_str = reinterpret_cast<FnCreateStr>(reinterpret_cast<void*>(
                    GetProcAddress(combase, "WindowsCreateString")));
                auto delete_str = reinterpret_cast<FnDeleteStr>(reinterpret_cast<void*>(
                    GetProcAddress(combase, "WindowsDeleteString")));
                auto get_fac = reinterpret_cast<FnGetFac>(reinterpret_cast<void*>(
                    GetProcAddress(combase, "RoGetActivationFactory")));
                if (ro_init) (void)ro_init(1 /* RO_INIT_MULTITHREADED */);
                if (create_str && delete_str && get_fac) {
                    void* hstr = nullptr;
                    if (create_str(runtimeClassName, static_cast<uint32_t>(std::wcslen(runtimeClassName)), &hstr) == 0 &&
                        hstr) {
                        const int32_t hr = get_fac(hstr, riid, outInspectable);
                        delete_str(hstr);
                        if (hr == 0 && *outInspectable) {
                            return kSuccess;
                        }
                    }
                }
            }
        }
#endif
        return static_cast<int32_t>(0x80040154u);  // REGDB_E_CLASSNOTREG -> caller falls back to combase.dll
    }
    auto* obj = new (std::nothrow) ShimWinRtObject();
    if (!obj) return static_cast<int32_t>(0x8007000Eu);
    obj->is_factory = (isFactory != 0);
    obj->vtbl = obj->is_factory ? kShimWinRtFactoryVtbl : kShimWinRtInstanceVtbl;
    obj->class_name = runtimeClassName;
    if (riid && !IsBaseWinRtIid(*static_cast<const WinRtGuid*>(riid))) {
        obj->custom_iid = *static_cast<const WinRtGuid*>(riid);
    } else if (obj->is_factory) {
        obj->custom_iid = SynthesizeStaticsIidForClass(runtimeClassName);
    } else {
        obj->custom_iid = WinRtGuid{};
    }
    *outInspectable = obj;
    return kSuccess;
}

}  // extern "C"
