#include "scarlettx/gdk/xgameruntime_abi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <malloc.h>
#  include <windows.h>
#endif

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/kernel/xruntime.h"
#include "xgr_api_tables.h"
#include "xgr_task_queue.h"

namespace fs = std::filesystem;

namespace {

bool TraceEnabled() {
    static std::atomic<int> cached{0};  // 0 = uninitialized, 1 = off, 2 = on (constexpr: no PE TLS)
    int s = cached.load(std::memory_order_relaxed);
    if (s == 0) {
        const char* v = std::getenv("SCARLETTX_TRACE");
        const bool on = v && v[0] != '\0' && v[0] != '0';
        s = on ? 2 : 1;
        cached.store(s, std::memory_order_relaxed);
    }
    return s == 2;
}

// ---------------------------------------------------------------------------
// Global handle registry (for XSystemIsHandleValid)
// ---------------------------------------------------------------------------
std::mutex g_valid_handles_mutex;
std::unordered_set<void*> g_valid_handles;

void RegisterValidHandle(void* handle) {
    if (!handle) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_valid_handles_mutex);
    g_valid_handles.insert(handle);
}

void UnregisterValidHandle(void* handle) {
    if (!handle) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_valid_handles_mutex);
    g_valid_handles.erase(handle);
}

bool IsRegisteredHandle(void* handle) {
    if (!handle) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_valid_handles_mutex);
    return g_valid_handles.count(handle) > 0;
}

// ---------------------------------------------------------------------------
// Helper for copying C strings into GDK output buffers
// ---------------------------------------------------------------------------
XGameResult CopyStringToBuffer(const std::string& src,
                               size_t dst_size,
                               char* dst,
                               size_t* used) {
    const size_t required = src.size() + 1;
    if (used) {
        *used = required;
    }
    if (!dst || dst_size < required) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(dst, src.c_str(), required);
    return XGR_S_OK;
}

// ---------------------------------------------------------------------------
// Async helpers for HLE-implemented operations (engine: xgr_task_queue.cpp)
// ---------------------------------------------------------------------------
using XAsyncState = xgr::HleAsyncState;

XGameResult BeginAsyncOperation(XAsyncBlock* async,
                                std::function<void(XAsyncState*)> work_fn) {
    return xgr::BeginHleAsync(async, std::move(work_fn));
}

void CompleteAsyncState(XAsyncState* state, XGameResult hr, size_t result_size = 0) {
    xgr::CompleteHleAsync(state, hr, result_size);
}

std::shared_ptr<XAsyncState> GetAsyncState(XAsyncBlock* async) {
    return xgr::GetHleAsyncState(async);
}

// ---------------------------------------------------------------------------
// XUser implementation
// ---------------------------------------------------------------------------
}  // namespace

struct XUser {
    std::atomic<int32_t> ref_count{1};
    uint64_t xuid = 0x00090000005CA001ull;
    uint64_t local_id = 1;
    std::string gamertag_classic = "ScarlettXUser";
    std::string gamertag_modern = "ScarlettX";
    std::string gamertag_suffix = "1001";
    std::string gamertag_unique = "ScarlettX#1001";
    XUserState state = XUserState_SignedIn;
    XUserAgeGroup age_group = XUserAgeGroup_Adult;
    bool is_guest = false;
};

// ---------------------------------------------------------------------------
// XGameSave implementation (real persistent local-disk containers & blobs)
// ---------------------------------------------------------------------------
constexpr uint64_t kSaveQuotaBytes = 256ull * 1024ull * 1024ull;  // 256 MiB
constexpr const char* kSaveMetaFileName = "_meta.txt";

struct XGameSaveProviderCore {
    std::string config_id;
    fs::path root_dir;
    uint64_t max_quota_bytes = kSaveQuotaBytes;
    std::mutex mutex;
};

struct XGameSaveContainerCore {
    std::shared_ptr<XGameSaveProviderCore> provider;
    std::string name;
    fs::path dir_path;
};

struct XGameSaveUpdateCore {
    std::mutex mutex;
    std::shared_ptr<XGameSaveContainerCore> container;
    std::string display_name;
    std::map<std::string, std::vector<uint8_t>> pending_writes;
    std::vector<std::string> pending_deletes;
};

struct XGameSaveProvider {
    std::atomic<int32_t> ref_count{1};
    std::shared_ptr<XGameSaveProviderCore> core;
};

struct XGameSaveContainer {
    XGameSaveProviderHandle provider_handle = nullptr;
    std::shared_ptr<XGameSaveContainerCore> core;
};

struct XGameSaveUpdate {
    std::shared_ptr<XGameSaveUpdateCore> core;
};

namespace {

std::string SanitizeSaveName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out == kSaveMetaFileName) {
        out = "__meta.txt";
    }
    return out.empty() ? "default" : out;
}

// A name of only dots resolves to the directory itself or its parent.
bool IsValidSaveName(const char* name) {
    return name && name[std::strspn(name, ".")] != '\0';
}

uint64_t ConfiguredSaveQuotaBytes() {
    if (const char* env_q = std::getenv("SCARLETTX_SAVE_QUOTA_BYTES"); env_q && *env_q) {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(env_q, &end, 10);
        if (end && end != env_q && parsed > 0) {
            return static_cast<uint64_t>(parsed);
        }
    }
    return kSaveQuotaBytes;
}

fs::path ResolveSaveRoot(const std::string& config_id) {
    const char* env_root = std::getenv("SCARLETTX_SAVE_ROOT");
    fs::path base = (env_root && *env_root)
                        ? fs::path(env_root)
                        : (fs::temp_directory_path() / "scarlettx_saves");
    return base / SanitizeSaveName(config_id);
}

uint64_t ComputeDirectorySize(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return 0;
    }
    uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename() != kSaveMetaFileName) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

std::string ReadContainerDisplayName(const fs::path& container_dir, const std::string& fallback_name) {
    std::ifstream ifs(container_dir / kSaveMetaFileName, std::ios::binary);
    if (!ifs) {
        return fallback_name;
    }
    std::string line;
    std::getline(ifs, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line.empty() ? fallback_name : line;
}

// ---------------------------------------------------------------------------
// XPackage locale
// ---------------------------------------------------------------------------
std::string HostLocaleName() {
#if defined(_WIN32)
    wchar_t wide[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH) > 0) {
        std::string name;
        for (const wchar_t* p = wide; *p; ++p) {
            name.push_back(static_cast<char>(*p));  // locale names are ASCII
        }
        if (!name.empty()) {
            return name;
        }
    }
#endif
    return "en-US";
}

// The languages MicrosoftGame.config declares, in file order.
std::vector<std::string> PackageLanguages() {
    std::vector<std::string> languages;
    const char* root = std::getenv("SCARLETTX_PACKAGE_ROOT");
    if (!root || !*root) {
        return languages;
    }
    std::string xml;
    for (const char* name : {"MicrosoftGame.Config", "MicrosoftGame.config"}) {
        std::ifstream in(fs::path(root) / name, std::ios::binary);
        if (in) {
            xml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            break;
        }
    }
    // The template's commented-out examples are not declarations.
    for (size_t open; (open = xml.find("<!--")) != std::string::npos;) {
        const size_t close = xml.find("-->", open + 4);
        xml.erase(open, close == std::string::npos ? std::string::npos : close + 3 - open);
    }
    for (size_t pos = 0; (pos = xml.find("<Resource", pos)) != std::string::npos;) {
        pos += 9;
        const size_t end = xml.find('>', pos);
        if (end == std::string::npos) {
            break;
        }
        if (!std::isspace(static_cast<unsigned char>(xml[pos]))) {
            continue;  // <Resources>
        }
        const std::string tag = xml.substr(pos, end - pos);
        const size_t attr = tag.find("Language");
        const size_t quote = attr == std::string::npos ? attr : tag.find_first_of("\"'", attr);
        const size_t close = quote == std::string::npos ? quote : tag.find(tag[quote], quote + 1);
        if (close != std::string::npos) {
            languages.push_back(tag.substr(quote + 1, close - quote - 1));
        }
    }
    return languages;
}

bool SameText(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// The declared language that serves `locale`: the same name, else the same
// language of another region, else the package's first language.
std::string MatchPackageLanguage(const std::string& locale, const std::vector<std::string>& languages) {
    if (languages.empty()) {
        return locale;
    }
    for (const auto& language : languages) {
        if (SameText(language, locale)) {
            return language;
        }
    }
    const std::string primary = locale.substr(0, locale.find('-'));
    for (const auto& language : languages) {
        if (SameText(language.substr(0, language.find('-')), primary)) {
            return language;
        }
    }
    return languages.front();
}

// ---------------------------------------------------------------------------
// XMem hooks & region tracking
// ---------------------------------------------------------------------------
std::mutex g_mem_hooks_mutex;
XMemAllocFunction* g_mem_alloc_hook = nullptr;
XMemFreeFunction* g_mem_free_hook = nullptr;
std::atomic<size_t> g_mem_total_allocated{0};
std::atomic<size_t> g_mem_total_reserved{0};

struct VMemRegion {
    uintptr_t base = 0;
    size_t reserved_size = 0;
    size_t committed_bytes = 0;
    bool registered_kernelx = false;
};

std::mutex g_vmem_mutex;
std::map<uintptr_t, VMemRegion> g_vmem_regions;

// ---------------------------------------------------------------------------
// XThread FLS state (zero implicit PE TLS in xgameruntime.dll)
// ---------------------------------------------------------------------------
std::mutex g_ts_mutex;
std::unordered_map<uint32_t, bool> g_ts_by_tid;
std::mutex g_missing_api_mutex;
std::unordered_set<std::string> g_missing_apis;

#if defined(_WIN32)
INIT_ONCE g_ts_fls_once = INIT_ONCE_STATIC_INIT;
DWORD g_ts_fls_index = FLS_OUT_OF_INDEXES;

BOOL CALLBACK InitTsFlsOnce(PINIT_ONCE, PVOID, PVOID*) {
    g_ts_fls_index = FlsAlloc(nullptr);
    return TRUE;
}

bool GetCurrentThreadFlsTimeSensitive() {
    InitOnceExecuteOnce(&g_ts_fls_once, &InitTsFlsOnce, nullptr, nullptr);
    const uint32_t tid = GetCurrentThreadId();
    {
        std::lock_guard<std::mutex> lock(g_ts_mutex);
        auto it = g_ts_by_tid.find(tid);
        if (it != g_ts_by_tid.end()) {
            const bool val = it->second;
            if (g_ts_fls_index != FLS_OUT_OF_INDEXES) {
                FlsSetValue(g_ts_fls_index, reinterpret_cast<PVOID>(static_cast<uintptr_t>(val ? 1 : 0)));
            }
            return val;
        }
    }
    if (g_ts_fls_index == FLS_OUT_OF_INDEXES) {
        return false;
    }
    return FlsGetValue(g_ts_fls_index) != nullptr;
}

void SetCurrentThreadFlsTimeSensitive(bool is_ts) {
    InitOnceExecuteOnce(&g_ts_fls_once, &InitTsFlsOnce, nullptr, nullptr);
    if (g_ts_fls_index != FLS_OUT_OF_INDEXES) {
        FlsSetValue(g_ts_fls_index, reinterpret_cast<PVOID>(static_cast<uintptr_t>(is_ts ? 1 : 0)));
    }
    const uint32_t tid = GetCurrentThreadId();
    std::lock_guard<std::mutex> lock(g_ts_mutex);
    g_ts_by_tid[tid] = is_ts;
}
#else
thread_local bool g_posix_time_sensitive = false;
bool GetCurrentThreadFlsTimeSensitive() { return g_posix_time_sensitive; }
void SetCurrentThreadFlsTimeSensitive(bool is_ts) { g_posix_time_sensitive = is_ts; }
#endif

}  // namespace

std::string scarlettx::xgr::StoredSaveName(const std::string& name) { return SanitizeSaveName(name); }

extern "C" {

// ===========================================================================
// XGameRuntimeInit.h
// ===========================================================================
XGameResult XGameRuntimeInitialize() {
    auto st = scarlettx::XGameRuntimeHle::Instance().Initialize(
        scarlettx::DeviceType::XboxScarlettAnaconda);
    if (!st.ok()) {
        return XGR_E_FAIL;
    }
    xgr::EnsureProcessTaskQueue();
    return XGR_S_OK;
}

XGameResult XGameRuntimeInitializeWithOptions(const XGameRuntimeOptions* /*options*/) {
    return XGameRuntimeInitialize();
}

void XGameRuntimeUninitialize() {
    xgr::ShutdownTaskSystem();
    scarlettx::XGameRuntimeHle::Instance().Shutdown();
}

XGameResult XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature feature, bool* isAvailable) {
    if (!isAvailable) {
        return XGR_E_POINTER;
    }
    *isAvailable = static_cast<uint32_t>(feature) <= 21u;
    return XGR_S_OK;
}

// ===========================================================================
// XSystem.h
// ===========================================================================
uint32_t XSystemGetDeviceType() {
    return static_cast<uint32_t>(scarlettx::XGameRuntimeHle::Instance().GetDeviceType());
}

XSystemAnalyticsInfo XSystemGetAnalyticsInfo() {
    XSystemAnalyticsInfo info{};
    info.osVersion.major = 10;
    info.osVersion.minor = 0;
    info.osVersion.build = 26100;
    info.osVersion.revision = 2499;
    info.hostingOsVersion = info.osVersion;
    std::strncpy(info.family, "Xbox", sizeof(info.family) - 1);
    std::strncpy(info.form, "Xbox Series X", sizeof(info.form) - 1);
    return info;
}

XSystemRuntimeInfo XSystemGetRuntimeInfo() {
    XSystemRuntimeInfo info{};
    info.runtimeVersion.major = 10;
    info.runtimeVersion.minor = 0;
    info.runtimeVersion.build = 26100;
    info.runtimeVersion.revision = 2499;
    info.availableVersion = info.runtimeVersion;
    return info;
}

XGameResult XSystemGetAppSpecificDeviceId(
    size_t appSpecificDeviceIdSize,
    char* appSpecificDeviceId,
    size_t* appSpecificDeviceIdUsed) {
    return CopyStringToBuffer(
        "SCARLETTX-DEV-00000001-ANACONDA",
        appSpecificDeviceIdSize,
        appSpecificDeviceId,
        appSpecificDeviceIdUsed);
}

XGameResult XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed) {
    constexpr char kConsoleId[] = "SCARLETTX.CONSOLE.0001.000000000000000";
    static_assert(sizeof(kConsoleId) == 39, "XSystemConsoleIdBytes must be 38 chars + NUL");
    if (!consoleId || consoleIdSize == 0) {
        if (consoleIdUsed) {
            *consoleIdUsed = sizeof(kConsoleId);
        }
        return consoleId ? XGR_E_NOT_SUFFICIENT_BUFFER : XGR_E_POINTER;
    }
    if (consoleIdSize >= sizeof(kConsoleId)) {
        std::memcpy(consoleId, kConsoleId, sizeof(kConsoleId));
        if (consoleIdUsed) {
            *consoleIdUsed = sizeof(kConsoleId);
        }
        return XGR_S_OK;
    }
    const size_t copy_chars = consoleIdSize - 1;
    std::memcpy(consoleId, kConsoleId, copy_chars);
    consoleId[copy_chars] = '\0';
    if (consoleIdUsed) {
        *consoleIdUsed = consoleIdSize;
    }
    return XGR_S_OK;
}

XGameResult XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed) {
    return CopyStringToBuffer(
        "RETAIL",
        sandboxIdSize,
        sandboxId,
        sandboxIdUsed);
}

