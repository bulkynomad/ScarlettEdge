#include "scarlettx/core/flight_recorder.h"

#include "../loader/guest_modules.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#  include <tlhelp32.h>
#endif

namespace scarlettx {

std::atomic<uint8_t> FlightRecorder::s_enabled_state_{0};

const char* FlightEventCategoryName(FlightEventCategory cat) noexcept {
    switch (cat) {
        case FlightEventCategory::Lifecycle: return "Lifecycle";
        case FlightEventCategory::HleCall: return "HleCall";
        case FlightEventCategory::StubHit: return "StubHit";
        case FlightEventCategory::ComQiMiss: return "ComQiMiss";
        case FlightEventCategory::VtableSlotMiss: return "VtableSlotMiss";
        case FlightEventCategory::WinRtBroker: return "WinRtBroker";
        case FlightEventCategory::VfsAccess: return "VfsAccess";
        case FlightEventCategory::VfsMiss: return "VfsMiss";
        case FlightEventCategory::GpuBreadcrumb: return "GpuBreadcrumb";
        case FlightEventCategory::ShaderOrPsoFail: return "ShaderOrPsoFail";
        case FlightEventCategory::GpuDeviceRemoved: return "GpuDeviceRemoved";
        case FlightEventCategory::GuestFault: return "GuestFault";
        case FlightEventCategory::WatchdogHang: return "WatchdogHang";
        default: return "Unknown";
    }
}

namespace {

class ScopedSpinTryLock {
public:
    explicit ScopedSpinTryLock(std::atomic_flag& flag, int max_spins = 64) noexcept : flag_(flag) {
        for (int i = 0; i < max_spins; ++i) {
            if (!flag_.test_and_set(std::memory_order_acquire)) {
                locked_ = true;
                return;
            }
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
            _mm_pause();
#endif
        }
    }
    ~ScopedSpinTryLock() noexcept {
        if (locked_) {
            flag_.clear(std::memory_order_release);
        }
    }
    bool owns_lock() const noexcept { return locked_; }

private:
    std::atomic_flag& flag_;
    bool locked_ = false;
};

void CopyBoundedStr(char* dst, size_t dst_cap, const char* src) noexcept {
    if (!dst || dst_cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < dst_cap && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

std::string WideToUtf8Safe(const wchar_t* wide) {
    if (!wide || !*wide) return {};
#if defined(_WIN32)
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
#else
    std::string out;
    for (size_t i = 0; wide[i] != L'\0'; ++i) {
        out.push_back(wide[i] < 128 ? static_cast<char>(wide[i]) : '?');
    }
    return out;
#endif
}

void FormatGuidBytes(const void* guid_ptr, char (&out)[40]) noexcept {
    if (!guid_ptr) {
        CopyBoundedStr(out, sizeof(out), "{00000000-0000-0000-0000-000000000000}");
        return;
    }
    struct RawGuid {
        uint32_t d1;
        uint16_t d2;
        uint16_t d3;
        uint8_t d4[8];
    };
    RawGuid g{};
    std::memcpy(&g, guid_ptr, sizeof(g));
    std::snprintf(out, sizeof(out),
                  "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  g.d1, g.d2, g.d3,
                  g.d4[0], g.d4[1], g.d4[2], g.d4[3],
                  g.d4[4], g.d4[5], g.d4[6], g.d4[7]);
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8] = {};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string HexU64(uint64_t v) {
    char buf[24] = {};
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string HexU32(uint32_t v) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

uint64_t Fnv1a64(const void* data, size_t len) noexcept {
    uint64_t h = 14695981039346656037ull;
    const auto* p = static_cast<const uint8_t*>(data);
    if (!p) return h;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string SanitizeFilenameToken(const char* raw) {
    if (!raw || !*raw) return "unknown";
    std::string out;
    for (size_t i = 0; raw[i] != '\0' && out.size() < 32; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

uint32_t CurrentTid() noexcept {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentThreadId());
#else
    return 0;
#endif
}

uint32_t CurrentPid() noexcept {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

#if defined(_WIN32)

bool IsFatalHardwareOrFailFastException(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:          // 0xC0000005
        case EXCEPTION_IN_PAGE_ERROR:             // 0xC0000006
        case EXCEPTION_ILLEGAL_INSTRUCTION:       // 0xC000001D
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:  // 0xC0000025
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:     // 0xC000008C
        case EXCEPTION_INT_DIVIDE_BY_ZERO:        // 0xC0000094
        case EXCEPTION_INT_OVERFLOW:              // 0xC0000095
        case EXCEPTION_PRIV_INSTRUCTION:          // 0xC0000096
        case EXCEPTION_STACK_OVERFLOW:            // 0xC00000FD
        case 0xC0000374u:                         // STATUS_HEAP_CORRUPTION
        case 0xC0000409u:                         // STATUS_STACK_BUFFER_OVERRUN (__fastfail)
        case EXCEPTION_DATATYPE_MISALIGNMENT:     // 0x80000002
            return true;
        case EXCEPTION_BREAKPOINT:                // 0x80000003 (guest __debugbreak when no debugger)
            return !IsDebuggerPresent();
        default:
            return false;
    }
}

const char* ExceptionCodeName(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case 0xC0000374u: return "STATUS_HEAP_CORRUPTION";
        case 0xC0000409u: return "STATUS_STACK_BUFFER_OVERRUN";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
        default: return "UNKNOWN_EXCEPTION";
    }
}

bool IsCommittedReadableAddress(uint64_t va, size_t bytes) noexcept {
    if (va < 0x10000ull || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(va), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const uint64_t region_end =
        reinterpret_cast<uint64_t>(mbi.BaseAddress) + static_cast<uint64_t>(mbi.RegionSize);
    return va + bytes <= region_end;
}

bool IsExecutableCodeAddress(uint64_t va) noexcept {
    if (va < 0x10000ull) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(va), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) return false;
    constexpr DWORD kExecMask =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExecMask) != 0;
}

// Pure POD SEH helper (no C++ objects with destructors allowed in __try scope).
size_t UnwindX64FramesPod(const CONTEXT* initial_ctx,
                          uint64_t* out_rips,
                          uint64_t* out_rsps,
                          size_t max_frames) noexcept {
    if (!initial_ctx || !out_rips || !out_rsps || max_frames == 0) return 0;
    size_t count = 0;
    CONTEXT ctx = *initial_ctx;
    __try {
        for (size_t i = 0; i < max_frames; ++i) {
            const uint64_t cur_rip = ctx.Rip;
            const uint64_t cur_rsp = ctx.Rsp;
            if (i > 0 && cur_rip == 0) {
                break;
            }
            out_rips[count] = cur_rip;
            out_rsps[count] = cur_rsp;
            ++count;

            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(cur_rip, &image_base, nullptr);
            if (rf != nullptr) {
                PVOID handler_data = nullptr;
                DWORD64 establisher_frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                                 image_base,
                                 cur_rip,
                                 rf,
                                 &ctx,
                                 &handler_data,
                                 &establisher_frame,
                                 nullptr);
                if (ctx.Rip == 0 || (ctx.Rip == cur_rip && ctx.Rsp == cur_rsp)) {
                    break;
                }
            } else {
                // Leaf function, StubThunk trampoline, or wild jump to 0/unmapped VA:
                // on x64, [RSP] holds the return address pushed by CALL.
                if (!IsCommittedReadableAddress(cur_rsp, sizeof(uint64_t))) {
                    break;
                }
                uint64_t ret_addr = 0;
                std::memcpy(&ret_addr, reinterpret_cast<const void*>(cur_rsp), sizeof(uint64_t));
                if (ret_addr == 0 || ret_addr == cur_rip) {
                    break;
                }
                ctx.Rip = ret_addr;
                ctx.Rsp = cur_rsp + 8ull;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Stop unwinding cleanly if guest stack memory is corrupted.
    }
    return count;
}

std::atomic<uint32_t> g_active_crash_tid{0};

LONG WINAPI FlightVectoredExceptionHandler(PEXCEPTION_POINTERS ep) noexcept {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!FlightRecorder::IsEnabled()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsFatalHardwareOrFailFastException(code)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uint32_t tid = CurrentTid();
    uint32_t expected = 0;
    if (!g_active_crash_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    FlightRecorder::Instance().CaptureCrashReport(ep, "VectoredExceptionHandler", nullptr);
    g_active_crash_tid.store(0, std::memory_order_release);
    return EXCEPTION_CONTINUE_SEARCH;
}

HMODULE GetSystemD3D12Module() noexcept {
    static HMODULE mod = []() noexcept -> HMODULE {
        wchar_t sys_dir[MAX_PATH] = {};
        if (GetSystemDirectoryW(sys_dir, MAX_PATH) == 0) return nullptr;
        std::wstring p(sys_dir);
        p += L"\\d3d12.dll";
        return LoadLibraryW(p.c_str());
    }();
    return mod;
}

#endif  // _WIN32

}  // namespace

FlightRecorder::FlightRecorder() {
    start_qpc_ = NowQpc();
    last_heartbeat_qpc_.store(start_qpc_, std::memory_order_relaxed);
    for (auto& slot : committed_seq_) {
        slot.store(0, std::memory_order_relaxed);
    }
    for (auto& c : category_counts_) {
        c.store(0, std::memory_order_relaxed);
    }
}

FlightRecorder::~FlightRecorder() {
    Disable();
}

FlightRecorder& FlightRecorder::Instance() noexcept {
    static FlightRecorder instance;
    return instance;
}

int64_t FlightRecorder::QpcFreq() noexcept {
#if defined(_WIN32)
    static const int64_t freq = []() noexcept -> int64_t {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1000000ll;
    }();
    return freq;
#else
    return 1000000ll;
#endif
}

int64_t FlightRecorder::NowQpc() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
#else
    return 0;
#endif
}

uint64_t FlightRecorder::ElapsedUs() const noexcept {
    const int64_t now = NowQpc();
    const int64_t delta = (now > start_qpc_) ? (now - start_qpc_) : 0;
    return static_cast<uint64_t>((delta * 1000000ll) / QpcFreq());
}

bool FlightRecorder::IsEnabled() noexcept {
    const uint8_t s = s_enabled_state_.load(std::memory_order_relaxed);
    if (s == 2u) return true;
    if (s == 1u) return false;
    Instance().ProbeEnvOnce();
    return s_enabled_state_.load(std::memory_order_relaxed) == 2u;
}

void FlightRecorder::ProbeEnvOnce() noexcept {
    bool expected = false;
    if (!env_probing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const char* env_on = std::getenv("SCARLETTX_FLIGHT_RECORDER");
    if (!env_on || env_on[0] == '\0' || env_on[0] == '0') {
        uint8_t zero = 0;
        s_enabled_state_.compare_exchange_strong(zero, 1u, std::memory_order_release);
        env_probing_.store(false, std::memory_order_release);
        return;
    }

    FlightRecorderOptions opts;
    if (const char* dir = std::getenv("SCARLETTX_FLIGHT_RECORDER_DIR"); dir && *dir) {
        opts.output_dir = dir;
    } else {
        opts.output_dir = "flight_logs";
    }
    if (const char* to_ms = std::getenv("SCARLETTX_HANG_TIMEOUT_MS"); to_ms && *to_ms) {
        opts.hang_timeout_ms = static_cast<uint32_t>(std::strtoul(to_ms, nullptr, 10));
    }
    opts.reset_history = false;
    try {
        Enable(opts);
    } catch (...) {
        s_enabled_state_.store(1u, std::memory_order_release);
    }
    env_probing_.store(false, std::memory_order_release);
}

void FlightRecorder::ResetSession() noexcept {
    write_seq_.store(0, std::memory_order_relaxed);
    for (auto& slot : committed_seq_) {
        slot.store(0, std::memory_order_relaxed);
    }
    for (auto& c : category_counts_) {
        c.store(0, std::memory_order_relaxed);
    }
    start_qpc_ = NowQpc();
    heartbeat_seq_.store(0, std::memory_order_relaxed);
    last_heartbeat_qpc_.store(start_qpc_, std::memory_order_relaxed);
    last_hung_heartbeat_seq_.store(~0ull, std::memory_order_relaxed);
    {
        ScopedSpinTryLock lk(phase_lock_, 1024);
        CopyBoundedStr(last_phase_, sizeof(last_phase_), "init");
    }
    {
        ScopedSpinTryLock lk(miss_lock_, 1024);
        stub_miss_count_ = 0;
        com_qi_miss_count_ = 0;
        vtable_miss_count_ = 0;
        vfs_miss_count_ = 0;
    }
    {
        ScopedSpinTryLock lk(gpu_bc_lock_, 1024);
        total_gpu_breadcrumbs_ = 0;
    }
    crash_seq_.store(0, std::memory_order_relaxed);
    hang_seq_.store(0, std::memory_order_relaxed);
    dred_seq_.store(0, std::memory_order_relaxed);
    shader_seq_.store(0, std::memory_order_relaxed);
    last_crash_sig_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(artifacts_mu_);
        crash_artifacts_.clear();
        hang_artifacts_.clear();
        dred_artifacts_.clear();
        shader_artifacts_.clear();
        dumped_shader_hash_count_ = 0;
    }
}

void FlightRecorder::Enable(const FlightRecorderOptions& opts) {
    StopWatchdog();
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    if (opts.reset_history) {
        ResetSession();
    }
    opts_ = opts;
    if (opts_.output_dir.empty()) {
        opts_.output_dir = "flight_logs";
    }
    std::error_code ec;
    opts_.output_dir = std::filesystem::absolute(opts_.output_dir, ec).string();
    std::filesystem::create_directories(opts_.output_dir, ec);

    dred_enabled_flag_.store(opts_.enable_dred, std::memory_order_relaxed);
    shader_dump_flag_.store(opts_.dump_failing_shaders, std::memory_order_relaxed);
    last_heartbeat_qpc_.store(NowQpc(), std::memory_order_relaxed);

#if defined(_WIN32)
    if (opts_.install_veh && !veh_handle_) {
        // First=1 ensures hardware faults inside guest images are captured with exact RIP/RSP
        // before C++ or OS unwind mutates registers, while benign software exceptions are filtered.
        veh_handle_ = AddVectoredExceptionHandler(1, &FlightVectoredExceptionHandler);
    }
#endif

    s_enabled_state_.store(2u, std::memory_order_release);
    RecordEvent(FlightEventCategory::Lifecycle, "flight_recorder",
                ("Enabled -> " + opts_.output_dir).c_str(), 0, 0, opts_.hang_timeout_ms, 0);

    if (opts_.hang_timeout_ms > 0) {
        StartWatchdogLocked(opts_.hang_timeout_ms);
    }
}

void FlightRecorder::Disable() noexcept {
    s_enabled_state_.store(1u, std::memory_order_release);
    StopWatchdog();
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
#if defined(_WIN32)
    if (veh_handle_) {
        RemoveVectoredExceptionHandler(veh_handle_);
        veh_handle_ = nullptr;
    }
#endif
}

std::string FlightRecorder::OutputDirectory() const {
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    return opts_.output_dir;
}

void FlightRecorder::StartWatchdogLocked(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> wlk(watchdog_mu_);
    if (watchdog_thread_.joinable()) {
        return;
    }
    watchdog_stop_ = false;
    watchdog_thread_ = std::thread(&FlightRecorder::WatchdogLoop, this, timeout_ms);
}

void FlightRecorder::StopWatchdog() noexcept {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> wlk(watchdog_mu_);
        watchdog_stop_ = true;
        watchdog_cv_.notify_all();
        if (watchdog_thread_.joinable()) {
            to_join = std::move(watchdog_thread_);
        }
    }
    if (to_join.joinable()) {
        if (to_join.get_id() == std::this_thread::get_id()) {
            to_join.detach();
        } else {
            to_join.join();
        }
    }
}

void FlightRecorder::WatchdogLoop(uint32_t timeout_ms) {
    const uint32_t poll_ms = std::clamp<uint32_t>(timeout_ms / 4u, 10u, 250u);
    std::unique_lock<std::mutex> wlk(watchdog_mu_);
    while (!watchdog_stop_) {
        watchdog_cv_.wait_for(wlk, std::chrono::milliseconds(poll_ms));
        if (watchdog_stop_ || s_enabled_state_.load(std::memory_order_relaxed) != 2u) {
            break;
        }
        const int64_t last_qpc = last_heartbeat_qpc_.load(std::memory_order_relaxed);
        const int64_t now_qpc = NowQpc();
        const uint64_t elapsed_ms = (now_qpc > last_qpc)
            ? static_cast<uint64_t>(((now_qpc - last_qpc) * 1000ll) / QpcFreq())
            : 0ull;
        if (elapsed_ms >= timeout_ms) {
            const uint64_t hb = heartbeat_seq_.load(std::memory_order_relaxed);
            uint64_t prev_hung = last_hung_heartbeat_seq_.load(std::memory_order_relaxed);
            if (prev_hung != hb && hang_seq_.load(std::memory_order_relaxed) < kMaxHangSnapshots) {
                last_hung_heartbeat_seq_.store(hb, std::memory_order_relaxed);
                wlk.unlock();
                CaptureHangSnapshot("WatchdogStallTimeout", nullptr);
                wlk.lock();
            }
        }
    }
}

void FlightRecorder::RecordEvent(FlightEventCategory category,
                                 const char* subsystem,
                                 const char* detail,
                                 int32_t status_code,
                                 uint64_t caller_rip,
                                 uint64_t arg0,
                                 uint64_t arg1) noexcept {
    if (!IsEnabled()) return;

    const size_t cat_idx = static_cast<size_t>(category);
    if (cat_idx < static_cast<size_t>(FlightEventCategory::Count)) {
        category_counts_[cat_idx].fetch_add(1, std::memory_order_relaxed);
    }

    const uint64_t seq = write_seq_.fetch_add(1, std::memory_order_relaxed) + 1ull;
    const size_t idx = static_cast<size_t>((seq - 1ull) & (kRingCapacity - 1u));

    committed_seq_[idx].store(0ull, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acq_rel);

    FlightEvent& ev = ring_[idx];
    ev.seq = seq;
    ev.timestamp_us = ElapsedUs();
    ev.thread_id = CurrentTid();
    ev.category = category;
    ev.status_code = status_code;
    ev.caller_rip = caller_rip;
    ev.arg0 = arg0;
    ev.arg1 = arg1;
    CopyBoundedStr(ev.subsystem, sizeof(ev.subsystem), subsystem);
    CopyBoundedStr(ev.detail, sizeof(ev.detail), detail);

    committed_seq_[idx].store(seq, std::memory_order_release);
}

void FlightRecorder::RecordStubHit(const char* dll_name,
                                   const char* symbol_name,
                                   bool by_ordinal,
                                   uint16_t ordinal,
                                   const uint64_t* args4,
                                   uint64_t return_value,
                                   uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;

    char key[96] = {};
    if (by_ordinal) {
        std::snprintf(key, sizeof(key), "%s!#%u", dll_name ? dll_name : "?", static_cast<unsigned>(ordinal));
    } else {
        std::snprintf(key, sizeof(key), "%s!%s", dll_name ? dll_name : "?", symbol_name ? symbol_name : "?");
    }

    const uint64_t a0 = args4 ? args4[0] : 0ull;
    const uint64_t a1 = args4 ? args4[1] : 0ull;
    RecordEvent(FlightEventCategory::StubHit, "stub_thunk", key,
                static_cast<int32_t>(return_value), caller_rip, a0, a1);

    ScopedSpinTryLock lk(miss_lock_);
    if (!lk.owns_lock()) return;
    for (uint32_t i = 0; i < stub_miss_count_; ++i) {
        if (std::strcmp(stub_misses_[i].module_and_symbol, key) == 0) {
            stub_misses_[i].hit_count += 1ull;
            if (args4) {
                for (int k = 0; k < 4; ++k) stub_misses_[i].last_args[k] = args4[k];
            }
            return;
        }
    }
    if (stub_miss_count_ < kMaxUniqueMisses) {
        auto& dst = stub_misses_[stub_miss_count_++];
        CopyBoundedStr(dst.module_and_symbol, sizeof(dst.module_and_symbol), key);
        dst.hit_count = 1ull;
        dst.first_caller_rip = caller_rip;
        dst.return_value = return_value;
        if (args4) {
            for (int k = 0; k < 4; ++k) dst.last_args[k] = args4[k];
        } else {
            std::memset(dst.last_args, 0, sizeof(dst.last_args));
        }
    }
}

void FlightRecorder::RecordComQiMiss(const char* interface_tag,
                                     const void* iid_16bytes,
                                     uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;

    char iid_str[40] = {};
    FormatGuidBytes(iid_16bytes, iid_str);

    char detail[64] = {};
    std::snprintf(detail, sizeof(detail), "%s:%s", interface_tag ? interface_tag : "COM", iid_str);
    RecordEvent(FlightEventCategory::ComQiMiss,
                interface_tag ? interface_tag : "com_qi",
                iid_str,
                static_cast<int32_t>(0x80004002u),
                caller_rip);

    ScopedSpinTryLock lk(miss_lock_);
    if (!lk.owns_lock()) return;
    const char* tag = interface_tag ? interface_tag : "COM";
    for (uint32_t i = 0; i < com_qi_miss_count_; ++i) {
        if (std::strcmp(com_qi_misses_[i].iid_str, iid_str) == 0 &&
            std::strcmp(com_qi_misses_[i].interface_tag, tag) == 0) {
            com_qi_misses_[i].hit_count += 1ull;
            return;
        }
    }
    if (com_qi_miss_count_ < kMaxUniqueMisses) {
        auto& dst = com_qi_misses_[com_qi_miss_count_++];
        CopyBoundedStr(dst.interface_tag, sizeof(dst.interface_tag), tag);
        CopyBoundedStr(dst.iid_str, sizeof(dst.iid_str), iid_str);
        dst.hit_count = 1ull;
        dst.first_caller_rip = caller_rip;
    }
}

void FlightRecorder::RecordVtableSlotMiss(const char* interface_tag,
                                          int32_t slot,
                                          uint64_t arg0,
                                          uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;

    char detail[64] = {};
    std::snprintf(detail, sizeof(detail), "%s::vtable[%d] (+0x%X)",
                  interface_tag ? interface_tag : "COM", slot, static_cast<unsigned>(slot * 8));
    RecordEvent(FlightEventCategory::VtableSlotMiss,
                interface_tag ? interface_tag : "com_vtbl",
                detail,
                static_cast<int32_t>(0x80004001u),
                caller_rip,
                arg0,
                static_cast<uint64_t>(slot));

    ScopedSpinTryLock lk(miss_lock_);
    if (!lk.owns_lock()) return;
    const char* tag = interface_tag ? interface_tag : "COM";
    for (uint32_t i = 0; i < vtable_miss_count_; ++i) {
        if (vtable_misses_[i].slot == slot &&
            std::strcmp(vtable_misses_[i].interface_tag, tag) == 0) {
            vtable_misses_[i].hit_count += 1ull;
            vtable_misses_[i].last_arg0 = arg0;
            return;
        }
    }
    if (vtable_miss_count_ < kMaxUniqueMisses) {
        auto& dst = vtable_misses_[vtable_miss_count_++];
        CopyBoundedStr(dst.interface_tag, sizeof(dst.interface_tag), tag);
        dst.slot = slot;
        dst.hit_count = 1ull;
        dst.first_caller_rip = caller_rip;
        dst.last_arg0 = arg0;
    }
}

void FlightRecorder::RecordWinRtActivation(const char* api_name,
                                           const wchar_t* runtime_class_name,
                                           int32_t hr,
                                           bool handled_by_hle,
                                           uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;
    char cls_utf8[64] = {};
    if (runtime_class_name && *runtime_class_name) {
        size_t i = 0;
        while (i + 1 < sizeof(cls_utf8) && runtime_class_name[i] != L'\0') {
            const wchar_t wc = runtime_class_name[i];
            cls_utf8[i] = (wc >= 0x20 && wc < 0x7F) ? static_cast<char>(wc) : '?';
            ++i;
        }
        cls_utf8[i] = '\0';
    } else {
        CopyBoundedStr(cls_utf8, sizeof(cls_utf8), "<null>");
    }
    RecordEvent(FlightEventCategory::WinRtBroker,
                api_name ? api_name : "WinRT",
                cls_utf8,
                hr,
                caller_rip,
                handled_by_hle ? 1ull : 0ull,
                0ull);
}

void FlightRecorder::RecordVfsAccess(const char* virtual_path,
                                     bool resolved_ok,
                                     int32_t win32_or_hresult_err,
                                     uint64_t caller_rip) noexcept {
    if (!IsEnabled()) return;

    const char* path = (virtual_path && *virtual_path) ? virtual_path : "<empty>";
    const size_t len = std::strlen(path);
    const char* tail = (len >= 60u) ? (path + (len - 59u)) : path;

    if (resolved_ok) {
        RecordEvent(FlightEventCategory::VfsAccess, "vfs", tail, 0, caller_rip);
        return;
    }

    RecordEvent(FlightEventCategory::VfsMiss, "vfs", tail, win32_or_hresult_err, caller_rip);

    ScopedSpinTryLock lk(miss_lock_);
    if (!lk.owns_lock()) return;
    for (uint32_t i = 0; i < vfs_miss_count_; ++i) {
        if (std::strcmp(vfs_misses_[i].virtual_path, path) == 0) {
            vfs_misses_[i].hit_count += 1ull;
            vfs_misses_[i].error_code = win32_or_hresult_err;
            return;
        }
    }
    if (vfs_miss_count_ < kMaxUniqueMisses) {
        auto& dst = vfs_misses_[vfs_miss_count_++];
        CopyBoundedStr(dst.virtual_path, sizeof(dst.virtual_path), path);
        dst.error_code = win32_or_hresult_err;
        dst.hit_count = 1ull;
        dst.first_caller_rip = caller_rip;
    }
}

void FlightRecorder::RecordGpuBreadcrumb(uint64_t cmd_list_id,
                                         uint64_t marker_value,
                                         const char* tag) noexcept {
    if (!IsEnabled()) return;
    BeatProgress("GpuBreadcrumb");
    RecordEvent(FlightEventCategory::GpuBreadcrumb, "d3d12_xs",
                tag ? tag : "marker", 0, 0, cmd_list_id, marker_value);

    ScopedSpinTryLock lk(gpu_bc_lock_);
    if (!lk.owns_lock()) return;
    FlightGpuBreadcrumb& slot = recent_gpu_breadcrumbs_[static_cast<size_t>(total_gpu_breadcrumbs_ % 64ull)];
    slot.command_list_id = cmd_list_id;
    slot.marker_value = marker_value;
    CopyBoundedStr(slot.tag, sizeof(slot.tag), tag ? tag : "");
    total_gpu_breadcrumbs_ += 1ull;
}

void FlightRecorder::BeatProgress(const char* phase) noexcept {
    if (!IsEnabled()) return;
    last_heartbeat_qpc_.store(NowQpc(), std::memory_order_relaxed);
    heartbeat_seq_.fetch_add(1ull, std::memory_order_relaxed);
    if (phase && *phase) {
        ScopedSpinTryLock lk(phase_lock_, 8);
        if (lk.owns_lock()) {
            CopyBoundedStr(last_phase_, sizeof(last_phase_), phase);
        }
    }
}

FlightResolvedAddress FlightRecorder::ResolveCodeAddress(uint64_t va) noexcept {
    FlightResolvedAddress out{};
    out.raw_va = va;
    if (va < 0x10000ull) {
        return out;
    }

    guest::GuestModuleAddrInfo ginfo{};
    if (guest::TryResolveGuestAddressLockFree(va, &ginfo)) {
        out.resolved = true;
        out.is_guest = true;
        out.is_guest_exe = ginfo.is_exe;
        out.module_base = ginfo.base;
        out.rva = ginfo.rva;
        out.ghidra_va = ginfo.ghidra_va;
        CopyBoundedStr(out.module_name, sizeof(out.module_name), ginfo.name);
        return out;
    }

#if defined(_WIN32)
    using RtlPcToFileHeaderFn = PVOID(WINAPI*)(PVOID, PVOID*);
    static const RtlPcToFileHeaderFn real_pc_to_hdr = []() noexcept -> RtlPcToFileHeaderFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<RtlPcToFileHeaderFn>(
                           reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlPcToFileHeader")))
                     : nullptr;
    }();

    PVOID base = nullptr;
    if (real_pc_to_hdr && real_pc_to_hdr(reinterpret_cast<PVOID>(va), &base) && base) {
        out.resolved = true;
        out.is_guest = false;
        out.module_base = reinterpret_cast<uint64_t>(base);
        out.rva = static_cast<uint32_t>(va - out.module_base);

        char full_path[MAX_PATH + 1] = {};
        const DWORD n = GetModuleFileNameA(static_cast<HMODULE>(base), full_path, MAX_PATH);
        if (n > 0) {
            const char* slash = full_path;
            for (DWORD i = 0; i < n; ++i) {
                if (full_path[i] == '\\' || full_path[i] == '/') {
                    slash = full_path + i + 1;
                }
            }
            CopyBoundedStr(out.module_name, sizeof(out.module_name), slash);
        } else {
            CopyBoundedStr(out.module_name, sizeof(out.module_name), "host_module");
        }
    }
#endif
    return out;
}

std::string FlightRecorder::FormatResolvedAddress(const FlightResolvedAddress& addr) {
    if (addr.raw_va == 0) return "0x0";
    char buf[192] = {};
    if (!addr.resolved) {
        std::snprintf(buf, sizeof(buf), "0x%llX (unmapped)", static_cast<unsigned long long>(addr.raw_va));
        return buf;
    }
    if (addr.is_guest) {
        std::snprintf(buf, sizeof(buf), "%s+0x%X [guest Ghidra=0x%llX, raw=0x%llX]",
                      addr.module_name,
                      addr.rva,
                      static_cast<unsigned long long>(addr.ghidra_va),
                      static_cast<unsigned long long>(addr.raw_va));
    } else {
        std::snprintf(buf, sizeof(buf), "%s+0x%X [0x%llX]",
                      addr.module_name,
                      addr.rva,
                      static_cast<unsigned long long>(addr.raw_va));
    }
    return buf;
}

std::vector<FlightEvent> FlightRecorder::SnapshotRecentEvents(size_t max_events) const {
    const uint64_t head = write_seq_.load(std::memory_order_acquire);
    if (head == 0 || max_events == 0) return {};

    const size_t window = std::min<size_t>(max_events, kRingCapacity);
    const uint64_t start_seq = (head > window) ? (head - window + 1ull) : 1ull;

    std::vector<FlightEvent> out;
    out.reserve(static_cast<size_t>(head - start_seq + 1ull));
    for (uint64_t s = start_seq; s <= head; ++s) {
        const size_t idx = static_cast<size_t>((s - 1ull) & (kRingCapacity - 1u));
        const uint64_t c1 = committed_seq_[idx].load(std::memory_order_acquire);
        if (c1 != s) continue;
        FlightEvent copy = ring_[idx];
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t c2 = committed_seq_[idx].load(std::memory_order_relaxed);
        if (c2 == s && copy.seq == s) {
            out.push_back(copy);
        }
    }
    return out;
}

FlightRecorderStats FlightRecorder::GetStats() const noexcept {
    FlightRecorderStats st{};
    st.total_events = write_seq_.load(std::memory_order_relaxed);
    st.heartbeats = heartbeat_seq_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < static_cast<size_t>(FlightEventCategory::Count); ++i) {
        st.category_counts[i] = category_counts_[i].load(std::memory_order_relaxed);
    }
    {
        ScopedSpinTryLock lk(miss_lock_, 256);
        if (lk.owns_lock()) {
            st.unique_stub_misses = stub_miss_count_;
            st.unique_com_qi_misses = com_qi_miss_count_;
            st.unique_vtable_misses = vtable_miss_count_;
            st.unique_vfs_misses = vfs_miss_count_;
        }
    }
    st.crash_reports_written = crash_seq_.load(std::memory_order_relaxed);
    st.hang_snapshots_written = hang_seq_.load(std::memory_order_relaxed);
    st.dred_reports_written = dred_seq_.load(std::memory_order_relaxed);
    st.shader_dumps_written = shader_seq_.load(std::memory_order_relaxed);
    return st;
}

bool FlightRecorder::CaptureCrashReport(const void* win32_exception_pointers,
                                        const char* reason,
                                        std::string* out_json_path) noexcept {
#if defined(_WIN32)
    if (!win32_exception_pointers) return false;
    const auto* ep = static_cast<const EXCEPTION_POINTERS*>(win32_exception_pointers);
    if (!ep->ExceptionRecord || !ep->ContextRecord) return false;

    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const uint64_t fault_rip = ep->ContextRecord->Rip;
    const uint64_t sig = (static_cast<uint64_t>(code) << 32u) ^ (fault_rip * 1099511628211ull);
    const uint64_t prev_sig = last_crash_sig_.exchange(sig, std::memory_order_acq_rel);
    if (prev_sig == sig && out_json_path == nullptr) {
        // Avoid duplicate report when both first-chance VEH and outer SEH filter see the exact same fault.
        return false;
    }

    const uint32_t seq = crash_seq_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (seq > kMaxCrashReports) {
        crash_seq_.fetch_sub(1u, std::memory_order_relaxed);
        return false;
    }

    try {
        const std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);

        const uint32_t pid = CurrentPid();
        const uint32_t tid = CurrentTid();
        const FlightResolvedAddress resolved_rip = ResolveCodeAddress(fault_rip);

        std::string av_kind = "n/a";
        uint64_t av_target_va = 0;
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
            av_kind = (op == 0) ? "READ" : (op == 1 ? "WRITE" : (op == 8 ? "EXECUTE_DEP" : "OTHER"));
            av_target_va = static_cast<uint64_t>(ep->ExceptionRecord->ExceptionInformation[1]);
        }

        RecordEvent(FlightEventCategory::GuestFault,
                    "crash_handler",
                    ExceptionCodeName(code),
                    static_cast<int32_t>(code),
                    fault_rip,
                    av_target_va,
                    seq);

        constexpr size_t kMaxFrames = 32;
        uint64_t frame_rips[kMaxFrames] = {};
        uint64_t frame_rsps[kMaxFrames] = {};
        const size_t frame_count = UnwindX64FramesPod(ep->ContextRecord, frame_rips, frame_rsps, kMaxFrames);

        const auto recent_events = SnapshotRecentEvents(128);

        std::vector<FlightStubMissRecord> stubs;
        std::vector<FlightComQiMissRecord> qi_misses;
        std::vector<FlightVtableMissRecord> vtbl_misses;
        std::vector<FlightVfsMissRecord> vfs_misses;
        {
            ScopedSpinTryLock mlk(miss_lock_, 64);
            if (mlk.owns_lock()) {
                stubs.assign(stub_misses_, stub_misses_ + stub_miss_count_);
                qi_misses.assign(com_qi_misses_, com_qi_misses_ + com_qi_miss_count_);
                vtbl_misses.assign(vtable_misses_, vtable_misses_ + vtable_miss_count_);
                vfs_misses.assign(vfs_misses_, vfs_misses_ + vfs_miss_count_);
            }
        }

        std::vector<FlightGpuBreadcrumb> gpu_bcs;
        {
            ScopedSpinTryLock blk(gpu_bc_lock_, 64);
            if (blk.owns_lock() && total_gpu_breadcrumbs_ > 0) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(total_gpu_breadcrumbs_, 64ull));
                for (size_t i = 0; i < n; ++i) {
                    const uint64_t bidx = (total_gpu_breadcrumbs_ > 64ull) ? (total_gpu_breadcrumbs_ - n + i) : i;
                    gpu_bcs.push_back(recent_gpu_breadcrumbs_[static_cast<size_t>(bidx % 64ull)]);
                }
            }
        }

