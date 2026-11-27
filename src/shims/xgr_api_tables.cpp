// ScarlettX — xgameruntime QueryApiImpl tables.
//
// Slot layouts were read from the console runtime: each table is IUnknown
// followed by the listed methods, and every method takes the table as its
// first argument.
#include "xgr_api_tables.h"

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/gdk/xgameruntime_abi.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <utility>

namespace scarlettx::xgr {
namespace {

struct Guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

constexpr Guid kIidIUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
// XGameGetXboxTitleId.
constexpr Guid kApiGame = {0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84, 0x57, 0x56, 0xC5, 0x34, 0x89, 0x2B, 0x29}};
// XUser.
constexpr Guid kApiUser = {0x01ACD177u, 0x91F9u, 0x4763u, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}};
// Second interface of the XUser API: XUserGetGamertag with a component.
constexpr Guid kIidUserGamertag = {0xCEF4FAC0u, 0x7676u, 0x4A94u, {0xA1, 0x19, 0x4C, 0x43, 0xF9, 0xEB, 0x5B, 0x74}};
// XGameSave.
constexpr Guid kApiGameSave = {0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}};
// XSystemGetConsoleId, XSystemGetXboxLiveSandboxId.
constexpr Guid kApiSystem = {0xE349BD1Au, 0xFC20u, 0x4E40u, {0xB9, 0x9C, 0x41, 0x78, 0xCC, 0x6B, 0x40, 0x9F}};
// Interface ids a title built against a later GDK asks for instead of the
// API's own id. Its calls (XGame slot 3; XUser slots 3, 4, 7, 8, 9, 11, 23,
// 24, 25, 34) pass the same arguments as the main tables take.
constexpr Guid kIidGameLater = {0x2549F142u, 0x6419u, 0x4A06u, {0x97, 0xB5, 0x93, 0x1A, 0xAB, 0x7C, 0x2F, 0x34}};
constexpr Guid kIidUserLater = {0x079415E3u, 0x6727u, 0x437Fu, {0x8E, 0x9D, 0x8F, 0x8F, 0x9B, 0x24, 0x39, 0xF7}};
// For these the console object answers both ids with one table.
constexpr Guid kIidGameSaveLater = {0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6E, 0xE2}};
constexpr Guid kIidGameSave2510 = {0x1BFFF3AFu, 0xF14Au, 0x40A3u, {0x8E, 0x35, 0x9A, 0xDA, 0x90, 0x65, 0x93, 0xF9}};
constexpr Guid kIidSystemLater = {0x67CE4BFCu, 0xB1D1u, 0x4AC7u, {0xBC, 0x3A, 0xCB, 0x92, 0x19, 0xA9, 0x7A, 0x85}};
constexpr Guid kIidSystem2510 = {0x1861CF2Eu, 0xE18Bu, 0x4834u, {0xA9, 0xF5, 0xB4, 0xA4, 0xE6, 0xEF, 0xB4, 0xCF}};
// XSystemGetAnalyticsInfo (observed in Microsoft.GDK.PC 2510.5.6322).
constexpr Guid kApiAnalytics = {0xB884675Du, 0xB738u, 0x4A9Cu, {0x81, 0x5D, 0x9A, 0x9A, 0x1E, 0x0C, 0x6C, 0x9B}};
// XGameRuntimeIsFeatureAvailable.
constexpr Guid kApiFeature = {0x8836FE87u, 0xEDB9u, 0x4FE3u, {0x8D, 0xAD, 0x05, 0xF0, 0xD2, 0xCD, 0x5B, 0x40}};
// XGameEventWrite.
constexpr Guid kApiGameEvent = {0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5, 0xCA, 0xED, 0xF0, 0x54, 0x96, 0x0B, 0x4D}};
// XPackage, and the latest of the interface ids its console object answers.
constexpr Guid kApiPackage = {0xAF406016u, 0xE850u, 0x4AA8u, {0xA8, 0x8D, 0x2F, 0x3D, 0xCB, 0x9D, 0xAC, 0x7E}};
constexpr Guid kIidPackageLater = {0xF92D8712u, 0x2B27u, 0x4D8Au, {0xBF, 0x01, 0x11, 0xA6, 0xF8, 0xE3, 0xEB, 0x42}};
constexpr Guid kIidPackage2025 = {0xA8C4234Eu, 0x7F1Bu, 0x4D3Au, {0x9C, 0x81, 0x5E, 0x2B, 0x7D, 0x90, 0xA1, 0x14}};
constexpr Guid kIidPackage2510 = {0xE2A4734Bu, 0x2F4Au, 0x456Du, {0xAA, 0x8F, 0xD0, 0x65, 0xE0, 0x4F, 0xB2, 0x09}};
// GDK 2022-2026 XUser later IID & XStore API/IIDs (synthetic + official GDK 2510.5.6322).
constexpr Guid kIidUser2025 = {0x8B3F21C4u, 0x5D9Eu, 0x42A1u, {0x97, 0xC8, 0x1E, 0x4F, 0xA8, 0x02, 0xB6, 0x3D}};
constexpr Guid kApiStore = {0xCE8E7A26u, 0x2A0Eu, 0x4982u, {0xA2, 0x37, 0x71, 0xF9, 0x67, 0xB0, 0x5C, 0x3A}};
constexpr Guid kIidStore2025 = {0xD4B92E11u, 0x5C84u, 0x49A2u, {0x8E, 0x17, 0x63, 0xF0, 0xB8, 0xC9, 0x12, 0x45}};
constexpr Guid kApiStoreReal = {0x0DD112ACu, 0x7C24u, 0x448Cu, {0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C}};
constexpr Guid kIidStore2510 = {0xB09D803Cu, 0x2414u, 0x4A05u, {0x82, 0xC6, 0x66, 0xDF, 0xDC, 0x9E, 0x9A, 0x44}};
// Official GDK 2510 XDisplay HDR ({03F0FE74} / {35F07670}), XDisplay Timeout Deferral ({1B339674}),
// and XPersistentLocalStorage / PLS ({F4FAF4D4} / {41A4E10C}).
constexpr Guid kApiDisplayHdr = {0x03F0FE74u, 0xFDD9u, 0x4E5Cu, {0xB6, 0x30, 0xF9, 0x33, 0x9C, 0x47, 0xAC, 0xC5}};
constexpr Guid kIidDisplayHdr2510 = {0x35F07670u, 0x706Eu, 0x4BFBu, {0x94, 0x76, 0x09, 0x07, 0x98, 0xC5, 0xEB, 0xF3}};
constexpr Guid kApiDisplayTimeout = {0x1B339674u, 0x328Du, 0x4283u, {0xA2, 0x00, 0x31, 0x71, 0xF1, 0x8D, 0x36, 0x39}};
constexpr Guid kApiPls = {0xF4FAF4D4u, 0x2D04u, 0x4FCEu, {0xB3, 0xE0, 0x47, 0x4A, 0x71, 0x3A, 0x3E, 0x84}};
constexpr Guid kIidPls2510 = {0x41A4E10Cu, 0x5A7Eu, 0x41D9u, {0x8C, 0x37, 0x37, 0xBD, 0xE6, 0x2A, 0x07, 0xD6}};

constexpr XGameResult kNoInterface = static_cast<XGameResult>(0x80004002u);
constexpr XGameResult kNotImplemented = static_cast<XGameResult>(0x80004001u);
constexpr XGameResult kInsufficientBuffer = static_cast<XGameResult>(0x8007007Au);
constexpr size_t kConsoleIdBytes = 39;      // XSystemConsoleIdBytes
constexpr size_t kSandboxIdMaxBytes = 16;   // XSystemXboxLiveSandboxIdMaxBytes

bool Equal(const void* a, const Guid& b) { return a && std::memcmp(a, &b, sizeof(Guid)) == 0; }

bool Trace() {
    const char* v = std::getenv("SCARLETTX_TRACE");
    return v && v[0] != '\0' && v[0] != '0';
}

using Slot = void (*)();

struct Table {
    const Slot* vtbl;
    std::atomic<uint32_t> refs{1};
    Guid api;
    Guid iid;
};

// The vtable for interface `riid` of API `api_id`, or nullptr.
const Slot* FindVtbl(const void* api_id, const void* riid, Guid* iid);
void* NewTable(const Slot* vtbl, const void* api_id, const Guid& iid);

XGameResult SCARLETTX_GDK_CALLBACK TableQueryInterface(void* self, const void* riid, void** out) {
    if (!out) return XGR_E_POINTER;
    *out = nullptr;
    if (!riid) return XGR_E_POINTER;
    auto* table = static_cast<Table*>(self);
    if (Equal(riid, table->iid) || Equal(riid, kIidIUnknown)) {
        table->refs.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return XGR_S_OK;
    }
    // Another interface of the same API is a separate table object.
    Guid iid{};
    const Slot* vtbl = FindVtbl(&table->api, riid, &iid);
    if (!vtbl) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XGR::TableQI", riid, SCARLETTX_CALLER_RIP());
        return kNoInterface;
    }
    *out = NewTable(vtbl, &table->api, iid);
    return *out ? XGR_S_OK : XGR_E_OUTOFMEMORY;
}

uint32_t SCARLETTX_GDK_CALLBACK TableAddRef(void* self) {
    return static_cast<Table*>(self)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_GDK_CALLBACK TableRelease(void* self) {
    auto* table = static_cast<Table*>(self);
    const uint32_t left = table->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete table;
    return left;
}

// {973A344E} slot 3.
XGameResult SCARLETTX_GDK_CALLBACK GameGetXboxTitleId(void* /*self*/, uint32_t* title_id) {
    const XGameResult hr = XGameGetXboxTitleId(title_id);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameGetXboxTitleId -> 0x%08X (0x%08X)\n",
                     static_cast<unsigned>(hr), title_id ? static_cast<unsigned>(*title_id) : 0u);
    }
    return hr;
}

