#include "scarlettx/kernel/xmemory.h"

#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint64_t AlignUp64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

#if defined(_WIN32)
struct Win32AddrReq {
    PVOID LowestStartingAddress;
    PVOID HighestEndingAddress;
    SIZE_T Alignment;
};

struct alignas(8) Win32MemExtParam {
    ULONG64 Type;  // MemExtendedParameterAddressRequirements = 1
    PVOID Pointer;
};

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);

VirtualAlloc2Fn GetVirtualAlloc2() {
    static VirtualAlloc2Fn fn = []() -> VirtualAlloc2Fn {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) kb = LoadLibraryW(L"kernelbase.dll");
        return kb ? reinterpret_cast<VirtualAlloc2Fn>(
                        reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2")))
                  : nullptr;
    }();
    return fn;
}
#endif

void* AllocAlignedZeroedPages(size_t size, size_t alignment) {
#if defined(_WIN32)
    // VirtualAlloc2 / VirtualAlloc returns demand-zeroed OS virtual pages without
    // faulting in multi-GB arenas synchronously via std::memset.
    if (alignment > 0x10000u) {
        if (VirtualAlloc2Fn va2 = GetVirtualAlloc2()) {
            Win32AddrReq req{nullptr, nullptr, alignment};
            Win32MemExtParam ext{1ull, &req};
            if (void* p = va2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE, &ext, 1u)) {
                return p;
            }
        }
        // Fallback if VirtualAlloc2 is unavailable: probe an aligned region and commit it.
        if (size <= std::numeric_limits<size_t>::max() - alignment) {
            void* probe = VirtualAlloc(nullptr, size + alignment, MEM_RESERVE, PAGE_NOACCESS);
            if (probe) {
                const uintptr_t aligned_addr =
                    (reinterpret_cast<uintptr_t>(probe) + (alignment - 1u)) & ~(alignment - 1u);
                VirtualFree(probe, 0, MEM_RELEASE);
                if (void* p = VirtualAlloc(reinterpret_cast<PVOID>(aligned_addr), size,
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                    return p;
                }
            }
        }
        return nullptr;
    }
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0 || !ptr) {
        return nullptr;
    }
    std::memset(ptr, 0, size);
    return ptr;
#endif
}

void FreeAlignedPages(void* ptr) {
    if (!ptr) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    std::free(ptr);
#endif
}

}  // namespace

UnifiedMemoryManager::UnifiedMemoryManager() = default;

UnifiedMemoryManager::~UnifiedMemoryManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [va, block] : blocks_) {
        FreeAlignedPages(block.host_ptr);
    }
    blocks_.clear();
}

Status UnifiedMemoryManager::Allocate(size_t size,
                                      MemoryPageSize page_size,
                                      bool gpu_visible,
                                      bool write_combined,
                                      UnifiedMemoryBlock& out_block) {
    if (size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Allocation size must be > 0");
    }

    const size_t alignment = static_cast<size_t>(page_size);
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0 ||
        size > std::numeric_limits<size_t>::max() - (alignment - 1u)) {
        return Status::Error(StatusCode::InvalidArgument, "Unified memory allocation size overflow");
    }
    const size_t aligned_size = AlignUp(size, alignment);
    void* mem = AllocAlignedZeroedPages(aligned_size, alignment);
    if (!mem) {
        return Status::Error(StatusCode::OutOfMemory, "Unified memory allocation failed");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const GpuVirtualAddress va = AlignUp64(next_gpu_va_, static_cast<uint64_t>(alignment));
    next_gpu_va_ = va + aligned_size;

    UnifiedMemoryBlock block{};
    block.host_ptr = mem;
    block.gpu_va = va;
    block.size = aligned_size;
    block.page_size = page_size;
    block.gpu_visible = gpu_visible;
    block.write_combined = write_combined;

    blocks_[va] = block;
    total_allocated_bytes_ += aligned_size;
    out_block = block;
    return Status::Ok();
}

Status UnifiedMemoryManager::Free(GpuVirtualAddress gpu_va) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = blocks_.find(gpu_va);
    if (it == blocks_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown GPU virtual address");
    }
    FreeAlignedPages(it->second.host_ptr);
    total_allocated_bytes_ -= it->second.size;
    blocks_.erase(it);
    return Status::Ok();
}

bool UnifiedMemoryManager::LookupByGpuVa(GpuVirtualAddress gpu_va,
                                         UnifiedMemoryBlock& out_block) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [base_va, block] : blocks_) {
        if (gpu_va >= base_va && gpu_va - base_va < block.size) {
            out_block = block;
            return true;
        }
    }
    return false;
}

size_t UnifiedMemoryManager::TotalAllocatedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_allocated_bytes_;
}

size_t UnifiedMemoryManager::ActiveAllocationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.size();
}

}  // namespace scarlettx