        char phase_copy[48] = {};
        {
            ScopedSpinTryLock plk(phase_lock_, 32);
            if (plk.owns_lock()) {
                CopyBoundedStr(phase_copy, sizeof(phase_copy), last_phase_);
            }
        }

        const auto base_name = "crash_report_" + std::to_string(pid) + "_" + std::to_string(seq);
        const std::string json_path = (std::filesystem::path(out_dir) / (base_name + ".json")).string();
        const std::string txt_path = (std::filesystem::path(out_dir) / (base_name + ".txt")).string();

        const CONTEXT& c = *ep->ContextRecord;
        {
            std::ofstream jf(json_path, std::ios::binary);
            if (jf) {
                jf << "{\n"
                   << "  \"report_type\": \"crash_report\",\n"
                   << "  \"pid\": " << pid << ",\n"
                   << "  \"tid\": " << tid << ",\n"
                   << "  \"sequence\": " << seq << ",\n"
                   << "  \"reason\": \"" << JsonEscape(reason ? reason : "Exception") << "\",\n"
                   << "  \"last_phase\": \"" << JsonEscape(phase_copy) << "\",\n"
                   << "  \"exception_code\": \"" << HexU32(code) << "\",\n"
                   << "  \"exception_name\": \"" << ExceptionCodeName(code) << "\",\n"
                   << "  \"access_type\": \"" << av_kind << "\",\n"
                   << "  \"fault_target_va\": \"" << HexU64(av_target_va) << "\",\n"
                   << "  \"fault_rip\": \"" << HexU64(fault_rip) << "\",\n"
                   << "  \"fault_symbol\": \"" << JsonEscape(FormatResolvedAddress(resolved_rip)) << "\",\n"
                   << "  \"fault_module\": \"" << JsonEscape(resolved_rip.module_name) << "\",\n"
                   << "  \"fault_rva\": \"" << HexU32(resolved_rip.rva) << "\",\n"
                   << "  \"fault_ghidra_va\": \"" << HexU64(resolved_rip.ghidra_va) << "\",\n"
                   << "  \"is_guest_code\": " << (resolved_rip.is_guest ? "true" : "false") << ",\n"
                   << "  \"registers\": {\n"
                   << "    \"rax\": \"" << HexU64(c.Rax) << "\", \"rbx\": \"" << HexU64(c.Rbx) << "\",\n"
                   << "    \"rcx\": \"" << HexU64(c.Rcx) << "\", \"rdx\": \"" << HexU64(c.Rdx) << "\",\n"
                   << "    \"rsi\": \"" << HexU64(c.Rsi) << "\", \"rdi\": \"" << HexU64(c.Rdi) << "\",\n"
                   << "    \"rbp\": \"" << HexU64(c.Rbp) << "\", \"rsp\": \"" << HexU64(c.Rsp) << "\",\n"
                   << "    \"r8\": \"" << HexU64(c.R8) << "\", \"r9\": \"" << HexU64(c.R9) << "\",\n"
                   << "    \"r10\": \"" << HexU64(c.R10) << "\", \"r11\": \"" << HexU64(c.R11) << "\",\n"
                   << "    \"r12\": \"" << HexU64(c.R12) << "\", \"r13\": \"" << HexU64(c.R13) << "\",\n"
                   << "    \"r14\": \"" << HexU64(c.R14) << "\", \"r15\": \"" << HexU64(c.R15) << "\",\n"
                   << "    \"rip\": \"" << HexU64(c.Rip) << "\", \"eflags\": \"" << HexU32(c.EFlags) << "\"\n"
                   << "  },\n"
                   << "  \"stack_frames\": [\n";
                for (size_t i = 0; i < frame_count; ++i) {
                    const auto fa = ResolveCodeAddress(frame_rips[i]);
                    jf << "    {\"index\": " << i
                       << ", \"rip\": \"" << HexU64(frame_rips[i]) << "\""
                       << ", \"rsp\": \"" << HexU64(frame_rsps[i]) << "\""
                       << ", \"module\": \"" << JsonEscape(fa.module_name) << "\""
                       << ", \"rva\": \"" << HexU32(fa.rva) << "\""
                       << ", \"ghidra_va\": \"" << HexU64(fa.ghidra_va) << "\""
                       << ", \"is_guest\": " << (fa.is_guest ? "true" : "false")
                       << ", \"formatted\": \"" << JsonEscape(FormatResolvedAddress(fa)) << "\"}"
                       << (i + 1 < frame_count ? "," : "") << "\n";
                }
                jf << "  ],\n  \"gpu_breadcrumbs\": [\n";
                for (size_t i = 0; i < gpu_bcs.size(); ++i) {
                    jf << "    {\"cmd_list\": " << gpu_bcs[i].command_list_id
                       << ", \"marker\": " << gpu_bcs[i].marker_value
                       << ", \"tag\": \"" << JsonEscape(gpu_bcs[i].tag) << "\"}"
                       << (i + 1 < gpu_bcs.size() ? "," : "") << "\n";
                }
                jf << "  ],\n  \"stub_misses\": [\n";
                for (size_t i = 0; i < stubs.size(); ++i) {
                    const auto ca = ResolveCodeAddress(stubs[i].first_caller_rip);
                    jf << "    {\"symbol\": \"" << JsonEscape(stubs[i].module_and_symbol) << "\""
                       << ", \"hits\": " << stubs[i].hit_count
                       << ", \"caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                       << (i + 1 < stubs.size() ? "," : "") << "\n";
                }
                jf << "  ],\n  \"recent_events\": [\n";
                for (size_t i = 0; i < recent_events.size(); ++i) {
                    const auto& ev = recent_events[i];
                    const auto ca = ResolveCodeAddress(ev.caller_rip);
                    jf << "    {\"seq\": " << ev.seq
                       << ", \"t_us\": " << ev.timestamp_us
                       << ", \"tid\": " << ev.thread_id
                       << ", \"cat\": \"" << FlightEventCategoryName(ev.category) << "\""
                       << ", \"sub\": \"" << JsonEscape(ev.subsystem) << "\""
                       << ", \"detail\": \"" << JsonEscape(ev.detail) << "\""
                       << ", \"status\": \"" << HexU32(static_cast<uint32_t>(ev.status_code)) << "\""
                       << ", \"caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                       << (i + 1 < recent_events.size() ? "," : "") << "\n";
                }
                jf << "  ]\n}\n";
            }
        }

