#include "scarlettx/gdk/xgameruntime_abi.h"
#include "scarlettx/input/gameinput_abi.h"
#include "scarlettx/services/xsapi_abi.h"
#include "sx_check.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <objbase.h>
#include <windows.h>

extern "C" {
__declspec(dllimport) HRESULT WINAPI ScarlettX_GuestRoActivateInstance(
    void* activatableClassId, void** instance);
__declspec(dllimport) HRESULT WINAPI ScarlettX_GuestRoGetActivationFactory(
    void* activatableClassId, const GUID* iid, void** factory);
}
#endif

namespace {

#if defined(_WIN32)
#define TEST_CALL __stdcall
#else
#define TEST_CALL
#endif

template <typename... Args>
intptr_t CallVtbl(void* obj, int slot, Args... args) {
    void** vtbl = *reinterpret_cast<void***>(obj);
    using Fn = intptr_t(TEST_CALL*)(void*, Args...);
    return reinterpret_cast<Fn>(vtbl[slot])(obj, args...);
}

#if defined(_WIN32)
struct CombaseHStringApi {
    HMODULE combase = nullptr;
    HRESULT(WINAPI* create_str)(const wchar_t*, uint32_t, void**) = nullptr;
    HRESULT(WINAPI* delete_str)(void*) = nullptr;
    const wchar_t*(WINAPI* get_raw)(void*, uint32_t*) = nullptr;

    CombaseHStringApi() {
        combase = LoadLibraryW(L"combase.dll");
        if (combase) {
            create_str = reinterpret_cast<decltype(create_str)>(
                reinterpret_cast<void*>(GetProcAddress(combase, "WindowsCreateString")));
            delete_str = reinterpret_cast<decltype(delete_str)>(
                reinterpret_cast<void*>(GetProcAddress(combase, "WindowsDeleteString")));
            get_raw = reinterpret_cast<decltype(get_raw)>(
                reinterpret_cast<void*>(GetProcAddress(combase, "WindowsGetStringRawBuffer")));
        }
    }

    void* Make(const wchar_t* s) const {
        void* h = nullptr;
        SX_CHECK(create_str != nullptr);
        SX_CHECK(create_str(s, static_cast<uint32_t>(std::wcslen(s)), &h) == S_OK);
        return h;
    }

    std::wstring ReadAndFree(void* h) const {
        SX_CHECK(get_raw != nullptr && delete_str != nullptr);
        if (!h) return {};
        uint32_t len = 0;
        const wchar_t* raw = get_raw(h, &len);
        std::wstring out = raw ? std::wstring(raw, len) : std::wstring();
        SX_CHECK(delete_str(h) == S_OK);
        return out;
    }

    void Free(void* h) const {
        if (h && delete_str) {
            SX_CHECK(delete_str(h) == S_OK);
        }
    }
};

constexpr GUID kIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};
constexpr GUID kIidIInspectable = {
    0xAF86E2E0u, 0xB12Du, 0x4C6Au, {0x9Cu, 0x5Au, 0xD7u, 0xAAu, 0x65u, 0x10u, 0x1Eu, 0x90u}};
constexpr GUID kIidIActivationFactory = {
    0x00000035u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};
constexpr GUID kIidIGamepadStatics = {
    0x8BBCE529u, 0xD49Cu, 0x39E9u, {0x95u, 0x60u, 0xE4u, 0x7Du, 0xDEu, 0x96u, 0xB7u, 0xC8u}};
constexpr GUID kIidIGamepad = {
    0xBC7BB43Cu, 0x0A69u, 0x3903u, {0x9Eu, 0x9Du, 0xA5u, 0x0Fu, 0x86u, 0xA4u, 0x5Du, 0xE5u}};
constexpr GUID kIidIAnalyticsInfoStatics = {
    0x1D5EE066u, 0x188Du, 0x5BA9u, {0x43u, 0x87u, 0xACu, 0xAEu, 0xB0u, 0xE7u, 0xE3u, 0x05u}};
constexpr GUID kIidIAnalyticsVersionInfo = {
    0x926130B8u, 0x9955u, 0x4C74u, {0xBDu, 0xC1u, 0x7Cu, 0xD0u, 0xDEu, 0xCFu, 0x9Bu, 0x03u}};
