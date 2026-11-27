#include "scarlettx/input/gameinput_abi.h"
#include "scarlettx/core/flight_recorder.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

bool GameInputTraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

bool GameInputKbmEnvEnabled() {
    const char* v = std::getenv("SCARLETTX_GAMEINPUT_KBM");
    return v && v[0] != '\0' && v[0] != '0';
}

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t kE_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kE_NOINTERFACE = static_cast<int32_t>(0x80004002u);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kGAMEINPUT_E_DEVICE_NOT_FOUND = static_cast<int32_t>(0x838A0001u);
constexpr int32_t kGAMEINPUT_E_READING_NOT_FOUND = static_cast<int32_t>(0x838A0002u);

constexpr uint64_t kVirtualGamepadId = 1;
constexpr uint64_t kVirtualKeyboardId = 2;
constexpr uint64_t kVirtualMouseId = 3;

#if defined(_WIN32)
#define SCARLETTX_GI_CALL __stdcall
#else
#define SCARLETTX_GI_CALL
#endif

using GiComFn = intptr_t (SCARLETTX_GI_CALL*)(void*, uintptr_t, uintptr_t, uintptr_t);

struct GiGuid {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t d4[8];
};

constexpr GiGuid kIidIUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GiGuid kIidIAgileObject = {0x94EA2B94u, 0xE9CCu, 0x49E0u, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
// Official GDK v0 / v1 / v2 IGameInput IIDs
constexpr GiGuid kIidIGameInputV0 = {0x11BE2A7Eu, 0x4254u, 0x445Au, {0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x18}};
constexpr GiGuid kIidIGameInputV1 = {0x40FFB7E4u, 0x6150u, 0x407Au, {0xB4, 0x39, 0x13, 0x2B, 0xAD, 0xC0, 0x8D, 0x2D}};
constexpr GiGuid kIidIGameInputV2 = {0x20EFC1C7u, 0x5D9Au, 0x43BAu, {0xB2, 0x6F, 0xB8, 0x07, 0xFA, 0x48, 0x60, 0x9C}};
constexpr GiGuid kIidIGameInputDevice = {0x31DD86FBu, 0x4C1Bu, 0x408Au, {0x86, 0x8F, 0x43, 0x9B, 0x3C, 0xD4, 0x71, 0x25}};
constexpr GiGuid kIidIGameInputReading = {0x2156947Au, 0xE1FAu, 0x4DE0u, {0xA3, 0x0B, 0xD8, 0x12, 0x93, 0x1D, 0xBD, 0x8D}};
constexpr GiGuid kIidIGameInputDispatcher = {0x415EED2Eu, 0x98CBu, 0x42C2u, {0x8F, 0x28, 0xB9, 0x46, 0x01, 0x07, 0x4E, 0x31}};

bool GiGuidIs(uintptr_t riid, const GiGuid& guid) {
    return riid && std::memcmp(reinterpret_cast<const void*>(riid), &guid, sizeof(guid)) == 0;
}

struct GiDeviceProxy;

struct ReadingEntry {
    uint64_t handle = 0;
    uint64_t device_id = 0;
    uint32_t input_kind = 0;
    uint64_t timestamp_us = 0;
    GAMEINPUT_GAMEPAD_STATE gamepad{};
    std::vector<GAMEINPUT_KEY_STATE> keys;
    GAMEINPUT_MOUSE_STATE mouse{};
};

struct ComReadingSnapshot {
    uint64_t sequence = 0;
    uint64_t timestamp_us = 0;
    uint32_t input_kind = 0;
    GiDeviceProxy* device = nullptr;
    GAMEINPUT_GAMEPAD_STATE gamepad{};
    std::vector<GAMEINPUT_KEY_STATE> keys;
    GAMEINPUT_MOUSE_STATE mouse{};
};

using ReadingCallbackFn = void(SCARLETTX_GI_CALL*)(
    uint64_t callbackToken,
    void* context,
    void* reading,
    bool hasOverrunOccurred);

struct ReadingCallbackRegistration {
    uint64_t token = 0;
    void* device = nullptr;
    uint32_t input_filter = 0;
    float analog_threshold = 0.0f;
    void* context = nullptr;
    ReadingCallbackFn callback = nullptr;
    bool active = true;
};

using DeviceCallbackFn = void(SCARLETTX_GI_CALL*)(
    uint64_t callbackToken,
    void* context,
    void* device,
    uint64_t timestamp,
    uint32_t currentStatus,
    uint32_t previousStatus);

struct DeviceCallbackRegistration {
    uint64_t token = 0;
    void* device = nullptr;
    uint32_t input_filter = 0;
    uint32_t status_filter = 0;
    uint32_t enum_kind = 0;
    void* context = nullptr;
    DeviceCallbackFn callback = nullptr;
    bool active = true;
};

struct GameInputObject {
    // First field is the COM vtable pointer so real Xbox binaries can call
    // IGameInput methods directly through the pointer returned by GameInputCreate,
    // while synthetic C tests can still treat it as an opaque uint64_t handle.
    const GiComFn* lpVtbl = nullptr;
    std::atomic<uint32_t> ref_count{1};
    std::atomic<uint64_t> next_callback_token{1};
    std::mutex mu;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    uint64_t next_reading_id = 1;
    std::vector<GAMEINPUT_DEVICE_INFO> devices;
    std::unordered_map<uint64_t, GAMEINPUT_RUMBLE_PARAMS> rumble;
    std::deque<ReadingEntry> history;
    std::deque<ComReadingSnapshot> com_history;
    std::vector<ReadingCallbackRegistration> reading_callbacks;
    std::vector<DeviceCallbackRegistration> device_callbacks;
    std::deque<std::function<void()>> dispatcher_jobs;

    // State behind guest-facing readings (IGameInput::GetCurrentReading).
    bool gamepad_injected = false;
    GAMEINPUT_GAMEPAD_STATE injected_gamepad{};
    GAMEINPUT_GAMEPAD_STATE last_gamepad{};
    uint64_t gamepad_sequence = 1;

    bool keyboard_injected = false;
    std::vector<GAMEINPUT_KEY_STATE> injected_keys;
    std::vector<GAMEINPUT_KEY_STATE> last_keys;
    uint64_t keyboard_sequence = 1;

    GAMEINPUT_MOUSE_STATE last_mouse{};
    uint64_t mouse_sequence = 1;
    uint32_t focus_policy = 0;

    uint64_t NowUs() const {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) + 1;
    }

    void PushReading(ReadingEntry entry) {
        entry.handle = next_reading_id++;
        entry.timestamp_us = NowUs();
        history.push_back(std::move(entry));
        if (history.size() > 256) {
            history.pop_front();
        }
    }

    void PushComSnapshot(ComReadingSnapshot snap) {
        if (!com_history.empty() &&
            com_history.back().device == snap.device &&
            com_history.back().input_kind == snap.input_kind &&
            com_history.back().sequence == snap.sequence) {
            return;
        }
        com_history.push_back(std::move(snap));
        if (com_history.size() > 256) {
            com_history.pop_front();
        }
    }
};

struct InputState {
    std::mutex mu;
    std::unordered_map<uint64_t, std::shared_ptr<GameInputObject>> instances;
};

InputState& State() {
    static InputState s;
    return s;
}

std::shared_ptr<GameInputObject> GetInstance(uint64_t handle) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().instances.find(handle);
    return it == State().instances.end() ? nullptr : it->second;
}

intptr_t SCARLETTX_GI_CALL GiQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!obj || !riid) {
        return kE_INVALIDARG;
    }
    if (!GiGuidIs(riid, kIidIUnknown) &&
        !GiGuidIs(riid, kIidIAgileObject) &&
        !GiGuidIs(riid, kIidIGameInputV0) &&
        !GiGuidIs(riid, kIidIGameInputV1) &&
        !GiGuidIs(riid, kIidIGameInputV2)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "IGameInput", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return kE_NOINTERFACE;
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL GiAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    return obj ? static_cast<intptr_t>(obj->ref_count.fetch_add(1, std::memory_order_relaxed) + 1) : 0;
}