        {
            std::ofstream tf(txt_path);
            if (tf) {
                tf << "=== ScarlettEdge Flight-Recorder Crash Report ===\n"
                   << "PID / TID:      " << pid << " / " << tid << "\n"
                   << "Reason:         " << (reason ? reason : "Exception") << " (phase=" << phase_copy << ")\n"
                   << "Exception:      " << HexU32(code) << " (" << ExceptionCodeName(code) << ")\n"
                   << "Access Fault:   " << av_kind << " @ " << HexU64(av_target_va) << "\n"
                   << "Faulting RIP:   " << FormatResolvedAddress(resolved_rip) << "\n"
                   << "Registers:      RAX=" << HexU64(c.Rax) << " RBX=" << HexU64(c.Rbx)
                   << " RCX=" << HexU64(c.Rcx) << " RDX=" << HexU64(c.Rdx) << "\n"
                   << "                RSI=" << HexU64(c.Rsi) << " RDI=" << HexU64(c.Rdi)
                   << " RBP=" << HexU64(c.Rbp) << " RSP=" << HexU64(c.Rsp) << "\n"
                   << "                R8=" << HexU64(c.R8) << " R9=" << HexU64(c.R9)
                   << " R10=" << HexU64(c.R10) << " R11=" << HexU64(c.R11) << "\n\n"
                   << "Stack Trace (" << frame_count << " frames):\n";
                for (size_t i = 0; i < frame_count; ++i) {
                    tf << "  [" << std::setw(2) << i << "] "
                       << FormatResolvedAddress(ResolveCodeAddress(frame_rips[i]))
                       << " (RSP=" << HexU64(frame_rsps[i]) << ")\n";
                }
            }
        }

        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            crash_artifacts_.push_back(json_path);
        }
        if (out_json_path) *out_json_path = json_path;
        std::fprintf(stderr, "[ScarlettX][FlightRecorder] Wrote post-mortem crash dump: %s\n", json_path.c_str());
        return true;
    } catch (...) {
        return false;
    }