static std::atomic<bool> g_allow_full_download_bandwidth{false};
void XSystemAllowFullDownloadBandwidth(bool enable) {
    g_allow_full_download_bandwidth.store(enable, std::memory_order_relaxed);
    if (TraceEnabled()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XSystemAllowFullDownloadBandwidth(%s)\n",
                     enable ? "true" : "false");
        std::fflush(stderr);
    }
}

bool XSystemIsHandleValid(void* handle) {
    return IsRegisteredHandle(handle) || xgr::IsTaskQueueHandle(handle);
}

// ===========================================================================
// XUser.h
// ===========================================================================
XGameResult XUserAddAsync(XUserAddOptions /*options*/, XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    return BeginAsyncOperation(async, [](XAsyncState* st) {
        auto* user = new (std::nothrow) XUser();
        if (!user) {
            CompleteAsyncState(st, XGR_E_OUTOFMEMORY, 0);
            return;
        }
        scarlettx::XUserProfile profile;
        if (scarlettx::XGameRuntimeHle::Instance().GetDefaultUser(profile)) {
            user->xuid = profile.xuid;
            user->gamertag_classic = profile.gamertag;
        }
        RegisterValidHandle(user);
        {
            std::lock_guard<std::mutex> lk(st->mutex);
            st->result_handle = user;
            st->cleanup_result = [](void* h) { XUserCloseHandle(static_cast<XUserHandle>(h)); };
        }
        CompleteAsyncState(st, XGR_S_OK, sizeof(XUserHandle));
    });
}

XGameResult XUserAddResult(XAsyncBlock* async, XUserHandle* newUser) {
    if (!async || !newUser) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *newUser = nullptr;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        *newUser = nullptr;
        return XGR_E_FAIL;
    }
    XUserHandle out_user = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        out_user = static_cast<XUserHandle>(state->result_handle);
        state->result_handle = nullptr;
        state->cleanup_result = nullptr;
    }
    if (!out_user) {
        *newUser = nullptr;
        return XGR_E_FAIL;
    }
    *newUser = out_user;
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XUserDuplicateHandle(XUserHandle handle, XUserHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return XGR_E_INVALIDARG;
    }
    handle->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return XGR_S_OK;
}

void XUserCloseHandle(XUserHandle user) {
    if (!user) {
        return;
    }
    if (user->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        UnregisterValidHandle(user);
        delete user;
    }
}

XGameResult XUserGetId(XUserHandle user, uint64_t* userId) {
    if (!user || !userId) {
        return XGR_E_INVALIDARG;
    }
    *userId = user->xuid;
    return XGR_S_OK;
}

XGameResult XUserGetLocalId(XUserHandle user, XUserLocalId* userLocalId) {
    if (!user || !userLocalId) {
        return XGR_E_INVALIDARG;
    }
    userLocalId->value = user->local_id;
    return XGR_S_OK;
}

XGameResult XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed) {
    if (!user) {
        return XGR_E_INVALIDARG;
    }
    const std::string* src = &user->gamertag_classic;
    switch (gamertagComponent) {
        case XUserGamertagComponent_Classic:
            src = &user->gamertag_classic;
            break;
        case XUserGamertagComponent_Modern:
            src = &user->gamertag_modern;
            break;
        case XUserGamertagComponent_ModernSuffix:
            src = &user->gamertag_suffix;
            break;
        case XUserGamertagComponent_UniqueModern:
            src = &user->gamertag_unique;
            break;
        default:
            return XGR_E_INVALIDARG;
    }
    return CopyStringToBuffer(*src, gamertagSize, gamertag, gamertagUsed);
}

XGameResult XUserGetState(XUserHandle user, XUserState* state) {
    if (!user || !state) {
        return XGR_E_INVALIDARG;
    }
    *state = user->state;
    return XGR_S_OK;
}

XGameResult XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* ageGroup) {
    if (!user || !ageGroup) {
        return XGR_E_INVALIDARG;
    }
    *ageGroup = user->age_group;
    return XGR_S_OK;
}

XGameResult XUserGetIsGuest(XUserHandle user, bool* isGuest) {
    if (!user || !isGuest) {
        return XGR_E_INVALIDARG;
    }
    *isGuest = user->is_guest;
    return XGR_S_OK;
}

XGameResult XUserGetMaxUsers(uint32_t* maxUsers) {
    if (!maxUsers) {
        return XGR_E_POINTER;
    }
    *maxUsers = 16;
    return XGR_S_OK;
}

XGameResult XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions /*options*/,
    XUserPrivilege /*privilege*/,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason) {
    if (!user || !hasPrivilege) {
        return XGR_E_INVALIDARG;
    }
    *hasPrivilege = true;
    if (reason) {
        *reason = XUserPrivilegeDenyReason_None;
    }
    return XGR_S_OK;
}

XGameResult XUserGetDefaultXuid(uint64_t* out_xuid) {
    if (!out_xuid) {
        return XGR_E_INVALIDARG;
    }
    scarlettx::XUserProfile user;
    if (!scarlettx::XGameRuntimeHle::Instance().GetDefaultUser(user)) {
        return XGR_E_GAMERUNTIME_NOT_INITIALIZED;
    }
    *out_xuid = user.xuid;
    return XGR_S_OK;
}

XGameResult XUserFindUserById(uint64_t userId, XUserHandle* handle) {
    if (!handle) {
        return XGR_E_INVALIDARG;
    }
    auto* user = new (std::nothrow) XUser();
    if (!user) {
        return XGR_E_OUTOFMEMORY;
    }
    user->xuid = userId;
    RegisterValidHandle(user);
    *handle = user;
    return XGR_S_OK;
}

XGameResult XUserFindUserByLocalId(XUserLocalId userLocalId, XUserHandle* handle) {
    if (!handle) {
        return XGR_E_INVALIDARG;
    }
    auto* user = new (std::nothrow) XUser();
    if (!user) {
        return XGR_E_OUTOFMEMORY;
    }
    user->local_id = userLocalId.value;
    RegisterValidHandle(user);
    *handle = user;
    return XGR_S_OK;
}

XGameResult XUserIsSystemReserved(XUserHandle user, bool* isSystemReserved) {
    if (!user || !isSystemReserved) {
        return XGR_E_INVALIDARG;
    }
    *isSystemReserved = false;
    return XGR_S_OK;
}

XGameResult XUserIsSponsoredUser(XUserHandle user, bool* isSponsoredUser) {
    if (!user || !isSponsoredUser) {
        return XGR_E_INVALIDARG;
    }
    *isSponsoredUser = false;
    return XGR_S_OK;
}

XGameResult XUserResolvePrivilegeWithUiAsync(
    XUserHandle user,
    XUserPrivilegeOptions /*options*/,
    XUserPrivilege /*privilege*/,
    XAsyncBlock* async) {
    if (!user || !async) {
        return XGR_E_INVALIDARG;
    }
    return BeginAsyncOperation(async, [](XAsyncState* st) {
        CompleteAsyncState(st, XGR_S_OK, 0);
    });
}

XGameResult XUserResolvePrivilegeWithUiResult(XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    xgr::ReleaseHleAsyncResult(async);
    return hr;
}

XGameResult XUserResolveIssueWithUiAsync(
    XUserHandle user,
    const char* /*url*/,
    XAsyncBlock* async) {
    if (!user || !async) {
        return XGR_E_INVALIDARG;
    }
    return BeginAsyncOperation(async, [](XAsyncState* st) {
        CompleteAsyncState(st, XGR_S_OK, 0);
    });
}

XGameResult XUserResolveIssueWithUiResult(XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    xgr::ReleaseHleAsyncResult(async);
    return hr;
}

XGameResult XUserResolveIssueWithUiUtf16Async(
    XUserHandle user,
    const wchar_t* /*url*/,
    XAsyncBlock* async) {
    if (!user || !async) {
        return XGR_E_INVALIDARG;
    }
    return BeginAsyncOperation(async, [](XAsyncState* st) {
        CompleteAsyncState(st, XGR_S_OK, 0);
    });
}

XGameResult XUserResolveIssueWithUiUtf16Result(XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    xgr::ReleaseHleAsyncResult(async);
    return hr;
}

}  // extern "C"

namespace {

uint32_t PngCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void AppendPngChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, uint32_t len) {
    auto push_be32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
    };
    push_be32(len);
    const size_t crc_start = out.size();
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(type), reinterpret_cast<const uint8_t*>(type) + 4);
    if (data && len > 0) {
        out.insert(out.end(), data, data + len);
    }
    push_be32(PngCrc32(out.data() + crc_start, 4u + len));
}

std::vector<uint8_t> BuildValidGamerPicturePng() {
    static constexpr uint8_t kSig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    std::vector<uint8_t> png(kSig, kSig + sizeof(kSig));
    // 1x1 8-bit RGBA IHDR
    const uint8_t ihdr[13] = {0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0};
    AppendPngChunk(png, "IHDR", ihdr, sizeof(ihdr));
    // Uncompressed zlib stream containing 1 scanline: filter=0, RGBA=(0x10, 0x7C, 0x10, 0xFF)
    const uint8_t raw_row[5] = {0x00u, 0x10u, 0x7Cu, 0x10u, 0xFFu};
    uint32_t s1 = 1u, s2 = 0u;
    for (uint8_t b : raw_row) {
        s1 = (s1 + b) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    const uint32_t adler = (s2 << 16) | s1;
    const uint8_t idat[16] = {
        0x78u, 0x01u,                                // zlib header
        0x01u, 0x05u, 0x00u, 0xFAu, 0xFFu,           // final stored deflate block (len=5)
        raw_row[0], raw_row[1], raw_row[2], raw_row[3], raw_row[4],
        static_cast<uint8_t>((adler >> 24) & 0xFFu),
        static_cast<uint8_t>((adler >> 16) & 0xFFu),
        static_cast<uint8_t>((adler >> 8) & 0xFFu),
        static_cast<uint8_t>(adler & 0xFFu),
    };
    AppendPngChunk(png, "IDAT", idat, sizeof(idat));
    AppendPngChunk(png, "IEND", nullptr, 0);
    return png;
}

}  // namespace

extern "C" {

XGameResult XUserGetGamerPictureAsync(
    XUserHandle user,
    XUserGamerPictureSize pictureSize,
    XAsyncBlock* async) {
    if (!user || !async || static_cast<uint32_t>(pictureSize) > XUserGamerPictureSize_ExtraLarge) {
        return XGR_E_INVALIDARG;
    }
    return BeginAsyncOperation(async, [](XAsyncState* st) {
        std::vector<uint8_t> png = BuildValidGamerPicturePng();
        const size_t sz = png.size();
        st->result_payload = std::move(png);
        CompleteAsyncState(st, XGR_S_OK, sz);
    });
}

XGameResult XUserGetGamerPictureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) {
    if (!async || !bufferSize) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *bufferSize = 0;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        *bufferSize = 0;
        return XGR_E_INVALIDARG;
    }
    *bufferSize = state->result_size;
    return XGR_S_OK;
}

XGameResult XUserGetGamerPictureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) {
    if (!async || !buffer) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        return XGR_E_INVALIDARG;
    }
    if (bufferSize < state->result_size) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(buffer, state->result_payload.data(), state->result_size);
    if (bufferUsed) {
        *bufferUsed = state->result_size;
    }
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

}  // extern "C"

namespace {

std::string Base64EncodeBytes(const uint8_t* data, size_t len, bool url_safe = false) {
    static constexpr char kStdTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static constexpr char kUrlTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* tbl = url_safe ? kUrlTable : kStdTable;
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0u;
        const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0u;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(tbl[(triple >> 18) & 0x3Fu]);
        out.push_back(tbl[(triple >> 12) & 0x3Fu]);
        if (i + 1 < len) {
            out.push_back(tbl[(triple >> 6) & 0x3Fu]);
        } else if (!url_safe) {
            out.push_back('=');
        }
        if (i + 2 < len) {
            out.push_back(tbl[triple & 0x3Fu]);
        } else if (!url_safe) {
            out.push_back('=');
        }
    }
    return out;
}

void ComputeHmacSha256(const uint8_t* key, size_t key_len,
                       const uint8_t* msg, size_t msg_len,
                       uint8_t out_mac[32]) {
    std::memset(out_mac, 0, 32);
#if defined(_WIN32)
    static std::atomic<HMODULE> bcrypt_dll_cached{nullptr};
    static std::atomic<bool> bcrypt_probed{false};
    if (!bcrypt_probed.load(std::memory_order_acquire)) {
        HMODULE loaded = LoadLibraryW(L"bcrypt.dll");
        bcrypt_dll_cached.store(loaded, std::memory_order_relaxed);
        bcrypt_probed.store(true, std::memory_order_release);
    }
    HMODULE bcrypt_dll = bcrypt_dll_cached.load(std::memory_order_relaxed);
    if (bcrypt_dll) {
        using FnOpenAlg = long(WINAPI*)(void**, const wchar_t*, const wchar_t*, unsigned long);
        using FnHash = long(WINAPI*)(void*, uint8_t*, unsigned long, uint8_t*, unsigned long, uint8_t*, unsigned long);
        using FnCloseAlg = long(WINAPI*)(void*, unsigned long);
        auto open_alg = reinterpret_cast<FnOpenAlg>(reinterpret_cast<void*>(
            GetProcAddress(bcrypt_dll, "BCryptOpenAlgorithmProvider")));
        auto do_hash = reinterpret_cast<FnHash>(reinterpret_cast<void*>(
            GetProcAddress(bcrypt_dll, "BCryptHash")));
        auto close_alg = reinterpret_cast<FnCloseAlg>(reinterpret_cast<void*>(
            GetProcAddress(bcrypt_dll, "BCryptCloseAlgorithmProvider")));
        if (open_alg && do_hash && close_alg) {
            void* alg = nullptr;
            if (open_alg(&alg, L"SHA256", nullptr, 0x00000008ul) == 0 && alg) {
                const long st = do_hash(alg,
                                        const_cast<uint8_t*>(key), static_cast<unsigned long>(key_len),
                                        const_cast<uint8_t*>(msg), static_cast<unsigned long>(msg_len),
                                        out_mac, 32ul);
                close_alg(alg, 0ul);
                if (st == 0) return;
            }
        }
    }
#endif
    uint64_t h[4] = {0x6a09e667bb67ae85ull, 0x3c6ef372a54ff53aull,
                     0x510e527f9b05688cull, 0x1f83d9ab5be0cd19ull};
    for (size_t i = 0; i < key_len; ++i) {
        h[i & 3] = (h[i & 3] ^ key[i]) * 0x100000001b3ull;
    }
    for (size_t i = 0; i < msg_len; ++i) {
        h[i & 3] = (h[i & 3] ^ msg[i]) * 0x100000001b3ull + (h[(i + 1) & 3] >> 17);
    }
    std::memcpy(out_mac, h, 32);
}

std::string WideToUtf8Str(const wchar_t* ws) {
    if (!ws || !*ws) return {};
#if defined(_WIN32)
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    return s;
#else
    std::string s;
    for (const wchar_t* p = ws; *p; ++p) s.push_back(static_cast<char>(*p & 0x7F));
    return s;
#endif
}