intptr_t SCARLETTX_GI_CALL GiRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    if (!obj) return 0;
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        std::lock_guard<std::mutex> lk(State().mu);
        State().instances.erase(reinterpret_cast<uint64_t>(obj));
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GI_CALL GiGetCurrentTimestamp(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    return obj ? static_cast<intptr_t>(obj->NowUs()) : 0;
}

// ---------------------------------------------------------------------------
// Guest-facing IGameInputDevice, IGameInputReading, and IGameInputDispatcher.
// ---------------------------------------------------------------------------
constexpr size_t kDeviceInfoSize = 0x180;
constexpr size_t kInfoOffVendorId = 0x04;
constexpr size_t kInfoOffProductId = 0x06;
constexpr size_t kInfoOffDeviceId = 0x20;  // 32-byte APP_LOCAL_DEVICE_ID
constexpr size_t kInfoOffSupportedInput = 0x68;
constexpr size_t kInfoOffName = 0x130;
constexpr uint32_t kDeviceConnected = 0x1u;
constexpr uint32_t kDeviceInputEnabled = 0x2u;
constexpr uint32_t kPadStatus = kDeviceConnected | kDeviceInputEnabled;
constexpr uint32_t kPadSupportedInput =
    GAMEINPUT_KIND_GAMEPAD | GAMEINPUT_KIND_CONTROLLER_BUTTON | GAMEINPUT_KIND_CONTROLLER_AXIS;

struct GiString {
    uint32_t size_in_bytes;
    uint32_t code_point_count;
    const char* data;
};

struct GiDeviceProxy {
    const GiComFn* lpVtbl = nullptr;
    std::atomic<uint32_t> ref_count{1};
    GiString name{};
    alignas(8) uint8_t info[kDeviceInfoSize] = {};
    std::mutex rumble_mu;
    GAMEINPUT_RUMBLE_PARAMS current_rumble{};
    uint64_t rumble_transitions = 0;
};

GAMEINPUT_RUMBLE_PARAMS SanitizeRumbleParams(const GAMEINPUT_RUMBLE_PARAMS* params) {
    GAMEINPUT_RUMBLE_PARAMS out{};
    if (!params) return out;
    const auto clamp_motor = [](float v) -> float {
        if (!std::isfinite(v)) return 0.0f;
        return std::clamp(v, 0.0f, 1.0f);
    };
    out.LowFrequency = clamp_motor(params->LowFrequency);
    out.HighFrequency = clamp_motor(params->HighFrequency);
    out.LeftTrigger = clamp_motor(params->LeftTrigger);
    out.RightTrigger = clamp_motor(params->RightTrigger);
    return out;
}

struct GiReadingProxy {
    const GiComFn* lpVtbl = nullptr;
    std::atomic<uint32_t> ref_count{1};
    GiDeviceProxy* device = nullptr;
    uint32_t input_kind = 0;
    uint64_t sequence = 0;
    uint64_t timestamp = 0;
    GAMEINPUT_GAMEPAD_STATE gamepad{};
    std::vector<GAMEINPUT_KEY_STATE> keys;
    GAMEINPUT_MOUSE_STATE mouse{};
};

static_assert(sizeof(GAMEINPUT_GAMEPAD_STATE) == 0x1c, "the console copies 28 bytes of gamepad state");
static_assert(sizeof(GAMEINPUT_MOUSE_STATE) == 0x28, "GDK GameInputMouseState is 40 bytes");

GiDeviceProxy& VirtualGamepad();
GiDeviceProxy& VirtualKeyboard();
GiDeviceProxy& VirtualMouse();

intptr_t SCARLETTX_GI_CALL DevQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!GiGuidIs(riid, kIidIUnknown) && !GiGuidIs(riid, kIidIAgileObject) &&
        !GiGuidIs(riid, kIidIGameInputDevice)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "IGameInputDevice", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return kE_NOINTERFACE;
    }
    static_cast<GiDeviceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL DevAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<GiDeviceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GI_CALL DevRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto& count = static_cast<GiDeviceProxy*>(self)->ref_count;
    uint32_t current = count.load(std::memory_order_relaxed);
    while (current > 1 && !count.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel)) {
    }
    return static_cast<intptr_t>(current > 1 ? current - 1 : 1);
}

intptr_t SCARLETTX_GI_CALL DevGetDeviceInfo(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return reinterpret_cast<intptr_t>(static_cast<GiDeviceProxy*>(self)->info);
}

intptr_t SCARLETTX_GI_CALL DevGetDeviceStatus(void*, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(kPadStatus);
}

intptr_t SCARLETTX_GI_CALL DevGetBatteryState(void*, uintptr_t out_state, uintptr_t, uintptr_t) {
    if (out_state) {
        auto* fields = reinterpret_cast<uint32_t*>(out_state);
        fields[0] = fields[1] = fields[2] = fields[3] = 0;
        fields[4] = 0xFFFFFFFFu;
    }
    return 0;
}

// Bool-returning / void-returning device methods must return 0 (false), never E_NOTIMPL (0x80004001 -> AL==1).
intptr_t SCARLETTX_GI_CALL DevZeroBoolOrVoid(void*, uintptr_t, uintptr_t, uintptr_t) {
    return 0;
}

std::atomic<uint32_t> g_host_pad_slot{0};

intptr_t SCARLETTX_GI_CALL DevSetRumbleState(void* self, uintptr_t params, uintptr_t, uintptr_t) {
    if (params != 0 && params < 0x10000) return 0;  // not a pointer
    const GAMEINPUT_RUMBLE_PARAMS clean =
        SanitizeRumbleParams(reinterpret_cast<const GAMEINPUT_RUMBLE_PARAMS*>(params));
    if (auto* dev = static_cast<GiDeviceProxy*>(self)) {
        std::lock_guard<std::mutex> lk(dev->rumble_mu);
        if (std::memcmp(&dev->current_rumble, &clean, sizeof(clean)) != 0) {
            dev->current_rumble = clean;
            ++dev->rumble_transitions;
        }
    }
#if defined(_WIN32)
    struct Vibration {
        uint16_t low, high;
    };
    using XInputSetStateFn = DWORD (WINAPI*)(DWORD, Vibration*);
    static const XInputSetStateFn set_state = [] {
        const HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        return module ? reinterpret_cast<XInputSetStateFn>(
                            reinterpret_cast<void*>(GetProcAddress(module, "XInputSetState")))
                      : nullptr;
    }();
    Vibration v{};
    v.low = static_cast<uint16_t>(clean.LowFrequency * 65535.0f + 0.5f);
    v.high = static_cast<uint16_t>(clean.HighFrequency * 65535.0f + 0.5f);
    if (set_state) set_state(g_host_pad_slot.load(), &v);
#endif
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GI_CALL DevTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] device::vtable[" << Slot << "](a1=0x" << std::hex << a1 << ", a2=0x"
                  << a2 << ", a3=0x" << a3 << std::dec << ") -> E_NOTIMPL\n";
    }
    return kE_NOTIMPL;
}

template <size_t... Is>
constexpr std::array<GiComFn, sizeof...(Is) + 3> MakeDeviceVtable(std::index_sequence<Is...>) {
    return {&DevQueryInterface, &DevAddRef, &DevRelease, &DevTracedSlot<static_cast<int>(Is + 3)>...};
}

std::array<GiComFn, 22> BuildDeviceVtable() {
    auto vtbl = MakeDeviceVtable(std::make_index_sequence<19>{});
    vtbl[3] = &DevGetDeviceInfo;
    vtbl[4] = &DevGetDeviceStatus;
    vtbl[5] = &DevGetBatteryState;
    vtbl[7] = &DevZeroBoolOrVoid;   // IsForceFeedbackMotorPoweredOn -> bool (false)
    vtbl[8] = &DevZeroBoolOrVoid;   // SetForceFeedbackMotorGain -> void
    vtbl[10] = &DevSetRumbleState;  // SetRumbleState -> void
    vtbl[11] = &DevZeroBoolOrVoid;  // SetInputSynchronizationState -> void
    vtbl[12] = &DevZeroBoolOrVoid;  // SendInputSynchronizationHint -> void
    vtbl[13] = &DevZeroBoolOrVoid;  // PowerOff -> void
    vtbl[20] = &DevZeroBoolOrVoid;  // AcquireExclusiveRawDeviceAccess -> bool (false)
    vtbl[21] = &DevZeroBoolOrVoid;  // ReleaseExclusiveRawDeviceAccess -> void
    return vtbl;
}