#else
    (void)win32_exception_pointers;
    (void)reason;
    (void)out_json_path;
    return false;
#endif
}

void FlightRecorder::EnableD3D12DredIfConfigured() noexcept {
#if defined(_WIN32) && defined(__ID3D12DeviceRemovedExtendedDataSettings_INTERFACE_DEFINED__)
    const bool env_dred = []() noexcept {
        const char* v = std::getenv("SCARLETTX_DRED");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (!env_dred && (!IsEnabled() || !dred_enabled_flag_.load(std::memory_order_relaxed))) {
        return;
    }
    bool expected = false;
    if (!dred_applied_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    HMODULE d3d12_mod = GetSystemD3D12Module();
    if (!d3d12_mod) return;
    using GetDbgFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto get_dbg = reinterpret_cast<GetDbgFn>(
        reinterpret_cast<void*>(GetProcAddress(d3d12_mod, "D3D12GetDebugInterface")));
    if (!get_dbg) return;

    ID3D12DeviceRemovedExtendedDataSettings* dred_settings = nullptr;
    if (SUCCEEDED(get_dbg(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                          reinterpret_cast<void**>(&dred_settings))) &&
        dred_settings) {
        dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->Release();
        RecordEvent(FlightEventCategory::Lifecycle, "d3d12_dred",
                    "Enabled DRED AutoBreadcrumbs + PageFault", 0);
    }
#endif
}

std::string FlightRecorder::RecordGpuDeviceRemoved(void* native_id3d12_device,
                                                   int32_t triggering_hr,
                                                   const char* context_site,
                                                   const FlightGpuBreadcrumb* breadcrumbs,
                                                   size_t breadcrumb_count) noexcept {
    if (!IsEnabled()) return {};

    const uint32_t seq = dred_seq_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (seq > kMaxDredReports) {
        dred_seq_.fetch_sub(1u, std::memory_order_relaxed);
        return {};
    }

    try {
        int32_t removed_reason = triggering_hr;
        uint64_t page_fault_va = 0;
        bool has_dred_breadcrumbs = false;
        bool has_dred_page_fault = false;
        std::vector<std::string> dred_nodes;
        std::vector<std::string> fault_allocs;

#if defined(_WIN32)
        if (native_id3d12_device) {
            auto* dev = static_cast<ID3D12Device*>(native_id3d12_device);
            const HRESULT dev_reason = dev->GetDeviceRemovedReason();
            if (dev_reason != S_OK) {
                removed_reason = static_cast<int32_t>(dev_reason);
            }
#  if defined(__ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__)
            ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1),
                                              reinterpret_cast<void**>(&dred1))) &&
                dred1) {
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc_out{};
                if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc_out)) && bc_out.pHeadAutoBreadcrumbNode) {
                    has_dred_breadcrumbs = true;
                    const D3D12_AUTO_BREADCRUMB_NODE1* node = bc_out.pHeadAutoBreadcrumbNode;
                    for (size_t n = 0; node && n < 16; ++n, node = node->pNext) {
                        const std::string cl_name = node->pCommandListDebugNameW
                            ? WideToUtf8Safe(node->pCommandListDebugNameW)
                            : (node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "<unnamed_cl>");
                        const std::string cq_name = node->pCommandQueueDebugNameW
                            ? WideToUtf8Safe(node->pCommandQueueDebugNameW)
                            : (node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "<unnamed_q>");
                        const uint32_t completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
                        uint32_t last_op = 0u;
                        if (node->pCommandHistory && node->BreadcrumbCount > 0) {
                            const uint32_t idx = std::min<uint32_t>(completed, node->BreadcrumbCount - 1u);
                            last_op = static_cast<uint32_t>(node->pCommandHistory[idx]);
                        }
                        std::ostringstream ns;
                        ns << "{\"cmd_list\":\"" << JsonEscape(cl_name)
                           << "\",\"queue\":\"" << JsonEscape(cq_name)
                           << "\",\"completed\":" << completed
                           << ",\"total\":" << node->BreadcrumbCount
                           << ",\"last_op\":" << last_op << "}";
                        dred_nodes.push_back(ns.str());
                    }
                }

                D3D12_DRED_PAGE_FAULT_OUTPUT1 pf_out{};
                if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&pf_out))) {
                    has_dred_page_fault = true;
                    page_fault_va = pf_out.PageFaultVA;
                    const D3D12_DRED_ALLOCATION_NODE1* alloc = pf_out.pHeadExistingAllocationNode;
                    for (size_t n = 0; alloc && n < 16; ++n, alloc = alloc->pNext) {
                        const std::string name = alloc->ObjectNameW
                            ? WideToUtf8Safe(alloc->ObjectNameW)
                            : (alloc->ObjectNameA ? alloc->ObjectNameA : "<unnamed>");
                        std::ostringstream as;
                        as << "{\"kind\":\"existing\",\"type\":" << static_cast<int>(alloc->AllocationType)
                           << ",\"name\":\"" << JsonEscape(name) << "\"}";
                        fault_allocs.push_back(as.str());
                    }
                    const D3D12_DRED_ALLOCATION_NODE1* freed = pf_out.pHeadRecentFreedAllocationNode;
                    for (size_t n = 0; freed && n < 16; ++n, freed = freed->pNext) {
                        const std::string name = freed->ObjectNameW
                            ? WideToUtf8Safe(freed->ObjectNameW)
                            : (freed->ObjectNameA ? freed->ObjectNameA : "<unnamed>");
                        std::ostringstream as;
                        as << "{\"kind\":\"freed\",\"type\":" << static_cast<int>(freed->AllocationType)
                           << ",\"name\":\"" << JsonEscape(name) << "\"}";
                        fault_allocs.push_back(as.str());
                    }
                }
                dred1->Release();
            }
