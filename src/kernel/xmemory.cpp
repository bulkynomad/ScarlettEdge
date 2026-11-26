#include "scarlettx/kernel/xmemory.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace scarlettx {

namespace {

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void* AllocAligned(size_t size, size_t alignment) {
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void FreeAligned(void* ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

}  // namespace

UnifiedMemoryManager::UnifiedMemoryManager() = default;

UnifiedMemoryManager::~UnifiedMemoryManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [va, block] : blocks_) {
        FreeAligned(block.host_ptr);
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
    const size_t aligned_size = AlignUp(size, alignment);
    void* mem = AllocAligned(aligned_size, alignment);
    if (!mem) {
        return Status::Error(StatusCode::OutOfMemory, "Unified memory allocation failed");
    }
    std::memset(mem, 0, aligned_size);

    std::lock_guard<std::mutex> lock(mutex_);
    const GpuVirtualAddress va = AlignUp(static_cast<size_t>(next_gpu_va_), alignment);
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
    FreeAligned(it->second.host_ptr);
    total_allocated_bytes_ -= it->second.size;
    blocks_.erase(it);
    return Status::Ok();
}

bool UnifiedMemoryManager::LookupByGpuVa(GpuVirtualAddress gpu_va,
                                         UnifiedMemoryBlock& out_block) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [base_va, block] : blocks_) {
        if (gpu_va >= base_va && gpu_va < base_va + block.size) {
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
