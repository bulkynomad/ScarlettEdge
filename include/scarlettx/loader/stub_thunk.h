#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

struct StubCallRecord {
    uint32_t stub_id = 0;
    std::string dll_name;
    std::string symbol_name;
    uint16_t ordinal = 0;
    bool by_ordinal = false;
    uint64_t return_value = 0;
    uint64_t hit_count = 0;
    uint64_t last_args[4] = {0, 0, 0, 0};
    uint64_t last_caller_rip = 0;
};

// Allocates executable x86-64 Windows-ABI trampolines for unresolved guest
// imports so Scarlett binaries can continue executing while recording exact
// missing-import telemetry, with full x64 .pdata unwind registration and W^X
// PAGE_EXECUTE_READ code protection.
class StubThunkTable {
public:
    using InterceptCallback = uint64_t (*)(uint32_t stub_id,
                                           const uint64_t* args4,
                                           uint64_t caller_rip,
                                           void* user_ctx);

    explicit StubThunkTable(size_t max_stubs = 2048);
    ~StubThunkTable();

    StubThunkTable(const StubThunkTable&) = delete;
    StubThunkTable& operator=(const StubThunkTable&) = delete;

    Status Initialize();
    void Reset();
    void SetVerboseLogging(bool verbose) noexcept { verbose_logging_ = verbose; }
    void SetInterceptCallback(InterceptCallback cb, void* user_ctx = nullptr) noexcept;

    // Allocates a new x86-64 trampoline for an unresolved import and returns
    // its executable function pointer address in out_thunk_address.
    Status AllocateStub(const std::string& dll_name,
                        const std::string& symbol_name,
                        bool by_ordinal,
                        uint16_t ordinal,
                        uint64_t return_value,
                        void*& out_thunk_address,
                        uint32_t* out_stub_id = nullptr);

    std::vector<StubCallRecord> SnapshotRecords() const;
    size_t StubCount() const;
    uint64_t TotalHitCount() const;

    // Called from generated x86-64 trampolines.
    uint64_t Dispatch(uint32_t stub_id, const uint64_t* args4, uint64_t caller_rip = 0);

private:
    static constexpr size_t kSlotSize = 64;

    size_t max_stubs_ = 2048;
    uint8_t* code_buffer_ = nullptr;
    size_t code_region_size_ = 0;
    size_t code_buffer_size_ = 0;
    void* pdata_table_ = nullptr;
    bool pdata_registered_ = false;
    bool verbose_logging_ = false;
    InterceptCallback intercept_cb_ = nullptr;
    void* intercept_ctx_ = nullptr;

    mutable std::mutex mutex_;
    std::vector<StubCallRecord> records_;
};

}  // namespace scarlettx