#  endif
        }
#else
        (void)native_id3d12_device;
#endif

        RecordEvent(FlightEventCategory::GpuDeviceRemoved,
                    "d3d12_dred",
                    context_site ? context_site : "DeviceRemoved",
                    removed_reason,
                    0,
                    page_fault_va,
                    seq);

        std::vector<FlightGpuBreadcrumb> merged_bcs;
        if (breadcrumbs && breadcrumb_count > 0) {
            merged_bcs.assign(breadcrumbs, breadcrumbs + breadcrumb_count);
        } else {
            ScopedSpinTryLock blk(gpu_bc_lock_, 64);
            if (blk.owns_lock() && total_gpu_breadcrumbs_ > 0) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(total_gpu_breadcrumbs_, 64ull));
                for (size_t i = 0; i < n; ++i) {
                    const uint64_t bidx = (total_gpu_breadcrumbs_ > 64ull) ? (total_gpu_breadcrumbs_ - n + i) : i;
                    merged_bcs.push_back(recent_gpu_breadcrumbs_[static_cast<size_t>(bidx % 64ull)]);
                }
            }
        }

        const std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        const std::string json_path = (std::filesystem::path(out_dir) /
            ("gpu_dred_report_" + std::to_string(CurrentPid()) + "_" + std::to_string(seq) + ".json")).string();

        {
            std::ofstream jf(json_path, std::ios::binary);
            if (jf) {
                jf << "{\n"
                   << "  \"report_type\": \"gpu_dred_report\",\n"
                   << "  \"pid\": " << CurrentPid() << ",\n"
                   << "  \"sequence\": " << seq << ",\n"
                   << "  \"context_site\": \"" << JsonEscape(context_site ? context_site : "unknown") << "\",\n"
                   << "  \"triggering_hr\": \"" << HexU32(static_cast<uint32_t>(triggering_hr)) << "\",\n"
                   << "  \"device_removed_reason\": \"" << HexU32(static_cast<uint32_t>(removed_reason)) << "\",\n"
                   << "  \"has_dred_auto_breadcrumbs\": " << (has_dred_breadcrumbs ? "true" : "false") << ",\n"
                   << "  \"has_dred_page_fault\": " << (has_dred_page_fault ? "true" : "false") << ",\n"
                   << "  \"page_fault_va\": \"" << HexU64(page_fault_va) << "\",\n"
                   << "  \"dred_auto_breadcrumb_nodes\": [\n";
                for (size_t i = 0; i < dred_nodes.size(); ++i) {
                    jf << "    " << dred_nodes[i] << (i + 1 < dred_nodes.size() ? "," : "") << "\n";
                }
                jf << "  ],\n  \"dred_page_fault_allocations\": [\n";
                for (size_t i = 0; i < fault_allocs.size(); ++i) {
                    jf << "    " << fault_allocs[i] << (i + 1 < fault_allocs.size() ? "," : "") << "\n";
                }
                jf << "  ],\n  \"scarlettx_breadcrumbs\": [\n";
                for (size_t i = 0; i < merged_bcs.size(); ++i) {
                    jf << "    {\"cmd_list\": " << merged_bcs[i].command_list_id
                       << ", \"marker\": " << merged_bcs[i].marker_value
                       << ", \"tag\": \"" << JsonEscape(merged_bcs[i].tag) << "\"}"
                       << (i + 1 < merged_bcs.size() ? "," : "") << "\n";
                }
                jf << "  ]\n}\n";
            }
        }
        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            dred_artifacts_.push_back(json_path);
        }
        return json_path;
    } catch (...) {
        return {};
    }
}