// {E349BD1A} slot 3. The console always answers with exactly 39 bytes.
XGameResult SCARLETTX_GDK_CALLBACK SystemGetConsoleId(void* /*self*/, size_t size, char* buffer, size_t* used) {
    if (!buffer) return XGR_E_POINTER;
    if (size < kConsoleIdBytes) return kInsufficientBuffer;
    char id[kConsoleIdBytes] = {};
    size_t id_used = 0;
    const XGameResult hr = XSystemGetConsoleId(sizeof(id), id, &id_used);
    if (hr != XGR_S_OK) return hr;
    for (size_t i = std::strlen(id); i + 1 < kConsoleIdBytes; ++i) id[i] = '0';
    id[kConsoleIdBytes - 1] = '\0';
    std::memcpy(buffer, id, kConsoleIdBytes);
    if (used) *used = kConsoleIdBytes;
    return XGR_S_OK;
}

// {E349BD1A} slot 4.
XGameResult SCARLETTX_GDK_CALLBACK SystemGetXboxLiveSandboxId(void* /*self*/, size_t size, char* buffer,
                                                              size_t* used) {
    if (!buffer) return XGR_E_POINTER;
    if (size < kSandboxIdMaxBytes) return kInsufficientBuffer;
    return XSystemGetXboxLiveSandboxId(size, buffer, used);
}

// {E349BD1A} slot 5 (GDK 2510 iid {1861CF2E}): XSystemGetAppSpecificDeviceId.
XGameResult SCARLETTX_GDK_CALLBACK SystemGetAppSpecificDeviceId(void* /*self*/, size_t size, char* buffer,
                                                                size_t* used) {
    return XSystemGetAppSpecificDeviceId(size, buffer, used);
}

void SCARLETTX_GDK_CALLBACK SystemAllowFullDownloadBandwidth(void* /*self*/, bool enable) {
    XSystemAllowFullDownloadBandwidth(enable);
}

bool SCARLETTX_GDK_CALLBACK SystemIsHandleValidSlot(void* /*self*/, void* handle) {
    return XSystemIsHandleValid(handle);
}

XGameResult SCARLETTX_GDK_CALLBACK SystemHandleTrackSlot(void* /*self*/, void* /*cb*/, void* /*ctx*/) {
    return XGR_S_OK;
}

XGameResult LogUnimplemented(const char* name, const char* api, int slot, uintptr_t a1, uintptr_t a2, uintptr_t a3);

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK SystemUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented("XSystem", "E349BD1A", Index, a1, a2, a3);
}

const Slot kGameVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&GameGetXboxTitleId),
};

const Slot kSystemVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&SystemGetConsoleId),
    reinterpret_cast<Slot>(&SystemGetXboxLiveSandboxId),
    reinterpret_cast<Slot>(&SystemGetAppSpecificDeviceId),
    reinterpret_cast<Slot>(&SystemHandleTrackSlot),
    reinterpret_cast<Slot>(&SystemIsHandleValidSlot),
    reinterpret_cast<Slot>(&SystemAllowFullDownloadBandwidth),
    reinterpret_cast<Slot>(&SystemUnimplemented<9>),
    reinterpret_cast<Slot>(&SystemUnimplemented<10>),
    reinterpret_cast<Slot>(&SystemUnimplemented<11>),
};

// {B884675D} slot 3: XSystemGetAnalyticsInfo (x64 struct-by-value sret in RDX, returned in RAX).
XSystemAnalyticsInfo* SCARLETTX_GDK_CALLBACK AnalyticsGetInfo(void* /*self*/, XSystemAnalyticsInfo* out_sret) {
    if (!out_sret) return nullptr;
    *out_sret = XSystemGetAnalyticsInfo();
    const char* lockhart = std::getenv("SCARLETTX_LOCKHART");
    if (lockhart && lockhart[0] == '1') {
        std::memset(out_sret->form, 0, sizeof(out_sret->form));
        std::strncpy(out_sret->form, "Xbox Series S", sizeof(out_sret->form) - 1);
    }
    return out_sret;
}

const Slot kAnalyticsVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&AnalyticsGetInfo),
};

// {01ACD177}: XUser, in the declaration order of the public XUser.h. The
// console table was matched against that order at slots 5, 6, 9, 13, 22, 30,
// 32, 33 and 34; slots without an implementation log their name and fail.
constexpr int kUserSlots = 37;
constexpr const char* kUserSlotNames[kUserSlots] = {
    "QueryInterface", "AddRef", "Release",
    "XUserDuplicateHandle", "XUserCloseHandle", "XUserCompare", "XUserGetMaxUsers",
    "XUserAddAsync", "XUserAddResult", "XUserGetLocalId", "XUserFindUserByLocalId",
    "XUserGetId", "XUserFindUserById", "XUserGetIsGuest", "XUserGetState",
    "XUserGetGamertag", "XUserGetGamerPictureAsync", "XUserGetGamerPictureResultSize",
    "XUserGetGamerPictureResult", "XUserGetAgeGroup", "XUserCheckPrivilege",
    "XUserResolvePrivilegeWithUiAsync", "XUserResolvePrivilegeWithUiResult",
    "XUserGetTokenAndSignatureAsync", "XUserGetTokenAndSignatureResultSize",
    "XUserGetTokenAndSignatureResult", "XUserGetTokenAndSignatureUtf16Async",
    "XUserGetTokenAndSignatureUtf16ResultSize", "XUserGetTokenAndSignatureUtf16Result",
    "XUserResolveIssueWithUiAsync", "XUserResolveIssueWithUiResult",
    "XUserResolveIssueWithUiUtf16Async", "XUserResolveIssueWithUiUtf16Result",
    "XUserRegisterForChangeEvent", "XUserUnregisterForChangeEvent",
    "XUserGetSignOutDeferral", "XUserCloseSignOutDeferralHandle",
};

XGameResult LogUnimplemented(const char* name, const char* api, int slot, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] %s (ApiImpl{%s} slot %d)(0x%llx, 0x%llx, 0x%llx) -> E_NOTIMPL\n",
                 name, api, slot, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3));
    std::fflush(stderr);
    scarlettx::FlightRecorder::Instance().RecordVtableSlotMiss(
        name ? name : "XGR::ApiImpl", slot, static_cast<uint64_t>(a1), 0ull);
    return kNotImplemented;
}

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK UserUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented(kUserSlotNames[Index], "01ACD177", Index, a1, a2, a3);
}

XGameResult SCARLETTX_GDK_CALLBACK UserDuplicateHandle(void*, XUserHandle user, XUserHandle* duplicate) {
    return XUserDuplicateHandle(user, duplicate);
}

void SCARLETTX_GDK_CALLBACK UserCloseHandle(void*, XUserHandle user) { XUserCloseHandle(user); }