const auto kGiDeviceVtbl = BuildDeviceVtable();

GiDeviceProxy* InitVirtualDevice(const char* display_name, const char id_str[32],
                                 uint16_t vendor, uint16_t product, uint32_t supported) {
    auto* d = new GiDeviceProxy();
    d->lpVtbl = kGiDeviceVtbl.data();
    const uint32_t name_len = static_cast<uint32_t>(std::strlen(display_name));
    d->name.size_in_bytes = name_len;
    d->name.code_point_count = name_len;
    d->name.data = display_name;
    const GiString* name_ptr = &d->name;
    std::memcpy(d->info + kInfoOffVendorId, &vendor, sizeof(vendor));
    std::memcpy(d->info + kInfoOffProductId, &product, sizeof(product));
    std::memcpy(d->info + kInfoOffDeviceId, id_str, 32);
    std::memcpy(d->info + kInfoOffSupportedInput, &supported, sizeof(supported));
    std::memcpy(d->info + kInfoOffName, &name_ptr, sizeof(name_ptr));
    return d;
}

GiDeviceProxy& VirtualGamepad() {
    static GiDeviceProxy* const device = [] {
        static const char kName[] = "ScarlettX Virtual Gamepad";
        static const char kId[32] = "ScarlettX.VirtualGamepad.0";
        return InitVirtualDevice(kName, kId, 0x045Eu, 0x0B12u, kPadSupportedInput);
    }();
    return *device;
}

GiDeviceProxy& VirtualKeyboard() {
    static GiDeviceProxy* const device = [] {
        static const char kName[] = "ScarlettX Virtual Keyboard";
        static const char kId[32] = "ScarlettX.VirtualKeyboard.0";
        return InitVirtualDevice(kName, kId, 0x045Eu, 0x0001u, GAMEINPUT_KIND_KEYBOARD);
    }();
    return *device;
}

GiDeviceProxy& VirtualMouse() {
    static GiDeviceProxy* const device = [] {
        static const char kName[] = "ScarlettX Virtual Mouse";
        static const char kId[32] = "ScarlettX.VirtualMouse.0";
        return InitVirtualDevice(kName, kId, 0x045Eu, 0x0002u, GAMEINPUT_KIND_MOUSE);
    }();
    return *device;
}

intptr_t SCARLETTX_GI_CALL ReadQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!GiGuidIs(riid, kIidIUnknown) && !GiGuidIs(riid, kIidIAgileObject) &&
        !GiGuidIs(riid, kIidIGameInputReading)) {
        return kE_NOINTERFACE;
    }
    static_cast<GiReadingProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL ReadAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<GiReadingProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GI_CALL ReadRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* reading = static_cast<GiReadingProxy*>(self);
    const uint32_t remaining = reading->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) delete reading;
    return static_cast<intptr_t>(remaining);
}

intptr_t SCARLETTX_GI_CALL ReadGetInputKind(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(static_cast<GiReadingProxy*>(self)->input_kind);
}

intptr_t SCARLETTX_GI_CALL ReadGetSequenceNumber(void* self, uintptr_t input_kind, uintptr_t, uintptr_t) {
    auto* reading = static_cast<GiReadingProxy*>(self);
    return (static_cast<uint32_t>(input_kind) & reading->input_kind) ? static_cast<intptr_t>(reading->sequence) : 0;
}

intptr_t SCARLETTX_GI_CALL ReadGetTimestamp(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(static_cast<GiReadingProxy*>(self)->timestamp);
}

intptr_t SCARLETTX_GI_CALL ReadGetDevice(void* self, uintptr_t out_device, uintptr_t, uintptr_t) {
    if (out_device) {
        auto* reading = static_cast<GiReadingProxy*>(self);
        GiDeviceProxy* dev = reading && reading->device ? reading->device : &VirtualGamepad();
        dev->ref_count.fetch_add(1, std::memory_order_relaxed);
        *reinterpret_cast<void**>(out_device) = dev;
    }
    return 0;
}

intptr_t SCARLETTX_GI_CALL ReadGetRawReport(void*, uintptr_t out_report, uintptr_t, uintptr_t) {
    if (out_report) {
        *reinterpret_cast<void**>(out_report) = nullptr;
    }
    return 0;
}

intptr_t SCARLETTX_GI_CALL ReadGetKeyCount(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* reading = static_cast<GiReadingProxy*>(self);
    if (!reading || (reading->input_kind & GAMEINPUT_KIND_KEYBOARD) == 0) return 0;
    return static_cast<intptr_t>(reading->keys.size());
}

intptr_t SCARLETTX_GI_CALL ReadGetKeyState(void* self, uintptr_t max_count, uintptr_t out_array, uintptr_t) {
    auto* reading = static_cast<GiReadingProxy*>(self);
    if (!reading || (reading->input_kind & GAMEINPUT_KIND_KEYBOARD) == 0) return 0;
    const uint32_t cap = static_cast<uint32_t>(max_count);
    const uint32_t n = std::min<uint32_t>(cap, static_cast<uint32_t>(reading->keys.size()));
    if (out_array && n > 0) {
        std::memcpy(reinterpret_cast<void*>(out_array), reading->keys.data(), n * sizeof(GAMEINPUT_KEY_STATE));
    }
    return static_cast<intptr_t>(n);
}

intptr_t SCARLETTX_GI_CALL ReadGetMouseState(void* self, uintptr_t out_state, uintptr_t, uintptr_t) {
    auto* reading = static_cast<GiReadingProxy*>(self);
    if (!reading || !out_state || (reading->input_kind & GAMEINPUT_KIND_MOUSE) == 0) {
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(out_state), &reading->mouse, sizeof(GAMEINPUT_MOUSE_STATE));
    return 1;
}

intptr_t SCARLETTX_GI_CALL ReadGetGamepadState(void* self, uintptr_t out_state, uintptr_t, uintptr_t) {
    auto* reading = static_cast<GiReadingProxy*>(self);
    if (!reading || !out_state || (reading->input_kind & GAMEINPUT_KIND_GAMEPAD) == 0) {
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(out_state), &reading->gamepad, sizeof(GAMEINPUT_GAMEPAD_STATE));
    return 1;
}

template <int Slot>
intptr_t SCARLETTX_GI_CALL ReadEmptySlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] reading::vtable[" << Slot << "](a1=0x" << std::hex << a1 << ", a2=0x"
                  << a2 << ", a3=0x" << a3 << std::dec << ") -> 0\n";
    }
    return 0;
}

template <size_t... Is>
constexpr std::array<GiComFn, sizeof...(Is) + 3> MakeReadingVtable(std::index_sequence<Is...>) {
    return {&ReadQueryInterface, &ReadAddRef, &ReadRelease, &ReadEmptySlot<static_cast<int>(Is + 3)>...};
}

std::array<GiComFn, 29> BuildReadingVtable() {
    auto vtbl = MakeReadingVtable(std::make_index_sequence<26>{});
    vtbl[3] = &ReadGetInputKind;
    vtbl[4] = &ReadGetSequenceNumber;
    vtbl[5] = &ReadGetTimestamp;
    vtbl[6] = &ReadGetDevice;
    vtbl[7] = &ReadGetRawReport;
    vtbl[14] = &ReadGetKeyCount;
    vtbl[15] = &ReadGetKeyState;
    vtbl[16] = &ReadGetMouseState;
    vtbl[22] = &ReadGetGamepadState;
    return vtbl;
}

const auto kGiReadingVtbl = BuildReadingVtable();

// ---------------------------------------------------------------------------
// IGameInputDispatcher COM object (IGameInput::CreateDispatcher, slot 15)
// ---------------------------------------------------------------------------
struct GiDispatcherProxy {
    const GiComFn* lpVtbl = nullptr;
    std::atomic<uint32_t> ref_count{1};
    std::weak_ptr<GameInputObject> owner;
#if defined(_WIN32)
    HANDLE wait_event = nullptr;
#endif
    ~GiDispatcherProxy() {
#if defined(_WIN32)
        if (wait_event) {
            CloseHandle(wait_event);
            wait_event = nullptr;
        }
#endif
    }
};