std::string FlightRecorder::DumpFailingShaderArtifact(const char* stage_or_api,
                                                      int32_t hr_or_status,
                                                      const void* raw_bytecode,
                                                      size_t raw_size,
                                                      const void* translated_bytecode,
                                                      size_t translated_size,
                                                      uint64_t caller_rip,
                                                      const char* extra_note) noexcept {
    if (!IsEnabled() || !shader_dump_flag_.load(std::memory_order_relaxed)) {
        return {};
    }

    const uint64_t blob_hash = (raw_bytecode && raw_size > 0)
        ? Fnv1a64(raw_bytecode, raw_size)
        : ((translated_bytecode && translated_size > 0) ? Fnv1a64(translated_bytecode, translated_size)
                                                        : Fnv1a64(stage_or_api ? stage_or_api : "pso",
                                                                  stage_or_api ? std::strlen(stage_or_api) : 3u));

    RecordEvent(FlightEventCategory::ShaderOrPsoFail,
                "shader_dump",
                stage_or_api ? stage_or_api : "PSO",
                hr_or_status,
                caller_rip,
                raw_size,
                blob_hash);

    try {
        std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
        const std::filesystem::path shaders_dir = std::filesystem::path(out_dir) / "shaders";
        uint32_t idx = 0;
        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            for (uint32_t i = 0; i < dumped_shader_hash_count_; ++i) {
                if (dumped_shader_hashes_[i] == blob_hash) {
                    return {};  // Already dumped this exact shader hash
                }
            }
            if (dumped_shader_hash_count_ >= kMaxShaderDumps) {
                return {};
            }
            dumped_shader_hashes_[dumped_shader_hash_count_++] = blob_hash;
            idx = shader_seq_.fetch_add(1u, std::memory_order_relaxed) + 1u;
        }

        std::error_code ec;
        std::filesystem::create_directories(shaders_dir, ec);

        char hash_hex[20] = {};
        std::snprintf(hash_hex, sizeof(hash_hex), "%016llx", static_cast<unsigned long long>(blob_hash));
        const std::string stem = "shader_fail_" + std::to_string(idx) + "_" +
                                 SanitizeFilenameToken(stage_or_api) + "_" + hash_hex;

        std::string raw_file;
        if (raw_bytecode && raw_size > 0) {
            raw_file = (shaders_dir / (stem + "_raw.bin")).string();
            std::ofstream rf(raw_file, std::ios::binary);
            if (rf) {
                rf.write(static_cast<const char*>(raw_bytecode), static_cast<std::streamsize>(raw_size));
            }
        }

        std::string tr_file;
        if (translated_bytecode && translated_size > 0) {
            tr_file = (shaders_dir / (stem + "_translated.dxil")).string();
            std::ofstream tf(tr_file, std::ios::binary);
            if (tf) {
                tf.write(static_cast<const char*>(translated_bytecode),
                         static_cast<std::streamsize>(translated_size));
            }
        }

        char magic_str[16] = "none";
        if (raw_bytecode && raw_size >= 4) {
            const auto* b = static_cast<const uint8_t*>(raw_bytecode);
            std::snprintf(magic_str, sizeof(magic_str), "%02X %02X %02X %02X", b[0], b[1], b[2], b[3]);
        }

        const auto caller_resolved = ResolveCodeAddress(caller_rip);
        const std::string meta_path = (shaders_dir / (stem + "_meta.json")).string();
        {
            std::ofstream mf(meta_path, std::ios::binary);
            if (mf) {
                mf << "{\n"
                   << "  \"artifact_type\": \"failing_shader_or_pso\",\n"
                   << "  \"index\": " << idx << ",\n"
                   << "  \"stage_or_api\": \"" << JsonEscape(stage_or_api ? stage_or_api : "PSO") << "\",\n"
                   << "  \"hresult\": \"" << HexU32(static_cast<uint32_t>(hr_or_status)) << "\",\n"
                   << "  \"hash_fnv1a64\": \"0x" << hash_hex << "\",\n"
                   << "  \"raw_magic_hex\": \"" << magic_str << "\",\n"
                   << "  \"raw_size\": " << raw_size << ",\n"
                   << "  \"translated_size\": " << translated_size << ",\n"
                   << "  \"raw_path\": \"" << JsonEscape(raw_file) << "\",\n"
                   << "  \"translated_path\": \"" << JsonEscape(tr_file) << "\",\n"
                   << "  \"caller\": \"" << JsonEscape(FormatResolvedAddress(caller_resolved)) << "\",\n"
                   << "  \"note\": \"" << JsonEscape(extra_note ? extra_note : "") << "\"\n"
                   << "}\n";
            }
        }
        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            shader_artifacts_.push_back(meta_path);
        }
        return meta_path;
    } catch (...) {
        return {};
    }
}

