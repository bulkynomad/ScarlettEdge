#include "scarlettx/loader/stub_thunk.h"

#include <cstring>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

extern "C" uint64_t ScarlettxStubDispatchEntry(void* table_ptr,
                                               uint32_t stub_id,
                                               const uint64_t* args4) {
    auto* table = static_cast<StubThunkTable*>(table_ptr);
    if (!table) {
        return 0;
    }
    return table->Dispatch(stub_id, args4);
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
#if defined(_WIN32)
    if (code_buffer_) {
        VirtualFree(code_buffer_, 0, MEM_RELEASE);
        code_buffer_ = nullptr;
        code_buffer_size_ = 0;
    }
#endif
}

Status StubThunkTable::Initialize() {
#if defined(_WIN32)
    if (code_buffer_) {
        return Status::Ok();
    }
    if (max_stubs_ == 0) {
        return Status::Error(StatusCode::InvalidArgument, "max_stubs must be > 0");
    }
    code_buffer_size_ = max_stubs_ * kSlotSize;
    void* mem = VirtualAlloc(nullptr, code_buffer_size_, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (!mem) {
        code_buffer_size_ = 0;
        return Status::Error(StatusCode::OutOfMemory, "VirtualAlloc failed for StubThunkTable");
    }
    code_buffer_ = static_cast<uint8_t*>(mem);
    std::memset(code_buffer_, 0xCC, code_buffer_size_);
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
    if (records_.size() >= max_stubs_) {
        return Status::Error(StatusCode::OutOfMemory, "StubThunkTable capacity exhausted");
    }

    const uint32_t stub_id = static_cast<uint32_t>(records_.size());
    StubCallRecord rec;
    rec.stub_id = stub_id;
    rec.dll_name = dll_name;
    rec.symbol_name = symbol_name;
    rec.by_ordinal = by_ordinal;
    rec.ordinal = ordinal;
    rec.return_value = return_value;
    records_.push_back(std::move(rec));

    uint8_t* slot = code_buffer_ + static_cast<size_t>(stub_id) * kSlotSize;
    uint8_t* p = slot;

    // Windows x64 ABI trampoline (61 bytes):
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
    //   add rsp, 0x48
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
        0x48, 0x83, 0xC4, 0x48,  // add rsp, 0x48
        0xC3,                    // ret
    };
    std::memcpy(p, kEpilogue, sizeof(kEpilogue));
    p += sizeof(kEpilogue);

    FlushInstructionCache(GetCurrentProcess(), slot, kSlotSize);

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

uint64_t StubThunkTable::Dispatch(uint32_t stub_id, const uint64_t* args4) {
    std::string dll;
    std::string sym;
    uint16_t ord = 0;
    bool by_ord = false;
    uint64_t ret = 0;
    uint64_t hits = 0;

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
        dll = rec.dll_name;
        sym = rec.symbol_name;
        ord = rec.ordinal;
        by_ord = rec.by_ordinal;
        ret = rec.return_value;
    }

    if (hits == 1 || verbose_logging_) {
        std::cout << "[ScarlettX][STUB] " << dll << "!";
        if (by_ord) {
            std::cout << "#" << ord;
        } else {
            std::cout << sym;
        }
        std::cout << " (hit #" << hits << ") -> returning 0x" << std::hex << ret << std::dec << "\n";
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
