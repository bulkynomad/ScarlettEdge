#define SCARLETTX_BUILDING_KERNELX 1
#include "scarlettx/kernel/kernelx_abi.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#include <condition_variable>
#endif

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kOutOfMemory = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kTimeout = static_cast<int32_t>(0x800705B4u);

struct MemBlock {
    void* va = nullptr;
    uint64_t gpu_va = 0;
    uint64_t size = 0;
    uint32_t page_size = 0;
    uint32_t flags = 0;
};

struct KernelXState {
    std::mutex mu;
    uint64_t next_gpu_va = 0x0000010000000000ull;
    uint64_t total_allocated = 0;
    uint64_t peak_allocated = 0;
    std::unordered_map<uintptr_t, MemBlock> by_va;
    std::unordered_map<uint64_t, uintptr_t> by_gpu_va;
};

KernelXState& State() {
    static KernelXState s;
    return s;
}

#if !defined(_WIN32)
struct PosixEvent {
    std::mutex mu;
    std::condition_variable cv;
    bool manual_reset = false;
    bool signaled = false;
};
#endif

}  // namespace

extern "C" {

int32_t SCARLETTX_KERNELX_CALL KernelXAllocateMemory(
    uint64_t size, uint32_t pageSize, uint32_t flags, void** outVirtualAddress, uint64_t* outGpuAddress) {
    if (!outVirtualAddress || !outGpuAddress || size == 0) return kInvalidArg;

    const uint32_t align = (pageSize == KERNELX_PAGE_2MB) ? 0x200000 :
                           (pageSize == KERNELX_PAGE_64KB) ? 0x10000 : 0x1000;
    const uint64_t aligned_size = (size + align - 1) & ~(static_cast<uint64_t>(align) - 1);

    void* mem = nullptr;
#if defined(_WIN32)
    mem = VirtualAlloc(nullptr, aligned_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    if (posix_memalign(&mem, align, aligned_size) != 0) mem = nullptr;
#endif
    if (!mem) return kOutOfMemory;
    std::memset(mem, 0, aligned_size);

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t gpu_addr = State().next_gpu_va;
    State().next_gpu_va += aligned_size;
    State().total_allocated += aligned_size;
    if (State().total_allocated > State().peak_allocated) {
        State().peak_allocated = State().total_allocated;
    }

    MemBlock b{};
    b.va = mem;
    b.gpu_va = gpu_addr;
    b.size = aligned_size;
    b.page_size = align;
    b.flags = flags;
    State().by_va[reinterpret_cast<uintptr_t>(mem)] = b;
    State().by_gpu_va[gpu_addr] = reinterpret_cast<uintptr_t>(mem);

    *outVirtualAddress = mem;
    *outGpuAddress = gpu_addr;
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXFreeMemory(void* virtualAddress) {
    if (!virtualAddress) return kSuccess;
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().by_va.find(reinterpret_cast<uintptr_t>(virtualAddress));
    if (it == State().by_va.end()) return kInvalidArg;

    State().total_allocated = State().total_allocated >= it->second.size ?
                              State().total_allocated - it->second.size : 0;
    State().by_gpu_va.erase(it->second.gpu_va);
#if defined(_WIN32)
    VirtualFree(virtualAddress, 0, MEM_RELEASE);
#else
    std::free(virtualAddress);
#endif
    State().by_va.erase(it);
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXMapVirtualToGpu(const void* virtualAddress, uint64_t* outGpuAddress) {
    if (!virtualAddress || !outGpuAddress) return kInvalidArg;
    const uintptr_t va = reinterpret_cast<uintptr_t>(virtualAddress);
    std::lock_guard<std::mutex> lk(State().mu);
    for (const auto& kv : State().by_va) {
        const uintptr_t base = kv.first;
        if (va >= base && va < base + kv.second.size) {
            *outGpuAddress = kv.second.gpu_va + (va - base);
            return kSuccess;
        }
    }
    return kInvalidArg;
}

int32_t SCARLETTX_KERNELX_CALL KernelXMapGpuToVirtual(uint64_t gpuAddress, void** outVirtualAddress) {
    if (!outVirtualAddress) return kInvalidArg;
    std::lock_guard<std::mutex> lk(State().mu);
    for (const auto& kv : State().by_va) {
        const uint64_t base = kv.second.gpu_va;
        if (gpuAddress >= base && gpuAddress < base + kv.second.size) {
            *outVirtualAddress = reinterpret_cast<void*>(kv.first + (gpuAddress - base));
            return kSuccess;
        }
    }
    return kInvalidArg;
}

int32_t SCARLETTX_KERNELX_CALL KernelXProtectMemory(void* virtualAddress, uint64_t size, uint32_t protectFlags) {
    if (!virtualAddress || size == 0) return kInvalidArg;
#if defined(_WIN32)
    DWORD old_p = 0;
    DWORD new_p = (protectFlags & KERNELX_MEM_READ_ONLY) ? PAGE_READONLY : PAGE_READWRITE;
    if (!VirtualProtect(virtualAddress, size, new_p, &old_p)) return kInvalidArg;
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXQueryMemoryStatistics(KERNELX_MEMORY_STATS* outStats) {
    if (!outStats) return kInvalidArg;
    std::lock_guard<std::mutex> lk(State().mu);
    constexpr uint64_t kAnacondaTotal = 16ull * 1024ull * 1024ull * 1024ull;
    outStats->totalUnifiedBytes = kAnacondaTotal;
    outStats->allocatedBytes = State().total_allocated;
    outStats->freeBytes = (kAnacondaTotal > State().total_allocated) ? (kAnacondaTotal - State().total_allocated) : 0;
    outStats->peakAllocatedBytes = State().peak_allocated;
    outStats->activeAllocations = static_cast<uint32_t>(State().by_va.size());
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXCreateSynchronizationEvent(
    int32_t manualReset, int32_t initialState, KERNELX_EVENT_HANDLE* outEvent) {
    if (!outEvent) return kInvalidArg;
#if defined(_WIN32)
    HANDLE ev = CreateEventW(nullptr, manualReset ? TRUE : FALSE, initialState ? TRUE : FALSE, nullptr);
    if (!ev) return kOutOfMemory;
    *outEvent = reinterpret_cast<KERNELX_EVENT_HANDLE>(ev);
#else
    auto* ev = new PosixEvent();
    ev->manual_reset = (manualReset != 0);
    ev->signaled = (initialState != 0);
    *outEvent = reinterpret_cast<KERNELX_EVENT_HANDLE>(ev);
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXSetSynchronizationEvent(KERNELX_EVENT_HANDLE eventHandle) {
    if (!eventHandle) return kInvalidArg;
#if defined(_WIN32)
    SetEvent(reinterpret_cast<HANDLE>(eventHandle));
#else
    auto* ev = static_cast<PosixEvent*>(eventHandle);
    {
        std::lock_guard<std::mutex> lk(ev->mu);
        ev->signaled = true;
    }
    if (ev->manual_reset) ev->cv.notify_all();
    else ev->cv.notify_one();
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXResetSynchronizationEvent(KERNELX_EVENT_HANDLE eventHandle) {
    if (!eventHandle) return kInvalidArg;
#if defined(_WIN32)
    ResetEvent(reinterpret_cast<HANDLE>(eventHandle));
#else
    auto* ev = static_cast<PosixEvent*>(eventHandle);
    std::lock_guard<std::mutex> lk(ev->mu);
    ev->signaled = false;
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXWaitForSynchronizationEvent(KERNELX_EVENT_HANDLE eventHandle, uint32_t timeoutMs) {
    if (!eventHandle) return kInvalidArg;
#if defined(_WIN32)
    DWORD res = WaitForSingleObject(reinterpret_cast<HANDLE>(eventHandle), timeoutMs);
    if (res == WAIT_OBJECT_0) return kSuccess;
    if (res == WAIT_TIMEOUT) return kTimeout;
    return kInvalidArg;
#else
    auto* ev = static_cast<PosixEvent*>(eventHandle);
    std::unique_lock<std::mutex> lk(ev->mu);
    if (timeoutMs == 0xFFFFFFFFu) {
        ev->cv.wait(lk, [ev] { return ev->signaled; });
        if (!ev->manual_reset) ev->signaled = false;
        return kSuccess;
    }
    bool ok = ev->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [ev] { return ev->signaled; });
    if (ok) {
        if (!ev->manual_reset) ev->signaled = false;
        return kSuccess;
    }
    return kTimeout;
#endif
}

int32_t SCARLETTX_KERNELX_CALL KernelXCloseSynchronizationEvent(KERNELX_EVENT_HANDLE eventHandle) {
    if (!eventHandle) return kInvalidArg;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(eventHandle));
#else
    delete static_cast<PosixEvent*>(eventHandle);
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXQueryPerformanceCounter(uint64_t* outCounter) {
    if (!outCounter) return kInvalidArg;
#if defined(_WIN32)
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    *outCounter = static_cast<uint64_t>(li.QuadPart);
#else
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    *outCounter = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXQueryPerformanceFrequency(uint64_t* outFrequency) {
    if (!outFrequency) return kInvalidArg;
#if defined(_WIN32)
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    *outFrequency = static_cast<uint64_t>(li.QuadPart);
#else
    *outFrequency = 1000000000ull; // nanoseconds
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXGetSystemInfo(KERNELX_SYSTEM_INFO* outInfo) {
    if (!outInfo) return kInvalidArg;
    outInfo->consoleType = 1; // Xbox Series X (Anaconda)
    outInfo->computeUnitCount = 52; // 52 RDNA2 CUs active @ 1.825 GHz (12.15 TFLOPS)
    outInfo->totalPhysicalMemory = 16ull * 1024ull * 1024ull * 1024ull; // 16 GB GDDR6
    outInfo->memoryBandwidthBytesPerSec = 560ull * 1024ull * 1024ull * 1024ull; // 560 GB/s peak
    outInfo->cpuCoreCount = 8;
    outInfo->cpuClockMhz = 3800; // 3.8 GHz Zen 2 (3.66 GHz SMT)
    return kSuccess;
}

}  // extern "C"