bool FlightRecorder::CaptureHangSnapshot(const char* reason,
                                         std::string* out_json_path) noexcept {
#if defined(_WIN32)
    const uint32_t seq = hang_seq_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (seq > kMaxHangSnapshots) {
        hang_seq_.fetch_sub(1u, std::memory_order_relaxed);
        return false;
    }

    struct RawThreadSample {
        uint32_t tid = 0;
        uint64_t rip = 0;
        uint64_t rsp = 0;
        uint64_t rbp = 0;
        uint32_t stack_word_count = 0;
        uint64_t stack_words[16] = {};
    };

    constexpr size_t kMaxSampledThreads = 64;
    RawThreadSample samples[kMaxSampledThreads]{};
    size_t sample_count = 0;

    const DWORD pid = GetCurrentProcessId();
    const DWORD self_tid = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok && sample_count < kMaxSampledThreads;
             ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self_tid) {
                continue;
            }
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;

            // CRITICAL: Zero heap allocations, zero locks, and zero module/symbol lookups while suspended!
            if (SuspendThread(th) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(th, &ctx)) {
                    RawThreadSample& dst = samples[sample_count++];
                    dst.tid = te.th32ThreadID;
                    dst.rip = ctx.Rip;
                    dst.rsp = ctx.Rsp;
                    dst.rbp = ctx.Rbp;
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (ctx.Rsp >= 0x10000ull &&
                        VirtualQuery(reinterpret_cast<LPCVOID>(ctx.Rsp), &mbi, sizeof(mbi)) != 0 &&
                        mbi.State == MEM_COMMIT &&
                        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) {
                        const uint64_t region_end =
                            reinterpret_cast<uint64_t>(mbi.BaseAddress) + static_cast<uint64_t>(mbi.RegionSize);
                        for (uint32_t w = 0; w < 16u; ++w) {
                            const uint64_t addr = ctx.Rsp + static_cast<uint64_t>(w) * 8ull;
                            if (addr + 8ull > region_end) break;
                            std::memcpy(&dst.stack_words[w], reinterpret_cast<const void*>(addr), sizeof(uint64_t));
                            dst.stack_word_count = w + 1u;
                        }
                    }
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
    }

    try {
        char phase_copy[48] = {};
        {
            ScopedSpinTryLock plk(phase_lock_, 32);
            if (plk.owns_lock()) {
                CopyBoundedStr(phase_copy, sizeof(phase_copy), last_phase_);
            }
        }

        const int64_t last_qpc = last_heartbeat_qpc_.load(std::memory_order_relaxed);
        const uint64_t stall_ms = static_cast<uint64_t>(
            std::max<int64_t>(0, NowQpc() - last_qpc) * 1000ll / QpcFreq());

        RecordEvent(FlightEventCategory::WatchdogHang,
                    "watchdog",
                    reason ? reason : "HangTimeout",
                    0,
                    sample_count > 0 ? samples[0].rip : 0ull,
                    stall_ms,
                    sample_count);

        const std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        const std::string base = "hang_snapshot_" + std::to_string(pid) + "_" + std::to_string(seq);
        const std::string json_path = (std::filesystem::path(out_dir) / (base + ".json")).string();
        const std::string txt_path = (std::filesystem::path(out_dir) / (base + ".txt")).string();

        const auto recent_events = SnapshotRecentEvents(64);

        {
            std::ofstream jf(json_path, std::ios::binary);
            if (jf) {
                jf << "{\n"
                   << "  \"report_type\": \"hang_snapshot\",\n"
                   << "  \"pid\": " << pid << ",\n"
                   << "  \"sequence\": " << seq << ",\n"
                   << "  \"reason\": \"" << JsonEscape(reason ? reason : "WatchdogStallTimeout") << "\",\n"
                   << "  \"last_phase\": \"" << JsonEscape(phase_copy) << "\",\n"
                   << "  \"stall_duration_ms\": " << stall_ms << ",\n"
                   << "  \"heartbeat_seq\": " << heartbeat_seq_.load(std::memory_order_relaxed) << ",\n"
                   << "  \"sampled_thread_count\": " << sample_count << ",\n"
                   << "  \"threads\": [\n";
                for (size_t i = 0; i < sample_count; ++i) {
                    const auto& ts = samples[i];
                    const auto rip_res = ResolveCodeAddress(ts.rip);
                    jf << "    {\n"
                       << "      \"tid\": " << ts.tid << ",\n"
                       << "      \"rip\": \"" << HexU64(ts.rip) << "\",\n"
                       << "      \"rsp\": \"" << HexU64(ts.rsp) << "\",\n"
                       << "      \"symbol\": \"" << JsonEscape(FormatResolvedAddress(rip_res)) << "\",\n"
                       << "      \"is_guest\": " << (rip_res.is_guest ? "true" : "false") << ",\n"
                       << "      \"stack_return_candidates\": [";
                    bool first_cand = true;
                    for (uint32_t w = 0; w < ts.stack_word_count; ++w) {
                        const uint64_t cand = ts.stack_words[w];
                        if (!IsExecutableCodeAddress(cand)) continue;
                        const auto ca = ResolveCodeAddress(cand);
                        if (!ca.resolved) continue;
                        if (!first_cand) jf << ", ";
                        first_cand = false;
                        jf << "\"" << JsonEscape(FormatResolvedAddress(ca)) << "\"";
                    }
                    jf << "]\n    }" << (i + 1 < sample_count ? "," : "") << "\n";
                }
                jf << "  ],\n  \"recent_events\": [\n";
                for (size_t i = 0; i < recent_events.size(); ++i) {
                    const auto& ev = recent_events[i];
                    jf << "    {\"seq\": " << ev.seq
                       << ", \"t_us\": " << ev.timestamp_us
                       << ", \"tid\": " << ev.thread_id
                       << ", \"cat\": \"" << FlightEventCategoryName(ev.category) << "\""
                       << ", \"sub\": \"" << JsonEscape(ev.subsystem) << "\""
                       << ", \"detail\": \"" << JsonEscape(ev.detail) << "\"}"
                       << (i + 1 < recent_events.size() ? "," : "") << "\n";
                }
                jf << "  ]\n}\n";
            }
        }

        {
            std::ofstream tf(txt_path);
            if (tf) {
                tf << "=== ScarlettEdge Flight-Recorder Hang Snapshot ===\n"
                   << "PID:          " << pid << "\n"
                   << "Reason:       " << (reason ? reason : "WatchdogStallTimeout") << "\n"
                   << "Stalled For:  " << stall_ms << " ms (last_phase=" << phase_copy << ")\n"
                   << "Threads (" << sample_count << "):\n";
                for (size_t i = 0; i < sample_count; ++i) {
                    const auto& ts = samples[i];
                    tf << "  - TID " << ts.tid << ": RIP=" << FormatResolvedAddress(ResolveCodeAddress(ts.rip))
                       << " RSP=" << HexU64(ts.rsp) << "\n";
                }
            }
        }

        {
            std::lock_guard<std::mutex> alk(artifacts_mu_);
            hang_artifacts_.push_back(json_path);
        }
        if (out_json_path) *out_json_path = json_path;
        return true;
    } catch (...) {
        return false;
    }
#else
    (void)reason;
    (void)out_json_path;
    return false;
#endif
}

