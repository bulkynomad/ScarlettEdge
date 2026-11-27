#pragma once

// ScarlettEdge 4-Part Flight-Recorder Diagnostic Suite
//
// Shared across scarlettx.exe and all 22 HLE shim DLLs via scarlettx_core.dll:
//   1. Lock-free 4096-slot process-wide event ring buffer + bounded deduplicated miss tables
//      (StubThunk missing imports, unhandled COM QueryInterface IIDs, unimplemented COM vtable
//      slots, WinRT runtime class activations, and G:\ VFS / DirectStorage file misses).
//   2. Vectored Exception Handler (VEH) + SEH post-mortem dumper walking x64 .pdata unwind
//      tables (RtlLookupFunctionEntry / RtlVirtualUnwind) to emit mapped guest PE64 RVAs
//      (Module+0xRVA and Ghidra 0x140000000+RVA) + host DLL RVAs into JSON/text crash reports.
//   3. Optional D3D12 DRED (Auto-Breadcrumbs + Page-Fault VA) extractor + bounded failing
//      Xbox DXBC/DXIL/XS2 + translated DXIL shader/PSO binary artifact dumper.
//   4. Background progress-heartbeat hang/deadlock watchdog suspending stalled threads only
//      long enough to read CONTEXT + raw stack words, plus post-run flight_summary.json/.txt.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#  include <intrin.h>
#  define SCARLETTX_CALLER_RIP() reinterpret_cast<uint64_t>(_ReturnAddress())
#elif defined(__GNUC__) || defined(__clang__)
#  define SCARLETTX_CALLER_RIP() reinterpret_cast<uint64_t>(__builtin_return_address(0))
#else
#  define SCARLETTX_CALLER_RIP() static_cast<uint64_t>(0)
#endif

namespace scarlettx {

enum class FlightEventCategory : uint8_t {
    Lifecycle = 0,
    HleCall = 1,
    StubHit = 2,
    ComQiMiss = 3,
    VtableSlotMiss = 4,
    WinRtBroker = 5,
    VfsAccess = 6,
    VfsMiss = 7,
    GpuBreadcrumb = 8,
    ShaderOrPsoFail = 9,
    GpuDeviceRemoved = 10,
    GuestFault = 11,
    WatchdogHang = 12,
    Count = 13,
};

const char* FlightEventCategoryName(FlightEventCategory cat) noexcept;

struct alignas(64) FlightEvent {
    uint64_t seq = 0;
    uint64_t timestamp_us = 0;
    uint32_t thread_id = 0;
    FlightEventCategory category = FlightEventCategory::Lifecycle;
    uint8_t reserved_pad[3] = {};
    int32_t status_code = 0;
    uint64_t caller_rip = 0;
    uint64_t arg0 = 0;
    uint64_t arg1 = 0;
    char subsystem[24] = {};
    char detail[64] = {};
};

struct FlightResolvedAddress {
    uint64_t raw_va = 0;
    uint64_t module_base = 0;
    uint32_t rva = 0;
    uint64_t ghidra_va = 0;  // Non-zero for mapped guest PE64 images (0x140000000+RVA / 0x180000000+RVA)
    bool resolved = false;
    bool is_guest = false;
    bool is_guest_exe = false;
    char module_name[64] = {};
};

struct FlightRecorderOptions {
    std::string output_dir;              // Defaults to ./flight_logs when empty
    uint32_t hang_timeout_ms = 15000;    // 0 = disable background hang watchdog
    bool install_veh = true;             // Install AddVectoredExceptionHandler crash catcher
    bool enable_dred = true;             // Enable D3D12 DRED auto-breadcrumbs + page faults
    bool dump_failing_shaders = true;    // Dump raw + translated shader blobs on PSO/shader failure
    bool reset_history = true;           // Clear previous session counters/ring when enabling
};

struct FlightGpuBreadcrumb {
    uint64_t command_list_id = 0;
    uint64_t marker_value = 0;
    char tag[64] = {};
};

struct FlightStubMissRecord {
    char module_and_symbol[96] = {};
    uint64_t hit_count = 0;
    uint64_t first_caller_rip = 0;
    uint64_t last_args[4] = {};
    uint64_t return_value = 0;
};

struct FlightComQiMissRecord {
    char interface_tag[48] = {};
    char iid_str[40] = {};
    uint64_t hit_count = 0;
    uint64_t first_caller_rip = 0;
};

struct FlightVtableMissRecord {
    char interface_tag[48] = {};
    int32_t slot = 0;
    uint64_t hit_count = 0;
    uint64_t first_caller_rip = 0;
    uint64_t last_arg0 = 0;
};

struct FlightVfsMissRecord {
    char virtual_path[160] = {};
    int32_t error_code = 0;
    uint64_t hit_count = 0;
    uint64_t first_caller_rip = 0;
};

struct FlightFramePacingSummary {
    uint64_t frames_recorded = 0;
    uint64_t stutter_count = 0;  // frames > 33.33 ms
    double avg_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

struct FlightRecorderStats {
    uint64_t total_events = 0;
    uint64_t heartbeats = 0;
    uint64_t category_counts[static_cast<size_t>(FlightEventCategory::Count)] = {};
    uint32_t unique_stub_misses = 0;
    uint32_t unique_com_qi_misses = 0;
    uint32_t unique_vtable_misses = 0;
    uint32_t unique_vfs_misses = 0;
    uint32_t crash_reports_written = 0;
    uint32_t hang_snapshots_written = 0;
    uint32_t dred_reports_written = 0;
    uint32_t shader_dumps_written = 0;
};

class FlightRecorder {
public:
    static constexpr size_t kRingCapacity = 4096;
    static constexpr size_t kMaxUniqueMisses = 256;
    static constexpr uint32_t kMaxCrashReports = 4;
    static constexpr uint32_t kMaxHangSnapshots = 4;
    static constexpr uint32_t kMaxDredReports = 8;
    static constexpr uint32_t kMaxShaderDumps = 32;

