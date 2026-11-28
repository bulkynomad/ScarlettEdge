#include "scarlettx/loader/stub_thunk.h"

#include "scarlettx/core/flight_recorder.h"

#include <cstring>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

constexpr size_t kPageSize = 0x1000u;

size_t AlignUpPage(size_t v) {
    return (v + kPageSize - 1u) & ~(kPageSize - 1u);
}

size_t AlignUp16(size_t v) {
    return (v + 15u) & ~size_t{15u};
}

// Keep C++ linkage (never extern "C" or noexcept) so MSVC /EHsc emits unwind
// metadata permitting C++ and SEH exceptions to propagate across StubThunk frames.
uint64_t ScarlettxStubDispatchEntry(void* table_ptr,
                                    uint32_t stub_id,
                                    const uint64_t* args4) {
    auto* table = static_cast<StubThunkTable*>(table_ptr);
    if (!table) {
        return 0;
    }
    // In AllocateStub's x64 frame (`sub rsp, 0x48`), `args4` points to `[rsp+0x20]`,
    // so `args4[5]` (`[rsp+0x48]`) is the exact guest return address pushed by `call`.
    const uint64_t caller_rip = args4 ? args4[5] : 0ull;
    return table->Dispatch(stub_id, args4, caller_rip);
}

void WriteU32LE(uint8_t* dst, uint32_t v) {
    std::memcpy(dst, &v, sizeof(v));
}

void WriteU64LE(uint8_t* dst, uint64_t v) {
    std::memcpy(dst, &v, sizeof(v));
}

}  // namespace

StubThunkTable::StubThunkTable(size_t max_stubs) : max_stubs_(max_stubs) {}

StubThunkTable::~StubThunkTable() {
    Reset();
}

void StubThunkTable::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    if (pdata_registered_ && pdata_table_) {
        RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(pdata_table_));
    }
    if (code_buffer_) {
        VirtualFree(code_buffer_, 0, MEM_RELEASE);
    }
#endif
    code_buffer_ = nullptr;
    code_region_size_ = 0;
    code_buffer_size_ = 0;
    pdata_table_ = nullptr;
    pdata_registered_ = false;
    records_.clear();
}

void StubThunkTable::SetInterceptCallback(InterceptCallback cb, void* user_ctx) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    intercept_cb_ = cb;
    intercept_ctx_ = user_ctx;
}

Status StubThunkTable::Initialize() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex_);
    if (code_buffer_) {
        return Status::Ok();
    }
    if (max_stubs_ == 0 || max_stubs_ > 0x100000u) {
        return Status::Error(StatusCode::InvalidArgument, "max_stubs must be in 1..1048576");
    }

    // Slab layout within a single contiguous VirtualAlloc reservation (< 4 GiB RVA span):
    //   [0 .. code_region_size_)                  -> 64B x64 trampolines (PAGE_EXECUTE_READ)
    //   [code_region_size_ .. +8)                 -> shared x64 UNWIND_INFO (UWOP_ALLOC_SMALL 0x48)
    //   [code_region_size_ + 16 .. total_size)    -> sorted RUNTIME_FUNCTION[max_stubs_] (.pdata)
    code_region_size_ = AlignUpPage(max_stubs_ * kSlotSize);
    const size_t xdata_size = 16u;
    const size_t pdata_bytes = max_stubs_ * sizeof(RUNTIME_FUNCTION);
    const size_t meta_region_size = AlignUpPage(AlignUp16(xdata_size + pdata_bytes));
    code_buffer_size_ = code_region_size_ + meta_region_size;

    void* mem = VirtualAlloc(nullptr, code_buffer_size_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        code_region_size_ = 0;
        code_buffer_size_ = 0;
        return Status::Error(StatusCode::OutOfMemory, "VirtualAlloc failed for StubThunkTable");
    }
    code_buffer_ = static_cast<uint8_t*>(mem);
    std::memset(code_buffer_, 0xCC, code_region_size_);

    // Emit shared x64 UNWIND_INFO matching `sub rsp, 0x48` (4-byte prologue at +0..+4):
    //   Version = 1, Flags = 0 (UNW_FLAG_NHANDLER)
    //   SizeOfProlog = 4
    //   CountOfCodes = 1
    //   FrameRegister = 0, FrameOffset = 0
    //   UNWIND_CODE[0]: CodeOffset = 4, UnwindOp = UWOP_ALLOC_SMALL (2), OpInfo = 8 ((0x48 - 8) / 8)
    static constexpr uint8_t kUnwindInfo[8] = {
        0x01u, 0x04u, 0x01u, 0x00u,
        0x04u, static_cast<uint8_t>((8u << 4) | 2u), 0x00u, 0x00u,
    };
    const DWORD unwind_info_rva = static_cast<DWORD>(code_region_size_);
    std::memcpy(code_buffer_ + unwind_info_rva, kUnwindInfo, sizeof(kUnwindInfo));

    auto* pdata = reinterpret_cast<PRUNTIME_FUNCTION>(code_buffer_ + code_region_size_ + xdata_size);
    for (size_t i = 0; i < max_stubs_; ++i) {
        const DWORD begin_rva = static_cast<DWORD>(i * kSlotSize);
        pdata[i].BeginAddress = begin_rva;
        pdata[i].EndAddress = begin_rva + static_cast<DWORD>(kSlotSize);
        pdata[i].UnwindData = unwind_info_rva;
    }

    DWORD old_prot = 0;
    if (!VirtualProtect(code_buffer_ + code_region_size_, meta_region_size, PAGE_READONLY, &old_prot) ||
        !VirtualProtect(code_buffer_, code_region_size_, PAGE_EXECUTE_READ, &old_prot)) {
        VirtualFree(code_buffer_, 0, MEM_RELEASE);
        code_buffer_ = nullptr;
        code_region_size_ = 0;
        code_buffer_size_ = 0;
        return Status::Error(StatusCode::DeviceError,
                             "VirtualProtect failed for StubThunkTable W^X pages");
    }
    FlushInstructionCache(GetCurrentProcess(), code_buffer_, code_region_size_);

    if (!RtlAddFunctionTable(pdata, static_cast<DWORD>(max_stubs_),
                             reinterpret_cast<DWORD64>(code_buffer_))) {
        VirtualFree(code_buffer_, 0, MEM_RELEASE);
        code_buffer_ = nullptr;
        code_region_size_ = 0;
        code_buffer_size_ = 0;
        return Status::Error(StatusCode::DeviceError,
                             "RtlAddFunctionTable failed for StubThunkTable .pdata");
    }
    pdata_table_ = pdata;
    pdata_registered_ = true;
    return Status::Ok();