std::wstring Utf8ToWideStr(const std::string& s) {
    if (s.empty()) return {};
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

void BuildTokenAndSignatureStrings(
    uint64_t xuid,
    const std::string& method,
    const std::string& url,
    const std::string& canonical_headers,
    const void* body_buf,
    size_t body_size,
    std::string& out_token,
    std::string& out_signature) {
    char xuid_dec[32]{};
    std::snprintf(xuid_dec, sizeof(xuid_dec), "%llu", static_cast<unsigned long long>(xuid));
    char xuid_key[64]{};
    std::snprintf(xuid_key, sizeof(xuid_key), "ScarlettEdge-XSTS-SessionKey-%016llX",
                  static_cast<unsigned long long>(xuid));

    if (const char* live_tok = std::getenv("SCARLETTX_XBL_TOKEN"); live_tok && *live_tok) {
        out_token = live_tok;
    } else {
        const std::string jwt_hdr = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
        const std::string jwt_pay = std::string("{\"xuid\":\"") + xuid_dec +
                                    "\",\"sbx\":\"RETAIL\",\"iss\":\"xsts.auth.xboxlive.com\"}";
        const std::string b64_hdr = Base64EncodeBytes(
            reinterpret_cast<const uint8_t*>(jwt_hdr.data()), jwt_hdr.size(), true);
        const std::string b64_pay = Base64EncodeBytes(
            reinterpret_cast<const uint8_t*>(jwt_pay.data()), jwt_pay.size(), true);
        const std::string signing_in = b64_hdr + "." + b64_pay;
        uint8_t jwt_mac[32]{};
        ComputeHmacSha256(reinterpret_cast<const uint8_t*>(xuid_key), std::strlen(xuid_key),
                          reinterpret_cast<const uint8_t*>(signing_in.data()), signing_in.size(), jwt_mac);
        const std::string b64_mac = Base64EncodeBytes(jwt_mac, sizeof(jwt_mac), true);
        out_token = std::string("XBL3.0 x=") + xuid_dec + ";" + signing_in + "." + b64_mac;
    }

    std::vector<uint8_t> canonical;
    auto push_str = [&](const std::string& s) {
        canonical.insert(canonical.end(), s.begin(), s.end());
        canonical.push_back('\n');
    };
    push_str(method.empty() ? "GET" : method);
    push_str(url);
    push_str(canonical_headers);
    if (body_buf && body_size > 0) {
        const auto* bp = static_cast<const uint8_t*>(body_buf);
        canonical.insert(canonical.end(), bp, bp + body_size);
    }

    uint8_t sig_raw[44]{};
    sig_raw[3] = 1u;
    sig_raw[9] = 0x01u;
    sig_raw[10] = 0x9Au;
    ComputeHmacSha256(reinterpret_cast<const uint8_t*>(xuid_key), std::strlen(xuid_key),
                      canonical.data(), canonical.size(), sig_raw + 12);
    out_signature = Base64EncodeBytes(sig_raw, sizeof(sig_raw), false);
}

}  // namespace

extern "C" {

XGameResult XUserGetTokenAndSignatureAsync(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions /*options*/,
    const char* method,
    const char* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureHttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async) {
    if (!async || !user) {
        return XGR_E_INVALIDARG;
    }

    std::string hdr_joined;
    if (headers && headerCount > 0) {
        for (size_t i = 0; i < headerCount; ++i) {
            if (headers[i].name) hdr_joined += headers[i].name;
            hdr_joined += ':';
            if (headers[i].value) hdr_joined += headers[i].value;
            hdr_joined += ';';
        }
    }

    std::string token;
    std::string signature;
    BuildTokenAndSignatureStrings(
        user->xuid,
        method ? method : "GET",
        url ? url : "",
        hdr_joined,
        bodyBuffer,
        bodySize,
        token,
        signature);

    const size_t token_bytes = token.size() + 1;
    const size_t sig_bytes = signature.size() + 1;
    const size_t required_size = sizeof(XUserGetTokenAndSignatureData) + token_bytes + sig_bytes;

    return BeginAsyncOperation(async, [token = std::move(token), signature = std::move(signature), required_size](XAsyncState* st) {
        std::vector<uint8_t> payload;
        auto append = [&](const void* data, size_t len) {
            const auto* p = static_cast<const uint8_t*>(data);
            payload.insert(payload.end(), p, p + len);
        };
        uint32_t tlen = static_cast<uint32_t>(token.size() + 1);
        uint32_t slen = static_cast<uint32_t>(signature.size() + 1);
        append(&tlen, sizeof(tlen));
        append(token.c_str(), tlen);
        append(&slen, sizeof(slen));
        append(signature.c_str(), slen);

        st->result_payload = std::move(payload);
        CompleteAsyncState(st, XGR_S_OK, required_size);
    });
}

XGameResult XUserGetTokenAndSignatureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) {
    if (!async || !bufferSize) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        return XGR_E_INVALIDARG;
    }
    *bufferSize = state->result_size;
    return XGR_S_OK;
}

XGameResult XUserGetTokenAndSignatureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureData** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        return XGR_E_INVALIDARG;
    }
    if (bufferSize < state->result_size) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }

    const auto& payload = state->result_payload;
    if (payload.size() < sizeof(uint32_t) * 2) {
        return XGR_E_FAIL;
    }

    size_t off = 0;
    uint32_t tlen = 0;
    std::memcpy(&tlen, payload.data() + off, sizeof(tlen));
    off += sizeof(tlen);
    const char* token_src = reinterpret_cast<const char*>(payload.data() + off);
    off += tlen;

    uint32_t slen = 0;
    std::memcpy(&slen, payload.data() + off, sizeof(slen));
    off += sizeof(slen);
    const char* sig_src = reinterpret_cast<const char*>(payload.data() + off);

    auto* data = reinterpret_cast<XUserGetTokenAndSignatureData*>(buffer);
    char* str_ptr = reinterpret_cast<char*>(data + 1);

    data->tokenSize = tlen;
    data->signatureSize = slen;

    std::memcpy(str_ptr, token_src, tlen);
    data->token = str_ptr;
    str_ptr += tlen;

    std::memcpy(str_ptr, sig_src, slen);
    data->signature = str_ptr;
    str_ptr += slen;

    *ptrToBuffer = data;
    if (bufferUsed) {
        *bufferUsed = state->result_size;
    }

    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XUserGetTokenAndSignatureUtf16Async(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions /*options*/,
    const wchar_t* method,
    const wchar_t* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureUtf16HttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async) {
    if (!async || !user) {
        return XGR_E_INVALIDARG;
    }

    std::string hdr_joined;
    if (headers && headerCount > 0) {
        for (size_t i = 0; i < headerCount; ++i) {
            hdr_joined += WideToUtf8Str(headers[i].name);
            hdr_joined += ':';
            hdr_joined += WideToUtf8Str(headers[i].value);
            hdr_joined += ';';
        }
    }

    std::string tok_utf8, sig_utf8;
    BuildTokenAndSignatureStrings(
        user->xuid,
        method ? WideToUtf8Str(method) : "GET",
        WideToUtf8Str(url),
        hdr_joined,
        bodyBuffer,
        bodySize,
        tok_utf8,
        sig_utf8);

    std::wstring wtok = Utf8ToWideStr(tok_utf8);
    std::wstring wsig = Utf8ToWideStr(sig_utf8);
    const size_t tok_chars = wtok.size() + 1;
    const size_t sig_chars = wsig.size() + 1;
    const size_t required_size =
        sizeof(XUserGetTokenAndSignatureUtf16Data) + (tok_chars + sig_chars) * sizeof(wchar_t);

    return BeginAsyncOperation(async, [wtok = std::move(wtok), wsig = std::move(wsig), required_size](XAsyncState* st) {
        std::vector<uint8_t> payload;
        auto append = [&](const void* d, size_t n) {
            const auto* p = static_cast<const uint8_t*>(d);
            payload.insert(payload.end(), p, p + n);
        };
        const uint32_t tc = static_cast<uint32_t>(wtok.size() + 1);
        const uint32_t sc = static_cast<uint32_t>(wsig.size() + 1);
        append(&tc, sizeof(tc));
        append(wtok.c_str(), tc * sizeof(wchar_t));
        append(&sc, sizeof(sc));
        append(wsig.c_str(), sc * sizeof(wchar_t));
        st->result_payload = std::move(payload);
        CompleteAsyncState(st, XGR_S_OK, required_size);
    });
}

XGameResult XUserGetTokenAndSignatureUtf16ResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) {
    return XUserGetTokenAndSignatureResultSize(async, bufferSize);
}

XGameResult XUserGetTokenAndSignatureUtf16Result(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureUtf16Data** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) return hr;
    auto state = GetAsyncState(async);
    if (!state) return XGR_E_INVALIDARG;
    if (bufferSize < state->result_size) return XGR_E_NOT_SUFFICIENT_BUFFER;

    const auto& payload = state->result_payload;
    if (payload.size() < sizeof(uint32_t) * 2) return XGR_E_FAIL;

    size_t off = 0;
    uint32_t tc = 0;
    std::memcpy(&tc, payload.data() + off, sizeof(tc));
    off += sizeof(tc);
    const auto* wtok_src = reinterpret_cast<const wchar_t*>(payload.data() + off);
    off += static_cast<size_t>(tc) * sizeof(wchar_t);

    uint32_t sc = 0;
    std::memcpy(&sc, payload.data() + off, sizeof(sc));
    off += sizeof(sc);
    const auto* wsig_src = reinterpret_cast<const wchar_t*>(payload.data() + off);

    auto* data = reinterpret_cast<XUserGetTokenAndSignatureUtf16Data*>(buffer);
    auto* wstr_ptr = reinterpret_cast<wchar_t*>(data + 1);

    data->tokenCount = tc;
    data->signatureCount = sc;
    std::memcpy(wstr_ptr, wtok_src, static_cast<size_t>(tc) * sizeof(wchar_t));
    data->token = wstr_ptr;
    wstr_ptr += tc;
    std::memcpy(wstr_ptr, wsig_src, static_cast<size_t>(sc) * sizeof(wchar_t));
    data->signature = wstr_ptr;

    *ptrToBuffer = data;
    if (bufferUsed) *bufferUsed = state->result_size;
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

// ===========================================================================
// XGameSave.h
// ===========================================================================
}  // extern "C"

namespace {

XGameResult SubmitUpdateCore(const std::shared_ptr<XGameSaveUpdateCore>& ucore) {
    if (!ucore || !ucore->container || !ucore->container->provider) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> ulock(ucore->mutex);
    auto& ccore = ucore->container;
    auto& pcore = ccore->provider;
    std::lock_guard<std::mutex> plock(pcore->mutex);

    std::error_code ec;
    fs::create_directories(ccore->dir_path, ec);
    if (ec) {
        return XGR_E_FAIL;
    }

    // Enforce provider quota atomically before mutating blobs on disk.
    const uint64_t current_used = ComputeDirectorySize(pcore->root_dir);
    int64_t delta_bytes = 0;
    std::unordered_set<std::string> accounted_existing;
    for (const auto& del_name : ucore->pending_deletes) {
        const std::string sanitized = SanitizeSaveName(del_name);
        if (accounted_existing.insert(sanitized).second) {
            const fs::path p = ccore->dir_path / sanitized;
            if (fs::is_regular_file(p, ec)) {
                delta_bytes -= static_cast<int64_t>(fs::file_size(p, ec));
            }
        }
    }
    for (const auto& [blob_name, bytes] : ucore->pending_writes) {
        const std::string sanitized = SanitizeSaveName(blob_name);
        if (accounted_existing.insert(sanitized).second) {
            const fs::path p = ccore->dir_path / sanitized;
            if (fs::is_regular_file(p, ec)) {
                delta_bytes -= static_cast<int64_t>(fs::file_size(p, ec));
            }
        }
        delta_bytes += static_cast<int64_t>(bytes.size());
    }
    const uint64_t projected_used =
        (delta_bytes < 0 && static_cast<uint64_t>(-delta_bytes) > current_used)
            ? 0ull
            : static_cast<uint64_t>(static_cast<int64_t>(current_used) + delta_bytes);
    if (projected_used > pcore->max_quota_bytes) {
        return XGR_E_GS_QUOTA_EXCEEDED;
    }

    for (const auto& del_name : ucore->pending_deletes) {
        fs::remove(ccore->dir_path / SanitizeSaveName(del_name), ec);
    }
    for (const auto& [blob_name, bytes] : ucore->pending_writes) {
        fs::path file_path = ccore->dir_path / SanitizeSaveName(blob_name);
        std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return XGR_E_FAIL;
        }
        if (!bytes.empty()) {
            ofs.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        if (!ofs.good()) {
            return XGR_E_FAIL;
        }
    }
    // Persist unsanitized container display name in _meta.txt
    if (!ucore->display_name.empty()) {
        std::ofstream meta_ofs(ccore->dir_path / kSaveMetaFileName, std::ios::binary | std::ios::trunc);
        if (meta_ofs) {
            meta_ofs.write(ucore->display_name.data(),
                           static_cast<std::streamsize>(ucore->display_name.size()));
        }
    }
    return XGR_S_OK;
}

}  // namespace

extern "C" {

XGameResult XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool /*syncOnDemand*/,
    XGameSaveProviderHandle* provider) {
    if (!requestingUser || !IsValidSaveName(configurationId) || !provider) {
        return XGR_E_INVALIDARG;
    }
    auto pcore = std::make_shared<XGameSaveProviderCore>();
    pcore->config_id = configurationId;
    pcore->root_dir = ResolveSaveRoot(pcore->config_id);
    pcore->max_quota_bytes = ConfiguredSaveQuotaBytes();
    std::error_code ec;
    fs::create_directories(pcore->root_dir, ec);
    if (ec) {
        return XGR_E_FAIL;
    }
    auto* p = new (std::nothrow) XGameSaveProvider();
    if (!p) {
        return XGR_E_OUTOFMEMORY;
    }
    p->core = std::move(pcore);
    RegisterValidHandle(p);
    *provider = p;
    return XGR_S_OK;
}

XGameResult XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async) {
    if (!requestingUser || !configurationId || !async) {
        return XGR_E_INVALIDARG;
    }
    if (!IsValidSaveName(configurationId)) {
        return XGR_E_INVALIDARG;
    }
    XUserHandle dup_user = nullptr;
    if (XUserDuplicateHandle(requestingUser, &dup_user) != XGR_S_OK) {
        return XGR_E_INVALIDARG;
    }
    std::string cfg = configurationId;
    XGameResult hr = BeginAsyncOperation(async, [dup_user, cfg, syncOnDemand](XAsyncState* st) {
        XGameSaveProviderHandle prov = nullptr;
        XGameResult init_hr = XGameSaveInitializeProvider(
            dup_user, cfg.c_str(), syncOnDemand, &prov);
        XUserCloseHandle(dup_user);
        if (init_hr == XGR_S_OK && prov) {
            std::lock_guard<std::mutex> lk(st->mutex);
            st->result_handle = prov;
            st->cleanup_result = [](void* h) {
                XGameSaveCloseProvider(static_cast<XGameSaveProviderHandle>(h));
            };
        }
        CompleteAsyncState(st, init_hr, sizeof(XGameSaveProviderHandle));
    });
    if (hr < 0) {
        XUserCloseHandle(dup_user);
    }
    return hr;
}

XGameResult XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider) {
    if (!async || !provider) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *provider = nullptr;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        *provider = nullptr;
        return XGR_E_FAIL;
    }
    XGameSaveProviderHandle out_prov = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        out_prov = static_cast<XGameSaveProviderHandle>(state->result_handle);
        state->result_handle = nullptr;
        state->cleanup_result = nullptr;
    }
    if (!out_prov) {
        *provider = nullptr;
        return XGR_E_FAIL;
    }
    *provider = out_prov;
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

void XGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    if (!provider) {
        return;
    }
    if (provider->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        UnregisterValidHandle(provider);
        delete provider;
    }
}

XGameResult XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota) {
    if (!provider || !provider->core || !remainingQuota) {
        return XGR_E_INVALIDARG;
    }
    auto pcore = provider->core;
    std::lock_guard<std::mutex> lock(pcore->mutex);
    const uint64_t used = ComputeDirectorySize(pcore->root_dir);
    *remainingQuota = (used >= pcore->max_quota_bytes)
                          ? 0
                          : static_cast<int64_t>(pcore->max_quota_bytes - used);
    return XGR_S_OK;
}

XGameResult XGameSaveGetRemainingQuotaAsync(
    XGameSaveProviderHandle provider,
    XAsyncBlock* async) {
    if (!provider || !provider->core || !async) {
        return XGR_E_INVALIDARG;
    }
    auto pcore = provider->core;
    return BeginAsyncOperation(async, [pcore](XAsyncState* st) {
        int64_t remaining = 0;
        {
            std::lock_guard<std::mutex> lock(pcore->mutex);
            const uint64_t used = ComputeDirectorySize(pcore->root_dir);
            remaining = (used >= pcore->max_quota_bytes)
                            ? 0
                            : static_cast<int64_t>(pcore->max_quota_bytes - used);
        }
        st->result_payload.resize(sizeof(int64_t));
        std::memcpy(st->result_payload.data(), &remaining, sizeof(int64_t));
        CompleteAsyncState(st, XGR_S_OK, sizeof(int64_t));
    });
}

XGameResult XGameSaveGetRemainingQuotaResult(
    XAsyncBlock* async,
    int64_t* remainingQuota) {
    if (!async || !remainingQuota) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *remainingQuota = 0;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state || state->result_payload.size() < sizeof(int64_t)) {
        *remainingQuota = 0;
        return XGR_E_FAIL;
    }
    std::memcpy(remainingQuota, state->result_payload.data(), sizeof(int64_t));
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* containerContext) {
    if (!provider || !provider->core || !IsValidSaveName(name) || !containerContext) {
        return XGR_E_INVALIDARG;
    }
    auto ccore = std::make_shared<XGameSaveContainerCore>();
    ccore->provider = provider->core;
    ccore->name = name;
    ccore->dir_path = provider->core->root_dir / SanitizeSaveName(name);
    {
        std::lock_guard<std::mutex> lock(provider->core->mutex);
        std::error_code ec;
        fs::create_directories(ccore->dir_path, ec);
    }
    auto* c = new (std::nothrow) XGameSaveContainer();
    if (!c) {
        return XGR_E_OUTOFMEMORY;
    }
    provider->ref_count.fetch_add(1, std::memory_order_relaxed);
    c->provider_handle = provider;
    c->core = std::move(ccore);
    RegisterValidHandle(c);
    *containerContext = c;
    return XGR_S_OK;
}

void XGameSaveCloseContainer(XGameSaveContainerHandle context) {
    if (!context) {
        return;
    }
    XGameSaveProviderHandle prov = context->provider_handle;
    UnregisterValidHandle(context);
    delete context;
    if (prov) {
        XGameSaveCloseProvider(prov);
    }
}

XGameResult XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* name) {
    if (!provider || !provider->core || !IsValidSaveName(name)) {
        return XGR_E_INVALIDARG;
    }
    auto pcore = provider->core;
    std::lock_guard<std::mutex> lock(pcore->mutex);
    std::error_code ec;
    fs::remove_all(pcore->root_dir / SanitizeSaveName(name), ec);
    return ec ? XGR_E_FAIL : XGR_S_OK;
}

XGameResult XGameSaveDeleteContainerAsync(
    XGameSaveProviderHandle provider,
    const char* name,
    XAsyncBlock* async) {
    if (!provider || !provider->core || !IsValidSaveName(name) || !async) {
        return XGR_E_INVALIDARG;
    }
    auto pcore = provider->core;
    std::string cname = name;
    return BeginAsyncOperation(async, [pcore, cname](XAsyncState* st) {
        std::error_code ec;
        {
            std::lock_guard<std::mutex> lock(pcore->mutex);
            fs::remove_all(pcore->root_dir / SanitizeSaveName(cname), ec);
        }
        CompleteAsyncState(st, ec ? XGR_E_FAIL : XGR_S_OK, 0);
    });
}

XGameResult XGameSaveDeleteContainerResult(XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    xgr::ReleaseHleAsyncResult(async);
    return hr;
}

XGameResult XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback* callback) {
    if (!provider || !provider->core || !callback) {
        return XGR_E_INVALIDARG;
    }
    struct Entry {
        std::string name;
        std::string display_name;
        uint32_t blob_count;
        uint64_t total_size;
    };
    std::vector<Entry> entries;
    auto pcore = provider->core;
    std::unique_lock<std::mutex> lock(pcore->mutex);
    std::error_code ec;
    if (!fs::exists(pcore->root_dir, ec)) {
        return XGR_S_OK;
    }
    for (const auto& entry : fs::directory_iterator(pcore->root_dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string cname = entry.path().filename().string();
        uint32_t blob_count = 0;
        uint64_t total_size = 0;
        for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
            if (f.is_regular_file(ec) && f.path().filename() != kSaveMetaFileName) {
                ++blob_count;
                total_size += f.file_size(ec);
            }
        }
        entries.push_back({cname, ReadContainerDisplayName(entry.path(), cname), blob_count, total_size});
    }
    lock.unlock();
    for (const auto& entry : entries) {
        XGameSaveContainerInfo info{};
        info.name = entry.name.c_str();
        info.displayName = entry.display_name.c_str();
        info.blobCount = entry.blob_count;
        info.totalSize = entry.total_size;
        info.lastModifiedTime = 0;
        info.needsSync = false;
        if (!callback(&info, context)) {
            break;
        }
    }
    return XGR_S_OK;
}

XGameResult XGameSaveEnumerateContainerInfoByName(
    XGameSaveProviderHandle provider,
    const char* containerNamePrefix,
    void* context,
    XGameSaveContainerInfoCallback* callback) {
    if (!provider || !provider->core || !callback) {
        return XGR_E_INVALIDARG;
    }
    const std::string prefix =
        (containerNamePrefix && *containerNamePrefix) ? SanitizeSaveName(containerNamePrefix) : "";
    struct Entry {
        std::string name;
        std::string display_name;
        uint32_t blob_count;
        uint64_t total_size;
    };
    std::vector<Entry> entries;
    auto pcore = provider->core;
    std::unique_lock<std::mutex> lock(pcore->mutex);
    std::error_code ec;
    if (!fs::exists(pcore->root_dir, ec)) {
        return XGR_S_OK;
    }
    for (const auto& entry : fs::directory_iterator(pcore->root_dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string cname = entry.path().filename().string();
        if (!prefix.empty() && cname.rfind(prefix, 0) != 0) {
            continue;
        }
        uint32_t blob_count = 0;
        uint64_t total_size = 0;
        for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
            if (f.is_regular_file(ec) && f.path().filename() != kSaveMetaFileName) {
                ++blob_count;
                total_size += f.file_size(ec);
            }
        }
        entries.push_back({cname, ReadContainerDisplayName(entry.path(), cname), blob_count, total_size});
    }
    lock.unlock();
    for (const auto& entry : entries) {
        XGameSaveContainerInfo info{};
        info.name = entry.name.c_str();
        info.displayName = entry.display_name.c_str();
        info.blobCount = entry.blob_count;
        info.totalSize = entry.total_size;
        info.lastModifiedTime = 0;
        info.needsSync = false;
        if (!callback(&info, context)) {
            break;
        }
    }
    return XGR_S_OK;
}

XGameResult XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* displayName,
    XGameSaveUpdateHandle* updateContext) {
    if (!container || !container->core || !updateContext) {
        return XGR_E_INVALIDARG;
    }
    auto ucore = std::make_shared<XGameSaveUpdateCore>();
    ucore->container = container->core;
    ucore->display_name = displayName ? displayName : container->core->name;
    auto* u = new (std::nothrow) XGameSaveUpdate();
    if (!u) {
        return XGR_E_OUTOFMEMORY;
    }
    u->core = std::move(ucore);
    RegisterValidHandle(u);
    *updateContext = u;
    return XGR_S_OK;
}

void XGameSaveCloseUpdate(XGameSaveUpdateHandle context) {
    if (!context) {
        return;
    }
    UnregisterValidHandle(context);
    delete context;
}

XGameResult XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount) {
    if (!updateContext || !updateContext->core || !IsValidSaveName(blobName) ||
        (!data && byteCount > 0)) {
        return XGR_E_INVALIDARG;
    }
    std::vector<uint8_t> copy;
    if (byteCount > 0) {
        copy.assign(data, data + byteCount);
    }
    std::lock_guard<std::mutex> lock(updateContext->core->mutex);
    updateContext->core->pending_writes[blobName] = std::move(copy);
    return XGR_S_OK;
}

XGameResult XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName) {
    if (!updateContext || !updateContext->core || !IsValidSaveName(blobName)) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(updateContext->core->mutex);
    updateContext->core->pending_writes.erase(blobName);
    updateContext->core->pending_deletes.emplace_back(blobName);
    return XGR_S_OK;
}

XGameResult XGameSaveSubmitUpdate(XGameSaveUpdateHandle updateContext) {
    if (!updateContext || !updateContext->core) {
        return XGR_E_INVALIDARG;
    }
    return SubmitUpdateCore(updateContext->core);
}

XGameResult XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async) {
    if (!updateContext || !updateContext->core || !async) {
        return XGR_E_INVALIDARG;
    }
    auto ucore = updateContext->core;
    return BeginAsyncOperation(async, [ucore](XAsyncState* st) {
        XGameResult hr = SubmitUpdateCore(ucore);
        CompleteAsyncState(st, hr, 0);
    });
}

XGameResult XGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    xgr::ReleaseHleAsyncResult(async);
    return hr;
}

XGameResult XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData) {
    if (!container || !container->core || !blobNames || !countOfBlobs || !blobData) {
        return XGR_E_INVALIDARG;
    }
    auto ccore = container->core;
    auto pcore = ccore->provider;
    std::lock_guard<std::mutex> lock(pcore->mutex);

    const uint32_t requested = *countOfBlobs;
    const size_t headers_bytes = sizeof(XGameSaveBlob) * requested;
    if (blobsSize < headers_bytes) {
        return XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(blobData);
    size_t cursor = headers_bytes;
    uint32_t read_count = 0;

    for (uint32_t i = 0; i < requested; ++i) {
        if (!IsValidSaveName(blobNames[i])) {
            return XGR_E_INVALIDARG;
        }
        const std::string name = blobNames[i];
        fs::path file_path = ccore->dir_path / SanitizeSaveName(name);
        std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            return XGR_E_GS_BLOB_NOT_FOUND;
        }
        const std::streamsize fsize = ifs.tellg();
        ifs.seekg(0, std::ios::beg);

        const size_t name_len = name.size() + 1;
        const size_t data_len = static_cast<size_t>(fsize > 0 ? fsize : 0);
        if (cursor + name_len + data_len > blobsSize) {
            return XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL;
        }

        char* name_dst = reinterpret_cast<char*>(base + cursor);
        std::memcpy(name_dst, name.c_str(), name_len);
        cursor += name_len;

        uint8_t* data_dst = base + cursor;
        if (data_len > 0) {
            ifs.read(reinterpret_cast<char*>(data_dst), fsize);
        }
        cursor += data_len;

        blobData[i].info.name = name_dst;
        blobData[i].info.size = static_cast<uint32_t>(data_len);
        blobData[i].data = data_dst;
        ++read_count;
    }

    *countOfBlobs = read_count;
    return XGR_S_OK;
}

XGameResult XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async) {
    if (!container || !container->core || !blobNames || !async) {
        return XGR_E_INVALIDARG;
    }
    std::vector<std::string> names;
    names.reserve(countOfBlobs);
    for (uint32_t i = 0; i < countOfBlobs; ++i) {
        if (!IsValidSaveName(blobNames[i])) {
            return XGR_E_INVALIDARG;
        }
        names.emplace_back(blobNames[i]);
    }

    auto ccore = container->core;
    return BeginAsyncOperation(async, [ccore, names = std::move(names)](XAsyncState* st) {
        auto pcore = ccore->provider;
        std::lock_guard<std::mutex> lock(pcore->mutex);

        size_t required_gdk_size = sizeof(XGameSaveBlob) * names.size();
        std::vector<std::pair<std::string, std::vector<uint8_t>>> blobs;
        blobs.reserve(names.size());

        for (const auto& name : names) {
            fs::path file_path = ccore->dir_path / SanitizeSaveName(name);
            std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
            if (!ifs) {
                CompleteAsyncState(st, XGR_E_GS_BLOB_NOT_FOUND, 0);
                return;
            }
            const std::streamsize fsize = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes(static_cast<size_t>(fsize > 0 ? fsize : 0));
            if (!bytes.empty()) {
                ifs.read(reinterpret_cast<char*>(bytes.data()), fsize);
            }
            required_gdk_size += (name.size() + 1) + bytes.size();
            blobs.emplace_back(name, std::move(bytes));
        }

        std::vector<uint8_t> packed;
        auto append = [&](const void* src, size_t n) {
            const auto* p = static_cast<const uint8_t*>(src);
            packed.insert(packed.end(), p, p + n);
        };
        uint32_t cnt = static_cast<uint32_t>(blobs.size());
        append(&cnt, sizeof(cnt));
        for (const auto& [name, bytes] : blobs) {
            uint32_t nlen = static_cast<uint32_t>(name.size() + 1);
            uint32_t dlen = static_cast<uint32_t>(bytes.size());
            append(&nlen, sizeof(nlen));
            append(&dlen, sizeof(dlen));
            append(name.c_str(), nlen);
            if (dlen > 0) {
                append(bytes.data(), dlen);
            }
        }
        st->result_payload = std::move(packed);
        CompleteAsyncState(st, XGR_S_OK, required_gdk_size);
    });
}

XGameResult XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs) {
    if (!async || !blobData || !countOfBlobs) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        return XGR_E_INVALIDARG;
    }
    if (blobsSize < state->result_size) {
        return XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL;
    }

    const auto& packed = state->result_payload;
    if (packed.size() < sizeof(uint32_t)) {
        return XGR_E_FAIL;
    }
    size_t p_off = 0;
    uint32_t cnt = 0;
    std::memcpy(&cnt, packed.data() + p_off, sizeof(cnt));
    p_off += sizeof(cnt);

    uint8_t* base = reinterpret_cast<uint8_t*>(blobData);
    size_t cursor = sizeof(XGameSaveBlob) * cnt;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t nlen = 0;
        uint32_t dlen = 0;
        std::memcpy(&nlen, packed.data() + p_off, sizeof(nlen));
        p_off += sizeof(nlen);
        std::memcpy(&dlen, packed.data() + p_off, sizeof(dlen));
        p_off += sizeof(dlen);

        char* name_dst = reinterpret_cast<char*>(base + cursor);
        std::memcpy(name_dst, packed.data() + p_off, nlen);
        p_off += nlen;
        cursor += nlen;

        uint8_t* data_dst = base + cursor;
        if (dlen > 0) {
            std::memcpy(data_dst, packed.data() + p_off, dlen);
            p_off += dlen;
            cursor += dlen;
        }

        blobData[i].info.name = name_dst;
        blobData[i].info.size = dlen;
        blobData[i].data = data_dst;
    }

    *countOfBlobs = cnt;
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback* callback) {
    if (!container || !container->core || !callback) {
        return XGR_E_INVALIDARG;
    }
    auto ccore = container->core;
    auto pcore = ccore->provider;
    std::vector<std::pair<std::string, uint32_t>> entries;
    std::unique_lock<std::mutex> lock(pcore->mutex);
    std::error_code ec;
    if (!fs::exists(ccore->dir_path, ec)) {
        return XGR_S_OK;
    }
    for (const auto& entry : fs::directory_iterator(ccore->dir_path, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().filename() == kSaveMetaFileName) {
            continue;
        }
        entries.emplace_back(entry.path().filename().string(),
                             static_cast<uint32_t>(entry.file_size(ec)));
    }
    lock.unlock();
    for (const auto& [bname, size] : entries) {
        XGameSaveBlobInfo info{};
        info.name = bname.c_str();
        info.size = size;
        if (!callback(&info, context)) {
            break;
        }
    }
    return XGR_S_OK;
}