intptr_t SCARLETTX_GI_CALL DispQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!self || !riid) return kE_INVALIDARG;
    if (!GiGuidIs(riid, kIidIUnknown) && !GiGuidIs(riid, kIidIAgileObject) &&
        !GiGuidIs(riid, kIidIGameInputDispatcher)) {
        return kE_NOINTERFACE;
    }
    static_cast<GiDispatcherProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL DispAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<GiDispatcherProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GI_CALL DispRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* disp = static_cast<GiDispatcherProxy*>(self);
    const uint32_t rem = disp->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete disp;
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GI_CALL DispDispatch(void* self, uintptr_t quota_us, uintptr_t, uintptr_t) {
    auto* disp = static_cast<GiDispatcherProxy*>(self);
    if (!disp) return 0;
    auto owner = disp->owner.lock();
    if (!owner) return 0;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lk(owner->mu);
            if (owner->dispatcher_jobs.empty()) {
                return 0;  // false: queue drained
            }
            job = std::move(owner->dispatcher_jobs.front());
            owner->dispatcher_jobs.pop_front();
        }
        if (job) job();
        const auto elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
        if (elapsed_us >= static_cast<uint64_t>(quota_us)) {
            std::lock_guard<std::mutex> lk(owner->mu);
            return owner->dispatcher_jobs.empty() ? 0 : 1;
        }
    }
}

intptr_t SCARLETTX_GI_CALL DispOpenWaitHandle(void* self, uintptr_t out_handle_ptr, uintptr_t, uintptr_t) {
    auto* out = reinterpret_cast<void**>(out_handle_ptr);
    if (!out) return kE_POINTER;
    *out = nullptr;
    auto* disp = static_cast<GiDispatcherProxy*>(self);
    if (!disp) return kE_INVALIDARG;
#if defined(_WIN32)
    if (!disp->wait_event) return kE_HANDLE;
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), disp->wait_event, GetCurrentProcess(),
                         &dup, 0, FALSE, DUPLICATE_SAME_ACCESS) || !dup) {
        return kE_OUTOFMEMORY;
    }
    *out = dup;
    return kS_OK;
#else
    return kE_NOTIMPL;
#endif
}

const std::array<GiComFn, 5> kGiDispatcherVtbl = {
    &DispQueryInterface,
    &DispAddRef,
    &DispRelease,
    &DispDispatch,
    &DispOpenWaitHandle,
};

// ---------------------------------------------------------------------------
// Host input polling helpers
// ---------------------------------------------------------------------------
bool HasFocusedProcessGuiWindow() {
#if defined(_WIN32)
    const HWND fg = GetForegroundWindow();
    if (!fg || fg == GetConsoleWindow()) {
        return false;
    }
    DWORD focused_process = 0;
    GetWindowThreadProcessId(fg, &focused_process);
    return focused_process == GetCurrentProcessId();
#else
    return false;
#endif
}

void ReadHostGamepad(GAMEINPUT_GAMEPAD_STATE* out) {
    *out = GAMEINPUT_GAMEPAD_STATE{};
#if defined(_WIN32)
    struct XInputPad {
        uint16_t buttons;
        uint8_t left_trigger, right_trigger;
        int16_t lx, ly, rx, ry;
    };
    struct XInputStateBlock {
        uint32_t packet;
        XInputPad pad;
    };
    using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XInputStateBlock*);
    static const XInputGetStateFn get_state = [] {
        const HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        return module ? reinterpret_cast<XInputGetStateFn>(
                            reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")))
                      : nullptr;
    }();
    static std::atomic<ULONGLONG> next_scan{0};
    XInputStateBlock host{};
    bool connected = get_state && get_state(g_host_pad_slot.load(), &host) == 0;
    if (!connected && get_state && GetTickCount64() >= next_scan.load()) {
        next_scan.store(GetTickCount64() + 1000);
        for (DWORD i = 0; i < 4 && !connected; ++i) {
            connected = get_state(i, &host) == 0;
            if (connected) g_host_pad_slot.store(i);
        }
    }
    if (connected) {
        static const struct {
            uint16_t xinput;
            uint32_t gameinput;
        } kButtons[] = {
            {0x0001, GAMEINPUT_GAMEPAD_DPAD_UP},         {0x0002, GAMEINPUT_GAMEPAD_DPAD_DOWN},
            {0x0004, GAMEINPUT_GAMEPAD_DPAD_LEFT},       {0x0008, GAMEINPUT_GAMEPAD_DPAD_RIGHT},
            {0x0010, GAMEINPUT_GAMEPAD_MENU},            {0x0020, GAMEINPUT_GAMEPAD_VIEW},
            {0x0040, GAMEINPUT_GAMEPAD_LEFT_THUMBSTICK}, {0x0080, GAMEINPUT_GAMEPAD_RIGHT_THUMBSTICK},
            {0x0100, GAMEINPUT_GAMEPAD_LEFT_SHOULDER},   {0x0200, GAMEINPUT_GAMEPAD_RIGHT_SHOULDER},
            {0x1000, GAMEINPUT_GAMEPAD_A},               {0x2000, GAMEINPUT_GAMEPAD_B},
            {0x4000, GAMEINPUT_GAMEPAD_X},               {0x8000, GAMEINPUT_GAMEPAD_Y},
        };
        for (const auto& b : kButtons) {
            if (host.pad.buttons & b.xinput) out->Buttons |= b.gameinput;
        }
        const auto axis = [](int16_t v) { return std::max(-1.0f, static_cast<float>(v) / 32767.0f); };
        out->LeftTrigger = static_cast<float>(host.pad.left_trigger) / 255.0f;
        out->RightTrigger = static_cast<float>(host.pad.right_trigger) / 255.0f;
        out->LeftThumbstickX = axis(host.pad.lx);
        out->LeftThumbstickY = axis(host.pad.ly);
        out->RightThumbstickX = axis(host.pad.rx);
        out->RightThumbstickY = axis(host.pad.ry);
    }

    if (!HasFocusedProcessGuiWindow()) return;
    const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    static const struct {
        int key;
        uint32_t gameinput;
    } kKeys[] = {
        {VK_UP, GAMEINPUT_GAMEPAD_DPAD_UP},     {VK_DOWN, GAMEINPUT_GAMEPAD_DPAD_DOWN},
        {VK_LEFT, GAMEINPUT_GAMEPAD_DPAD_LEFT}, {VK_RIGHT, GAMEINPUT_GAMEPAD_DPAD_RIGHT},
        {VK_RETURN, GAMEINPUT_GAMEPAD_A},       {VK_SPACE, GAMEINPUT_GAMEPAD_A},
        {VK_ESCAPE, GAMEINPUT_GAMEPAD_B},       {VK_BACK, GAMEINPUT_GAMEPAD_B},
        {'X', GAMEINPUT_GAMEPAD_X},             {'Y', GAMEINPUT_GAMEPAD_Y},
        {'Q', GAMEINPUT_GAMEPAD_LEFT_SHOULDER}, {'E', GAMEINPUT_GAMEPAD_RIGHT_SHOULDER},
        {VK_TAB, GAMEINPUT_GAMEPAD_VIEW},       {'P', GAMEINPUT_GAMEPAD_MENU},
    };
    for (const auto& k : kKeys) {
        if (k.key == VK_RETURN && down(VK_MENU)) continue;
        if (down(k.key)) out->Buttons |= k.gameinput;
    }
    const auto stick = [&](int negative, int positive, float* value) {
        const float keys = (down(positive) ? 1.0f : 0.0f) - (down(negative) ? 1.0f : 0.0f);
        if (keys != 0.0f) *value = keys;
    };
    stick('A', 'D', &out->LeftThumbstickX);
    stick('S', 'W', &out->LeftThumbstickY);
    stick('J', 'L', &out->RightThumbstickX);
    stick('K', 'I', &out->RightThumbstickY);
    if (down('1')) out->LeftTrigger = 1.0f;
    if (down('3')) out->RightTrigger = 1.0f;
#endif
}