constexpr GUID kIidIPackageStatics = {
    0x4E534BDFu, 0x2960u, 0x4878u, {0x97u, 0xA4u, 0x96u, 0x24u, 0xDEu, 0xB7u, 0x2Fu, 0x2Du}};
constexpr GUID kIidIPackage = {
    0x163C792Fu, 0xBD75u, 0x413Cu, {0xBFu, 0x23u, 0xB1u, 0xFEu, 0x7Bu, 0x95u, 0xD8u, 0x25u}};
constexpr GUID kIidIPackageId = {
    0x1ADB665Eu, 0x37C7u, 0x4790u, {0x99u, 0x80u, 0xDDu, 0x7Au, 0xE7u, 0x4Eu, 0x8Bu, 0xB2u}};
constexpr GUID kIidBogus = {
    0xDEADBEEFu, 0x1234u, 0x5678u, {0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu, 0x01u, 0x02u}};

void TestWinRtBroker() {
    std::cout << "[test_winrt_middleware] 1. Testing WinRT Broker (Gamepad, AnalyticsInfo, Package)...\n";
    CombaseHStringApi hstr;

    // 1a. Windows.Gaming.Input.Gamepad
    {
        void* cls = hstr.Make(L"Windows.Gaming.Input.Gamepad");
        void* fac = nullptr;
        SX_CHECK(ScarlettX_GuestRoGetActivationFactory(cls, &kIidIActivationFactory, &fac) == S_OK);
        SX_CHECK(fac != nullptr);

        // Factory GetRuntimeClassName -> E_ILLEGAL_METHOD_CALL per WinRT spec
        void* fac_name = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(fac, 4, &fac_name)) == static_cast<HRESULT>(0x8000000Eu));
        SX_CHECK(fac_name == nullptr);

        // Factory GetTrustLevel -> BaseTrust (0)
        int32_t trust = -1;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(fac, 5, &trust)) == S_OK);
        SX_CHECK(trust == 0);

        // Factory GetIids -> includes IActivationFactory + IGamepadStatics
        ULONG iid_cnt = 0;
        GUID* iids = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(fac, 3, &iid_cnt, &iids)) == S_OK);
        SX_CHECK(iid_cnt >= 2 && iids != nullptr);
        CoTaskMemFree(iids);

        // QI for IGamepadStatics & register/unregister event token
        void* statics = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(fac, 0, &kIidIGamepadStatics, &statics)) == S_OK);
        SX_CHECK(statics != nullptr);
        int64_t ev_tok = 0;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(statics, 6, static_cast<void*>(nullptr), &ev_tok)) == S_OK);
        SX_CHECK(ev_tok > 0);
        SX_CHECK(static_cast<HRESULT>(CallVtbl(statics, 7, ev_tok)) == S_OK);

        // Bogus IID -> E_NOINTERFACE
        void* bogus = reinterpret_cast<void*>(0x1);
        SX_CHECK(static_cast<HRESULT>(CallVtbl(statics, 0, &kIidBogus, &bogus)) == E_NOINTERFACE);
        SX_CHECK(bogus == nullptr);

        // ActivateInstance -> IGamepad instance
        void* inst = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(fac, 6, &inst)) == S_OK);
        SX_CHECK(inst != nullptr);
        void* inst_name_h = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(inst, 4, &inst_name_h)) == S_OK);
        SX_CHECK(hstr.ReadAndFree(inst_name_h) == L"Windows.Gaming.Input.Gamepad");
        void* igp = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(inst, 0, &kIidIGamepad, &igp)) == S_OK);
        SX_CHECK(igp == inst);
        CallVtbl(igp, 2);
        SX_CHECK(CallVtbl(inst, 2) == 0);

        // Direct RoActivateInstance
        void* inst2 = nullptr;
        SX_CHECK(ScarlettX_GuestRoActivateInstance(cls, &inst2) == S_OK && inst2 != nullptr);
        SX_CHECK(CallVtbl(inst2, 2) == 0);

        CallVtbl(statics, 2);
        SX_CHECK(CallVtbl(fac, 2) == 0);
        hstr.Free(cls);
    }

    // 1b. Windows.System.Profile.AnalyticsInfo
    {
        void* cls = hstr.Make(L"Windows.System.Profile.AnalyticsInfo");
        void* statics = nullptr;
        SX_CHECK(ScarlettX_GuestRoGetActivationFactory(cls, &kIidIAnalyticsInfoStatics, &statics) == S_OK);
        SX_CHECK(statics != nullptr);

        // get_DeviceForm -> L"Xbox"
        void* form_h = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(statics, 7, &form_h)) == S_OK);
        SX_CHECK(hstr.ReadAndFree(form_h) == L"Xbox");

        // get_VersionInfo -> IAnalyticsVersionInfo
        void* ver = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(statics, 6, &ver)) == S_OK);
        SX_CHECK(ver != nullptr);
        void* qi_ver = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(ver, 0, &kIidIAnalyticsVersionInfo, &qi_ver)) == S_OK);
        SX_CHECK(qi_ver == ver);
        CallVtbl(qi_ver, 2);

        void* fam_h = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(ver, 6, &fam_h)) == S_OK);
        SX_CHECK(hstr.ReadAndFree(fam_h) == L"Windows.Xbox");

        void* fam_ver_h = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(ver, 7, &fam_ver_h)) == S_OK);
        SX_CHECK(!hstr.ReadAndFree(fam_ver_h).empty());

        void* ver_cls_h = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(ver, 4, &ver_cls_h)) == S_OK);
        SX_CHECK(hstr.ReadAndFree(ver_cls_h) == L"Windows.System.Profile.AnalyticsVersionInfo");

        SX_CHECK(CallVtbl(ver, 2) == 0);
        SX_CHECK(CallVtbl(statics, 2) == 0);
        hstr.Free(cls);
    }

    // 1c. Windows.ApplicationModel.Package
    {
        void* cls = hstr.Make(L"Windows.ApplicationModel.Package");
        void* statics = nullptr;
        SX_CHECK(ScarlettX_GuestRoGetActivationFactory(cls, &kIidIPackageStatics, &statics) == S_OK);
        SX_CHECK(statics != nullptr);

        // get_Current -> IPackage
        void* pkg = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(statics, 6, &pkg)) == S_OK);
        SX_CHECK(pkg != nullptr);
        void* qi_pkg = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(pkg, 0, &kIidIPackage, &qi_pkg)) == S_OK);
        SX_CHECK(qi_pkg == pkg);
        CallVtbl(qi_pkg, 2);

        uint8_t is_framework = 1;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(pkg, 9, &is_framework)) == S_OK);
        SX_CHECK(is_framework == 0);

        // get_Id -> IPackageId
        void* pkg_id = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(pkg, 6, &pkg_id)) == S_OK);
        SX_CHECK(pkg_id != nullptr);
        void* qi_id = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(pkg_id, 0, &kIidIPackageId, &qi_id)) == S_OK);
        SX_CHECK(qi_id == pkg_id);
        CallVtbl(qi_id, 2);

        void* name_h = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(pkg_id, 6, &name_h)) == S_OK);
        SX_CHECK(hstr.ReadAndFree(name_h) == L"ScarlettX.Title");

        uint16_t ver[4]{};
        SX_CHECK(static_cast<HRESULT>(CallVtbl(pkg_id, 7, ver)) == S_OK);
        SX_CHECK(ver[0] == 1u && ver[1] == 0u);

        int32_t arch = 0;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(pkg_id, 8, &arch)) == S_OK);
        SX_CHECK(arch == 9);  // x64

        void* family_h = nullptr;
        SX_CHECK(static_cast<HRESULT>(CallVtbl(pkg_id, 13, &family_h)) == S_OK);
        SX_CHECK(hstr.ReadAndFree(family_h) == L"ScarlettX.Title_scarlettx0000");

        SX_CHECK(CallVtbl(pkg_id, 2) == 0);
        SX_CHECK(CallVtbl(pkg, 2) == 0);
        SX_CHECK(CallVtbl(statics, 2) == 0);
        hstr.Free(cls);
    }
}
#endif