// ===========================================================================
// XGameSaveFiles.h
// ===========================================================================
// The folder is the configuration's save directory itself: a title uses
// either containers or plain files, never both.
XGameResult XGameSaveFilesGetFolderWithUiAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    XAsyncBlock* async) {
    if (!requestingUser || !configurationId || !*configurationId || !async) {
        return XGR_E_INVALIDARG;
    }
    std::string cfg = configurationId;
    return BeginAsyncOperation(async, [cfg](XAsyncState* st) {
        std::string folder;
        try {
            const fs::path dir = fs::absolute(ResolveSaveRoot(cfg)).make_preferred();
            fs::create_directories(dir);
            folder = dir.string();
        } catch (const std::exception&) {
            CompleteAsyncState(st, XGR_E_FAIL, 0);
            return;
        }
        st->result_payload.assign(folder.c_str(), folder.c_str() + folder.size() + 1);
        CompleteAsyncState(st, XGR_S_OK, st->result_payload.size());
    });
}

XGameResult XGameSaveFilesGetFolderWithUiResult(
    XAsyncBlock* async,
    size_t folderSize,
    char* folderResult) {
    if (!async || !folderResult) {
        return XGR_E_INVALIDARG;
    }
    // As on the console, an operation that has not finished is E_PENDING.
    XGameResult hr = XAsyncGetStatus(async, false);
    if (hr != XGR_S_OK) {
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state || state->result_payload.empty()) {
        return XGR_E_FAIL;
    }
    if (folderSize < state->result_payload.size()) {
        return XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL;
    }
    std::memcpy(folderResult, state->result_payload.data(), state->result_payload.size());
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XGameSaveFilesGetRemainingQuota(
    XUserHandle userContext,
    const char* configurationId,
    int64_t* remainingQuota) {
    if (!remainingQuota) {
        return XGR_E_INVALIDARG;
    }
    *remainingQuota = 0;
    if (!userContext || !configurationId || !*configurationId) {
        return XGR_E_INVALIDARG;
    }
    const uint64_t max_q = ConfiguredSaveQuotaBytes();
    const uint64_t used = ComputeDirectorySize(ResolveSaveRoot(configurationId));
    *remainingQuota = used >= max_q ? 0 : static_cast<int64_t>(max_q - used);
    return XGR_S_OK;
}

XGameResult XGameSaveFilesGetRemainingQuotaAsync(
    XUserHandle userContext,
    const char* configurationId,
    XAsyncBlock* async) {
    if (!userContext || !configurationId || !*configurationId || !async) {
        return XGR_E_INVALIDARG;
    }
    XUserHandle dup_user = nullptr;
    if (XUserDuplicateHandle(userContext, &dup_user) != XGR_S_OK) {
        return XGR_E_INVALIDARG;
    }
    std::string cfg = configurationId;
    XGameResult hr = BeginAsyncOperation(async, [dup_user, cfg](XAsyncState* st) {
        int64_t remaining = 0;
        XGameResult q_hr = XGameSaveFilesGetRemainingQuota(dup_user, cfg.c_str(), &remaining);
        XUserCloseHandle(dup_user);
        st->result_payload.resize(sizeof(int64_t));
        std::memcpy(st->result_payload.data(), &remaining, sizeof(int64_t));
        CompleteAsyncState(st, q_hr, sizeof(int64_t));
    });
    if (hr < 0) {
        XUserCloseHandle(dup_user);
    }
    return hr;
}

XGameResult XGameSaveFilesGetRemainingQuotaResult(
    XAsyncBlock* async,
    int64_t* remainingQuota) {
    if (!async || !remainingQuota) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *remainingQuota = 0;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state || state->result_payload.size() < sizeof(int64_t)) {
        *remainingQuota = 0;
        return XGR_E_FAIL;
    }
    std::memcpy(remainingQuota, state->result_payload.data(), sizeof(int64_t));
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

// ===========================================================================
// XPackage.h & XStore.h (DLC Discovery, VFS Mounting & Retail Licensing)
// ===========================================================================
}  // extern "C"

struct XPackageMountObject {
    std::string package_id;
    std::string host_path;
};

struct XStoreContextObject {
    XUserHandle user = nullptr;
};

struct XStoreProductQueryObject {
    struct Entry {
        std::string store_id;
        std::string title;
        std::string description;
        uint32_t kind = 1;
    };
    std::vector<Entry> items;
};

namespace {

struct DiscoveredDlcEntry {
    std::string package_id;
    std::string store_id;
    std::string display_name;
    std::string host_path;
};

std::vector<DiscoveredDlcEntry> DiscoverInstalledDlcPackages() {
    std::vector<DiscoveredDlcEntry> out;
    std::vector<fs::path> search_roots;
    if (const char* dlc_env = std::getenv("SCARLETTX_DLC_ROOT"); dlc_env && *dlc_env) {
        search_roots.emplace_back(dlc_env);
    }
    if (const char* pkg_env = std::getenv("SCARLETTX_PACKAGE_ROOT"); pkg_env && *pkg_env) {
        search_roots.push_back(fs::path(pkg_env) / "dlc");
    }

    std::error_code ec;
    for (const auto& root : search_roots) {
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            const std::string stem = entry.path().stem().string();
            if (stem.empty()) continue;
            bool is_dlc = entry.is_directory(ec);
            if (!is_dlc && entry.is_regular_file(ec)) {
                std::string ext = entry.path().extension().string();
                for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                is_dlc = (ext == ".xvc" || ext == ".xvd");
            }
            if (!is_dlc) continue;

            DiscoveredDlcEntry dlc;
            dlc.package_id = "SCARLETTEDGE.DLC." + SanitizeSaveName(stem);
            dlc.store_id = ("9N" + SanitizeSaveName(stem) + "000000000000").substr(0, 12);
            dlc.display_name = stem;
            dlc.host_path = fs::absolute(entry.path(), ec).make_preferred().string();
            out.push_back(std::move(dlc));
        }
    }
    return out;
}

}  // namespace

extern "C" {

XGameResult XPackageGetUserLocale(size_t localeSize, char* locale) {
    if (!locale) {
        return XGR_E_POINTER;
    }
    std::string name;
    if (const char* forced = std::getenv("SCARLETTX_LOCALE"); forced && *forced) {
        name = forced;
    } else {
        name = MatchPackageLanguage(HostLocaleName(), PackageLanguages());
    }
    return CopyStringToBuffer(name, localeSize, locale, nullptr);
}

XGameResult XPackageEnumeratePackages(
    XPackageKind kind,
    XPackageEnumerationScope /*scope*/,
    void* context,
    XPackageEnumerationCallback* callback) {
    if (!callback) {
        return XGR_E_INVALIDARG;
    }
    if (kind != XPackageKind_Game && kind != XPackageKind_Content) {
        return XGR_E_INVALIDARG;
    }
    if (kind == XPackageKind_Game) {
        std::string game_name = "ScarlettX Game";
        if (const char* pkg_env = std::getenv("SCARLETTX_PACKAGE_ROOT"); pkg_env && *pkg_env) {
            const std::string stem = fs::path(pkg_env).filename().string();
            if (!stem.empty()) {
                game_name = stem;
            }
        }
        XPackageDetails game_det{};
        game_det.packageIdentifier = "SCARLETTEDGE.GAME.MAIN";
        game_det.version.major = 1;
        game_det.kind = XPackageKind_Game;
        game_det.displayName = game_name.c_str();
        game_det.description = "ScarlettEdge Base Game Package";
        game_det.publisher = "ScarlettEdge";
        game_det.storeId = "9N5CA0000100";
        game_det.installing = false;
        (void)callback(context, &game_det);
        return XGR_S_OK;
    }
    const auto dlcs = DiscoverInstalledDlcPackages();
    for (const auto& dlc : dlcs) {
        XPackageDetails det{};
        det.packageIdentifier = dlc.package_id.c_str();
        det.version.major = 1;
        det.kind = XPackageKind_Content;
        det.displayName = dlc.display_name.c_str();
        det.description = "ScarlettEdge Add-On Content Package";
        det.publisher = "ScarlettEdge";
        det.storeId = dlc.store_id.c_str();
        det.installing = false;
        if (!callback(context, &det)) {
            break;
        }
    }
    return XGR_S_OK;
}

XGameResult XPackageMountWithUiAsync(
    const char* packageIdentifier,
    XAsyncBlock* async) {
    if (!packageIdentifier || !async) {
        return XGR_E_INVALIDARG;
    }
    std::string pkg_id = packageIdentifier;
    std::string resolved_path;
    std::error_code ec;
    if (pkg_id.empty() || pkg_id == "default" || pkg_id == "SCARLETTEDGE.GAME.MAIN") {
        if (const char* root = std::getenv("SCARLETTX_PACKAGE_ROOT"); root && *root && fs::exists(root, ec)) {
            resolved_path = fs::absolute(fs::path(root), ec).make_preferred().string();
        } else {
            resolved_path = fs::current_path(ec).make_preferred().string();
        }
    } else {
        const auto dlcs = DiscoverInstalledDlcPackages();
        for (const auto& dlc : dlcs) {
            if (dlc.package_id == pkg_id || dlc.display_name == pkg_id || dlc.store_id == pkg_id) {
                resolved_path = dlc.host_path;
                break;
            }
        }
        if (resolved_path.empty() && fs::exists(pkg_id, ec)) {
            resolved_path = fs::absolute(pkg_id, ec).make_preferred().string();
        }
    }
    if (resolved_path.empty()) {
        return XGR_E_NOTFOUND;
    }
    while (resolved_path.size() > 3 && (resolved_path.back() == '\\' || resolved_path.back() == '/')) {
        resolved_path.pop_back();
    }
    return BeginAsyncOperation(async, [pkg_id = std::move(pkg_id), resolved_path = std::move(resolved_path)](XAsyncState* st) {
        auto* mh = new (std::nothrow) XPackageMountObject();
        if (!mh) {
            CompleteAsyncState(st, XGR_E_OUTOFMEMORY, 0);
            return;
        }
        mh->package_id = pkg_id;
        mh->host_path = resolved_path;
        RegisterValidHandle(mh);
        {
            std::lock_guard<std::mutex> lk(st->mutex);
            st->result_handle = mh;
            st->cleanup_result = [](void* h) {
                XPackageCloseMountHandle(static_cast<XPackageMountHandle>(h));
            };
        }
        CompleteAsyncState(st, XGR_S_OK, sizeof(XPackageMountHandle));
    });
}

XGameResult XPackageMountWithUiResult(
    XAsyncBlock* async,
    XPackageMountHandle* mountHandle) {
    if (!async || !mountHandle) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *mountHandle = nullptr;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        *mountHandle = nullptr;
        return XGR_E_FAIL;
    }
    XPackageMountHandle out_mh = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        out_mh = static_cast<XPackageMountHandle>(state->result_handle);
        state->result_handle = nullptr;
        state->cleanup_result = nullptr;
    }
    if (!out_mh) {
        *mountHandle = nullptr;
        return XGR_E_FAIL;
    }
    *mountHandle = out_mh;
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XPackageGetMountPathSize(
    XPackageMountHandle mountHandle,
    size_t* pathSize) {
    if (!mountHandle || !pathSize) return XGR_E_INVALIDARG;
    *pathSize = mountHandle->host_path.size() + 1;
    return XGR_S_OK;
}

XGameResult XPackageGetMountPath(
    XPackageMountHandle mountHandle,
    size_t pathSize,
    char* path) {
    if (!mountHandle || !path) return XGR_E_INVALIDARG;
    return CopyStringToBuffer(mountHandle->host_path, pathSize, path, nullptr);
}

void XPackageCloseMountHandle(XPackageMountHandle mountHandle) {
    if (!mountHandle) return;
    UnregisterValidHandle(mountHandle);
    delete mountHandle;
}

XGameResult XStoreCreateContext(
    XUserHandle user,
    XStoreContextHandle* storeContextHandle) {
    if (!storeContextHandle) return XGR_E_INVALIDARG;
    auto* ctx = new (std::nothrow) XStoreContextObject();
    if (!ctx) return XGR_E_OUTOFMEMORY;
    ctx->user = user;
    RegisterValidHandle(ctx);
    *storeContextHandle = ctx;
    return XGR_S_OK;
}

void XStoreCloseContextHandle(XStoreContextHandle storeContextHandle) {
    if (!storeContextHandle) return;
    UnregisterValidHandle(storeContextHandle);
    delete storeContextHandle;
}

XGameResult XStoreQueryGameLicenseAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) return XGR_E_INVALIDARG;
    return BeginAsyncOperation(async, [](XAsyncState* st) {
        XStoreGameLicense lic{};
        std::strncpy(lic.skuStoreId, "9N5CA00001RETAIL", sizeof(lic.skuStoreId) - 1);
        lic.isActive = true;
        lic.isTrialOwnedByThisUser = false;
        lic.isDiscLicense = false;
        lic.isTrial = false;
        lic.trialTimeRemainingInSeconds = 0;
        lic.expirationDate = 0;
        st->result_payload.resize(sizeof(lic));
        std::memcpy(st->result_payload.data(), &lic, sizeof(lic));
        CompleteAsyncState(st, XGR_S_OK, sizeof(lic));
    });
}

XGameResult XStoreQueryGameLicenseResult(
    XAsyncBlock* async,
    XStoreGameLicense* license) {
    if (!async || !license) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) return hr;
    auto state = GetAsyncState(async);
    if (!state || state->result_payload.size() < sizeof(XStoreGameLicense)) return XGR_E_FAIL;
    std::memcpy(license, state->result_payload.data(), sizeof(XStoreGameLicense));
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XStoreQueryEntitledProductsAsync(
    XStoreContextHandle storeContextHandle,
    uint32_t /*productKinds*/,
    uint32_t maxItemsToRetrieveCount,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) return XGR_E_INVALIDARG;
    return BeginAsyncOperation(async, [maxItemsToRetrieveCount](XAsyncState* st) {
        auto* q = new (std::nothrow) XStoreProductQueryObject();
        if (!q) {
            CompleteAsyncState(st, XGR_E_OUTOFMEMORY, 0);
            return;
        }
        const auto dlcs = DiscoverInstalledDlcPackages();
        for (const auto& dlc : dlcs) {
            if (maxItemsToRetrieveCount > 0 && q->items.size() >= maxItemsToRetrieveCount) break;
            q->items.push_back({dlc.store_id, dlc.display_name, "Entitled Add-On Content", 1u});
        }
        RegisterValidHandle(q);
        {
            std::lock_guard<std::mutex> lk(st->mutex);
            st->result_handle = q;
            st->cleanup_result = [](void* h) {
                XStoreCloseProductsQueryHandle(static_cast<XStoreProductQueryHandle>(h));
            };
        }
        CompleteAsyncState(st, XGR_S_OK, sizeof(XStoreProductQueryHandle));
    });
}

