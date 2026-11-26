#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "scarlettx/core/status.h"
#include "scarlettx/core/types.h"

namespace scarlettx {

enum class MemoryPageSize : uint64_t {
    Page4KB = 4096,
    Page64KB = 65536,
    Page2MB = 2ull * 1024ull * 1024ull,
};

struct UnifiedMemoryBlock {
    void* host_ptr = nullptr;
    GpuVirtualAddress gpu_va = 0;
    size_t size = 0;
    MemoryPageSize page_size = MemoryPageSize::Page64KB;
    bool gpu_visible = true;
    bool write_combined = false;
};

// Emulates Scarlett's unified CPU/GPU virtual address space (64KB/2MB title memory pools).
class UnifiedMemoryManager {
public:
    UnifiedMemoryManager();
    ~UnifiedMemoryManager();

    UnifiedMemoryManager(const UnifiedMemoryManager&) = delete;
    UnifiedMemoryManager& operator=(const UnifiedMemoryManager&) = delete;

    Status Allocate(size_t size,
                    MemoryPageSize page_size,
                    bool gpu_visible,
                    bool write_combined,
                    UnifiedMemoryBlock& out_block);

    Status Free(GpuVirtualAddress gpu_va);
    bool LookupByGpuVa(GpuVirtualAddress gpu_va, UnifiedMemoryBlock& out_block) const;
    size_t TotalAllocatedBytes() const;
    size_t ActiveAllocationCount() const;

private:
    mutable std::mutex mutex_;
    GpuVirtualAddress next_gpu_va_ = 0x0000010000000000ull;
    size_t total_allocated_bytes_ = 0;
    std::unordered_map<GpuVirtualAddress, UnifiedMemoryBlock> blocks_;
};

}  // namespace scarlettx