struct DeviceCbState {
    int count = 0;
    uint64_t token = 0;
    void* device = nullptr;
    uint64_t timestamp = 0;
    uint32_t current_status = 0;
    uint32_t previous_status = 0xFFFFFFFFu;
};

void TEST_CALL OnVirtualDeviceEvent(
    uint64_t token,
    void* context,
    void* device,
    uint64_t timestamp,
    uint32_t current_status,
    uint32_t previous_status) {
    auto* st = static_cast<DeviceCbState*>(context);
    ++st->count;
    st->token = token;
    st->device = device;
    st->timestamp = timestamp;
    st->current_status = current_status;
    st->previous_status = previous_status;
}

void TestGameInputCallbacksAndRumble() {
    std::cout << "[test_winrt_middleware] 2. Testing GameInput callbacks & rumble transitions...\n";
    uint64_t gi = 0;
    SX_CHECK(GameInputCreate(&gi) == 0 && gi != 0);
    void* input = reinterpret_cast<void*>(gi);

    constexpr int kSlotRegisterDeviceCb = 9;
    constexpr int kSlotStopCb = 13;
    constexpr int kSlotUnregisterCb = 14;
    constexpr int kSlotDevSetRumble = 10;

    // 2a. Null callback rejected with E_INVALIDARG
    uint64_t bad_tok = 999;
    SX_CHECK(CallVtbl(input, kSlotRegisterDeviceCb,
                      static_cast<void*>(nullptr), uintptr_t{GAMEINPUT_KIND_GAMEPAD},
                      uintptr_t{0x3u}, uintptr_t{2u}, static_cast<void*>(nullptr),
                      static_cast<void*>(nullptr), &bad_tok) < 0);
    SX_CHECK(bad_tok == 0);

    // 2b. No-enumeration registration (enum_kind == 0) stores token without firing initial callback
    DeviceCbState no_enum{};
    uint64_t tok_no_enum = 0;
    SX_CHECK(CallVtbl(input, kSlotRegisterDeviceCb,
                      static_cast<void*>(nullptr), uintptr_t{GAMEINPUT_KIND_GAMEPAD},
                      uintptr_t{0x3u}, uintptr_t{0u}, static_cast<void*>(&no_enum),
                      &OnVirtualDeviceEvent, &tok_no_enum) == 0);
    SX_CHECK(tok_no_enum != 0);
    SX_CHECK(no_enum.count == 0);

    // 2c. Blocking enumeration invokes initial callback for virtual Xbox controller
    DeviceCbState enum_ev{};
    uint64_t tok_enum = 0;
    SX_CHECK(CallVtbl(input, kSlotRegisterDeviceCb,
                      static_cast<void*>(nullptr), uintptr_t{GAMEINPUT_KIND_GAMEPAD},
                      uintptr_t{0x3u}, uintptr_t{2u}, static_cast<void*>(&enum_ev),
                      &OnVirtualDeviceEvent, &tok_enum) == 0);
    SX_CHECK(tok_enum != 0 && tok_enum != tok_no_enum);
    SX_CHECK(enum_ev.count == 1);
    SX_CHECK(enum_ev.token == tok_enum);
    SX_CHECK(enum_ev.device != nullptr);
    SX_CHECK(enum_ev.timestamp > 0);
    SX_CHECK((enum_ev.current_status & 3u) == 3u);
    SX_CHECK(enum_ev.previous_status == 0u);

    // 2d. StopCallback + UnregisterCallback (second unregister returns false/0)
    CallVtbl(input, kSlotStopCb, tok_enum);
    SX_CHECK(CallVtbl(input, kSlotUnregisterCb, tok_enum, uintptr_t{0}) == 1);
    SX_CHECK(CallVtbl(input, kSlotUnregisterCb, tok_enum, uintptr_t{0}) == 0);
    SX_CHECK(CallVtbl(input, kSlotUnregisterCb, tok_no_enum, uintptr_t{0}) == 1);
    SX_CHECK(CallVtbl(input, kSlotUnregisterCb, uintptr_t{0}, uintptr_t{0}) == 0);

    // 2e. Rumble state transitions across COM IGameInputDevice::SetRumbleState & C API
    void* dev = enum_ev.device;
    GAMEINPUT_RUMBLE_PARAMS got{};
    SX_CHECK(GameInputGetRumbleState(gi, 1, &got) == 0);

    // Idle -> Active (with clamping & NaN sanitization)
    const GAMEINPUT_RUMBLE_PARAMS noisy{1.75f, -0.5f, 0.4f, std::numeric_limits<float>::quiet_NaN()};
    CallVtbl(dev, kSlotDevSetRumble, &noisy);
    SX_CHECK(GameInputGetRumbleState(gi, 1, &got) == 0);
    SX_CHECK(std::fabs(got.LowFrequency - 1.0f) < 1e-5f);
    SX_CHECK(std::fabs(got.HighFrequency - 0.0f) < 1e-5f);
    SX_CHECK(std::fabs(got.LeftTrigger - 0.4f) < 1e-5f);
    SX_CHECK(std::fabs(got.RightTrigger - 0.0f) < 1e-5f);

    // Active -> Updated via C API
    const GAMEINPUT_RUMBLE_PARAMS mid{0.3f, 0.7f, 0.2f, 0.9f};
    SX_CHECK(GameInputSetRumbleState(gi, 0, &mid) == 0);
    SX_CHECK(GameInputGetRumbleState(gi, 1, &got) == 0);
    SX_CHECK(std::fabs(got.LowFrequency - 0.3f) < 1e-5f);
    SX_CHECK(std::fabs(got.HighFrequency - 0.7f) < 1e-5f);
    SX_CHECK(std::fabs(got.LeftTrigger - 0.2f) < 1e-5f);
    SX_CHECK(std::fabs(got.RightTrigger - 0.9f) < 1e-5f);

    // Active -> Stopped via COM nullptr
    CallVtbl(dev, kSlotDevSetRumble, static_cast<void*>(nullptr));
    SX_CHECK(GameInputGetRumbleState(gi, 1, &got) == 0);
    SX_CHECK(got.LowFrequency == 0.0f && got.HighFrequency == 0.0f &&
             got.LeftTrigger == 0.0f && got.RightTrigger == 0.0f);

    SX_CHECK(GameInputDestroy(gi) == 0);
}