XGameResult XStoreQueryEntitledProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) {
    if (!async || !productQueryHandle) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *productQueryHandle = nullptr;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        *productQueryHandle = nullptr;
        return XGR_E_FAIL;
    }
    XStoreProductQueryHandle out_q = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        out_q = static_cast<XStoreProductQueryHandle>(state->result_handle);
        state->result_handle = nullptr;
        state->cleanup_result = nullptr;
    }
    if (!out_q) {
        *productQueryHandle = nullptr;
        return XGR_E_FAIL;
    }
    *productQueryHandle = out_q;
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult XStoreEnumerateProductsQuery(
    XStoreProductQueryHandle productQueryHandle,
    void* context,
    XStoreProductQueryCallback* callback) {
    static_assert(offsetof(XStoreProduct, inAppOfferToken) == 0x20,
                  "XStoreProduct::inAppOfferToken must be at +0x20");
    static_assert(offsetof(XStoreProduct, linkUri) == 0x28,
                  "XStoreProduct::linkUri must be at +0x28");
    static_assert(offsetof(XStoreProduct, price) == 0x38,
                  "XStoreProduct::price must be at +0x38");
    if (!productQueryHandle || !callback) return XGR_E_INVALIDARG;
    for (const auto& item : productQueryHandle->items) {
        XStoreProduct prod{};
        prod.storeId = item.store_id.c_str();
        prod.title = item.title.c_str();
        prod.description = item.description.c_str();
        prod.language = "en-US";
        prod.inAppOfferToken = "";
        prod.linkUri = "";
        prod.productKind = item.kind;
        prod.price.currencyCode = "USD";
        std::strncpy(prod.price.formattedBasePrice, "$0.00", sizeof(prod.price.formattedBasePrice) - 1);
        std::strncpy(prod.price.formattedPrice, "$0.00", sizeof(prod.price.formattedPrice) - 1);
        std::strncpy(prod.price.formattedRecurrencePrice, "$0.00", sizeof(prod.price.formattedRecurrencePrice) - 1);
        prod.hasDigitalDownload = true;
        prod.isInUserCollection = true;
        if (!callback(&prod, context)) break;
    }
    return XGR_S_OK;
}

void XStoreCloseProductsQueryHandle(XStoreProductQueryHandle productQueryHandle) {
    if (!productQueryHandle) return;
    UnregisterValidHandle(productQueryHandle);
    delete productQueryHandle;
}

// ===========================================================================
// XMem.h
// ===========================================================================
}  // extern "C"

namespace {

size_t DecodeXMemAlignment(uint64_t attrs) {
    // Honor XALLOC_ATTRIBUTES alignment code in bits 24..28 ((dwAttributes >> 24) & 0xF / 0x1F)
    // as well as Xbox page-size modifiers if passed in dwAttributes.
    const uint32_t align_code = static_cast<uint32_t>((attrs >> 24) & 0x1Fu);
    if (align_code >= 4u && align_code <= 21u) {
        return size_t{1} << align_code;
    }
    const uint32_t nibble = static_cast<uint32_t>((attrs >> 24) & 0xFu);
    if (nibble >= 4u) {
        return size_t{1} << nibble;
    }
    if ((attrs & 0x20400000ull) == 0x20400000ull) {
        return 0x10000u;
    }
    if (attrs & 0x20000000ull) {
        return 0x200000u;
    }
    const uint32_t page_type = static_cast<uint32_t>((attrs >> 29) & 0x3u);
    if (page_type == 1u) return 0x1000u;
    if (page_type == 2u) return 0x10000u;
    if (page_type == 3u) return 0x200000u;
    return 16u;
}

#if defined(_WIN32)
bool RegisterWithKernelX(void* va, size_t size) {
    if (!va || size == 0) return false;
    HMODULE kx = GetModuleHandleW(L"kernelx.dll");
    if (!kx) return false;
    FARPROC proc = GetProcAddress(kx, "KernelXRegisterExternalAllocation");
    if (!proc) return false;
    using RegFn = int32_t(SCARLETTX_GDK_CALLBACK*)(void*, uint64_t, void*, void*);
    auto fn = reinterpret_cast<RegFn>(reinterpret_cast<void*>(proc));
    int32_t rc = fn(va, static_cast<uint64_t>(size), nullptr, nullptr);
    if (rc == static_cast<int32_t>(0x80070057u)) {
        uint64_t dummy_gpu_va = 0;
        (void)fn(va, static_cast<uint64_t>(size), &dummy_gpu_va, &dummy_gpu_va);
    }
    return true;
}

void UnregisterWithKernelX(void* va, size_t size) {
    if (!va) return;
    HMODULE kx = GetModuleHandleW(L"kernelx.dll");
    if (!kx) return;
    FARPROC proc = GetProcAddress(kx, "KernelXUnregisterExternalAllocation");
    if (!proc) return;
    using UnregFn = int32_t(SCARLETTX_GDK_CALLBACK*)(void*, uint64_t);
    auto fn = reinterpret_cast<UnregFn>(reinterpret_cast<void*>(proc));
    (void)fn(va, static_cast<uint64_t>(size));
}
#endif

}  // namespace

extern "C" {

void* XMemAllocDefault(size_t dwSize, uint64_t dwAttributes) {
    const size_t align = DecodeXMemAlignment(dwAttributes);
    const size_t sz = dwSize == 0 ? align : ((dwSize + align - 1) & ~(align - 1));
#if defined(_WIN32)
    void* ptr = _aligned_malloc(sz, align);
    if (ptr) {
        std::memset(ptr, 0, sz);
    }
    return ptr;
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, align, sz) != 0) {
        return nullptr;
    }
    std::memset(ptr, 0, sz);
    return ptr;
#endif
}

void XMemFreeDefault(void* lpAddress, uint64_t /*dwAttributes*/) {
    if (!lpAddress) {
        return;
    }
#if defined(_WIN32)
    _aligned_free(lpAddress);
#else
    std::free(lpAddress);
#endif
}

void* XMemAlloc(size_t dwSize, uint64_t dwAttributes) {
    XMemAllocFunction* hook = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mem_hooks_mutex);
        hook = g_mem_alloc_hook;
    }
    if (hook) {
        return hook(dwSize, dwAttributes);
    }
    return XMemAllocDefault(dwSize, dwAttributes);
}

void XMemFree(void* lpAddress, uint64_t dwAttributes) {
    XMemFreeFunction* hook = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mem_hooks_mutex);
        hook = g_mem_free_hook;
    }
    if (hook) {
        hook(lpAddress, dwAttributes);
        return;
    }
    XMemFreeDefault(lpAddress, dwAttributes);
}

XGameResult XMemSetAllocationHooks(
    XMemAllocFunction* allocFunc,
    XMemFreeFunction* freeFunc) {
    if ((allocFunc == nullptr) != (freeFunc == nullptr)) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(g_mem_hooks_mutex);
    g_mem_alloc_hook = allocFunc;
    g_mem_free_hook = freeFunc;
    return XGR_S_OK;
}

XGameResult XMemGetAllocationHooks(
    XMemAllocFunction** allocFunc,
    XMemFreeFunction** freeFunc) {
    if (!allocFunc || !freeFunc) {
        return XGR_E_POINTER;
    }
    std::lock_guard<std::mutex> lock(g_mem_hooks_mutex);
    *allocFunc = g_mem_alloc_hook ? g_mem_alloc_hook : &XMemAllocDefault;
    *freeFunc = g_mem_free_hook ? g_mem_free_hook : &XMemFreeDefault;
    return XGR_S_OK;
}

void* XMemVirtualAlloc(
    void* lpAddress,
    size_t dwSize,
    uint32_t flAllocationType,
    uint64_t /*dwXMemFlags*/,
    uint32_t flProtect) {
#if defined(_WIN32)
    if (dwSize == 0) {
        return nullptr;
    }
    // Console page-size selectors:
    //   0x20400000 / 0x00400000 -> 64 KB pages
    //   0x20000000 / 0x40000000 -> 2 MB large pages
    // Strip Xbox-only MEM_GRAPHICS (0x04000000) and all console page/high bits before host VirtualAlloc.
    constexpr uint32_t kPage64kMask = 0x20400000u;
    size_t page = 0x1000;
    if ((flAllocationType & kPage64kMask) == kPage64kMask || (flAllocationType & 0x00400000u) != 0) {
        page = 0x10000;
    } else if ((flAllocationType & 0x20000000u) != 0 || (flAllocationType & 0x40000000u) != 0) {
        page = 0x200000;
    }
    constexpr DWORD kValidWin32AllocFlags =
        MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH | MEM_RESET_UNDO;
    DWORD flAlloc = static_cast<DWORD>(flAllocationType) & kValidWin32AllocFlags;
    if (flAlloc == 0) {
        flAlloc = MEM_RESERVE | MEM_COMMIT;
    }
    if (!lpAddress && (flAlloc & MEM_COMMIT) != 0) {
        flAlloc |= MEM_RESERVE;
    }
    DWORD protect = static_cast<DWORD>(flProtect) & 0x7FFu;
    if ((protect & 0xFFu) == 0) {
        protect = (flAlloc == MEM_RESERVE) ? PAGE_NOACCESS : PAGE_READWRITE;
    }
    const size_t size = (dwSize + page - 1) & ~(page - 1);
    void* result = nullptr;
    if (lpAddress || page <= 0x10000) {
        result = VirtualAlloc(lpAddress, size, flAlloc, protect);
    } else {
        struct Win32AddrReq {
            PVOID lowest;
            PVOID highest;
            SIZE_T alignment;
        };
        struct alignas(8) Win32ExtParam {
            ULONG64 type;
            PVOID ptr;
        };
        using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
        static std::atomic<VirtualAlloc2Fn> va2_cached{nullptr};
        static std::atomic<bool> va2_probed{false};
        if (!va2_probed.load(std::memory_order_acquire)) {
            HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
            VirtualAlloc2Fn fn = kb ? reinterpret_cast<VirtualAlloc2Fn>(
                                          reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2")))
                                    : nullptr;
            va2_cached.store(fn, std::memory_order_relaxed);
            va2_probed.store(true, std::memory_order_release);
        }
        if (VirtualAlloc2Fn va2 = va2_cached.load(std::memory_order_relaxed)) {
            Win32AddrReq req{nullptr, nullptr, static_cast<SIZE_T>(page)};
            Win32ExtParam ext{1ull /* MemExtendedParameterAddressRequirements */, &req};
            result = va2(GetCurrentProcess(), nullptr, size, flAlloc, protect, &ext, 1u);
        }
        if (!result) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                void* probe = VirtualAlloc(nullptr, size + page, MEM_RESERVE, PAGE_NOACCESS);
                if (!probe) break;
                VirtualFree(probe, 0, MEM_RELEASE);
                const uintptr_t aligned = (reinterpret_cast<uintptr_t>(probe) + page - 1) & ~(page - 1);
                if (void* p = VirtualAlloc(reinterpret_cast<void*>(aligned), size, flAlloc, protect)) {
                    result = p;
                    break;
                }
            }
        }
    }
    if (!result) {
        return nullptr;
    }

    const uintptr_t res_addr = reinterpret_cast<uintptr_t>(result);
    const bool is_reserve = (flAlloc & MEM_RESERVE) != 0;
    const bool is_commit = (flAlloc & MEM_COMMIT) != 0;
    bool register_kx = false;
    size_t register_kx_size = 0;
    {
        std::lock_guard<std::mutex> lock(g_vmem_mutex);
        if (is_reserve) {
            VMemRegion reg{};
            reg.base = res_addr;
            reg.reserved_size = size;
            reg.committed_bytes = is_commit ? size : 0;
            reg.registered_kernelx = true;
            g_vmem_regions[res_addr] = reg;
            g_mem_total_reserved.fetch_add(size, std::memory_order_relaxed);
            if (is_commit) {
                g_mem_total_allocated.fetch_add(size, std::memory_order_relaxed);
            }
            register_kx = true;
            register_kx_size = size;
        } else if (is_commit) {
            // Committing within an already-reserved region: do not double-count reserved bytes.
            auto it = g_vmem_regions.upper_bound(res_addr);
            if (it != g_vmem_regions.begin()) {
                --it;
                if (res_addr >= it->second.base &&
                    res_addr < it->second.base + it->second.reserved_size) {
                    const size_t avail = it->second.reserved_size - it->second.committed_bytes;
                    const size_t added = std::min(size, avail);
                    it->second.committed_bytes += added;
                    g_mem_total_allocated.fetch_add(added, std::memory_order_relaxed);
                } else {
                    g_mem_total_allocated.fetch_add(size, std::memory_order_relaxed);
                }
            } else {
                g_mem_total_allocated.fetch_add(size, std::memory_order_relaxed);
            }
        }
    }
    if (register_kx) {
        (void)RegisterWithKernelX(result, register_kx_size);
    }
    return result;
#else
    (void)lpAddress;
    (void)flAllocationType;
    (void)flProtect;
    const size_t alloc_sz = dwSize == 0 ? 4096 : dwSize;
    void* ptr = std::calloc(1, alloc_sz);
    if (ptr) {
        std::lock_guard<std::mutex> lock(g_vmem_mutex);
        const uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
        g_vmem_regions[base] = VMemRegion{base, alloc_sz, alloc_sz, false};
        g_mem_total_allocated.fetch_add(alloc_sz, std::memory_order_relaxed);
        g_mem_total_reserved.fetch_add(alloc_sz, std::memory_order_relaxed);
    }
    return ptr;
#endif
}

bool XMemVirtualFree(void* address, size_t size, uint32_t freeType) {
    if (!address) {
        return false;
    }
#if defined(_WIN32)
    DWORD flFree = freeType ? static_cast<DWORD>(freeType) : MEM_RELEASE;
    bool ok = VirtualFree(address, flFree == MEM_RELEASE ? 0 : size, flFree) != 0;
    if (ok) {
        const uintptr_t addr_val = reinterpret_cast<uintptr_t>(address);
        bool unregister_kx = false;
        size_t unregister_kx_size = 0;
        {
            std::lock_guard<std::mutex> lock(g_vmem_mutex);
            if (flFree == MEM_RELEASE) {
                auto it = g_vmem_regions.find(addr_val);
                size_t rel_reserved = size == 0 ? 4096 : size;
                size_t rel_committed = rel_reserved;
                if (it != g_vmem_regions.end()) {
                    rel_reserved = it->second.reserved_size;
                    rel_committed = it->second.committed_bytes;
                    unregister_kx = it->second.registered_kernelx;
                    unregister_kx_size = rel_reserved;
                    g_vmem_regions.erase(it);
                }
                const size_t cur_alloc = g_mem_total_allocated.load(std::memory_order_relaxed);
                g_mem_total_allocated.fetch_sub(std::min(rel_committed, cur_alloc), std::memory_order_relaxed);
                const size_t cur_res = g_mem_total_reserved.load(std::memory_order_relaxed);
                g_mem_total_reserved.fetch_sub(std::min(rel_reserved, cur_res), std::memory_order_relaxed);
            } else if ((flFree & MEM_DECOMMIT) != 0) {
                const size_t decommit_sz = (size == 0 ? 4096 : ((size + 0xFFFu) & ~size_t{0xFFFu}));
                size_t sub_committed = decommit_sz;
                auto it = g_vmem_regions.upper_bound(addr_val);
                if (it != g_vmem_regions.begin()) {
                    --it;
                    if (addr_val >= it->second.base &&
                        addr_val < it->second.base + it->second.reserved_size) {
                        sub_committed = std::min(decommit_sz, it->second.committed_bytes);
                        it->second.committed_bytes -= sub_committed;
                    }
                }
                const size_t cur_alloc = g_mem_total_allocated.load(std::memory_order_relaxed);
                g_mem_total_allocated.fetch_sub(std::min(sub_committed, cur_alloc), std::memory_order_relaxed);
            }
        }
        if (unregister_kx) {
            UnregisterWithKernelX(address, unregister_kx_size);
        }
    }
    return ok;
#else
    (void)size;
    (void)freeType;
    const uintptr_t addr_val = reinterpret_cast<uintptr_t>(address);
    {
        std::lock_guard<std::mutex> lock(g_vmem_mutex);
        auto it = g_vmem_regions.find(addr_val);
        if (it != g_vmem_regions.end()) {
            const size_t cur_alloc = g_mem_total_allocated.load(std::memory_order_relaxed);
            g_mem_total_allocated.fetch_sub(std::min(it->second.committed_bytes, cur_alloc), std::memory_order_relaxed);
            const size_t cur_res = g_mem_total_reserved.load(std::memory_order_relaxed);
            g_mem_total_reserved.fetch_sub(std::min(it->second.reserved_size, cur_res), std::memory_order_relaxed);
            g_vmem_regions.erase(it);
        }
    }
    std::free(address);
    return true;
#endif
}