void ReadHostKeyboard(std::vector<GAMEINPUT_KEY_STATE>* out_keys) {
    out_keys->clear();
#if defined(_WIN32)
    if (!HasFocusedProcessGuiWindow()) return;
    for (int vk = 0x08; vk <= 0xFE && out_keys->size() < 32; ++vk) {
        if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) continue;
        if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
            GAMEINPUT_KEY_STATE ks{};
            ks.VirtualKey = static_cast<uint8_t>(vk);
            ks.ScanCode = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
            const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR);
            ks.CodePoint = ch & 0x7FFFFFFFu;
            ks.IsDeadKey = (ch & 0x80000000u) ? 1u : 0u;
            out_keys->push_back(ks);
        }
    }
#endif
}

void ReadHostMouse(GAMEINPUT_MOUSE_STATE* out_mouse) {
    *out_mouse = GAMEINPUT_MOUSE_STATE{};
#if defined(_WIN32)
    if (!HasFocusedProcessGuiWindow()) return;
    POINT pt{};
    if (GetCursorPos(&pt)) {
        out_mouse->PositionX = pt.x;
        out_mouse->PositionY = pt.y;
    }
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) out_mouse->Buttons |= GAMEINPUT_MOUSE_LEFT_BUTTON;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) out_mouse->Buttons |= GAMEINPUT_MOUSE_RIGHT_BUTTON;
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) out_mouse->Buttons |= GAMEINPUT_MOUSE_MIDDLE_BUTTON;
    if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) out_mouse->Buttons |= GAMEINPUT_MOUSE_BUTTON4;
    if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) out_mouse->Buttons |= GAMEINPUT_MOUSE_BUTTON5;
#endif
}

GiReadingProxy* CreateReadingFromSnapshot(const ComReadingSnapshot& snap) {
    auto* reading = new (std::nothrow) GiReadingProxy();
    if (!reading) return nullptr;
    reading->lpVtbl = kGiReadingVtbl.data();
    reading->device = snap.device;
    reading->input_kind = snap.input_kind;
    reading->sequence = snap.sequence;
    reading->timestamp = snap.timestamp_us;
    reading->gamepad = snap.gamepad;
    reading->keys = snap.keys;
    reading->mouse = snap.mouse;
    return reading;
}

bool SnapshotMatchesFilter(const ComReadingSnapshot& snap, uint32_t input_kind, uintptr_t device) {
    const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((snap.input_kind & input_kind) != 0);
    const bool dev_ok = (device == 0) || (reinterpret_cast<uintptr_t>(snap.device) == device);
    return kind_ok && dev_ok;
}