void SCARLETTX_GDK_CALLBACK OnAsyncCount(XAsyncBlock* ab) {
    if (ab && ab->context) {
        static_cast<std::atomic<int>*>(ab->context)->fetch_add(1);
    }
}

void TestXsapiMiddleware() {
    std::cout << "[test_winrt_middleware] 3. Testing XSAPI achievements, player stats, and user profile...\n";
    SX_CHECK(XGameRuntimeInitialize() == 0);
    SX_CHECK(XblCleanup() == 0);
    SX_CHECK(XblInitialize("12345678-1234-1234-1234-1234567890ab") == 0);

    constexpr uint64_t kTestXuid = 0x0009000000007777ull;
    XblContextHandle ctx = nullptr;
    SX_CHECK(XblContextCreateHandle(kTestXuid, &ctx) == 0 && ctx != nullptr);

    // 3a. Offline Achievements unlock/progress/query round-trip
    {
        XAsyncBlock ab1{};
        SX_CHECK(XblAchievementsUpdateAchievementAsync(ctx, "ACH_EXPLORE", 50, &ab1) == 0);
        SX_CHECK(XblAchievementsUpdateAchievementResult(&ab1) == 0);

        // Monotonic progress: lowering from 50 to 25 must keep 50
        XAsyncBlock ab2{};
        SX_CHECK(XblAchievementsUpdateAchievementAsync(ctx, "ACH_EXPLORE", 25, &ab2) == 0);
        SX_CHECK(XblAchievementsUpdateAchievementResult(&ab2) == 0);

        // Unlock second achievement (150 clamps to 100)
        XAsyncBlock ab3{};
        SX_CHECK(XblAchievementsUpdateAchievementAsync(ctx, "ACH_BOSS", 150, &ab3) == 0);
        SX_CHECK(XblAchievementsUpdateAchievementResult(&ab3) == 0);

        XAsyncBlock qab{};
        SX_CHECK(XblAchievementsGetAchievementsForTitleIdAsync(ctx, 0x5CA04D01u, &qab) == 0);
        uint32_t unlocked_count = 0;
        XblAchievementsResultHandle res = nullptr;
        size_t used = 0;
        SX_CHECK(XblAchievementsGetAchievementsForTitleIdResult(
                     &qab, sizeof(unlocked_count), &unlocked_count, &res, &used) == 0);
        SX_CHECK(unlocked_count == 1u);
        SX_CHECK(res != nullptr);

        const XblAchievementRecord* recs = nullptr;
        size_t rec_count = 0;
        SX_CHECK(XblAchievementsResultGetAchievements(res, &recs, &rec_count) == 0);
        SX_CHECK(rec_count == 2u && recs != nullptr);
        // Sorted by id: ACH_BOSS (100, unlocked=1), ACH_EXPLORE (50, unlocked=0)
        SX_CHECK(std::strcmp(recs[0].id, "ACH_BOSS") == 0);
        SX_CHECK(recs[0].progressPercent == 100u && recs[0].isUnlocked == 1u);
        SX_CHECK(std::strcmp(recs[1].id, "ACH_EXPLORE") == 0);
        SX_CHECK(recs[1].progressPercent == 50u && recs[1].isUnlocked == 0u);
        SX_CHECK(XblAchievementsResultCloseHandle(res) == 0);
    }

    // 3b. Offline Player Stats write / read / update / delete round-trip
    {
        XblTitleManagedStatistic initial_stats[2]{};
        initial_stats[0].statisticName = "HighScore";
        initial_stats[0].statisticType = XblTitleManagedStatType_Number;
        initial_stats[0].numberValue = 9001.5;

        initial_stats[1].statisticName = "LoadoutClass";
        initial_stats[1].statisticType = XblTitleManagedStatType_String;
        initial_stats[1].stringValue = "Vanguard";

        XAsyncBlock w_ab{};
        SX_CHECK(XblTitleManagedStatsWriteAsync(ctx, kTestXuid, initial_stats, 2, &w_ab) == 0);
        SX_CHECK(XblTitleManagedStatsWriteResult(&w_ab) == 0);

        // Read number stat
        XAsyncBlock r1_ab{};
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticAsync(ctx, kTestXuid, nullptr, "HighScore", &r1_ab) == 0);
        size_t sz1 = 0;
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticResultSize(&r1_ab, &sz1) == 0 && sz1 > 0);
        std::vector<uint8_t> buf1(sz1, 0);
        XblTitleManagedStatistic out1{};
        size_t used1 = 0;
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticResult(&r1_ab, buf1.size(), buf1.data(), &out1, &used1) == 0);
        SX_CHECK(std::strcmp(out1.statisticName, "HighScore") == 0);
        SX_CHECK(out1.statisticType == XblTitleManagedStatType_Number);
        SX_CHECK(std::fabs(out1.numberValue - 9001.5) < 1e-6);

        // Read string stat
        XAsyncBlock r2_ab{};
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticAsync(ctx, kTestXuid, nullptr, "LoadoutClass", &r2_ab) == 0);
        size_t sz2 = 0;
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticResultSize(&r2_ab, &sz2) == 0 && sz2 > 0);
        std::vector<uint8_t> buf2(sz2, 0);
        XblTitleManagedStatistic out2{};
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticResult(&r2_ab, buf2.size(), buf2.data(), &out2, nullptr) == 0);
        SX_CHECK(out2.statisticType == XblTitleManagedStatType_String);
        SX_CHECK(out2.stringValue != nullptr && std::strcmp(out2.stringValue, "Vanguard") == 0);

        // Update stat & verify
        XblTitleManagedStatistic updated{};
        updated.statisticName = "HighScore";
        updated.statisticType = XblTitleManagedStatType_Number;
        updated.numberValue = 12345.0;
        XAsyncBlock u_ab{};
        SX_CHECK(XblTitleManagedStatsUpdateStatsAsync(ctx, &updated, 1, &u_ab) == 0);
        SX_CHECK(XblTitleManagedStatsUpdateStatsResult(&u_ab) == 0);

        XAsyncBlock r3_ab{};
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticAsync(ctx, kTestXuid, nullptr, "HighScore", &r3_ab) == 0);
        std::vector<uint8_t> buf3(128, 0);
        XblTitleManagedStatistic out3{};
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticResult(&r3_ab, buf3.size(), buf3.data(), &out3, nullptr) == 0);
        SX_CHECK(std::fabs(out3.numberValue - 12345.0) < 1e-6);

        // Delete stat & verify E_NOTFOUND
        const char* del_names[1] = {"LoadoutClass"};
        XAsyncBlock d_ab{};
        SX_CHECK(XblTitleManagedStatsDeleteStatsAsync(ctx, del_names, 1, &d_ab) == 0);
        SX_CHECK(XblTitleManagedStatsDeleteStatsResult(&d_ab) == 0);
        SX_CHECK(XblUserStatisticsGetSingleUserStatisticAsync(ctx, kTestXuid, nullptr, "LoadoutClass", nullptr) != 0);
    }

    // 3c. Offline User Profile query round-trip (custom + async callback + buffer copy)
    {
        XblUserProfile custom{};
        custom.xboxUserId = kTestXuid;
        std::strncpy(custom.gamertag, "CrimsonPilot", sizeof(custom.gamertag) - 1);
        std::strncpy(custom.modernGamertag, "CrimsonPilot", sizeof(custom.modernGamertag) - 1);
        std::strncpy(custom.modernGamertagSuffix, "#9001", sizeof(custom.modernGamertagSuffix) - 1);
        std::strncpy(custom.uniqueModernGamertag, "CrimsonPilot#9001", sizeof(custom.uniqueModernGamertag) - 1);
        std::strncpy(custom.gamerscore, "4250", sizeof(custom.gamerscore) - 1);
        std::strncpy(custom.gameDisplayPictureResizeUri,
                     "https://scarlettx.local/gamerpics/pilot.png",
                     sizeof(custom.gameDisplayPictureResizeUri) - 1);
        SX_CHECK(XblProfileSetLocalUserProfile(ctx, &custom) == 0);

        // XblContextGetGamertag stays in sync
        char gt[64]{};
        SX_CHECK(XblContextGetGamertag(ctx, gt, sizeof(gt)) == 0);
        SX_CHECK(std::strcmp(gt, "CrimsonPilot") == 0);

        std::atomic<int> cb_count{0};
        XAsyncBlock p_ab{};
        p_ab.context = &cb_count;
        p_ab.callback = &OnAsyncCount;
        SX_CHECK(XblProfileGetUserProfileAsync(ctx, kTestXuid, &p_ab) == 0);

        XblUserProfile out_profile{};
        XblUserProfile buf_profile{};
        size_t used = 0;
        SX_CHECK(XblProfileGetUserProfileResult(
                     &p_ab, sizeof(buf_profile), &buf_profile, &out_profile, &used) == 0);
        SX_CHECK(used == sizeof(XblUserProfile));
        SX_CHECK(out_profile.xboxUserId == kTestXuid);
        SX_CHECK(std::strcmp(out_profile.gamertag, "CrimsonPilot") == 0);
        SX_CHECK(std::strcmp(out_profile.uniqueModernGamertag, "CrimsonPilot#9001") == 0);
        SX_CHECK(std::strcmp(out_profile.gamerscore, "4250") == 0);
        SX_CHECK(std::strcmp(out_profile.gameDisplayPictureResizeUri,
                             "https://scarlettx.local/gamerpics/pilot.png") == 0);
        SX_CHECK(buf_profile.xboxUserId == kTestXuid);
        SX_CHECK(std::strcmp(buf_profile.gamertag, "CrimsonPilot") == 0);
    }

    SX_CHECK(XblContextCloseHandle(ctx) == 0);
    SX_CHECK(XblCleanup() == 0);
    XGameRuntimeUninitialize();
}

}  // namespace

int main() {
#if defined(_WIN32)
    TestWinRtBroker();
#endif
    TestGameInputCallbacksAndRumble();
    TestXsapiMiddleware();
    std::cout << "[test_winrt_middleware] All Track 4I WinRT & Middleware checks passed!\n";
    return 0;
}