XGameResult XMemGetVirtualAllocStats(XMemVirtualAllocStats* stats) {
    if (!stats) {
        return XGR_E_INVALIDARG;
    }
    stats->totalAllocatedBytes = g_mem_total_allocated.load(std::memory_order_relaxed);
    stats->totalReservedBytes = g_mem_total_reserved.load(std::memory_order_relaxed);
    return XGR_S_OK;
}

void XMemResetVirtualAllocStats() {
    g_mem_total_allocated.store(0, std::memory_order_relaxed);
    g_mem_total_reserved.store(0, std::memory_order_relaxed);
}

// ===========================================================================
// XThread.h & XGame.h
// ===========================================================================
bool XThreadIsTimeSensitive() {
    return GetCurrentThreadFlsTimeSensitive();
}

XGameResult XThreadSetTimeSensitive(bool isTimeSensitive) {
    SetCurrentThreadFlsTimeSensitive(isTimeSensitive);
    return XGR_S_OK;
}

void XThreadAssertNotTimeSensitive() {
    // Non-fatal in HLE; real console triggers a debug break if time-sensitive.
}

void XThreadAssertTimeSensitive() {
    // Non-fatal in HLE; real console triggers a debug break if not time-sensitive.
}

XGameResult XThreadGetTimeSensitive(bool* isTimeSensitive) {
    if (!isTimeSensitive) {
        return XGR_E_INVALIDARG;
    }
    *isTimeSensitive = GetCurrentThreadFlsTimeSensitive();
    return XGR_S_OK;
}

XGameResult XThreadSetTimeSensitiveThread(uint32_t threadId, bool isTimeSensitive) {
#if defined(_WIN32)
    const uint32_t self_tid = GetCurrentThreadId();
    if (threadId == 0 || threadId == self_tid) {
        SetCurrentThreadFlsTimeSensitive(isTimeSensitive);
        return XGR_S_OK;
    }
    // Every real Win32 TID has ((tid & 3) == 0); check before OpenThread because
    // NT's ExMapHandleToPointer masks the low 2 tag bits (1234 -> 1232) and could
    // otherwise match an unrelated process's thread 1232!
    if ((threadId & 3u) != 0u) {
        SetCurrentThreadFlsTimeSensitive(isTimeSensitive);
        return XGR_S_OK;
    }
    if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId)) {
        const DWORD pid = GetProcessIdOfThread(h);
        CloseHandle(h);
        if (pid != GetCurrentProcessId()) {
            return XGR_E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lock(g_ts_mutex);
        g_ts_by_tid[threadId] = isTimeSensitive;
        return XGR_S_OK;
    }
    {
        std::lock_guard<std::mutex> lock(g_ts_mutex);
        g_ts_by_tid[threadId] = isTimeSensitive;
    }
    return XGR_E_INVALIDARG;
#else
    (void)threadId;
    SetCurrentThreadFlsTimeSensitive(isTimeSensitive);
    return XGR_S_OK;
#endif
}

XGameResult XGameGetXboxTitleId(uint32_t* titleId) {
    if (!titleId) {
        return XGR_E_POINTER;
    }
    if (const char* env_tid = std::getenv("SCARLETTX_TITLE_ID"); env_tid && *env_tid) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(env_tid, &end, 16);
        if (end && end != env_tid) {
            *titleId = static_cast<uint32_t>(parsed);
            return XGR_S_OK;
        }
    }
    *titleId = 0x5CA00001u;
    return XGR_S_OK;
}

void XErrorReport(XGameResult hr, const char* msg) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] XErrorReport(hr=0x%08X, msg=%s)\n",
                 static_cast<unsigned>(hr), msg ? msg : "<null>");
    std::fflush(stderr);
}

XGameResult InitializeApiImpl(uint32_t build, uint32_t revision) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] InitializeApiImpl(build=%u, rev=%u)\n",
                 build, revision);
    std::fflush(stderr);
    return XGameRuntimeInitialize();
}

XGameResult InitializeApiImplEx(uint32_t build, uint32_t revision, uint64_t flags) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] InitializeApiImplEx(build=%u, rev=%u, flags=0x%llX)\n",
                 build, revision, static_cast<unsigned long long>(flags));
    std::fflush(stderr);
    return XGameRuntimeInitialize();
}

XGameResult InitializeApiImplEx2(uint32_t build, uint32_t revision, uint64_t flags,
                                 const XGameRuntimeOptions* options) {
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] InitializeApiImplEx2(build=%u, rev=%u, flags=0x%llX, options=%p)\n",
                 build, revision, static_cast<unsigned long long>(flags),
                 static_cast<const void*>(options));
    std::fflush(stderr);
    return XGameRuntimeInitialize();
}

void UninitializeApiImpl() {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] UninitializeApiImpl()\n");
    std::fflush(stderr);
    XGameRuntimeUninitialize();
}