intptr_t SCARLETTX_GI_CALL GiGetCurrentReading(
    void* self, uintptr_t input_kind, uintptr_t device, uintptr_t ppReading) {
    auto* obj = static_cast<GameInputObject*>(self);
    auto** out = reinterpret_cast<void**>(ppReading);
    if (!obj || !out) return kE_INVALIDARG;
    *out = nullptr;

    const uint32_t kind = static_cast<uint32_t>(input_kind);
    const uintptr_t pad_ptr = reinterpret_cast<uintptr_t>(&VirtualGamepad());
    const uintptr_t kb_ptr = reinterpret_cast<uintptr_t>(&VirtualKeyboard());
    const uintptr_t mouse_ptr = reinterpret_cast<uintptr_t>(&VirtualMouse());
    if (device != 0 && device != pad_ptr && device != kb_ptr && device != mouse_ptr) {
        return kGAMEINPUT_E_READING_NOT_FOUND;
    }

    // 1. Gamepad path
    if ((kind & GAMEINPUT_KIND_GAMEPAD) != 0 && (device == 0 || device == pad_ptr)) {
        ComReadingSnapshot snap{};
        snap.device = &VirtualGamepad();
        snap.input_kind = GAMEINPUT_KIND_GAMEPAD;
        static std::atomic<bool> first_poll{true};
        bool changed = first_poll.exchange(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(obj->mu);
            GAMEINPUT_GAMEPAD_STATE now{};
            if (obj->gamepad_injected) {
                now = obj->injected_gamepad;
            } else {
                ReadHostGamepad(&now);
            }
            if (std::memcmp(&now, &obj->last_gamepad, sizeof(now)) != 0) {
                obj->last_gamepad = now;
                ++obj->gamepad_sequence;
                changed = true;
            }
            snap.gamepad = now;
            snap.sequence = obj->gamepad_sequence;
            snap.timestamp_us = obj->NowUs();
            obj->PushComSnapshot(snap);
        }
        if (changed && GameInputTraceEnabled()) {
            std::cerr << "[ScarlettX][GameInput] IGameInput::GetCurrentReading: gamepad buttons=0x"
                      << std::hex << snap.gamepad.Buttons << std::dec << " seq=" << snap.sequence << "\n";
        }
        GiReadingProxy* reading = CreateReadingFromSnapshot(snap);
        if (!reading) return kE_OUTOFMEMORY;
        *out = reading;
        return kS_OK;
    }

    const bool kbm_available = HasFocusedProcessGuiWindow() || GameInputKbmEnvEnabled();

    // 2. Keyboard path (active when VirtualKeyboard device explicitly queried or game window focused)
    if ((kind & GAMEINPUT_KIND_KEYBOARD) != 0 &&
        (device == kb_ptr || (device == 0 && kbm_available))) {
        ComReadingSnapshot snap{};
        snap.device = &VirtualKeyboard();
        snap.input_kind = GAMEINPUT_KIND_KEYBOARD;
        {
            std::lock_guard<std::mutex> lk(obj->mu);
            std::vector<GAMEINPUT_KEY_STATE> now_keys;
            if (obj->keyboard_injected) {
                now_keys = obj->injected_keys;
            } else {
                ReadHostKeyboard(&now_keys);
            }
            const bool same = now_keys.size() == obj->last_keys.size() &&
                              (now_keys.empty() ||
                               std::memcmp(now_keys.data(), obj->last_keys.data(),
                                           now_keys.size() * sizeof(GAMEINPUT_KEY_STATE)) == 0);
            if (!same) {
                obj->last_keys = now_keys;
                ++obj->keyboard_sequence;
            }
            snap.keys = std::move(now_keys);
            snap.sequence = obj->keyboard_sequence;
            snap.timestamp_us = obj->NowUs();
            obj->PushComSnapshot(snap);
        }
        GiReadingProxy* reading = CreateReadingFromSnapshot(snap);
        if (!reading) return kE_OUTOFMEMORY;
        *out = reading;
        return kS_OK;
    }

    // 3. Mouse path (active when VirtualMouse device explicitly queried or game window focused)
    if ((kind & GAMEINPUT_KIND_MOUSE) != 0 &&
        (device == mouse_ptr || (device == 0 && kbm_available))) {
        ComReadingSnapshot snap{};
        snap.device = &VirtualMouse();
        snap.input_kind = GAMEINPUT_KIND_MOUSE;
        {
            std::lock_guard<std::mutex> lk(obj->mu);
            GAMEINPUT_MOUSE_STATE now_mouse{};
            ReadHostMouse(&now_mouse);
            if (std::memcmp(&now_mouse, &obj->last_mouse, sizeof(now_mouse)) != 0) {
                obj->last_mouse = now_mouse;
                ++obj->mouse_sequence;
            }
            snap.mouse = now_mouse;
            snap.sequence = obj->mouse_sequence;
            snap.timestamp_us = obj->NowUs();
            obj->PushComSnapshot(snap);
        }
        GiReadingProxy* reading = CreateReadingFromSnapshot(snap);
        if (!reading) return kE_OUTOFMEMORY;
        *out = reading;
        return kS_OK;
    }

    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::GetCurrentReading(kind=0x" << std::hex << input_kind
                  << ", device=0x" << device << std::dec << ") -> GAMEINPUT_E_READING_NOT_FOUND\n";
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

// IGameInput::GetNextReading (slot 5)
intptr_t SCARLETTX_GI_CALL GiGetNextReading(
    void* self, uintptr_t ref_ptr, uintptr_t input_kind, uintptr_t device, uintptr_t ppReading) {
    auto* obj = static_cast<GameInputObject*>(self);
    auto** out = reinterpret_cast<void**>(ppReading);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!obj || !ref_ptr) return kE_INVALIDARG;
    auto* ref = reinterpret_cast<GiReadingProxy*>(ref_ptr);

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& snap : obj->com_history) {
        if (snap.sequence > ref->sequence &&
            SnapshotMatchesFilter(snap, static_cast<uint32_t>(input_kind), device)) {
            GiReadingProxy* reading = CreateReadingFromSnapshot(snap);
            if (!reading) return kE_OUTOFMEMORY;
            *out = reading;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

// IGameInput::GetPreviousReading (slot 6)
intptr_t SCARLETTX_GI_CALL GiGetPreviousReading(
    void* self, uintptr_t ref_ptr, uintptr_t input_kind, uintptr_t device, uintptr_t ppReading) {
    auto* obj = static_cast<GameInputObject*>(self);
    auto** out = reinterpret_cast<void**>(ppReading);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!obj || !ref_ptr) return kE_INVALIDARG;
    auto* ref = reinterpret_cast<GiReadingProxy*>(ref_ptr);

    std::lock_guard<std::mutex> lk(obj->mu);
    for (auto it = obj->com_history.rbegin(); it != obj->com_history.rend(); ++it) {
        if (it->sequence < ref->sequence &&
            SnapshotMatchesFilter(*it, static_cast<uint32_t>(input_kind), device)) {
            GiReadingProxy* reading = CreateReadingFromSnapshot(*it);
            if (!reading) return kE_OUTOFMEMORY;
            *out = reading;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

// IGameInput::GetTemporalReading (slot 7)
intptr_t SCARLETTX_GI_CALL GiGetTemporalReading(
    void* /*self*/, uintptr_t /*timestamp*/, uintptr_t /*device*/, uintptr_t ppReading) {
    if (ppReading) {
        *reinterpret_cast<void**>(ppReading) = nullptr;
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

// IGameInput::RegisterReadingCallback (slot 8)
// Note: 4th parameter `analogThreshold` is `float` (passed in XMM3 on Windows x64 ABI!).
int32_t SCARLETTX_GI_CALL GiRegisterReadingCallback(
    void* self,
    void* device,
    uint32_t input_filter,
    float analog_threshold,
    void* context,
    ReadingCallbackFn callback_fn,
    uint64_t* out_token) {
    auto* obj = static_cast<GameInputObject*>(self);
    if (!obj || !callback_fn) {
        if (out_token) *out_token = 0;
        return kE_INVALIDARG;
    }
    const uint64_t tok = obj->next_callback_token.fetch_add(1, std::memory_order_relaxed);
    if (out_token) {
        *out_token = tok;
    }
    std::lock_guard<std::mutex> lk(obj->mu);
    obj->reading_callbacks.push_back(
        {tok, device, input_filter, analog_threshold, context, callback_fn, true});
    return kS_OK;
}

// IGameInput::RegisterDeviceCallback (vtable[9] / +0x48)
intptr_t SCARLETTX_GI_CALL GiRegisterDeviceCallback(
    void* self, uintptr_t device, uintptr_t input_filter, uintptr_t status_filter,
    uintptr_t enum_kind, uintptr_t context, uintptr_t callback_fn, uintptr_t out_token_ptr) {
    auto* obj = static_cast<GameInputObject*>(self);
    if (!obj || !callback_fn) {
        if (out_token_ptr) {
            *reinterpret_cast<uint64_t*>(out_token_ptr) = 0;
        }
        return kE_INVALIDARG;
    }
    const uint64_t tok = obj->next_callback_token.fetch_add(1, std::memory_order_relaxed);
    if (out_token_ptr) {
        *reinterpret_cast<uint64_t*>(out_token_ptr) = tok;
    }
    const uint32_t in_filter32 = static_cast<uint32_t>(input_filter);
    const uint32_t st_filter32 = static_cast<uint32_t>(status_filter);
    const uint32_t enum32 = static_cast<uint32_t>(enum_kind);
    auto cb = reinterpret_cast<DeviceCallbackFn>(callback_fn);
    {
        std::lock_guard<std::mutex> lk(obj->mu);
        obj->device_callbacks.push_back({
            tok,
            reinterpret_cast<void*>(device),
            in_filter32,
            st_filter32,
            enum32,
            reinterpret_cast<void*>(context),
            cb,
            true,
        });
    }
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::RegisterDeviceCallback(kind=0x"
                  << std::hex << input_filter << ", status=0x" << status_filter
                  << ", enum=" << std::dec << enum32 << ", context=0x" << std::hex
                  << context << ", callback=0x" << callback_fn << std::dec << ") -> S_OK (tok=" << tok << ")\n";
    }
    GiDeviceProxy& pad = VirtualGamepad();
    const bool matches = (device == 0 || device == reinterpret_cast<uintptr_t>(&pad)) &&
                         (in_filter32 & kPadSupportedInput) != 0 &&
                         (st_filter32 & kPadStatus) != 0;
    if (enum32 != 0 && matches) {
        cb(tok, reinterpret_cast<void*>(context), &pad, obj->NowUs(), kPadStatus, 0);
    }
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL GiStopCallback(void* self, uintptr_t tok, uintptr_t, uintptr_t) {
    if (auto* obj = static_cast<GameInputObject*>(self)) {
        std::lock_guard<std::mutex> lk(obj->mu);
        for (auto& cb : obj->reading_callbacks) {
            if (cb.token == static_cast<uint64_t>(tok)) cb.active = false;
        }
        for (auto& cb : obj->device_callbacks) {
            if (cb.token == static_cast<uint64_t>(tok)) cb.active = false;
        }
    }
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::StopCallback(tok=" << tok << ")\n";
    }
    return 0;
}

intptr_t SCARLETTX_GI_CALL GiUnregisterCallback(void* self, uintptr_t tok, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    if (!obj || tok == 0 || tok >= obj->next_callback_token.load(std::memory_order_relaxed)) {
        return 0;
    }
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(obj->mu);
        const size_t before_r = obj->reading_callbacks.size();
        obj->reading_callbacks.erase(
            std::remove_if(obj->reading_callbacks.begin(), obj->reading_callbacks.end(),
                           [tok](const ReadingCallbackRegistration& r) { return r.token == tok; }),
            obj->reading_callbacks.end());
        if (obj->reading_callbacks.size() != before_r) {
            removed = true;
        }
        const size_t before_d = obj->device_callbacks.size();
        obj->device_callbacks.erase(
            std::remove_if(obj->device_callbacks.begin(), obj->device_callbacks.end(),
                           [tok](const DeviceCallbackRegistration& r) { return r.token == tok; }),
            obj->device_callbacks.end());
        if (obj->device_callbacks.size() != before_d) {
            removed = true;
        }
    }
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::UnregisterCallback(tok=" << tok << ") -> "
                  << (removed ? "true" : "false") << "\n";
    }
    return removed ? 1 : 0;
}

// IGameInput::CreateDispatcher (slot 15)
intptr_t SCARLETTX_GI_CALL GiCreateDispatcher(void* self, uintptr_t out_disp_ptr, uintptr_t, uintptr_t) {
    auto** out = reinterpret_cast<void**>(out_disp_ptr);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!self) return kE_INVALIDARG;
    auto owner = GetInstance(reinterpret_cast<uint64_t>(self));
    auto* disp = new (std::nothrow) GiDispatcherProxy();
    if (!disp) return kE_OUTOFMEMORY;
    disp->lpVtbl = kGiDispatcherVtbl.data();
    disp->owner = owner;
#if defined(_WIN32)
    disp->wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!disp->wait_event) {
        delete disp;
        return kE_OUTOFMEMORY;
    }
#endif
    *out = disp;
    return kS_OK;
}

// IGameInput::FindDeviceFromId (slot 17)
intptr_t SCARLETTX_GI_CALL GiFindDeviceFromId(void* self, uintptr_t id_ptr, uintptr_t out_dev_ptr, uintptr_t) {
    auto** out = reinterpret_cast<void**>(out_dev_ptr);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!self || !id_ptr) return kE_INVALIDARG;
    const auto* raw = reinterpret_cast<const uint8_t*>(id_ptr);

    GiDeviceProxy* candidates[3] = {&VirtualGamepad(), &VirtualKeyboard(), &VirtualMouse()};
    for (GiDeviceProxy* dev : candidates) {
        if (std::memcmp(raw, dev->info + kInfoOffDeviceId, 32) == 0) {
            dev->ref_count.fetch_add(1, std::memory_order_relaxed);
            *out = dev;
            return kS_OK;
        }
    }
    // Also support 8-byte numeric DeviceId in the low 8 bytes with zero padding
    uint64_t low_id = 0;
    std::memcpy(&low_id, raw, sizeof(low_id));
    bool rest_zero = true;
    for (size_t i = 8; i < 32; ++i) {
        if (raw[i] != 0) { rest_zero = false; break; }
    }
    if (rest_zero) {
        GiDeviceProxy* matched = nullptr;
        if (low_id == kVirtualGamepadId) matched = &VirtualGamepad();
        else if (low_id == kVirtualKeyboardId) matched = &VirtualKeyboard();
        else if (low_id == kVirtualMouseId) matched = &VirtualMouse();
        if (matched) {
            matched->ref_count.fetch_add(1, std::memory_order_relaxed);
            *out = matched;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_DEVICE_NOT_FOUND;
}

// IGameInput::FindDeviceFromObject (slot 18)
intptr_t SCARLETTX_GI_CALL GiFindDeviceFromObject(void* self, uintptr_t obj_ptr, uintptr_t out_dev_ptr, uintptr_t) {
    auto** out = reinterpret_cast<void**>(out_dev_ptr);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!self || !obj_ptr) return kE_INVALIDARG;
    GiDeviceProxy* candidates[3] = {&VirtualGamepad(), &VirtualKeyboard(), &VirtualMouse()};
    for (GiDeviceProxy* dev : candidates) {
        if (obj_ptr == reinterpret_cast<uintptr_t>(dev)) {
            dev->ref_count.fetch_add(1, std::memory_order_relaxed);
            *out = dev;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_DEVICE_NOT_FOUND;
}

// IGameInput::FindDeviceFromPlatformHandle (slot 19)
intptr_t SCARLETTX_GI_CALL GiFindDeviceFromPlatformHandle(void* self, uintptr_t handle_val, uintptr_t out_dev_ptr, uintptr_t) {
    auto** out = reinterpret_cast<void**>(out_dev_ptr);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!self || !handle_val) return kE_INVALIDARG;
    return kGAMEINPUT_E_DEVICE_NOT_FOUND;
}

// IGameInput::FindDeviceFromPlatformString (slot 20)
intptr_t SCARLETTX_GI_CALL GiFindDeviceFromPlatformString(void* self, uintptr_t wstr_ptr, uintptr_t out_dev_ptr, uintptr_t) {
    auto** out = reinterpret_cast<void**>(out_dev_ptr);
    if (!out) return kE_POINTER;
    *out = nullptr;
    const auto* ws = reinterpret_cast<const wchar_t*>(wstr_ptr);
    if (!self || !ws || !*ws) return kE_INVALIDARG;
    std::wstring lower;
    for (const wchar_t* p = ws; *p; ++p) {
        lower.push_back(static_cast<wchar_t>(std::towlower(*p)));
    }
    GiDeviceProxy* matched = nullptr;
    if (lower.find(L"keyboard") != std::wstring::npos || lower.find(L"kbd") != std::wstring::npos) {
        matched = &VirtualKeyboard();
    } else if (lower.find(L"mouse") != std::wstring::npos) {
        matched = &VirtualMouse();
    } else if (lower.find(L"gamepad") != std::wstring::npos ||
               lower.find(L"controller") != std::wstring::npos ||
               lower.find(L"xbox") != std::wstring::npos ||
               lower.find(L"xinput") != std::wstring::npos ||
               lower.find(L"scarlett") != std::wstring::npos) {
        matched = &VirtualGamepad();
    }
    if (!matched) {
        return kGAMEINPUT_E_DEVICE_NOT_FOUND;
    }
    matched->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = matched;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL GiSetFocusPolicy(void* self, uintptr_t policy, uintptr_t, uintptr_t) {
    if (auto* obj = static_cast<GameInputObject*>(self)) {
        std::lock_guard<std::mutex> lk(obj->mu);
        obj->focus_policy = static_cast<uint32_t>(policy);
    }
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::SetFocusPolicy(0x"
                  << std::hex << policy << std::dec << ")\n";
    }
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GI_CALL GiTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return kE_NOTIMPL;
}

template <size_t... Is>
constexpr std::array<GiComFn, sizeof...(Is) + 3> MakeGiVtable(std::index_sequence<Is...>) {
    return {
        &GiQueryInterface,
        &GiAddRef,
        &GiRelease,
        &GiTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<GiComFn, 32> BuildGiVtable() {
    auto vtbl = MakeGiVtable(std::make_index_sequence<29>{});
    vtbl[3] = &GiGetCurrentTimestamp;
    vtbl[4] = &GiGetCurrentReading;
    vtbl[5] = reinterpret_cast<GiComFn>(&GiGetNextReading);
    vtbl[6] = reinterpret_cast<GiComFn>(&GiGetPreviousReading);
    vtbl[7] = &GiGetTemporalReading;
    vtbl[8] = reinterpret_cast<GiComFn>(&GiRegisterReadingCallback);
    vtbl[9] = reinterpret_cast<GiComFn>(&GiRegisterDeviceCallback);
    vtbl[13] = &GiStopCallback;
    vtbl[14] = &GiUnregisterCallback;
    vtbl[15] = &GiCreateDispatcher;
    vtbl[17] = &GiFindDeviceFromId;
    vtbl[18] = &GiFindDeviceFromObject;
    vtbl[19] = &GiFindDeviceFromPlatformHandle;
    vtbl[20] = &GiFindDeviceFromPlatformString;
    vtbl[22] = &GiSetFocusPolicy;
    return vtbl;
}

const auto kIGameInputVtbl = BuildGiVtable();

}  // namespace

extern "C" {

int32_t GameInputCreate(uint64_t* out_game_input) {
    if (!out_game_input) return kE_INVALIDARG;
    auto obj = std::make_shared<GameInputObject>();
    obj->lpVtbl = kIGameInputVtbl.data();

    GAMEINPUT_DEVICE_INFO pad{};
    pad.DeviceId = kVirtualGamepadId;
    pad.VendorId = 0x045Eu;   // Microsoft
    pad.ProductId = 0x0B12u;  // Xbox Series X|S Wireless Controller
    pad.SupportedInput = GAMEINPUT_KIND_GAMEPAD | GAMEINPUT_KIND_CONTROLLER_BUTTON |
                         GAMEINPUT_KIND_CONTROLLER_AXIS;
    pad.SupportedRumbleMotors = 0x0Fu;  // Low, High, LeftTrigger, RightTrigger
    std::strncpy(pad.DisplayName, "ScarlettX Virtual Xbox Series Controller", sizeof(pad.DisplayName) - 1);
    obj->devices.push_back(pad);

    GAMEINPUT_DEVICE_INFO kb{};
    kb.DeviceId = kVirtualKeyboardId;
    kb.VendorId = 0x045Eu;
    kb.ProductId = 0x0001u;
    kb.SupportedInput = GAMEINPUT_KIND_KEYBOARD;
    kb.SupportedRumbleMotors = 0;
    std::strncpy(kb.DisplayName, "ScarlettX Virtual Keyboard", sizeof(kb.DisplayName) - 1);
    obj->devices.push_back(kb);

    ReadingEntry pad_init{};
    pad_init.device_id = kVirtualGamepadId;
    pad_init.input_kind = GAMEINPUT_KIND_GAMEPAD;
    obj->PushReading(std::move(pad_init));

    ReadingEntry kb_init{};
    kb_init.device_id = kVirtualKeyboardId;
    kb_init.input_kind = GAMEINPUT_KIND_KEYBOARD;
    obj->PushReading(std::move(kb_init));

    ComReadingSnapshot com_pad_init{};
    com_pad_init.sequence = obj->gamepad_sequence;
    com_pad_init.timestamp_us = obj->NowUs();
    com_pad_init.input_kind = GAMEINPUT_KIND_GAMEPAD;
    com_pad_init.device = &VirtualGamepad();
    obj->PushComSnapshot(std::move(com_pad_init));

    const uint64_t h = reinterpret_cast<uint64_t>(obj.get());
    {
        std::lock_guard<std::mutex> lk(State().mu);
        State().instances[h] = std::move(obj);
    }
    *out_game_input = h;
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] GameInputCreate() -> S_OK (IGameInput*=0x"
                  << std::hex << h << std::dec << ")\n";
    }
    return kS_OK;
}

int32_t GameInputDestroy(uint64_t game_input) {
    std::lock_guard<std::mutex> lk(State().mu);
    return State().instances.erase(game_input) ? kS_OK : kE_HANDLE;
}

uint64_t GameInputGetCurrentTimestamp(uint64_t game_input) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    return obj->NowUs();
}

int32_t GameInputEnumerateDevices(
    uint64_t game_input,
    uint32_t input_kind,
    uint64_t* out_device_ids,
    uint32_t capacity,
    uint32_t* out_count) {
    if (!out_count) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    uint32_t count = 0;
    for (const auto& d : obj->devices) {
        if (input_kind == GAMEINPUT_KIND_UNKNOWN || (d.SupportedInput & input_kind) != 0) {
            if (out_device_ids && count < capacity) {
                out_device_ids[count] = d.DeviceId;
            }
            ++count;
        }
    }
    *out_count = count;
    return kS_OK;
}

int32_t GameInputGetDeviceInfo(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_DEVICE_INFO* out_info) {
    if (!out_info) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& d : obj->devices) {
        if (d.DeviceId == device_id) {
            *out_info = d;
            return kS_OK;
        }
    }
    return kE_INVALIDARG;
}

int32_t GameInputFindDeviceFromId(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_DEVICE_INFO* out_info) {
    return GameInputGetDeviceInfo(game_input, device_id, out_info);
}

int32_t GameInputGetCurrentReading(
    uint64_t game_input,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (auto it = obj->history.rbegin(); it != obj->history.rend(); ++it) {
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((it->input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (it->device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = it->handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

int32_t GameInputGetNextReading(
    uint64_t game_input,
    uint64_t reference_reading,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle <= reference_reading) continue;
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((e.input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (e.device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = e.handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

int32_t GameInputGetPreviousReading(
    uint64_t game_input,
    uint64_t reference_reading,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (auto it = obj->history.rbegin(); it != obj->history.rend(); ++it) {
        if (it->handle >= reference_reading) continue;
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((it->input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (it->device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = it->handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

uint64_t GameInputGetReadingTimestamp(uint64_t game_input, uint64_t reading) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading) return e.timestamp_us;
    }
    return 0;
}

int32_t GameInputGetGamepadState(
    uint64_t game_input,
    uint64_t reading,
    GAMEINPUT_GAMEPAD_STATE* out_state) {
    if (!out_state) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading && e.input_kind == GAMEINPUT_KIND_GAMEPAD) {
            *out_state = e.gamepad;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

uint32_t GameInputGetKeyState(
    uint64_t game_input,
    uint64_t reading,
    uint32_t max_keys,
    GAMEINPUT_KEY_STATE* out_keys) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading && e.input_kind == GAMEINPUT_KIND_KEYBOARD) {
            const uint32_t n = std::min<uint32_t>(max_keys, static_cast<uint32_t>(e.keys.size()));
            if (out_keys && n > 0) {
                std::memcpy(out_keys, e.keys.data(), n * sizeof(GAMEINPUT_KEY_STATE));
            }
            return n;
        }
    }
    return 0;
}

int32_t GameInputSetRumbleState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_RUMBLE_PARAMS* params) {
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;
    const uint64_t resolved_id = (device_id == 0) ? kVirtualGamepadId : device_id;
    const GAMEINPUT_RUMBLE_PARAMS p = SanitizeRumbleParams(params);
    {
        std::lock_guard<std::mutex> lk(obj->mu);
        obj->rumble[resolved_id] = p;
    }
    if (resolved_id == kVirtualGamepadId) {
        DevSetRumbleState(&VirtualGamepad(), reinterpret_cast<uintptr_t>(&p), 0, 0);
    }
    return kS_OK;
}

int32_t GameInputGetRumbleState(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_RUMBLE_PARAMS* out_params) {
    if (!out_params) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;
    const uint64_t resolved_id = (device_id == 0) ? kVirtualGamepadId : device_id;
    if (resolved_id == kVirtualGamepadId) {
        GiDeviceProxy& pad = VirtualGamepad();
        std::lock_guard<std::mutex> lk(pad.rumble_mu);
        *out_params = pad.current_rumble;
        return kS_OK;
    }
    std::lock_guard<std::mutex> lk(obj->mu);
    auto it = obj->rumble.find(resolved_id);
    if (it == obj->rumble.end()) {
        *out_params = {};
    } else {
        *out_params = it->second;
    }
    return kS_OK;
}

int32_t GameInputInjectGamepadState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_GAMEPAD_STATE* state) {
    if (!state) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    ReadingEntry entry{};
    entry.device_id = device_id == 0 ? kVirtualGamepadId : device_id;
    entry.input_kind = GAMEINPUT_KIND_GAMEPAD;
    entry.gamepad = *state;
    entry.gamepad.LeftTrigger = std::clamp(entry.gamepad.LeftTrigger, 0.0f, 1.0f);
    entry.gamepad.RightTrigger = std::clamp(entry.gamepad.RightTrigger, 0.0f, 1.0f);
    entry.gamepad.LeftThumbstickX = std::clamp(entry.gamepad.LeftThumbstickX, -1.0f, 1.0f);
    entry.gamepad.LeftThumbstickY = std::clamp(entry.gamepad.LeftThumbstickY, -1.0f, 1.0f);
    entry.gamepad.RightThumbstickX = std::clamp(entry.gamepad.RightThumbstickX, -1.0f, 1.0f);
    entry.gamepad.RightThumbstickY = std::clamp(entry.gamepad.RightThumbstickY, -1.0f, 1.0f);

    std::vector<ReadingCallbackRegistration> cbs;
    ComReadingSnapshot snap{};
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(obj->mu);
        obj->injected_gamepad = entry.gamepad;
        obj->gamepad_injected = true;
        if (std::memcmp(&entry.gamepad, &obj->last_gamepad, sizeof(entry.gamepad)) != 0) {
            obj->last_gamepad = entry.gamepad;
            ++obj->gamepad_sequence;
            changed = true;
        }
        snap.sequence = obj->gamepad_sequence;
        snap.timestamp_us = obj->NowUs();
        snap.input_kind = GAMEINPUT_KIND_GAMEPAD;
        snap.device = &VirtualGamepad();
        snap.gamepad = entry.gamepad;
        obj->PushComSnapshot(snap);
        obj->PushReading(std::move(entry));
        if (changed) {
            cbs = obj->reading_callbacks;
        }
    }
    for (const auto& reg : cbs) {
        if (!reg.active || !reg.callback) continue;
        if ((reg.input_filter & GAMEINPUT_KIND_GAMEPAD) == 0) continue;
        if (reg.device && reg.device != &VirtualGamepad()) continue;
        GiReadingProxy* r = CreateReadingFromSnapshot(snap);
        if (r) {
            reg.callback(reg.token, reg.context, r, false);
            ReadRelease(r, 0, 0, 0);
        }
    }
    return kS_OK;
}

int32_t GameInputInjectKeyState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_KEY_STATE* keys,
    uint32_t key_count) {
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    ReadingEntry entry{};
    entry.device_id = device_id == 0 ? kVirtualKeyboardId : device_id;
    entry.input_kind = GAMEINPUT_KIND_KEYBOARD;
    if (keys && key_count > 0) {
        entry.keys.assign(keys, keys + key_count);
    }

    std::lock_guard<std::mutex> lk(obj->mu);
    obj->injected_keys = entry.keys;
    obj->keyboard_injected = true;
    ++obj->keyboard_sequence;
    ComReadingSnapshot snap{};
    snap.sequence = obj->keyboard_sequence;
    snap.timestamp_us = obj->NowUs();
    snap.input_kind = GAMEINPUT_KIND_KEYBOARD;
    snap.device = &VirtualKeyboard();
    snap.keys = entry.keys;
    obj->PushComSnapshot(std::move(snap));
    obj->PushReading(std::move(entry));
    return kS_OK;
}

}  // extern "C"