    static FlightRecorder& Instance() noexcept;

    // Fast-path check (probes SCARLETTX_FLIGHT_RECORDER once per process outside DllMain,
    // then executes as a single relaxed atomic load).
    static bool IsEnabled() noexcept;

    void Enable(const FlightRecorderOptions& opts);
    void Disable() noexcept;
    void ResetSession() noexcept;

    std::string OutputDirectory() const;

    // -----------------------------------------------------------------------
    // Part 1: Lock-free event ring + deduplicated miss recorders + heartbeats
    // -----------------------------------------------------------------------
    void RecordEvent(FlightEventCategory category,
                     const char* subsystem,
                     const char* detail,
                     int32_t status_code = 0,
                     uint64_t caller_rip = 0,
                     uint64_t arg0 = 0,
                     uint64_t arg1 = 0) noexcept;

    void RecordStubHit(const char* dll_name,
                       const char* symbol_name,
                       bool by_ordinal,
                       uint16_t ordinal,
                       const uint64_t* args4,
                       uint64_t return_value,
                       uint64_t caller_rip) noexcept;

    void RecordComQiMiss(const char* interface_tag,
                         const void* iid_16bytes,
                         uint64_t caller_rip) noexcept;

    void RecordVtableSlotMiss(const char* interface_tag,
                              int32_t slot,
                              uint64_t arg0,
                              uint64_t caller_rip) noexcept;

    void RecordWinRtActivation(const char* api_name,
                               const wchar_t* runtime_class_name,
                               int32_t hr,
                               bool handled_by_hle,
                               uint64_t caller_rip) noexcept;

    void RecordVfsAccess(const char* virtual_path,
                         bool resolved_ok,
                         int32_t win32_or_hresult_err,
                         uint64_t caller_rip) noexcept;

    void RecordGpuBreadcrumb(uint64_t cmd_list_id,
                             uint64_t marker_value,
                             const char* tag) noexcept;

    void BeatProgress(const char* phase) noexcept;

    // Lock-free frame-time recorder for p50/p95/p99 & stutter profiling.
    void RecordFrameTimeUs(uint64_t frame_us) noexcept;
    FlightFramePacingSummary GetFramePacingSummary() const noexcept;

    // -----------------------------------------------------------------------
    // Part 2: Crash handler (VEH / SEH) + x64 RVA stack-walk dumper
    // -----------------------------------------------------------------------
    static FlightResolvedAddress ResolveCodeAddress(uint64_t va) noexcept;
    static std::string FormatResolvedAddress(const FlightResolvedAddress& addr);

    // Captures a full post-mortem JSON + text crash dump from Win32 EXCEPTION_POINTERS*.
    // Safe to call from both VEH and SEH __except filters.
    bool CaptureCrashReport(const void* win32_exception_pointers,
                            const char* reason,
                            std::string* out_json_path = nullptr) noexcept;

    // -----------------------------------------------------------------------
    // Part 3: GPU DRED enablement/extraction + failing shader/PSO binary dump
    // -----------------------------------------------------------------------
    void EnableD3D12DredIfConfigured() noexcept;

    std::string RecordGpuDeviceRemoved(void* native_id3d12_device,
                                       int32_t triggering_hr,
                                       const char* context_site,
                                       const FlightGpuBreadcrumb* breadcrumbs = nullptr,
                                       size_t breadcrumb_count = 0) noexcept;

    std::string DumpFailingShaderArtifact(const char* stage_or_api,
                                          int32_t hr_or_status,
                                          const void* raw_bytecode,
                                          size_t raw_size,
                                          const void* translated_bytecode,
                                          size_t translated_size,
                                          uint64_t caller_rip,
                                          const char* extra_note = nullptr) noexcept;