// Zero when both handles refer to the same user.
int32_t SCARLETTX_GDK_CALLBACK UserCompare(void*, XUserHandle a, XUserHandle b) {
    if (a == b) return 0;
    uint64_t id_a = 0;
    uint64_t id_b = 0;
    const bool has_a = a && XUserGetId(a, &id_a) == XGR_S_OK;
    const bool has_b = b && XUserGetId(b, &id_b) == XGR_S_OK;
    if (has_a != has_b) return has_a ? 1 : -1;
    return id_a == id_b ? 0 : (id_a < id_b ? -1 : 1);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetMaxUsers(void*, uint32_t* max_users) { return XUserGetMaxUsers(max_users); }

XGameResult SCARLETTX_GDK_CALLBACK UserAddAsync(void*, XUserAddOptions options, XAsyncBlock* async) {
    const XGameResult hr = XUserAddAsync(options, async);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XUserAddAsync(options=%u, async=%p) -> 0x%08X\n",
                     static_cast<unsigned>(options), static_cast<void*>(async), static_cast<unsigned>(hr));
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK UserAddResult(void*, XAsyncBlock* async, XUserHandle* user) {
    const XGameResult hr = XUserAddResult(async, user);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XUserAddResult(async=%p) -> 0x%08X\n",
                     static_cast<void*>(async), static_cast<unsigned>(hr));
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetLocalId(void*, XUserHandle user, XUserLocalId* local_id) {
    return XUserGetLocalId(user, local_id);
}

XGameResult SCARLETTX_GDK_CALLBACK UserFindUserByLocalId(void*, XUserLocalId local_id, XUserHandle* handle) {
    return XUserFindUserByLocalId(local_id, handle);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetId(void*, XUserHandle user, uint64_t* id) { return XUserGetId(user, id); }

XGameResult SCARLETTX_GDK_CALLBACK UserFindUserById(void*, uint64_t user_id, XUserHandle* handle) {
    if (user_id == 0 && !handle) return kNotImplemented;
    return XUserFindUserById(user_id, handle);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureAsync(
    void*,
    XUserHandle user,
    XUserGamerPictureSize picture_size,
    XAsyncBlock* async) {
    return XUserGetGamerPictureAsync(user, picture_size, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureResultSize(void*, XAsyncBlock* async, size_t* buffer_size) {
    return XUserGetGamerPictureResultSize(async, buffer_size);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureResult(
    void*,
    XAsyncBlock* async,
    size_t buffer_size,
    void* buffer,
    size_t* buffer_used) {
    return XUserGetGamerPictureResult(async, buffer_size, buffer, buffer_used);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetIsGuest(void*, XUserHandle user, bool* is_guest) {
    return XUserGetIsGuest(user, is_guest);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetState(void*, XUserHandle user, XUserState* state) {
    return XUserGetState(user, state);
}

// {CEF4FAC0} slot 3. The console checks the buffer against the component's
// maximum size, whatever the actual gamertag length.
XGameResult SCARLETTX_GDK_CALLBACK UserGetGamertagComponent(void*, XUserHandle user, uint32_t component, size_t size,
                                                            char* gamertag, size_t* used) {
    constexpr size_t kMaxBytes[] = {16, 97, 15, 101};  // classic, modern, modern suffix, unique modern
    if (used) *used = 0;
    if (component > 3) return XGR_E_INVALIDARG;
    if (size < kMaxBytes[component]) return kInsufficientBuffer;
    return XUserGetGamertag(user, static_cast<XUserGamertagComponent>(component), size, gamertag, used);
}

// {01ACD177} slot 15: the original signature without a component; the
// console forwards it to the method above with the classic component.
XGameResult SCARLETTX_GDK_CALLBACK UserGetGamertag(void* self, XUserHandle user, size_t size, char* gamertag,
                                                   size_t* used) {
    return UserGetGamertagComponent(self, user, 0, size, gamertag, used);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetAgeGroup(void*, XUserHandle user, XUserAgeGroup* age_group) {
    return XUserGetAgeGroup(user, age_group);
}

XGameResult SCARLETTX_GDK_CALLBACK UserCheckPrivilege(void*, XUserHandle user, XUserPrivilegeOptions options,
                                                      XUserPrivilege privilege, bool* has_privilege,
                                                      XUserPrivilegeDenyReason* reason) {
    return XUserCheckPrivilege(user, options, privilege, has_privilege, reason);
}

// The signed-in user never changes in the HLE, so registrations only hand
// out a token; the callback is never invoked.
std::atomic<uint64_t> g_user_change_tokens{0};

XGameResult SCARLETTX_GDK_CALLBACK UserRegisterForChangeEvent(void*, XTaskQueueHandle /*queue*/, void* /*context*/,
                                                              void* callback, XTaskQueueRegistrationToken* token) {
    if (!callback || !token) return XGR_E_INVALIDARG;
    std::memset(token, 0, sizeof(*token));
    const uint64_t id = g_user_change_tokens.fetch_add(1, std::memory_order_relaxed) + 1;
    std::memcpy(token, &id, sizeof(id) < sizeof(*token) ? sizeof(id) : sizeof(*token));
    return XGR_S_OK;
}

bool SCARLETTX_GDK_CALLBACK UserUnregisterForChangeEvent(void*, XTaskQueueRegistrationToken token, bool /*wait*/) {
    uint64_t id = 0;
    std::memcpy(&id, &token, sizeof(id) < sizeof(token) ? sizeof(id) : sizeof(token));
    return id != 0 && id <= g_user_change_tokens.load(std::memory_order_relaxed);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureAsync(
    void* /*self*/,
    XUserHandle user,
    XUserGetTokenAndSignatureOptions options,
    const char* method,
    const char* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureHttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async) {
    const XGameResult hr = XUserGetTokenAndSignatureAsync(
        user, options, method, url, headerCount, headers, bodySize, bodyBuffer, async);
    if (Trace()) {
        std::fprintf(stderr,
                     "[ScarlettX][xgameruntime] XUserGetTokenAndSignatureAsync(user=%p, url=%s, async=%p) -> 0x%08X\n",
                     static_cast<void*>(user), url ? url : "(null)", static_cast<void*>(async), static_cast<unsigned>(hr));
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureResultSize(
    void* /*self*/,
    XAsyncBlock* async,
    size_t* bufferSize) {
    return XUserGetTokenAndSignatureResultSize(async, bufferSize);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureResult(
    void* /*self*/,
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureData** ptrToBuffer,
    size_t* bufferUsed) {
    return XUserGetTokenAndSignatureResult(async, bufferSize, buffer, ptrToBuffer, bufferUsed);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolvePrivilegeWithUiAsync(
    void* /*self*/,
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    XAsyncBlock* async) {
    return XUserResolvePrivilegeWithUiAsync(user, options, privilege, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolvePrivilegeWithUiResult(void* /*self*/, XAsyncBlock* async) {
    return XUserResolvePrivilegeWithUiResult(async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureUtf16Async(
    void* /*self*/,
    XUserHandle user,
    XUserGetTokenAndSignatureOptions options,
    const wchar_t* method,
    const wchar_t* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureUtf16HttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async) {
    return XUserGetTokenAndSignatureUtf16Async(
        user, options, method, url, headerCount, headers, bodySize, bodyBuffer, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureUtf16ResultSize(
    void* /*self*/,
    XAsyncBlock* async,
    size_t* bufferSize) {
    return XUserGetTokenAndSignatureUtf16ResultSize(async, bufferSize);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureUtf16Result(
    void* /*self*/,
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureUtf16Data** ptrToBuffer,
    size_t* bufferUsed) {
    return XUserGetTokenAndSignatureUtf16Result(async, bufferSize, buffer, ptrToBuffer, bufferUsed);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolveIssueWithUiAsync(
    void* /*self*/,
    XUserHandle user,
    const char* url,
    XAsyncBlock* async) {
    return XUserResolveIssueWithUiAsync(user, url, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolveIssueWithUiResult(void* /*self*/, XAsyncBlock* async) {
    return XUserResolveIssueWithUiResult(async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolveIssueWithUiUtf16Async(
    void* /*self*/,
    XUserHandle user,
    const wchar_t* url,
    XAsyncBlock* async) {
    return XUserResolveIssueWithUiUtf16Async(user, url, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolveIssueWithUiUtf16Result(void* /*self*/, XAsyncBlock* async) {
    return XUserResolveIssueWithUiUtf16Result(async);
}

template <size_t... I>
std::array<Slot, kUserSlots> MakeUserVtbl(std::index_sequence<I...>) {
    std::array<Slot, kUserSlots> vtbl = {reinterpret_cast<Slot>(&UserUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&UserDuplicateHandle);
    vtbl[4] = reinterpret_cast<Slot>(&UserCloseHandle);
    vtbl[5] = reinterpret_cast<Slot>(&UserCompare);
    vtbl[6] = reinterpret_cast<Slot>(&UserGetMaxUsers);
    vtbl[7] = reinterpret_cast<Slot>(&UserAddAsync);
    vtbl[8] = reinterpret_cast<Slot>(&UserAddResult);
    vtbl[9] = reinterpret_cast<Slot>(&UserGetLocalId);
    vtbl[10] = reinterpret_cast<Slot>(&UserFindUserByLocalId);
    vtbl[11] = reinterpret_cast<Slot>(&UserGetId);
    vtbl[12] = reinterpret_cast<Slot>(&UserFindUserById);
    vtbl[13] = reinterpret_cast<Slot>(&UserGetIsGuest);
    vtbl[14] = reinterpret_cast<Slot>(&UserGetState);
    vtbl[15] = reinterpret_cast<Slot>(&UserGetGamertag);
    vtbl[16] = reinterpret_cast<Slot>(&UserGetGamerPictureAsync);
    vtbl[17] = reinterpret_cast<Slot>(&UserGetGamerPictureResultSize);
    vtbl[18] = reinterpret_cast<Slot>(&UserGetGamerPictureResult);
    vtbl[19] = reinterpret_cast<Slot>(&UserGetAgeGroup);
    vtbl[20] = reinterpret_cast<Slot>(&UserCheckPrivilege);
    vtbl[21] = reinterpret_cast<Slot>(&UserResolvePrivilegeWithUiAsync);
    vtbl[22] = reinterpret_cast<Slot>(&UserResolvePrivilegeWithUiResult);
    vtbl[23] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureAsync);
    vtbl[24] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureResultSize);
    vtbl[25] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureResult);
    vtbl[26] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureUtf16Async);
    vtbl[27] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureUtf16ResultSize);
    vtbl[28] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureUtf16Result);
    vtbl[29] = reinterpret_cast<Slot>(&UserResolveIssueWithUiAsync);
    vtbl[30] = reinterpret_cast<Slot>(&UserResolveIssueWithUiResult);
    vtbl[31] = reinterpret_cast<Slot>(&UserResolveIssueWithUiUtf16Async);
    vtbl[32] = reinterpret_cast<Slot>(&UserResolveIssueWithUiUtf16Result);
    vtbl[33] = reinterpret_cast<Slot>(&UserRegisterForChangeEvent);
    vtbl[34] = reinterpret_cast<Slot>(&UserUnregisterForChangeEvent);
    return vtbl;
}
const std::array<Slot, kUserSlots> kUserVtbl = MakeUserVtbl(std::make_index_sequence<kUserSlots>{});

const Slot kUserGamertagVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&UserGetGamertagComponent),
};

// {704C3F58}: XGameSave, in the declaration order of the public XGameSave.h.
// The console code names slots 3, 7, 13, 14, 15, 18, 19 and 20 in its error
// reports, and the order of the others was matched by argument shape.
// Slots 30-32 are XGameSaveFiles.h in the same way: the console names slot
// 32, and a title's calls to 30 and 31 pass the Async/Result arguments.
constexpr int kGameSaveSlots = 35;
constexpr const char* kGameSaveSlotNames[kGameSaveSlots] = {
    "QueryInterface", "AddRef", "Release",
    "XGameSaveInitializeProvider", "XGameSaveInitializeProviderAsync", "XGameSaveInitializeProviderResult",
    "XGameSaveCloseProvider", "XGameSaveGetRemainingQuota", "XGameSaveGetRemainingQuotaAsync",
    "XGameSaveGetRemainingQuotaResult", "XGameSaveDeleteContainer", "XGameSaveDeleteContainerAsync",
    "XGameSaveDeleteContainerResult", "XGameSaveGetContainerInfo", "XGameSaveEnumerateContainerInfo",
    "XGameSaveEnumerateContainerInfoByName", "XGameSaveCreateContainer", "XGameSaveCloseContainer",
    "XGameSaveEnumerateBlobInfo", "XGameSaveEnumerateBlobInfoByName", "XGameSaveReadBlobData",
    "XGameSaveReadBlobDataAsync", "XGameSaveReadBlobDataResult", "XGameSaveCreateUpdate",
    "XGameSaveCloseUpdate", "XGameSaveSubmitBlobWrite", "XGameSaveSubmitBlobDelete",
    "XGameSaveSubmitUpdate", "XGameSaveSubmitUpdateAsync", "XGameSaveSubmitUpdateResult",
    "XGameSaveFilesGetFolderWithUiAsync", "XGameSaveFilesGetFolderWithUiResult",
    "XGameSaveFilesGetRemainingQuota", "XGameSaveFilesGetRemainingQuotaAsync",
    "XGameSaveFilesGetRemainingQuotaResult",
};

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK GameSaveUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented(kGameSaveSlotNames[Index], "704C3F58", Index, a1, a2, a3);
}

void TraceGameSave(const char* name, XGameResult hr) {
    if (!Trace()) return;
    std::fprintf(stderr, "[ScarlettX][xgameruntime] %s -> 0x%08X\n", name, static_cast<unsigned>(hr));
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProvider(void*, XUserHandle user, const char* configuration_id,
                                                          bool sync_on_demand, XGameSaveProviderHandle* provider) {
    const XGameResult hr = XGameSaveInitializeProvider(user, configuration_id, sync_on_demand, provider);
    TraceGameSave("XGameSaveInitializeProvider", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProviderAsync(void*, XUserHandle user, const char* configuration_id,
                                                               bool sync_on_demand, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveInitializeProviderAsync(user, configuration_id, sync_on_demand, async);
    TraceGameSave("XGameSaveInitializeProviderAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProviderResult(void*, XAsyncBlock* async,
                                                                XGameSaveProviderHandle* provider) {
    return XGameSaveInitializeProviderResult(async, provider);
}

void SCARLETTX_GDK_CALLBACK SaveCloseProvider(void*, XGameSaveProviderHandle provider) {
    XGameSaveCloseProvider(provider);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveGetRemainingQuota(void*, XGameSaveProviderHandle provider, int64_t* quota) {
    return XGameSaveGetRemainingQuota(provider, quota);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveGetRemainingQuotaAsync(void*, XGameSaveProviderHandle provider,
                                                              XAsyncBlock* async) {
    return XGameSaveGetRemainingQuotaAsync(provider, async);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveGetRemainingQuotaResult(void*, XAsyncBlock* async, int64_t* quota) {
    return XGameSaveGetRemainingQuotaResult(async, quota);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveDeleteContainer(void*, XGameSaveProviderHandle provider, const char* name) {
    const XGameResult hr = XGameSaveDeleteContainer(provider, name);
    TraceGameSave("XGameSaveDeleteContainer", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveDeleteContainerAsync(void*, XGameSaveProviderHandle provider, const char* name,
                                                            XAsyncBlock* async) {
    if (!provider && !name && !async) return kNotImplemented;
    const XGameResult hr = XGameSaveDeleteContainerAsync(provider, name, async);
    TraceGameSave("XGameSaveDeleteContainerAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveDeleteContainerResult(void*, XAsyncBlock* async) {
    return XGameSaveDeleteContainerResult(async);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateContainerInfo(void*, XGameSaveProviderHandle provider, void* context,
                                                              XGameSaveContainerInfoCallback* callback) {
    const XGameResult hr = XGameSaveEnumerateContainerInfo(provider, context, callback);
    TraceGameSave("XGameSaveEnumerateContainerInfo", hr);
    return hr;
}

// Slots 13, 15 and 19 filter the full enumeration by name or name prefix. The
// enumeration reports stored names, so the filter compares in that form.
template <typename Info, typename Callback>
struct SaveNameFilter {
    std::string name;
    bool exact;
    void* context;
    Callback* callback;

    static bool SCARLETTX_GDK_CALLBACK Thunk(const Info* info, void* filter_ptr) {
        auto* filter = static_cast<SaveNameFilter*>(filter_ptr);
        const char* name = info && info->name ? info->name : "";
        const bool match = filter->exact ? filter->name == name
                                         : std::strncmp(name, filter->name.c_str(), filter->name.size()) == 0;
        return match ? filter->callback(info, filter->context) : true;
    }
};
using SaveContainerFilter = SaveNameFilter<XGameSaveContainerInfo, XGameSaveContainerInfoCallback>;
using SaveBlobFilter = SaveNameFilter<XGameSaveBlobInfo, XGameSaveBlobInfoCallback>;

// An empty prefix matches every name.
std::string StoredSavePrefix(const char* prefix) {
    return prefix && *prefix ? StoredSaveName(prefix) : std::string();
}

XGameResult SCARLETTX_GDK_CALLBACK SaveGetContainerInfo(void*, XGameSaveProviderHandle provider, const char* name,
                                                        void* context, XGameSaveContainerInfoCallback* callback) {
    if (!name || !callback) return XGR_E_INVALIDARG;
    SaveContainerFilter filter{StoredSaveName(name), true, context, callback};
    return XGameSaveEnumerateContainerInfo(provider, &filter, &SaveContainerFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateContainerInfoByName(void*, XGameSaveProviderHandle provider,
                                                                    const char* prefix, void* context,
                                                                    XGameSaveContainerInfoCallback* callback) {
    if (!callback) return XGR_E_INVALIDARG;
    SaveContainerFilter filter{StoredSavePrefix(prefix), false, context, callback};
    return XGameSaveEnumerateContainerInfo(provider, &filter, &SaveContainerFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveCreateContainer(void*, XGameSaveProviderHandle provider, const char* name,
                                                       XGameSaveContainerHandle* container) {
    const XGameResult hr = XGameSaveCreateContainer(provider, name, container);
    TraceGameSave("XGameSaveCreateContainer", hr);
    return hr;
}

void SCARLETTX_GDK_CALLBACK SaveCloseContainer(void*, XGameSaveContainerHandle container) {
    XGameSaveCloseContainer(container);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateBlobInfo(void*, XGameSaveContainerHandle container, void* context,
                                                         XGameSaveBlobInfoCallback* callback) {
    return XGameSaveEnumerateBlobInfo(container, context, callback);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateBlobInfoByName(void*, XGameSaveContainerHandle container,
                                                               const char* prefix, void* context,
                                                               XGameSaveBlobInfoCallback* callback) {
    if (!callback) return XGR_E_INVALIDARG;
    SaveBlobFilter filter{StoredSavePrefix(prefix), false, context, callback};
    return XGameSaveEnumerateBlobInfo(container, &filter, &SaveBlobFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobData(void*, XGameSaveContainerHandle container, const char** names,
                                                    uint32_t* count, size_t blobs_size, XGameSaveBlob* blobs) {
    const XGameResult hr = XGameSaveReadBlobData(container, names, count, blobs_size, blobs);
    TraceGameSave("XGameSaveReadBlobData", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobDataAsync(void*, XGameSaveContainerHandle container,
                                                         const char** names, uint32_t count, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveReadBlobDataAsync(container, names, count, async);
    TraceGameSave("XGameSaveReadBlobDataAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobDataResult(void*, XAsyncBlock* async, size_t blobs_size,
                                                          XGameSaveBlob* blobs, uint32_t* count) {
    return XGameSaveReadBlobDataResult(async, blobs_size, blobs, count);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveCreateUpdate(void*, XGameSaveContainerHandle container,
                                                    const char* display_name, XGameSaveUpdateHandle* update) {
    return XGameSaveCreateUpdate(container, display_name, update);
}

void SCARLETTX_GDK_CALLBACK SaveCloseUpdate(void*, XGameSaveUpdateHandle update) { XGameSaveCloseUpdate(update); }

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitBlobWrite(void*, XGameSaveUpdateHandle update, const char* blob_name,
                                                       const uint8_t* data, size_t byte_count) {
    return XGameSaveSubmitBlobWrite(update, blob_name, data, byte_count);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitBlobDelete(void*, XGameSaveUpdateHandle update, const char* blob_name) {
    return XGameSaveSubmitBlobDelete(update, blob_name);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdate(void*, XGameSaveUpdateHandle update) {
    const XGameResult hr = XGameSaveSubmitUpdate(update);
    TraceGameSave("XGameSaveSubmitUpdate", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdateAsync(void*, XGameSaveUpdateHandle update, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveSubmitUpdateAsync(update, async);
    TraceGameSave("XGameSaveSubmitUpdateAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdateResult(void*, XAsyncBlock* async) {
    return XGameSaveSubmitUpdateResult(async);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveFilesGetFolderWithUiAsync(void*, XUserHandle user, const char* configuration_id,
                                                                 XAsyncBlock* async) {
    const XGameResult hr = XGameSaveFilesGetFolderWithUiAsync(user, configuration_id, async);
    TraceGameSave("XGameSaveFilesGetFolderWithUiAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveFilesGetFolderWithUiResult(void*, XAsyncBlock* async, size_t folder_size,
                                                                  char* folder) {
    const XGameResult hr = XGameSaveFilesGetFolderWithUiResult(async, folder_size, folder);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameSaveFilesGetFolderWithUiResult -> 0x%08X (%s)\n",
                     static_cast<unsigned>(hr), hr == XGR_S_OK ? folder : "");
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveFilesGetRemainingQuota(void*, XUserHandle user, const char* configuration_id,
                                                              int64_t* quota) {
    return XGameSaveFilesGetRemainingQuota(user, configuration_id, quota);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveFilesGetRemainingQuotaAsync(void*, XUserHandle user,
                                                                   const char* configuration_id, XAsyncBlock* async) {
    return XGameSaveFilesGetRemainingQuotaAsync(user, configuration_id, async);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveFilesGetRemainingQuotaResult(void*, XAsyncBlock* async, int64_t* quota) {
    return XGameSaveFilesGetRemainingQuotaResult(async, quota);
}

template <size_t... I>
std::array<Slot, kGameSaveSlots> MakeGameSaveVtbl(std::index_sequence<I...>) {
    std::array<Slot, kGameSaveSlots> vtbl = {reinterpret_cast<Slot>(&GameSaveUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&SaveInitializeProvider);
    vtbl[4] = reinterpret_cast<Slot>(&SaveInitializeProviderAsync);
    vtbl[5] = reinterpret_cast<Slot>(&SaveInitializeProviderResult);
    vtbl[6] = reinterpret_cast<Slot>(&SaveCloseProvider);
    vtbl[7] = reinterpret_cast<Slot>(&SaveGetRemainingQuota);
    vtbl[8] = reinterpret_cast<Slot>(&SaveGetRemainingQuotaAsync);
    vtbl[9] = reinterpret_cast<Slot>(&SaveGetRemainingQuotaResult);
    vtbl[10] = reinterpret_cast<Slot>(&SaveDeleteContainer);
    vtbl[11] = reinterpret_cast<Slot>(&SaveDeleteContainerAsync);
    vtbl[12] = reinterpret_cast<Slot>(&SaveDeleteContainerResult);
    vtbl[13] = reinterpret_cast<Slot>(&SaveGetContainerInfo);
    vtbl[14] = reinterpret_cast<Slot>(&SaveEnumerateContainerInfo);
    vtbl[15] = reinterpret_cast<Slot>(&SaveEnumerateContainerInfoByName);
    vtbl[16] = reinterpret_cast<Slot>(&SaveCreateContainer);
    vtbl[17] = reinterpret_cast<Slot>(&SaveCloseContainer);
    vtbl[18] = reinterpret_cast<Slot>(&SaveEnumerateBlobInfo);
    vtbl[19] = reinterpret_cast<Slot>(&SaveEnumerateBlobInfoByName);
    vtbl[20] = reinterpret_cast<Slot>(&SaveReadBlobData);
    vtbl[21] = reinterpret_cast<Slot>(&SaveReadBlobDataAsync);
    vtbl[22] = reinterpret_cast<Slot>(&SaveReadBlobDataResult);
    vtbl[23] = reinterpret_cast<Slot>(&SaveCreateUpdate);
    vtbl[24] = reinterpret_cast<Slot>(&SaveCloseUpdate);
    vtbl[25] = reinterpret_cast<Slot>(&SaveSubmitBlobWrite);
    vtbl[26] = reinterpret_cast<Slot>(&SaveSubmitBlobDelete);
    vtbl[27] = reinterpret_cast<Slot>(&SaveSubmitUpdate);
    vtbl[28] = reinterpret_cast<Slot>(&SaveSubmitUpdateAsync);
    vtbl[29] = reinterpret_cast<Slot>(&SaveSubmitUpdateResult);
    vtbl[30] = reinterpret_cast<Slot>(&SaveFilesGetFolderWithUiAsync);
    vtbl[31] = reinterpret_cast<Slot>(&SaveFilesGetFolderWithUiResult);
    vtbl[32] = reinterpret_cast<Slot>(&SaveFilesGetRemainingQuota);
    vtbl[33] = reinterpret_cast<Slot>(&SaveFilesGetRemainingQuotaAsync);
    vtbl[34] = reinterpret_cast<Slot>(&SaveFilesGetRemainingQuotaResult);
    return vtbl;
}
const std::array<Slot, kGameSaveSlots> kGameSaveVtbl = MakeGameSaveVtbl(std::make_index_sequence<kGameSaveSlots>{});

// {8836FE87} slot 3, the table's only method. The console knows features 0
// to 21 (the public XGameRuntimeFeature values) and has them all; 10,
// XNetworking, depends on its network stack being up, which it is here.
constexpr uint32_t kLastRuntimeFeature = 21;

bool SCARLETTX_GDK_CALLBACK FeatureIsAvailable(void* /*self*/, uint32_t feature) {
    const bool available = feature <= kLastRuntimeFeature;
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameRuntimeIsFeatureAvailable(%u) -> %d\n",
                     static_cast<unsigned>(feature), available ? 1 : 0);
    }
    return available;
}

const Slot kFeatureVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&FeatureIsAvailable),
};

// {BBFBDCC7} slot 3, the table's only method. There is no service to send
// the event to, so it is accepted and dropped.
XGameResult SCARLETTX_GDK_CALLBACK GameEventWrite(void* /*self*/, XUserHandle user, const char* /*service_config_id*/,
                                                  const char* /*play_session_id*/, const char* event_name,
                                                  const char* /*dimensions_json*/, const char* /*measurements_json*/) {
    const XGameResult hr = user && event_name ? XGR_S_OK : XGR_E_INVALIDARG;
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameEventWrite(%s) -> 0x%08X\n",
                     event_name ? event_name : "<null>", static_cast<unsigned>(hr));
    }
    return hr;
}

const Slot kGameEventVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&GameEventWrite),
};

// {AF406016}: XPackage. The console object answers seven interface ids with
// one 39-entry table. Three methods are identified, by a title's calls and
// by the console code behind the slot; in the console table 20, 28 and 35
// are one function, as are 21, 29 and 36. The rest log the call and fail.
//   11       XPackageGetUserLocale(size, buffer)
//   20/28/35 XPackageEnumeratePackages(kind, scope, context, callback)
//   21/29/36 XPackageRegisterPackageInstalled(queue, context, callback, token)
constexpr int kPackageSlots = 48;

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK PackageUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented("XPackage", "AF406016", Index, a1, a2, a3);
}

XGameResult SCARLETTX_GDK_CALLBACK PackageGetUserLocale(void* /*self*/, size_t size, char* locale) {
    const XGameResult hr = XPackageGetUserLocale(size, locale);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XPackageGetUserLocale -> 0x%08X (%s)\n",
                     static_cast<unsigned>(hr), hr == XGR_S_OK ? locale : "");
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageEnumeratePackages(void* /*self*/, uint32_t kind, uint32_t scope,
                                                            void* context, void* callback) {
    if (!callback) return XGR_E_INVALIDARG;
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XPackageEnumeratePackages(kind=%u, scope=%u)\n",
                     static_cast<unsigned>(kind), static_cast<unsigned>(scope));
    }
    return XPackageEnumeratePackages(
        static_cast<XPackageKind>(kind),
        static_cast<XPackageEnumerationScope>(scope),
        context,
        reinterpret_cast<XPackageEnumerationCallback*>(callback));
}

XGameResult SCARLETTX_GDK_CALLBACK PackageMountWithUiAsync(void* /*self*/, const char* pkg_id, XAsyncBlock* async) {
    return XPackageMountWithUiAsync(pkg_id, async);
}

XGameResult SCARLETTX_GDK_CALLBACK PackageMountWithUiResult(void* /*self*/, XAsyncBlock* async,
                                                            XPackageMountHandle* mount) {
    return XPackageMountWithUiResult(async, mount);
}

XGameResult SCARLETTX_GDK_CALLBACK PackageGetMountPathSize(void* /*self*/, XPackageMountHandle mount, size_t* sz) {
    return XPackageGetMountPathSize(mount, sz);
}

XGameResult SCARLETTX_GDK_CALLBACK PackageGetMountPath(void* /*self*/, XPackageMountHandle mount, size_t sz,
                                                       char* path) {
    return XPackageGetMountPath(mount, sz, path);
}

void SCARLETTX_GDK_CALLBACK PackageCloseMountHandle(void* /*self*/, XPackageMountHandle mount) {
    XPackageCloseMountHandle(mount);
}

// Nothing is ever installed while the title runs, so the callback is never
// invoked; the registration only hands out a token.
std::atomic<uint64_t> g_package_installed_tokens{0};

XGameResult SCARLETTX_GDK_CALLBACK PackageRegisterPackageInstalled(void* /*self*/, XTaskQueueHandle /*queue*/,
                                                                   void* /*context*/, void* callback,
                                                                   XTaskQueueRegistrationToken* token) {
    if (!callback || !token) return XGR_E_INVALIDARG;
    token->token = g_package_installed_tokens.fetch_add(1, std::memory_order_relaxed) + 1;
    return XGR_S_OK;
}

bool SCARLETTX_GDK_CALLBACK PackageUnregisterPackageInstalledSlot(void* /*self*/, uint64_t /*token*/, bool /*wait*/) {
    return true;
}

// XPackage Intelligent Delivery (slots 3..19 in XPackage.h declaration order):
struct PackageInstallMonitorObj {
    uint64_t magic = 0x504B474D4F4E3031ull;
};

XGameResult SCARLETTX_GDK_CALLBACK PackageGetCurrentProcessIdSlot(void* /*self*/, size_t size, char* buffer) {
    if (!buffer) return XGR_E_POINTER;
    if (size < 33u) return kInsufficientBuffer;
    uint32_t tid = 0x5CA00001u;
    (void)XGameGetXboxTitleId(&tid);
    std::memset(buffer, 0, size);
    std::snprintf(buffer, 33u, "SCARLETTX_PKG_00000000_%08X", static_cast<unsigned>(tid));
    return XGR_S_OK;
}

bool SCARLETTX_GDK_CALLBACK PackageIsPackagedProcessSlot(void* /*self*/) {
    return true;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageCreateInstallMonitorSlot(void* /*self*/, const char* /*pkg_id*/,
                                                                   uint32_t /*sel_count*/, const void* /*selectors*/,
                                                                   uint32_t /*interval_ms*/, XTaskQueueHandle /*queue*/,
                                                                   void** out_mon) {
    if (!out_mon) return XGR_E_POINTER;
    auto* mon = new (std::nothrow) PackageInstallMonitorObj();
    if (!mon) return XGR_E_OUTOFMEMORY;
    *out_mon = mon;
    return XGR_S_OK;
}

void SCARLETTX_GDK_CALLBACK PackageCloseInstallMonitorSlot(void* /*self*/, void* mon) {
    delete static_cast<PackageInstallMonitorObj*>(mon);
}

struct PackageInstallProgressAbi {
    uint64_t totalBytes;
    uint64_t installedBytes;
    uint64_t launchBytes;
    bool launchable;
    bool completed;
};

void SCARLETTX_GDK_CALLBACK PackageGetInstallProgressSlot(void* /*self*/, void* /*mon*/,
                                                          PackageInstallProgressAbi* out_prog) {
    if (!out_prog) return;
    constexpr uint64_t kDefaultPkgBytes = 64ull * 1024ull * 1024ull;
    out_prog->totalBytes = kDefaultPkgBytes;
    out_prog->installedBytes = kDefaultPkgBytes;
    out_prog->launchBytes = kDefaultPkgBytes;
    out_prog->launchable = true;
    out_prog->completed = true;
}

bool SCARLETTX_GDK_CALLBACK PackageUpdateInstallMonitorSlot(void* /*self*/, void* /*mon*/) {
    return false;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageRegisterInstallProgressSlot(void* /*self*/, void* /*mon*/, void* /*ctx*/,
                                                                      void* cb, XTaskQueueRegistrationToken* tok) {
    if (!cb || !tok) return XGR_E_INVALIDARG;
    tok->token = g_package_installed_tokens.fetch_add(1, std::memory_order_relaxed) + 1;
    return XGR_S_OK;
}

bool SCARLETTX_GDK_CALLBACK PackageUnregisterInstallProgressSlot(void* /*self*/, void* /*mon*/,
                                                                 uint64_t /*tok*/, bool /*wait*/) {
    return true;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageFindChunkAvailabilitySlot(void* /*self*/, const char* /*pkg_id*/,
                                                                    uint32_t /*sel_count*/, const void* /*selectors*/,
                                                                    uint32_t* out_avail) {
    if (!out_avail) return XGR_E_POINTER;
    *out_avail = 0u;  // XPackageChunkAvailability::Ready
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageNoopChunkSlot(void* /*self*/, uintptr_t, uintptr_t, uintptr_t) {
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageInstallChunksSlot(void* self, const char* pkg_id, uint32_t sel_count,
                                                            const void* selectors, uint32_t interval_ms,
                                                            bool /*suppress_ui*/, XTaskQueueHandle queue,
                                                            void** out_mon) {
    return PackageCreateInstallMonitorSlot(self, pkg_id, sel_count, selectors, interval_ms, queue, out_mon);
}

XGameResult SCARLETTX_GDK_CALLBACK SimpleNoopAsyncProvider(XAsyncOp op, const XAsyncProviderData* data) {
    if (op == XAsyncOp_DoWork) {
        XAsyncComplete(data->async, XGR_S_OK, sizeof(void*));
    }
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageInstallChunksAsyncSlot(void* /*self*/, const char* /*pkg_id*/,
                                                                 uint32_t /*sel_count*/, const void* /*selectors*/,
                                                                 uint32_t /*interval_ms*/, bool /*suppress_ui*/,
                                                                 XAsyncBlock* async) {
    if (!async) return XGR_E_INVALIDARG;
    const XGameResult hr = XAsyncBegin(async, nullptr, reinterpret_cast<const void*>(&PackageInstallChunksAsyncSlot),
                                       "XPackageInstallChunksAsync", &SimpleNoopAsyncProvider);
    return hr == XGR_S_OK ? XAsyncSchedule(async, 0) : hr;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageInstallChunksResultSlot(void* /*self*/, XAsyncBlock* async, void** out_mon) {
    if (!async || !out_mon) return XGR_E_POINTER;
    *out_mon = nullptr;
    void* dummy = nullptr;
    const XGameResult hr = XAsyncGetResult(async, reinterpret_cast<const void*>(&PackageInstallChunksAsyncSlot),
                                           sizeof(dummy), &dummy, nullptr);
    if (hr != XGR_S_OK) return hr;
    auto* mon = new (std::nothrow) PackageInstallMonitorObj();
    if (!mon) return XGR_E_OUTOFMEMORY;
    *out_mon = mon;
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK PackageEstimateDownloadSizeSlot(void* /*self*/, const char* /*pkg_id*/,
                                                                   uint32_t /*sel_count*/, const void* /*selectors*/,
                                                                   uint64_t* out_size, bool* out_confirm) {
    if (!out_size) return XGR_E_POINTER;
    *out_size = 0ull;
    if (out_confirm) *out_confirm = false;
    return XGR_S_OK;
}

template <size_t... I>
std::array<Slot, kPackageSlots> MakePackageVtbl(std::index_sequence<I...>) {
    std::array<Slot, kPackageSlots> vtbl = {reinterpret_cast<Slot>(&PackageUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&PackageGetCurrentProcessIdSlot);
    vtbl[4] = reinterpret_cast<Slot>(&PackageIsPackagedProcessSlot);
    vtbl[5] = reinterpret_cast<Slot>(&PackageCreateInstallMonitorSlot);
    vtbl[6] = reinterpret_cast<Slot>(&PackageCloseInstallMonitorSlot);
    vtbl[7] = reinterpret_cast<Slot>(&PackageGetInstallProgressSlot);
    vtbl[8] = reinterpret_cast<Slot>(&PackageUpdateInstallMonitorSlot);
    vtbl[9] = reinterpret_cast<Slot>(&PackageRegisterInstallProgressSlot);
    vtbl[10] = reinterpret_cast<Slot>(&PackageUnregisterInstallProgressSlot);
    vtbl[11] = reinterpret_cast<Slot>(&PackageGetUserLocale);
    vtbl[12] = reinterpret_cast<Slot>(&PackageFindChunkAvailabilitySlot);
    vtbl[13] = reinterpret_cast<Slot>(&PackageNoopChunkSlot);
    vtbl[14] = reinterpret_cast<Slot>(&PackageNoopChunkSlot);
    vtbl[15] = reinterpret_cast<Slot>(&PackageInstallChunksSlot);
    vtbl[16] = reinterpret_cast<Slot>(&PackageInstallChunksAsyncSlot);
    vtbl[17] = reinterpret_cast<Slot>(&PackageInstallChunksResultSlot);
    vtbl[18] = reinterpret_cast<Slot>(&PackageEstimateDownloadSizeSlot);
    vtbl[19] = reinterpret_cast<Slot>(&PackageNoopChunkSlot);
    for (int slot : {20, 28, 35, 39}) vtbl[slot] = reinterpret_cast<Slot>(&PackageEnumeratePackages);
    for (int slot : {21, 29, 36, 40}) vtbl[slot] = reinterpret_cast<Slot>(&PackageRegisterPackageInstalled);
    vtbl[22] = reinterpret_cast<Slot>(&PackageMountWithUiAsync);
    vtbl[23] = reinterpret_cast<Slot>(&PackageMountWithUiResult);
    vtbl[24] = reinterpret_cast<Slot>(&PackageGetMountPathSize);
    vtbl[25] = reinterpret_cast<Slot>(&PackageGetMountPath);
    vtbl[26] = reinterpret_cast<Slot>(&PackageCloseMountHandle);
    vtbl[41] = reinterpret_cast<Slot>(&PackageUnregisterPackageInstalledSlot);
    return vtbl;
}
const std::array<Slot, kPackageSlots> kPackageVtbl = MakePackageVtbl(std::make_index_sequence<kPackageSlots>{});

// Official GDK 2510 XDisplay HDR ({03F0FE74} / {35F07670}) & Timeout Deferral ({1B339674}).
struct DisplayHdrModeInfoAbi {
    float minToneMapLuminance;
    float maxToneMapLuminance;
    float maxFullFrameToneMapLuminance;
};

uint32_t SCARLETTX_GDK_CALLBACK DisplayTryEnableHdrModeSlot(void* /*self*/, uint32_t /*pref*/,
                                                            DisplayHdrModeInfoAbi* out_info) {
    const char* hdr_env = std::getenv("SCARLETTX_HDR");
    const bool hdr_on = hdr_env && hdr_env[0] == '1';
    if (out_info) {
        out_info->minToneMapLuminance = 0.0f;
        out_info->maxToneMapLuminance = hdr_on ? 1000.0f : 250.0f;
        out_info->maxFullFrameToneMapLuminance = hdr_on ? 600.0f : 250.0f;
    }
    return hdr_on ? 1u /*Enabled*/ : 2u /*Disabled*/;
}

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK DisplayHdrUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented("XDisplayHdr", "03F0FE74", Index, a1, a2, a3);
}

const Slot kDisplayHdrVtbl[16] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&DisplayTryEnableHdrModeSlot),      // slot 3
    reinterpret_cast<Slot>(&DisplayTryEnableHdrModeSlot),      // slot 4 (GDK 2510 {35F07670})
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<5>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<6>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<7>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<8>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<9>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<10>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<11>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<12>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<13>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<14>),
    reinterpret_cast<Slot>(&DisplayHdrUnimplemented<15>),
};

struct DisplayDeferralObj {
    uint32_t tag = 0x44454652u;
};

void SCARLETTX_GDK_CALLBACK DisplayCloseTimeoutDeferralSlot(void* /*self*/, void* handle) {
    delete static_cast<DisplayDeferralObj*>(handle);
}

XGameResult SCARLETTX_GDK_CALLBACK DisplayAcquireTimeoutDeferralSlot(void* /*self*/, void** out_handle) {
    if (!out_handle) return XGR_E_POINTER;
    auto* d = new (std::nothrow) DisplayDeferralObj();
    if (!d) return XGR_E_OUTOFMEMORY;
    *out_handle = d;
    return XGR_S_OK;
}

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK DisplayTimeoutUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented("XDisplayTimeout", "1B339674", Index, a1, a2, a3);
}

const Slot kDisplayTimeoutVtbl[16] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&DisplayCloseTimeoutDeferralSlot),    // slot 3 fallback
    reinterpret_cast<Slot>(&DisplayAcquireTimeoutDeferralSlot),  // slot 4 (observed in GDK 2510)
    reinterpret_cast<Slot>(&DisplayCloseTimeoutDeferralSlot),    // slot 5 (observed in GDK 2510)
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<6>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<7>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<8>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<9>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<10>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<11>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<12>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<13>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<14>),
    reinterpret_cast<Slot>(&DisplayTimeoutUnimplemented<15>),
};

// Official GDK 2510 XPersistentLocalStorage / PLS ({F4FAF4D4} / {41A4E10C}).
struct PlsSpaceInfoAbi {
    uint64_t availableFreeBytes;
    uint64_t totalFreeBytes;
    uint64_t usedBytes;
    uint64_t totalBytes;
};

std::string ResolveHostPlsDirectory() {
    uint32_t tid = 0x5CA00001u;
    (void)XGameGetXboxTitleId(&tid);
    char sub[32] = {};
    std::snprintf(sub, sizeof(sub), "pls_%08X", static_cast<unsigned>(tid));
    std::error_code ec;
    const char* env_root = std::getenv("SCARLETTX_SAVE_ROOT");
    std::filesystem::path base = (env_root && *env_root)
                                     ? std::filesystem::path(env_root)
                                     : (std::filesystem::temp_directory_path(ec) / "scarlettx_saves");
    std::filesystem::path dir = base / sub;
    std::filesystem::create_directories(dir, ec);
    std::string s = dir.string();
    while (s.size() > 3u && (s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }
    return s;
}

XGameResult SCARLETTX_GDK_CALLBACK PlsGetPathSizeSlot(void* /*self*/, size_t* out_size) {
    if (!out_size) return XGR_E_POINTER;
    const std::string dir = ResolveHostPlsDirectory();
    *out_size = dir.size() + 1u;
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK PlsGetPathSlot(void* /*self*/, size_t size, char* path, size_t* out_used) {
    if (!path) return XGR_E_POINTER;
    const std::string dir = ResolveHostPlsDirectory();
    const size_t need = dir.size() + 1u;
    if (size < need) return kInsufficientBuffer;
    std::memcpy(path, dir.c_str(), need);
    if (out_used) *out_used = need;
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK PlsGetSpaceInfoSlot(void* /*self*/, PlsSpaceInfoAbi* out_info) {
    if (!out_info) return XGR_E_POINTER;
    const std::string dir = ResolveHostPlsDirectory();
    std::error_code ec;
    uint64_t used = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            used += entry.file_size(ec);
        }
    }
    constexpr uint64_t kPlsBudgetBytes = 4ull << 30;  // 4 GiB persistent local scratch storage
    const uint64_t free_bytes = (used < kPlsBudgetBytes) ? (kPlsBudgetBytes - used) : 0ull;
    out_info->availableFreeBytes = free_bytes;
    out_info->totalFreeBytes = free_bytes;
    out_info->usedBytes = used;
    out_info->totalBytes = kPlsBudgetBytes;
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK PlsPromptSpaceProvider(XAsyncOp op, const XAsyncProviderData* data) {
    if (op == XAsyncOp_DoWork) {
        XAsyncComplete(data->async, XGR_S_OK, 0);
    }
    return XGR_S_OK;
}

XGameResult SCARLETTX_GDK_CALLBACK PlsPromptUserForSpaceAsyncSlot(void* /*self*/, uint64_t /*req_bytes*/,
                                                                  XAsyncBlock* async) {
    if (!async) return XGR_E_INVALIDARG;
    const XGameResult hr = XAsyncBegin(async, nullptr, reinterpret_cast<const void*>(&PlsPromptUserForSpaceAsyncSlot),
                                       "XPersistentLocalStoragePromptUserForSpaceAsync", &PlsPromptSpaceProvider);
    return hr == XGR_S_OK ? XAsyncSchedule(async, 0) : hr;
}

XGameResult SCARLETTX_GDK_CALLBACK PlsPromptUserForSpaceResultSlot(void* /*self*/, XAsyncBlock* async) {
    if (!async) return XGR_E_INVALIDARG;
    return XAsyncGetResult(async, reinterpret_cast<const void*>(&PlsPromptUserForSpaceAsyncSlot), 0, nullptr, nullptr);
}

XGameResult SCARLETTX_GDK_CALLBACK PlsMountForPackageSlot(void* /*self*/, const char* pkg_id,
                                                          XPackageMountHandle* out_mount) {
    if (!pkg_id || !out_mount) return XGR_E_POINTER;
    *out_mount = nullptr;
    return XGR_S_OK;
}

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK PlsUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented("XPersistentLocalStorage", "F4FAF4D4", Index, a1, a2, a3);
}

const Slot kPlsVtbl[16] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&PlsGetPathSizeSlot),                // slot 3
    reinterpret_cast<Slot>(&PlsGetPathSlot),                    // slot 4
    reinterpret_cast<Slot>(&PlsGetSpaceInfoSlot),               // slot 5
    reinterpret_cast<Slot>(&PlsPromptUserForSpaceAsyncSlot),    // slot 6
    reinterpret_cast<Slot>(&PlsPromptUserForSpaceResultSlot),   // slot 7
    reinterpret_cast<Slot>(&PlsMountForPackageSlot),            // slot 8
    reinterpret_cast<Slot>(&PlsUnimplemented<9>),
    reinterpret_cast<Slot>(&PlsUnimplemented<10>),
    reinterpret_cast<Slot>(&PlsUnimplemented<11>),
    reinterpret_cast<Slot>(&PlsUnimplemented<12>),
    reinterpret_cast<Slot>(&PlsUnimplemented<13>),
    reinterpret_cast<Slot>(&PlsUnimplemented<14>),
    reinterpret_cast<Slot>(&PlsUnimplemented<15>),
};

// {CE8E7A26}: XStore (GDK 2022-2026 game license & DLC entitlement queries).
XGameResult SCARLETTX_GDK_CALLBACK StoreCreateContext(void* /*self*/, XUserHandle user, XStoreContextHandle* out) {
    return XStoreCreateContext(user, out);
}
void SCARLETTX_GDK_CALLBACK StoreCloseContextHandle(void* /*self*/, XStoreContextHandle ctx) {
    XStoreCloseContextHandle(ctx);
}
XGameResult SCARLETTX_GDK_CALLBACK StoreQueryGameLicenseAsync(void* /*self*/, XStoreContextHandle ctx,
                                                              XAsyncBlock* async) {
    return XStoreQueryGameLicenseAsync(ctx, async);
}
XGameResult SCARLETTX_GDK_CALLBACK StoreQueryGameLicenseResult(void* /*self*/, XAsyncBlock* async,
                                                               XStoreGameLicense* lic) {
    return XStoreQueryGameLicenseResult(async, lic);
}
XGameResult SCARLETTX_GDK_CALLBACK StoreQueryEntitledProductsAsync(void* /*self*/, XStoreContextHandle ctx,
                                                                   uint32_t kinds, uint32_t max_items,
                                                                   XAsyncBlock* async) {
    return XStoreQueryEntitledProductsAsync(ctx, kinds, max_items, async);
}
XGameResult SCARLETTX_GDK_CALLBACK StoreQueryEntitledProductsResult(void* /*self*/, XAsyncBlock* async,
                                                                    XStoreProductQueryHandle* out) {
    return XStoreQueryEntitledProductsResult(async, out);
}
XGameResult SCARLETTX_GDK_CALLBACK StoreEnumerateProductsQuery(void* /*self*/, XStoreProductQueryHandle q,
                                                               void* ctx, XStoreProductQueryCallback* cb) {
    return XStoreEnumerateProductsQuery(q, ctx, cb);
}
void SCARLETTX_GDK_CALLBACK StoreCloseProductsQueryHandle(void* /*self*/, XStoreProductQueryHandle q) {
    XStoreCloseProductsQueryHandle(q);
}

constexpr int kStoreSlots = 40;

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK StoreUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented("XStore", "CE8E7A26", Index, a1, a2, a3);
}

template <size_t... I>
std::array<Slot, kStoreSlots> MakeStoreVtbl(std::index_sequence<I...>) {
    std::array<Slot, kStoreSlots> vtbl = {reinterpret_cast<Slot>(&StoreUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&StoreCreateContext);               // 3 (+0x18)
    vtbl[4] = reinterpret_cast<Slot>(&StoreCloseContextHandle);          // 4 (+0x20)
    vtbl[5] = reinterpret_cast<Slot>(&StoreQueryGameLicenseAsync);       // 5 (+0x28)
    vtbl[6] = reinterpret_cast<Slot>(&StoreQueryGameLicenseResult);      // 6 (+0x30)
    vtbl[7] = reinterpret_cast<Slot>(&StoreQueryEntitledProductsAsync);  // 7 (+0x38)
    vtbl[8] = reinterpret_cast<Slot>(&StoreQueryEntitledProductsResult); // 8 (+0x40)
    vtbl[9] = reinterpret_cast<Slot>(&StoreEnumerateProductsQuery);      // 9 (+0x48)
    vtbl[10] = reinterpret_cast<Slot>(&StoreCloseProductsQueryHandle);   // 10 (+0x50)
    return vtbl;
}
const std::array<Slot, kStoreSlots> kStoreVtbl = MakeStoreVtbl(std::make_index_sequence<kStoreSlots>{});

// Official GDK 2510 XStore table ({0DD112AC} / {B09D803C}): slots follow public XStore.h declaration order.
constexpr int kRealStoreSlots = 68;

bool SCARLETTX_GDK_CALLBACK StoreProductsQueryHasMorePagesSlot(void* /*self*/, XStoreProductQueryHandle /*q*/) {
    return false;
}

XGameResult SCARLETTX_GDK_CALLBACK StoreQueryCurrentGameAsyncSlot(void* /*self*/, XStoreContextHandle ctx,
                                                                  XAsyncBlock* async) {
    return XStoreQueryEntitledProductsAsync(ctx, 0xFFFFFFFFu, 25u, async);
}

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK RealStoreUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented("XStore2510", "0DD112AC", Index, a1, a2, a3);
}

template <size_t... I>
std::array<Slot, kRealStoreSlots> MakeRealStoreVtbl(std::index_sequence<I...>) {
    std::array<Slot, kRealStoreSlots> vtbl = {
        reinterpret_cast<Slot>(&RealStoreUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&StoreCreateContext);                 // XStoreCreateContext
    vtbl[4] = reinterpret_cast<Slot>(&StoreCloseContextHandle);            // XStoreCloseContextHandle
    vtbl[5] = reinterpret_cast<Slot>(&StoreQueryEntitledProductsAsync);    // XStoreQueryAssociatedProductsAsync
    vtbl[6] = reinterpret_cast<Slot>(&StoreQueryEntitledProductsResult);   // XStoreQueryAssociatedProductsResult
    vtbl[9] = reinterpret_cast<Slot>(&StoreQueryEntitledProductsAsync);    // XStoreQueryEntitledProductsAsync
    vtbl[10] = reinterpret_cast<Slot>(&StoreQueryEntitledProductsResult);  // XStoreQueryEntitledProductsResult
    vtbl[11] = reinterpret_cast<Slot>(&StoreQueryCurrentGameAsyncSlot);    // XStoreQueryProductForCurrentGameAsync
    vtbl[12] = reinterpret_cast<Slot>(&StoreQueryEntitledProductsResult);  // XStoreQueryProductForCurrentGameResult
    vtbl[15] = reinterpret_cast<Slot>(&StoreEnumerateProductsQuery);       // XStoreEnumerateProductsQuery
    vtbl[16] = reinterpret_cast<Slot>(&StoreProductsQueryHasMorePagesSlot);// XStoreProductsQueryHasMorePages
    vtbl[19] = reinterpret_cast<Slot>(&StoreCloseProductsQueryHandle);     // XStoreCloseProductsQueryHandle
    vtbl[28] = reinterpret_cast<Slot>(&StoreQueryGameLicenseAsync);        // XStoreQueryGameLicenseAsync
    vtbl[29] = reinterpret_cast<Slot>(&StoreQueryGameLicenseResult);       // XStoreQueryGameLicenseResult
    return vtbl;
}
const std::array<Slot, kRealStoreSlots> kRealStoreVtbl =
    MakeRealStoreVtbl(std::make_index_sequence<kRealStoreSlots>{});

// Discovery table for API ids nothing implements, enabled with
// SCARLETTX_XGR_DISCOVER=1: every method logs its slot and first arguments
// and fails, so a trace shows which slots a title uses.
constexpr int kDiscoverySlots = 96;

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK DiscoverySlot(void* self, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    char iid[40] = {};
    const Guid& g = static_cast<Table*>(self)->iid;
    std::snprintf(iid, sizeof(iid), "%08X-%04X-%04X", static_cast<unsigned>(g.data1), g.data2, g.data3);
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{%s}::+0x%X (slot %d)(a1=0x%llx, a2=0x%llx, a3=0x%llx) -> E_NOTIMPL\n",
                 iid, Index * 8, Index, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3));
    std::fflush(stderr);
    scarlettx::FlightRecorder::Instance().RecordVtableSlotMiss(
        iid, Index, static_cast<uint64_t>(a1), SCARLETTX_CALLER_RIP());
    return kNotImplemented;
}

template <size_t... I>
constexpr std::array<Slot, kDiscoverySlots> MakeDiscoveryVtbl(std::index_sequence<I...>) {
    return {reinterpret_cast<Slot>(&TableQueryInterface), reinterpret_cast<Slot>(&TableAddRef),
            reinterpret_cast<Slot>(&TableRelease), reinterpret_cast<Slot>(&DiscoverySlot<static_cast<int>(I) + 3>)...};
}
const std::array<Slot, kDiscoverySlots> kDiscoveryVtbl = MakeDiscoveryVtbl(std::make_index_sequence<kDiscoverySlots - 3>{});

bool DiscoveryEnabled() {
    const char* v = std::getenv("SCARLETTX_XGR_DISCOVER");
    return v && v[0] != '\0' && v[0] != '0';
}

void* NewTable(const Slot* vtbl, const void* api_id, const Guid& iid) {
    auto* table = new (std::nothrow) Table();
    if (!table) return nullptr;
    table->vtbl = vtbl;
    std::memcpy(&table->api, api_id, sizeof(Guid));
    table->iid = iid;
    return table;
}

const Slot* FindVtbl(const void* api_id, const void* riid, Guid* iid) {
    // An API's main interface has the API's own id.
    const bool main = std::memcmp(api_id, riid, sizeof(Guid)) == 0 || Equal(riid, kIidIUnknown);
    std::memcpy(iid, main ? api_id : riid, sizeof(Guid));
    if (Equal(api_id, kApiGame)) {
        if (main || Equal(riid, kIidGameLater)) return kGameVtbl;
    } else if (Equal(api_id, kApiSystem)) {
        if (main || Equal(riid, kIidSystemLater) || Equal(riid, kIidSystem2510)) return kSystemVtbl;
    } else if (Equal(api_id, kApiAnalytics)) {
        if (main) return kAnalyticsVtbl;
    } else if (Equal(api_id, kApiGameSave)) {
        if (main || Equal(riid, kIidGameSaveLater) || Equal(riid, kIidGameSave2510)) return kGameSaveVtbl.data();
    } else if (Equal(api_id, kApiUser)) {
        if (main || Equal(riid, kIidUserLater) || Equal(riid, kIidUser2025)) return kUserVtbl.data();
        if (Equal(riid, kIidUserGamertag)) return kUserGamertagVtbl;
    } else if (Equal(api_id, kApiFeature)) {
        if (main) return kFeatureVtbl;
    } else if (Equal(api_id, kApiGameEvent)) {
        if (main) return kGameEventVtbl;
    } else if (Equal(api_id, kApiPackage)) {
        if (main || Equal(riid, kIidPackageLater) || Equal(riid, kIidPackage2025) || Equal(riid, kIidPackage2510)) {
            return kPackageVtbl.data();
        }
    } else if (Equal(api_id, kApiStore)) {
        if (main || Equal(riid, kIidStore2025)) return kStoreVtbl.data();
    } else if (Equal(api_id, kApiStoreReal)) {
        if (main || Equal(riid, kIidStore2510)) return kRealStoreVtbl.data();
    } else if (Equal(api_id, kApiDisplayHdr)) {
        if (main || Equal(riid, kIidDisplayHdr2510)) return kDisplayHdrVtbl;
    } else if (Equal(api_id, kApiDisplayTimeout)) {
        if (main) return kDisplayTimeoutVtbl;
    } else if (Equal(api_id, kApiPls)) {
        if (main || Equal(riid, kIidPls2510)) return kPlsVtbl;
    }
    // An API or newer interface id nothing implements yet: fall back to discovery table if enabled.
    return DiscoveryEnabled() ? kDiscoveryVtbl.data() : nullptr;
}

}  // namespace

void* CreateApiTable(const void* api_id, const void* riid) {
    if (!api_id || !riid) return nullptr;
    Guid iid{};
    const Slot* vtbl = FindVtbl(api_id, riid, &iid);
    return vtbl ? NewTable(vtbl, api_id, iid) : nullptr;
}

}  // namespace scarlettx::xgr