bool FlightRecorder::WriteSummaryReport(std::string* out_json_path,
                                        std::string* out_txt_path) {
    const std::string out_dir = OutputDirectory().empty() ? "flight_logs" : OutputDirectory();
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    const std::string json_path = (std::filesystem::path(out_dir) / "flight_summary.json").string();
    const std::string txt_path = (std::filesystem::path(out_dir) / "flight_summary.txt").string();

    const FlightRecorderStats st = GetStats();
    const auto recent_events = SnapshotRecentEvents(256);

    std::vector<FlightStubMissRecord> stubs;
    std::vector<FlightComQiMissRecord> qi_misses;
    std::vector<FlightVtableMissRecord> vtbl_misses;
    std::vector<FlightVfsMissRecord> vfs_misses;
    {
        ScopedSpinTryLock mlk(miss_lock_, 512);
        if (mlk.owns_lock()) {
            stubs.assign(stub_misses_, stub_misses_ + stub_miss_count_);
            qi_misses.assign(com_qi_misses_, com_qi_misses_ + com_qi_miss_count_);
            vtbl_misses.assign(vtable_misses_, vtable_misses_ + vtable_miss_count_);
            vfs_misses.assign(vfs_misses_, vfs_misses_ + vfs_miss_count_);
        }
    }
    std::sort(stubs.begin(), stubs.end(), [](const auto& a, const auto& b) { return a.hit_count > b.hit_count; });
    std::sort(qi_misses.begin(), qi_misses.end(), [](const auto& a, const auto& b) { return a.hit_count > b.hit_count; });
    std::sort(vtbl_misses.begin(), vtbl_misses.end(), [](const auto& a, const auto& b) { return a.hit_count > b.hit_count; });
    std::sort(vfs_misses.begin(), vfs_misses.end(), [](const auto& a, const auto& b) { return a.hit_count > b.hit_count; });

    std::vector<std::string> crashes, hangs, dreds, shaders;
    {
        std::lock_guard<std::mutex> alk(artifacts_mu_);
        crashes = crash_artifacts_;
        hangs = hang_artifacts_;
        dreds = dred_artifacts_;
        shaders = shader_artifacts_;
    }

    char phase_copy[48] = {};
    {
        ScopedSpinTryLock plk(phase_lock_, 128);
        if (plk.owns_lock()) {
            CopyBoundedStr(phase_copy, sizeof(phase_copy), last_phase_);
        }
    }

    const char* title_id = std::getenv("SCARLETTX_TITLE_ID");
    const char* pkg_root = std::getenv("SCARLETTX_PACKAGE_ROOT");
    const uint64_t uptime_ms = ElapsedUs() / 1000ull;

    bool ok_json = false;
    {
        std::ofstream jf(json_path, std::ios::binary);
        if (jf) {
            jf << "{\n"
               << "  \"report_type\": \"flight_summary\",\n"
               << "  \"pid\": " << CurrentPid() << ",\n"
               << "  \"title_id\": \"" << JsonEscape(title_id ? title_id : "") << "\",\n"
               << "  \"package_root\": \"" << JsonEscape(pkg_root ? pkg_root : "") << "\",\n"
               << "  \"uptime_ms\": " << uptime_ms << ",\n"
               << "  \"last_phase\": \"" << JsonEscape(phase_copy) << "\",\n"
               << "  \"total_events\": " << st.total_events << ",\n"
               << "  \"heartbeats\": " << st.heartbeats << ",\n"
               << "  \"category_counts\": {\n";
            for (size_t i = 0; i < static_cast<size_t>(FlightEventCategory::Count); ++i) {
                jf << "    \"" << FlightEventCategoryName(static_cast<FlightEventCategory>(i)) << "\": "
                   << st.category_counts[i]
                   << (i + 1 < static_cast<size_t>(FlightEventCategory::Count) ? "," : "") << "\n";
            }
            jf << "  },\n  \"missing_imports\": [\n";
            for (size_t i = 0; i < stubs.size(); ++i) {
                const auto ca = ResolveCodeAddress(stubs[i].first_caller_rip);
                jf << "    {\"symbol\": \"" << JsonEscape(stubs[i].module_and_symbol) << "\""
                   << ", \"hits\": " << stubs[i].hit_count
                   << ", \"return_value\": \"" << HexU64(stubs[i].return_value) << "\""
                   << ", \"last_rcx\": \"" << HexU64(stubs[i].last_args[0]) << "\""
                   << ", \"last_rdx\": \"" << HexU64(stubs[i].last_args[1]) << "\""
                   << ", \"first_caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < stubs.size() ? "," : "") << "\n";
            }
            jf << "  ],\n  \"com_qi_misses\": [\n";
            for (size_t i = 0; i < qi_misses.size(); ++i) {
                const auto ca = ResolveCodeAddress(qi_misses[i].first_caller_rip);
                jf << "    {\"interface\": \"" << JsonEscape(qi_misses[i].interface_tag) << "\""
                   << ", \"iid\": \"" << JsonEscape(qi_misses[i].iid_str) << "\""
                   << ", \"hits\": " << qi_misses[i].hit_count
                   << ", \"first_caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < qi_misses.size() ? "," : "") << "\n";
            }
            jf << "  ],\n  \"vtable_slot_misses\": [\n";
            for (size_t i = 0; i < vtbl_misses.size(); ++i) {
                const auto ca = ResolveCodeAddress(vtbl_misses[i].first_caller_rip);
                jf << "    {\"interface\": \"" << JsonEscape(vtbl_misses[i].interface_tag) << "\""
                   << ", \"slot\": " << vtbl_misses[i].slot
                   << ", \"offset\": \"+0x" << std::hex << std::uppercase << (vtbl_misses[i].slot * 8) << std::dec << "\""
                   << ", \"hits\": " << vtbl_misses[i].hit_count
                   << ", \"first_caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < vtbl_misses.size() ? "," : "") << "\n";
            }
            jf << "  ],\n  \"vfs_misses\": [\n";
            for (size_t i = 0; i < vfs_misses.size(); ++i) {
                const auto ca = ResolveCodeAddress(vfs_misses[i].first_caller_rip);
                jf << "    {\"path\": \"" << JsonEscape(vfs_misses[i].virtual_path) << "\""
                   << ", \"error\": \"" << HexU32(static_cast<uint32_t>(vfs_misses[i].error_code)) << "\""
                   << ", \"hits\": " << vfs_misses[i].hit_count
                   << ", \"first_caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < vfs_misses.size() ? "," : "") << "\n";
            }
            auto write_str_vec = [&](const char* key, const std::vector<std::string>& vec, bool comma) {
                jf << "  \"" << key << "\": [";
                for (size_t i = 0; i < vec.size(); ++i) {
                    if (i > 0) jf << ", ";
                    jf << "\"" << JsonEscape(vec[i]) << "\"";
                }
                jf << "]" << (comma ? "," : "") << "\n";
            };
            jf << "  ],\n";
            write_str_vec("crash_reports", crashes, true);
            write_str_vec("hang_snapshots", hangs, true);
            write_str_vec("gpu_dred_reports", dreds, true);
            write_str_vec("shader_fail_artifacts", shaders, true);
            jf << "  \"recent_events\": [\n";
            for (size_t i = 0; i < recent_events.size(); ++i) {
                const auto& ev = recent_events[i];
                const auto ca = ResolveCodeAddress(ev.caller_rip);
                jf << "    {\"seq\": " << ev.seq
                   << ", \"t_us\": " << ev.timestamp_us
                   << ", \"tid\": " << ev.thread_id
                   << ", \"cat\": \"" << FlightEventCategoryName(ev.category) << "\""
                   << ", \"sub\": \"" << JsonEscape(ev.subsystem) << "\""
                   << ", \"detail\": \"" << JsonEscape(ev.detail) << "\""
                   << ", \"status\": \"" << HexU32(static_cast<uint32_t>(ev.status_code)) << "\""
                   << ", \"caller\": \"" << JsonEscape(FormatResolvedAddress(ca)) << "\"}"
                   << (i + 1 < recent_events.size() ? "," : "") << "\n";
            }
            jf << "  ]\n}\n";
            ok_json = jf.good();
        }
    }

    {
        std::ofstream tf(txt_path);
        if (tf) {
            tf << "=== ScarlettEdge Flight-Recorder Diagnostic Summary ===\n"
               << "PID:                 " << CurrentPid() << "\n"
               << "Title ID:            " << (title_id && *title_id ? title_id : "<none>") << "\n"
               << "Uptime:              " << uptime_ms << " ms (heartbeats=" << st.heartbeats
               << ", last_phase=" << phase_copy << ")\n"
               << "Total Ring Events:   " << st.total_events << "\n"
               << "Unique Stub Misses:  " << stubs.size() << "\n"
               << "Unique COM QI Miss:  " << qi_misses.size() << "\n"
               << "Unique Vtable Miss:  " << vtbl_misses.size() << "\n"
               << "Unique VFS Misses:   " << vfs_misses.size() << "\n"
               << "Crash Reports:       " << crashes.size() << "\n"
               << "Hang Snapshots:      " << hangs.size() << "\n"
               << "GPU DRED Reports:    " << dreds.size() << "\n"
               << "Shader Fail Dumps:   " << shaders.size() << "\n";
            if (!stubs.empty()) {
                tf << "\nTop Missing Imports (StubThunk):\n";
                for (const auto& s : stubs) {
                    tf << "  - " << s.module_and_symbol << " (hits=" << s.hit_count
                       << ", caller=" << FormatResolvedAddress(ResolveCodeAddress(s.first_caller_rip)) << ")\n";
                }
            }
            if (!qi_misses.empty()) {
                tf << "\nUnhandled COM QueryInterface IIDs:\n";
                for (const auto& q : qi_misses) {
                    tf << "  - " << q.interface_tag << " " << q.iid_str << " (hits=" << q.hit_count << ")\n";
                }
            }
            if (!vtbl_misses.empty()) {
                tf << "\nUnimplemented COM Vtable Slots:\n";
                for (const auto& v : vtbl_misses) {
                    tf << "  - " << v.interface_tag << "::vtable[" << v.slot << "] (hits=" << v.hit_count << ")\n";
                }
            }
            if (!vfs_misses.empty()) {
                tf << "\nMissing G:\\ / PackageVfs Assets:\n";
                for (const auto& m : vfs_misses) {
                    tf << "  - " << m.virtual_path << " (hits=" << m.hit_count
                       << ", err=" << HexU32(static_cast<uint32_t>(m.error_code)) << ")\n";
                }
            }
        }
    }

    if (out_json_path) *out_json_path = json_path;
    if (out_txt_path) *out_txt_path = txt_path;
    return ok_json;
}

}  // namespace scarlettx