    // -----------------------------------------------------------------------
    // Part 4: Watchdog hang snapshot + post-run summary report
    // -----------------------------------------------------------------------
    bool CaptureHangSnapshot(const char* reason,
                             std::string* out_json_path = nullptr) noexcept;

    bool WriteSummaryReport(std::string* out_json_path = nullptr,
                            std::string* out_txt_path = nullptr);

    std::vector<FlightEvent> SnapshotRecentEvents(size_t max_events = 256) const;
    FlightRecorderStats GetStats() const noexcept;

private:
    FlightRecorder();
    ~FlightRecorder();
    FlightRecorder(const FlightRecorder&) = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;

    void ProbeEnvOnce() noexcept;
    void StartWatchdogLocked(uint32_t timeout_ms);
    void StopWatchdog() noexcept;
    void WatchdogLoop(uint32_t timeout_ms);

    uint64_t ElapsedUs() const noexcept;
    static int64_t NowQpc() noexcept;
    static int64_t QpcFreq() noexcept;

    // 0 = unprobed env, 1 = disabled, 2 = enabled
    static std::atomic<uint8_t> s_enabled_state_;
    std::atomic<bool> env_probing_{false};

    mutable std::mutex lifecycle_mu_;
    FlightRecorderOptions opts_{};
    void* veh_handle_ = nullptr;
    std::atomic<bool> dred_enabled_flag_{false};
    std::atomic<bool> dred_applied_{false};
    std::atomic<bool> shader_dump_flag_{false};

    int64_t start_qpc_ = 0;

    // Lock-free MPMC ring buffer
    alignas(64) FlightEvent ring_[kRingCapacity];
    std::atomic<uint64_t> committed_seq_[kRingCapacity];
    alignas(64) std::atomic<uint64_t> write_seq_{0};
    std::atomic<uint64_t> category_counts_[static_cast<size_t>(FlightEventCategory::Count)];

    // Progress heartbeat & background hang watchdog
    alignas(64) std::atomic<uint64_t> heartbeat_seq_{0};
    std::atomic<int64_t> last_heartbeat_qpc_{0};
    mutable std::atomic_flag phase_lock_ = ATOMIC_FLAG_INIT;
    char last_phase_[48] = "init";

    std::mutex watchdog_mu_;
    std::condition_variable watchdog_cv_;
    std::thread watchdog_thread_;
    bool watchdog_stop_ = false;
    std::atomic<uint64_t> last_hung_heartbeat_seq_{~0ull};

    // Bounded non-blocking deduplicated miss registries (guarded by try_lock spinflag)
    mutable std::atomic_flag miss_lock_ = ATOMIC_FLAG_INIT;
    FlightStubMissRecord stub_misses_[kMaxUniqueMisses];
    uint32_t stub_miss_count_ = 0;
    FlightComQiMissRecord com_qi_misses_[kMaxUniqueMisses];
    uint32_t com_qi_miss_count_ = 0;
    FlightVtableMissRecord vtable_misses_[kMaxUniqueMisses];
    uint32_t vtable_miss_count_ = 0;
    FlightVfsMissRecord vfs_misses_[kMaxUniqueMisses];
    uint32_t vfs_miss_count_ = 0;

    // Recent GPU breadcrumbs mirror (64 slots, try_lock guarded)
    mutable std::atomic_flag gpu_bc_lock_ = ATOMIC_FLAG_INIT;
    FlightGpuBreadcrumb recent_gpu_breadcrumbs_[64];
    uint64_t total_gpu_breadcrumbs_ = 0;

    // Lock-free frame-time ring buffer (1024 recent frame durations in microseconds)
    static constexpr size_t kFrameTimeRingCap = 1024;
    std::atomic<uint64_t> frame_times_us_[kFrameTimeRingCap] = {};
    std::atomic<uint64_t> frame_time_count_{0};
    std::atomic<uint64_t> frame_stutter_count_{0};

    // Crash/artifact counters & deduplication
    std::atomic<uint32_t> crash_seq_{0};
    std::atomic<uint32_t> hang_seq_{0};
    std::atomic<uint32_t> dred_seq_{0};
    std::atomic<uint32_t> shader_seq_{0};
    std::atomic<uint64_t> last_crash_sig_{0};

    mutable std::mutex artifacts_mu_;
    std::vector<std::string> crash_artifacts_;
    std::vector<std::string> hang_artifacts_;
    std::vector<std::string> dred_artifacts_;
    std::vector<std::string> shader_artifacts_;
    uint64_t dumped_shader_hashes_[kMaxShaderDumps] = {};
    uint32_t dumped_shader_hash_count_ = 0;
};

}  // namespace scarlettx