static void FormatGuid(const void* guid_ptr, char* out, size_t out_size) {
    if (!guid_ptr) {
        std::snprintf(out, out_size, "<null>");
        return;
    }
    const auto* b = reinterpret_cast<const uint8_t*>(guid_ptr);
    const uint32_t d1 = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    const uint16_t d2 = static_cast<uint16_t>(b[4]) | (static_cast<uint16_t>(b[5]) << 8);
    const uint16_t d3 = static_cast<uint16_t>(b[6]) | (static_cast<uint16_t>(b[7]) << 8);
    std::snprintf(out, out_size, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  d1, d2, d3, b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

}  // extern "C"

namespace {

struct XgrGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

static_assert(sizeof(XgrGuid) == 16, "GUID must be 16 bytes");

bool GuidEquals(const void* a_ptr, const XgrGuid& b) {
    if (!a_ptr) {
        return false;
    }
    return std::memcmp(a_ptr, &b, sizeof(XgrGuid)) == 0;
}

// Standard COM IUnknown IID: {00000000-0000-0000-C000-000000000046}
constexpr XgrGuid kIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// ApiImpl GUID {073B7DCB-1FCF-4030-94BE-E3C9EB623428}.
// Microsoft's interface name for this table is not asserted. The slot map below
// comes from the system xgameruntime.dll of the target package: its table has 32
// entries, and each of slots 3-30 is a thunk that drops `this` and tail-calls a
// flat function. A slot is given a public-header name only where the forwarded
// argument list matches that declaration and its position follows the order of
// XAsync.h, XAsyncProvider.h, XTaskQueue.h and XThread.h; the title's own call
// sites confirm slots 6, 12, 16, 17 and 29.
//    3 XAsyncGetStatus            4 XAsyncGetResultSize         5 XAsyncCancel
//    6 XAsyncRun                  7 XAsyncBegin                 8 (unidentified)
//    9 XAsyncSchedule            10 XAsyncComplete             11 XAsyncGetResult
//   12 XTaskQueueCreate          13 XTaskQueueCreateComposite  14 XTaskQueueGetPort
//   15 XTaskQueueDuplicateHandle 16 XTaskQueueDispatch         17 XTaskQueueCloseHandle
//   18 XTaskQueueSubmitCallback  19 XTaskQueueSubmitDelayedCallback
//   20 XTaskQueueRegisterWaiter  21 XTaskQueueUnregisterWaiter 22 XTaskQueueTerminate
//   23 XTaskQueueRegisterMonitor 24 XTaskQueueUnregisterMonitor
//   25 XTaskQueueGetCurrentProcessTaskQueue  26 XTaskQueueSetCurrentProcessTaskQueue
//   27 XThreadSetTimeSensitive   28 XThreadIsTimeSensitive     29 XThreadAssertNotTimeSensitive
//   30 (unidentified, no arguments)          31 (not a guest-callable method)
// Unidentified slots log the call and return E_NOTIMPL.
constexpr XgrGuid kGuidApiImpl073B7DCB = {
    0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};

struct XgrApiImpl073B7DCBVtable {
    void* slots[32];
};

struct XgrApiImpl073B7DCB {
    const XgrApiImpl073B7DCBVtable* vtbl;
    std::atomic<uint32_t> ref_count{1};
};

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl073B7DCB_QueryInterface(
    void* self_ptr,
    const void* riid,
    void** ppv) {
    if (!ppv) {
        return XGR_E_POINTER;
    }
    *ppv = nullptr;
    if (!self_ptr || !riid) {
        return XGR_E_INVALIDARG;
    }
    if (GuidEquals(riid, kGuidApiImpl073B7DCB) || GuidEquals(riid, kIidIUnknown)) {
        auto* self = static_cast<XgrApiImpl073B7DCB*>(self_ptr);
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return XGR_S_OK;
    }
    return static_cast<XGameResult>(0x80004002u);  // E_NOINTERFACE
}

uint32_t SCARLETTX_GDK_CALLBACK ApiImpl073B7DCB_AddRef(void* self_ptr) {
    if (!self_ptr) {
        return 0;
    }
    auto* self = static_cast<XgrApiImpl073B7DCB*>(self_ptr);
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_GDK_CALLBACK ApiImpl073B7DCB_Release(void* self_ptr) {
    if (!self_ptr) {
        return 0;
    }
    auto* self = static_cast<XgrApiImpl073B7DCB*>(self_ptr);
    const uint32_t remaining = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete self;
    }
    return remaining;
}

// The slots below ignore `this` and forward to the flat implementation, as the
// real table does. `S` is the slot's flat signature without the leading `this`.
template <typename R, typename... A>
struct ApiImplForward {
    template <R (*Fn)(A...)>
    static R SCARLETTX_GDK_CALLBACK Call(void* /*self*/, A... args) {
        return Fn(args...);
    }
};

#define XGR_FORWARD(ret, fn, ...) \
    reinterpret_cast<void*>(&ApiImplForward<ret, ##__VA_ARGS__>::template Call<&fn>)

XGameResult Slot12_TaskQueueCreate(uint32_t work_mode, uint32_t completion_mode,
                                   XTaskQueueHandle* queue) {
    return XTaskQueueCreate(static_cast<XTaskQueueDispatchMode>(work_mode),
                            static_cast<XTaskQueueDispatchMode>(completion_mode), queue);
}
XGameResult Slot14_GetPort(XTaskQueueHandle queue, uint32_t port, XTaskQueuePortHandle* out) {
    return XTaskQueueGetPort(queue, static_cast<XTaskQueuePort>(port), out);
}
bool Slot16_Dispatch(XTaskQueueHandle queue, uint32_t port, uint32_t timeout_ms) {
    return XTaskQueueDispatch(queue, static_cast<XTaskQueuePort>(port), timeout_ms);
}
XGameResult Slot18_Submit(XTaskQueueHandle queue, uint32_t port, void* context,
                          XTaskQueueCallback* callback) {
    return XTaskQueueSubmitCallback(queue, static_cast<XTaskQueuePort>(port), context, callback);
}
XGameResult Slot19_SubmitDelayed(XTaskQueueHandle queue, uint32_t port, uint32_t delay_ms,
                                 void* context, XTaskQueueCallback* callback) {
    return XTaskQueueSubmitDelayedCallback(queue, static_cast<XTaskQueuePort>(port), delay_ms,
                                           context, callback);
}
XGameResult Slot20_RegisterWaiter(XTaskQueueHandle queue, uint32_t port, void* wait_handle,
                                  void* context, XTaskQueueCallback* callback,
                                  XTaskQueueRegistrationToken* token) {
    return XTaskQueueRegisterWaiter(queue, static_cast<XTaskQueuePort>(port), wait_handle,
                                    context, callback, token);
}
void Slot21_UnregisterWaiter(XTaskQueueHandle queue, uint64_t token) {
    XTaskQueueUnregisterWaiter(queue, XTaskQueueRegistrationToken{token});
}
void Slot24_UnregisterMonitor(XTaskQueueHandle queue, uint64_t token) {
    XTaskQueueUnregisterMonitor(queue, XTaskQueueRegistrationToken{token});
}

template <uint32_t SlotIndex>
XGameResult SCARLETTX_GDK_CALLBACK ApiImpl073B7DCB_UnimplementedSlot(void* /*self_ptr*/) {
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{073B7DCB}::+0x%X (slot %u) -> E_NOTIMPL\n",
                 SlotIndex * 8u,
                 SlotIndex);
    std::fflush(stderr);
    return static_cast<XGameResult>(0x80004001u);  // E_NOTIMPL
}

const XgrApiImpl073B7DCBVtable kApiImpl073B7DCBVtable = {{
    reinterpret_cast<void*>(&ApiImpl073B7DCB_QueryInterface),                             // 0
    reinterpret_cast<void*>(&ApiImpl073B7DCB_AddRef),                                     // 1
    reinterpret_cast<void*>(&ApiImpl073B7DCB_Release),                                    // 2
    XGR_FORWARD(XGameResult, XAsyncGetStatus, XAsyncBlock*, bool),                        // 3
    XGR_FORWARD(XGameResult, XAsyncGetResultSize, XAsyncBlock*, size_t*),                 // 4
    XGR_FORWARD(void, XAsyncCancel, XAsyncBlock*),                                        // 5
    XGR_FORWARD(XGameResult, XAsyncRun, XAsyncBlock*, XAsyncWork*),                       // 6
    XGR_FORWARD(XGameResult, XAsyncBegin, XAsyncBlock*, void*, const void*, const char*,
                XAsyncProvider*),                                                         // 7
    reinterpret_cast<void*>(&ApiImpl073B7DCB_UnimplementedSlot<8>),                       // 8
    XGR_FORWARD(XGameResult, XAsyncSchedule, XAsyncBlock*, uint32_t),                     // 9
    XGR_FORWARD(void, XAsyncComplete, XAsyncBlock*, XGameResult, size_t),                 // 10
    XGR_FORWARD(XGameResult, XAsyncGetResult, XAsyncBlock*, const void*, size_t, void*,
                size_t*),                                                                 // 11
    XGR_FORWARD(XGameResult, Slot12_TaskQueueCreate, uint32_t, uint32_t,
                XTaskQueueHandle*),                                                       // 12
    XGR_FORWARD(XGameResult, XTaskQueueCreateComposite, XTaskQueuePortHandle,
                XTaskQueuePortHandle, XTaskQueueHandle*),                                 // 13
    XGR_FORWARD(XGameResult, Slot14_GetPort, XTaskQueueHandle, uint32_t,
                XTaskQueuePortHandle*),                                                   // 14
    XGR_FORWARD(XGameResult, XTaskQueueDuplicateHandle, XTaskQueueHandle,
                XTaskQueueHandle*),                                                       // 15
    XGR_FORWARD(bool, Slot16_Dispatch, XTaskQueueHandle, uint32_t, uint32_t),             // 16
    XGR_FORWARD(void, XTaskQueueCloseHandle, XTaskQueueHandle),                           // 17
    XGR_FORWARD(XGameResult, Slot18_Submit, XTaskQueueHandle, uint32_t, void*,
                XTaskQueueCallback*),                                                     // 18
    XGR_FORWARD(XGameResult, Slot19_SubmitDelayed, XTaskQueueHandle, uint32_t, uint32_t,
                void*, XTaskQueueCallback*),                                              // 19
    XGR_FORWARD(XGameResult, Slot20_RegisterWaiter, XTaskQueueHandle, uint32_t, void*,
                void*, XTaskQueueCallback*, XTaskQueueRegistrationToken*),                // 20
    XGR_FORWARD(void, Slot21_UnregisterWaiter, XTaskQueueHandle, uint64_t),               // 21
    XGR_FORWARD(XGameResult, XTaskQueueTerminate, XTaskQueueHandle, bool, void*,
                XTaskQueueTerminatedCallback*),                                           // 22
    XGR_FORWARD(XGameResult, XTaskQueueRegisterMonitor, XTaskQueueHandle, void*,
                XTaskQueueMonitorCallback*, XTaskQueueRegistrationToken*),                // 23
    XGR_FORWARD(void, Slot24_UnregisterMonitor, XTaskQueueHandle, uint64_t),              // 24
    XGR_FORWARD(bool, XTaskQueueGetCurrentProcessTaskQueue, XTaskQueueHandle*),           // 25
    XGR_FORWARD(void, XTaskQueueSetCurrentProcessTaskQueue, XTaskQueueHandle),            // 26
    XGR_FORWARD(XGameResult, XThreadSetTimeSensitive, bool),                              // 27
    XGR_FORWARD(bool, XThreadIsTimeSensitive),                                            // 28
    XGR_FORWARD(void, XThreadAssertNotTimeSensitive),                                     // 29
    XGR_FORWARD(void, XThreadAssertTimeSensitive),                                        // 30
    reinterpret_cast<void*>(&ApiImpl073B7DCB_UnimplementedSlot<31>),                      // 31
}};

// Observed second GDK ApiImpl GUID from real Scarlett target disassembly:
//   {37E56907-2F10-41E8-B72F-36EDB185331A}
// Official Microsoft interface name is not asserted; vtable offsets and
// Windows x64 register ABIs below are derived from all 8 target wrapper sites:
//   +0x00 (slot  0): QueryInterface (this, riid, ppv) -> HRESULT
//   +0x08 (slot  1): AddRef (this) -> uint32_t
//   +0x10 (slot  2): Release (this) -> uint32_t
//   +0x18 (slot  3): (this) -> HRESULT
//   +0x20 (slot  4): (this) -> HRESULT
//   +0x28 (slot  5): (this, arg1, uint16_t* out_val) -> HRESULT
//   +0x30 (slot  6): (this, arg1, arg2, arg3, uint64_t* out_token) -> HRESULT
//   +0x38 (slot  7): (this, uint64_t token, uint8_t flag) -> bool/u8
//   +0x78 (slot 15): (this, void* out_state) -> HRESULT (caller polls byte +0x0c != 0)
//   +0x80 (slot 16): (this, arg1, arg2, arg3, uint64_t* out_token) -> HRESULT
//   +0x88 (slot 17): (this, uint64_t token, uint8_t flag) -> bool/u8
constexpr XgrGuid kGuidApiImpl37E56907 = {
    0x37E56907u, 0x2F10u, 0x41E8u, {0xB7, 0x2F, 0x36, 0xED, 0xB1, 0x85, 0x33, 0x1A}};
// Interface id a title built against a later GDK asks this API for. Its
// calls to slots 15, 16 and 17 pass the same arguments; it also calls
// slots 8, 9, 10 and 14, which have no implementation here.
constexpr XgrGuid kGuidApiImpl37E56907Later = {
    0xBF2346B2u, 0x39AFu, 0x4658u, {0xB5, 0xEA, 0x44, 0x71, 0x3C, 0x7E, 0x83, 0xB3}};

struct XgrApiImpl37E56907Vtable {
    void* slots[24];
};

struct XgrApiImpl37E56907 {
    const XgrApiImpl37E56907Vtable* vtbl;
    std::atomic<uint32_t> ref_count{1};
};

std::atomic<uint64_t> g_api37e5_next_token{1};

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_QueryInterface(
    void* self_ptr,
    const void* riid,
    void** ppv) {
    if (!ppv) {
        return XGR_E_POINTER;
    }
    *ppv = nullptr;
    if (!self_ptr || !riid) {
        return XGR_E_INVALIDARG;
    }
    if (GuidEquals(riid, kGuidApiImpl37E56907) || GuidEquals(riid, kGuidApiImpl37E56907Later) ||
        GuidEquals(riid, kIidIUnknown)) {
        auto* self = static_cast<XgrApiImpl37E56907*>(self_ptr);
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return XGR_S_OK;
    }
    return static_cast<XGameResult>(0x80004002u);  // E_NOINTERFACE
}

uint32_t SCARLETTX_GDK_CALLBACK ApiImpl37E56907_AddRef(void* self_ptr) {
    if (!self_ptr) {
        return 0;
    }
    auto* self = static_cast<XgrApiImpl37E56907*>(self_ptr);
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Release(void* self_ptr) {
    if (!self_ptr) {
        return 0;
    }
    auto* self = static_cast<XgrApiImpl37E56907*>(self_ptr);
    const uint32_t remaining = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete self;
    }
    return remaining;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot03(void* /*self_ptr*/) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x18() -> S_OK\n");
    std::fflush(stderr);
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot04(void* /*self_ptr*/) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x20() -> S_OK\n");
    std::fflush(stderr);
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot05(
    void* /*self_ptr*/,
    uintptr_t arg1,
    uint16_t* out_val) {
    if (!out_val) {
        return XGR_E_POINTER;
    }
    *out_val = 0;
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x28(a1=0x%llx) -> S_OK (val=0)\n",
                 static_cast<unsigned long long>(arg1));
    std::fflush(stderr);
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot06(
    void* /*self_ptr*/,
    uintptr_t arg1,
    uintptr_t arg2,
    uintptr_t arg3,
    uint64_t* out_token) {
    if (!out_token) {
        return XGR_E_POINTER;
    }
    const uint64_t tok = g_api37e5_next_token.fetch_add(1, std::memory_order_relaxed);
    *out_token = tok;
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x30(a1=0x%llx, a2=0x%llx, a3=0x%llx) -> S_OK (tok=%llu)\n",
                 static_cast<unsigned long long>(arg1),
                 static_cast<unsigned long long>(arg2),
                 static_cast<unsigned long long>(arg3),
                 static_cast<unsigned long long>(tok));
    std::fflush(stderr);
    return XGR_S_OK;
}

bool SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot07(
    void* /*self_ptr*/,
    uint64_t token,
    uint8_t flag) {
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x38(tok=%llu, flag=%u) -> true\n",
                 static_cast<unsigned long long>(token),
                 static_cast<unsigned>(flag));
    std::fflush(stderr);
    return true;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot15(
    void* /*self_ptr*/,
    void* out_state) {
    if (!out_state) {
        return XGR_E_POINTER;
    }
    // XNetworkingConnectivityHint is 16 bytes; the caller polls the byte at
    // +0x0c (networkInitialized) until non-zero (with 50ms Sleep). The other
    // fields are reported as unknown rather than left uninitialised.
    std::memset(out_state, 0, 16);
    static_cast<uint8_t*>(out_state)[0x0c] = 1;
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x78(out=%p) -> S_OK (byte+0x0c=1)\n",
                 out_state);
    std::fflush(stderr);
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot16(
    void* /*self_ptr*/,
    uintptr_t arg1,
    uintptr_t arg2,
    uintptr_t arg3,
    uint64_t* out_token) {
    if (!out_token) {
        return XGR_E_POINTER;
    }
    const uint64_t tok = g_api37e5_next_token.fetch_add(1, std::memory_order_relaxed);
    *out_token = tok;
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x80(a1=0x%llx, a2=0x%llx, a3=0x%llx) -> S_OK (tok=%llu)\n",
                 static_cast<unsigned long long>(arg1),
                 static_cast<unsigned long long>(arg2),
                 static_cast<unsigned long long>(arg3),
                 static_cast<unsigned long long>(tok));
    std::fflush(stderr);
    return XGR_S_OK;
}

bool SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot17(
    void* /*self_ptr*/,
    uint64_t token,
    uint8_t flag) {
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x88(tok=%llu, flag=%u) -> true\n",
                 static_cast<unsigned long long>(token),
                 static_cast<unsigned>(flag));
    std::fflush(stderr);
    return true;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot08(
    void* /*self_ptr*/,
    uint16_t* out_port) {
    if (!out_port) return XGR_E_POINTER;
    *out_port = 3074u;
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot09(
    void* /*self_ptr*/,
    XAsyncBlock* async) {
    if (!async) return XGR_E_INVALIDARG;
    return BeginAsyncOperation(async, [](XAsyncState* st) {
        uint16_t port = 3074u;
        st->result_payload.resize(sizeof(port));
        std::memcpy(st->result_payload.data(), &port, sizeof(port));
        CompleteAsyncState(st, XGR_S_OK, sizeof(port));
    });
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot10(
    void* /*self_ptr*/,
    XAsyncBlock* async,
    uint16_t* out_port) {
    if (!async || !out_port) return XGR_E_INVALIDARG;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) return hr;
    *out_port = 3074u;
    xgr::ReleaseHleAsyncResult(async);
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_Slot14(
    void* /*self_ptr*/,
    const char* /*url*/,
    size_t* out_size) {
    if (out_size) *out_size = 16u;
    return XGR_S_OK;
}

template <uint32_t SlotIndex>
XGameResult SCARLETTX_GDK_CALLBACK ApiImpl37E56907_UnimplementedSlot(void* /*self_ptr*/) {
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{37E56907}::+0x%X (slot %u) -> E_NOTIMPL\n",
                 SlotIndex * 8u,
                 SlotIndex);
    std::fflush(stderr);
    scarlettx::FlightRecorder::Instance().RecordVtableSlotMiss(
        "XgrApiImpl{37E56907}", static_cast<int32_t>(SlotIndex), 0ull, SCARLETTX_CALLER_RIP());
    return static_cast<XGameResult>(0x80004001u);  // E_NOTIMPL
}

const XgrApiImpl37E56907Vtable kApiImpl37E56907Vtable = {{
    reinterpret_cast<void*>(&ApiImpl37E56907_QueryInterface),        // 0  (+0x00)
    reinterpret_cast<void*>(&ApiImpl37E56907_AddRef),                // 1  (+0x08)
    reinterpret_cast<void*>(&ApiImpl37E56907_Release),               // 2  (+0x10)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot03),                // 3  (+0x18)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot04),                // 4  (+0x20)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot05),                // 5  (+0x28)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot06),                // 6  (+0x30)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot07),                // 7  (+0x38)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot08),                // 8  (+0x40)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot09),                // 9  (+0x48)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot10),                // 10 (+0x50)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<11>), // 11 (+0x58)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<12>), // 12 (+0x60)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<13>), // 13 (+0x68)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot14),                // 14 (+0x70)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot15),                // 15 (+0x78)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot16),                // 16 (+0x80)
    reinterpret_cast<void*>(&ApiImpl37E56907_Slot17),                // 17 (+0x88)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<18>), // 18 (+0x90)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<19>), // 19 (+0x98)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<20>), // 20 (+0xA0)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<21>), // 21 (+0xA8)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<22>), // 22 (+0xB0)
    reinterpret_cast<void*>(&ApiImpl37E56907_UnimplementedSlot<23>), // 23 (+0xB8)
}};

}  // namespace

extern "C" {

XGameResult QueryApiImpl(const void* api_id, const void* riid, void** out_impl) {
    if (!api_id || !riid || !out_impl) {
        return XGR_E_POINTER;
    }
    *out_impl = nullptr;
    char api_str[64] = {};
    char iid_str[64] = {};
    FormatGuid(api_id, api_str, sizeof(api_str));
    FormatGuid(riid, iid_str, sizeof(iid_str));

    if (GuidEquals(api_id, kGuidApiImpl073B7DCB) &&
        (GuidEquals(riid, kGuidApiImpl073B7DCB) || GuidEquals(riid, kIidIUnknown))) {
        auto* impl = new (std::nothrow) XgrApiImpl073B7DCB();
        if (!impl) {
            return XGR_E_OUTOFMEMORY;
        }
        impl->vtbl = &kApiImpl073B7DCBVtable;
        *out_impl = impl;
        if (TraceEnabled()) {
            std::fprintf(stderr,
                         "[ScarlettX][xgameruntime] QueryApiImpl(api=%s, iid=%s) -> S_OK (%p)\n",
                         api_str, iid_str, static_cast<void*>(impl));
            std::fflush(stderr);
        }
        return XGR_S_OK;
    }

    if (GuidEquals(api_id, kGuidApiImpl37E56907) &&
        (GuidEquals(riid, kGuidApiImpl37E56907) || GuidEquals(riid, kGuidApiImpl37E56907Later) ||
         GuidEquals(riid, kIidIUnknown))) {
        auto* impl = new (std::nothrow) XgrApiImpl37E56907();
        if (!impl) {
            return XGR_E_OUTOFMEMORY;
        }
        impl->vtbl = &kApiImpl37E56907Vtable;
        *out_impl = impl;
        if (TraceEnabled()) {
            std::fprintf(stderr,
                         "[ScarlettX][xgameruntime] QueryApiImpl(api=%s, iid=%s) -> S_OK (%p)\n",
                         api_str, iid_str, static_cast<void*>(impl));
            std::fflush(stderr);
        }
        return XGR_S_OK;
    }

    if (void* table = scarlettx::xgr::CreateApiTable(api_id, riid)) {
        *out_impl = table;
        if (TraceEnabled()) {
            std::fprintf(stderr,
                         "[ScarlettX][xgameruntime] QueryApiImpl(api=%s, iid=%s) -> S_OK (%p)\n",
                         api_str, iid_str, table);
            std::fflush(stderr);
        }
        return XGR_S_OK;
    }

    // A title often waits forever on an API it cannot get, so the first
    // request for each missing table is reported even without --trace.
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_missing_api_mutex);
        first = g_missing_apis.insert(std::string(api_str) + iid_str).second;
    }
    if (first || TraceEnabled()) {
        std::fprintf(stderr,
                     "[ScarlettX][xgameruntime] QueryApiImpl(api=%s, iid=%s) -> E_NOINTERFACE\n",
                     api_str, iid_str);
        std::fflush(stderr);
    }
    scarlettx::FlightRecorder::Instance().RecordComQiMiss(
        "XGR::QueryApiImpl", riid, SCARLETTX_CALLER_RIP());
    return static_cast<XGameResult>(0x80004002u);
}

}  // extern "C"