#else
    return Status::Error(StatusCode::NotImplemented,
                         "Executable x86-64 stub thunks require Windows x64");
#endif
}

Status StubThunkTable::AllocateStub(const std::string& dll_name,
                                    const std::string& symbol_name,
                                    bool by_ordinal,
                                    uint16_t ordinal,
                                    uint64_t return_value,
                                    void*& out_thunk_address,
                                    uint32_t* out_stub_id) {
    out_thunk_address = nullptr;
#if defined(_WIN32)
    if (!code_buffer_) {
        Status st = Initialize();
        if (!st.ok()) {
            return st;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!code_buffer_ || records_.size() >= max_stubs_) {
        return Status::Error(StatusCode::OutOfMemory, "StubThunkTable capacity exhausted");
    }

    const uint32_t stub_id = static_cast<uint32_t>(records_.size());
    uint8_t* slot = code_buffer_ + static_cast<size_t>(stub_id) * kSlotSize;
    uint8_t* page_base = reinterpret_cast<uint8_t*>(
        reinterpret_cast<uintptr_t>(slot) & ~(kPageSize - 1u));

    DWORD old_prot = 0;
    if (!VirtualProtect(page_base, kPageSize, PAGE_READWRITE, &old_prot)) {
        return Status::Error(StatusCode::DeviceError,
                             "VirtualProtect(PAGE_READWRITE) failed while emitting StubThunk");
    }

    uint8_t* p = slot;

    // Windows x64 ABI trampoline (64 bytes, canonical prologue + epilogue):
    //   sub rsp, 0x48              ; 32B shadow + 32B saved args + 8B alignment
    //   mov [rsp+0x20], rcx
    //   mov [rsp+0x28], rdx
    //   mov [rsp+0x30], r8
    //   mov [rsp+0x38], r9
    //   mov rcx, <this>
    //   mov edx, <stub_id>
    //   lea r8, [rsp+0x20]
    //   mov rax, <&ScarlettxStubDispatchEntry>
    //   call rax
    //   xorps xmm0, xmm0           ; deterministic 0.0f/0.0 for float/double stubs
    //   add rsp, 0x48              ; canonical x64 epilogue immediately preceding ret
    //   ret
    static constexpr uint8_t kPrologue[] = {
        0x48, 0x83, 0xEC, 0x48,              // sub rsp, 0x48
        0x48, 0x89, 0x4C, 0x24, 0x20,        // mov [rsp+0x20], rcx
        0x48, 0x89, 0x54, 0x24, 0x28,        // mov [rsp+0x28], rdx
        0x4C, 0x89, 0x44, 0x24, 0x30,        // mov [rsp+0x30], r8
        0x4C, 0x89, 0x4C, 0x24, 0x38,        // mov [rsp+0x38], r9
    };
    std::memcpy(p, kPrologue, sizeof(kPrologue));
    p += sizeof(kPrologue);

    *p++ = 0x48;
    *p++ = 0xB9;  // mov rcx, imm64
    WriteU64LE(p, reinterpret_cast<uint64_t>(this));
    p += 8;

    *p++ = 0xBA;  // mov edx, imm32
    WriteU32LE(p, stub_id);
    p += 4;

    static constexpr uint8_t kLeaR8[] = {0x4C, 0x8D, 0x44, 0x24, 0x20};  // lea r8, [rsp+0x20]
    std::memcpy(p, kLeaR8, sizeof(kLeaR8));
    p += sizeof(kLeaR8);

    *p++ = 0x48;
    *p++ = 0xB8;  // mov rax, imm64
    WriteU64LE(p, reinterpret_cast<uint64_t>(&ScarlettxStubDispatchEntry));
    p += 8;

    static constexpr uint8_t kEpilogue[] = {
        0xFF, 0xD0,              // call rax
        0x0F, 0x57, 0xC0,        // xorps xmm0, xmm0
        0x48, 0x83, 0xC4, 0x48,  // add rsp, 0x48
        0xC3,                    // ret
    };
    static_assert(sizeof(kPrologue) + 10 + 5 + sizeof(kLeaR8) + 10 + sizeof(kEpilogue) <= kSlotSize,
                  "StubThunkTable x64 trampoline exceeds kSlotSize");
    std::memcpy(p, kEpilogue, sizeof(kEpilogue));
    p += sizeof(kEpilogue);

    FlushInstructionCache(GetCurrentProcess(), slot, kSlotSize);
    DWORD rx_prot = 0;
    if (!VirtualProtect(page_base, kPageSize, PAGE_EXECUTE_READ, &rx_prot)) {
        return Status::Error(StatusCode::DeviceError,
                             "VirtualProtect(PAGE_EXECUTE_READ) failed after emitting StubThunk");
    }

    StubCallRecord rec;
    rec.stub_id = stub_id;
    rec.dll_name = dll_name;
    rec.symbol_name = symbol_name;
    rec.by_ordinal = by_ordinal;
    rec.ordinal = ordinal;
    rec.return_value = return_value;
    records_.push_back(std::move(rec));

    out_thunk_address = slot;
    if (out_stub_id) {
        *out_stub_id = stub_id;
    }
    return Status::Ok();
#else
    (void)dll_name;
    (void)symbol_name;
    (void)by_ordinal;
    (void)ordinal;
    (void)return_value;
    (void)out_stub_id;
    return Status::Error(StatusCode::NotImplemented,
                         "Executable x86-64 stub thunks require Windows x64");
#endif
}

uint64_t StubThunkTable::Dispatch(uint32_t stub_id, const uint64_t* args4, uint64_t caller_rip) {
    std::string dll;
    std::string sym;
    uint16_t ord = 0;
    bool by_ord = false;
    uint64_t ret = 0;
    uint64_t hits = 0;
    InterceptCallback intercept = nullptr;
    void* intercept_ctx = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stub_id >= records_.size()) {
            return 0;
        }
        StubCallRecord& rec = records_[stub_id];
        rec.hit_count += 1;
        hits = rec.hit_count;
        if (args4) {
            for (int i = 0; i < 4; ++i) {
                rec.last_args[i] = args4[i];
            }
        }
        if (caller_rip != 0) {
            rec.last_caller_rip = caller_rip;
        }
        dll = rec.dll_name;
        sym = rec.symbol_name;
        ord = rec.ordinal;
        by_ord = rec.by_ordinal;
        ret = rec.return_value;
        intercept = intercept_cb_;
        intercept_ctx = intercept_ctx_;
    }

    FlightRecorder::Instance().RecordStubHit(
        dll.c_str(), sym.c_str(), by_ord, ord, args4, ret, caller_rip);

    if (hits == 1 || verbose_logging_) {
        std::cout << "[ScarlettX][STUB] " << dll << "!";
        if (by_ord) {
            std::cout << "#" << ord;
        } else {
            std::cout << sym;
        }
        std::cout << " (hit #" << hits << ") -> returning 0x" << std::hex << ret << std::dec << "\n";
    }

    if (intercept) {
        return intercept(stub_id, args4, caller_rip, intercept_ctx);
    }
    return ret;
}

std::vector<StubCallRecord> StubThunkTable::SnapshotRecords() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_;
}

size_t StubThunkTable::StubCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

uint64_t StubThunkTable::TotalHitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    for (const auto& r : records_) {
        total += r.hit_count;
    }
    return total;
}

}  // namespace scarlettx
