#define SCARLETTX_BUILDING_KERNELX 1
#include "scarlettx/kernel/kernelx_abi.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
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

bool IsLockhartMode() {
    const char* env = std::getenv("SCARLETTX_LOCKHART");
    return env && env[0] != '\0' && env[0] != '0';
}

struct MemBlock {
    void* va = nullptr;
    uint64_t gpu_va = 0;
    uint64_t size = 0;
    uint32_t page_size = 0;
    uint32_t flags = 0;
    bool is_external = false;
#if defined(_WIN32)
    HANDLE section = nullptr;
    bool mapped_view = false;
#endif
};

#if defined(_WIN32)
struct AliasBlock {
    void* va = nullptr;
    HANDLE section = nullptr;
    uintptr_t parent_va = 0;
    uint64_t offset = 0;
    uint64_t gpu_va = 0;
    uint64_t size = 0;
};

struct Win32AddressRequirements {
    PVOID LowestStartingAddress;
    PVOID HighestEndingAddress;
    SIZE_T Alignment;
};

struct alignas(8) Win32MemExtendedParam {
    ULONG64 Type;  // MemExtendedParameterAddressRequirements = 1
    PVOID Pointer;
};

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn =
    PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);

struct Win32MemApi {
    VirtualAlloc2Fn virtual_alloc2 = nullptr;
    MapViewOfFile3Fn map_view3 = nullptr;
    Win32MemApi() {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) kb = LoadLibraryW(L"kernelbase.dll");
        if (kb) {
            virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(
                reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2")));
            map_view3 = reinterpret_cast<MapViewOfFile3Fn>(
                reinterpret_cast<void*>(GetProcAddress(kb, "MapViewOfFile3")));
        }
    }
};

const Win32MemApi& MemApi() {
    static Win32MemApi api;
    return api;
}

void* MapSectionWithPlaceholder(HANDLE section, uint64_t offset, uint64_t size, uint32_t alignment = 0) {
    const auto& api = MemApi();
    if (api.virtual_alloc2 && api.map_view3) {
        constexpr ULONG kMemReservePlaceholder = 0x00040000u;
        constexpr ULONG kMemReplacePlaceholder = 0x00004000u;
        Win32AddressRequirements req{nullptr, nullptr, static_cast<SIZE_T>(alignment)};
        Win32MemExtendedParam ext{1ull, &req};
        void* ext_ptr = (alignment > 0x10000u) ? static_cast<void*>(&ext) : nullptr;
        ULONG ext_cnt = (alignment > 0x10000u) ? 1u : 0u;

        void* placeholder = api.virtual_alloc2(
            GetCurrentProcess(), nullptr, static_cast<SIZE_T>(size),
            MEM_RESERVE | kMemReservePlaceholder, PAGE_NOACCESS, ext_ptr, ext_cnt);
        if (placeholder) {
            void* mapped = api.map_view3(
                section, GetCurrentProcess(), placeholder, offset, static_cast<SIZE_T>(size),
                kMemReplacePlaceholder, PAGE_READWRITE, nullptr, 0);
            if (mapped) {
                return mapped;
            }
            VirtualFree(placeholder, 0, MEM_RELEASE);
        }
    }
    return MapViewOfFile(
        section, FILE_MAP_ALL_ACCESS,
        static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset & 0xFFFFFFFFu),
        static_cast<SIZE_T>(size));
}
#endif

struct KernelXState {
    std::mutex mu;
    uint64_t next_gpu_va = 0x0000010000000000ull;
    uint64_t total_allocated = 0;
    uint64_t peak_allocated = 0;
    std::unordered_map<uintptr_t, MemBlock> by_va;
    std::unordered_map<uint64_t, uintptr_t> by_gpu_va;
#if defined(_WIN32)
    std::unordered_map<uintptr_t, AliasBlock> aliases;
#endif
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
    *outVirtualAddress = nullptr;
    *outGpuAddress = 0;

    const uint64_t align = (pageSize == KERNELX_PAGE_2MB) ? 0x200000ull :
                           (pageSize == KERNELX_PAGE_64KB) ? 0x10000ull : 0x1000ull;
    if (size > std::numeric_limits<uint64_t>::max() - (align - 1ull)) {
        return kInvalidArg;
    }
    const uint64_t aligned_size = (size + (align - 1ull)) & ~(align - 1ull);
    if (aligned_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return kOutOfMemory;
    }

    void* mem = nullptr;
#if defined(_WIN32)
    HANDLE section = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(aligned_size >> 32),
        static_cast<DWORD>(aligned_size & 0xFFFFFFFFu), nullptr);
    bool mapped_view = false;
    if (section) {
        mem = MapSectionWithPlaceholder(section, 0, aligned_size, static_cast<uint32_t>(align));
        if (mem) {
            mapped_view = true;
        } else {
            CloseHandle(section);
            section = nullptr;
        }
    }
    if (!mem) {
        mem = VirtualAlloc(nullptr, static_cast<SIZE_T>(aligned_size),
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!mem) return kOutOfMemory;
    // Fresh CreateFileMappingW / VirtualAlloc pages are already kernel-zeroed on demand.
#else
    if (posix_memalign(&mem, static_cast<size_t>(align), static_cast<size_t>(aligned_size)) != 0) {
        mem = nullptr;
    }
    if (!mem) return kOutOfMemory;
    std::memset(mem, 0, static_cast<size_t>(aligned_size));
#endif

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t gpu_addr = (State().next_gpu_va + (align - 1ull)) & ~(align - 1ull);
    State().next_gpu_va = gpu_addr + aligned_size;
    State().total_allocated += aligned_size;
    if (State().total_allocated > State().peak_allocated) {
        State().peak_allocated = State().total_allocated;
    }

    MemBlock b{};
    b.va = mem;
    b.gpu_va = gpu_addr;
    b.size = aligned_size;
    b.page_size = static_cast<uint32_t>(align);
    b.flags = flags;
    b.is_external = false;
#if defined(_WIN32)
    b.section = section;
    b.mapped_view = mapped_view;
#endif
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
    if (it == State().by_va.end() || it->second.is_external) return kInvalidArg;

    State().total_allocated = State().total_allocated >= it->second.size ?
                              State().total_allocated - it->second.size : 0;
    State().by_gpu_va.erase(it->second.gpu_va);
#if defined(_WIN32)
    if (it->second.mapped_view) {
        UnmapViewOfFile(virtualAddress);
        if (it->second.section) {
            CloseHandle(it->second.section);
        }
    } else {
        VirtualFree(virtualAddress, 0, MEM_RELEASE);
    }
#else
    std::free(virtualAddress);
#endif
    State().by_va.erase(it);
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXRegisterExternalAllocation(
    void* virtualAddress, uint64_t size, uint64_t* outGpuAddress) {
    if (!virtualAddress || size == 0) return kInvalidArg;
    const uintptr_t va_key = reinterpret_cast<uintptr_t>(virtualAddress);
    if (size > std::numeric_limits<uintptr_t>::max() - va_key) {
        return kInvalidArg;
    }

    constexpr uint64_t kGpuAlign = 0x10000ull;
    if (size > std::numeric_limits<uint64_t>::max() - (2ull * kGpuAlign)) {
        return kInvalidArg;
    }

    std::lock_guard<std::mutex> lk(State().mu);
    auto existing = State().by_va.find(va_key);
    if (existing != State().by_va.end()) {
        if (existing->second.is_external && size > existing->second.size) {
            const uint64_t delta = size - existing->second.size;
            existing->second.size = size;
            State().total_allocated += delta;
            if (State().total_allocated > State().peak_allocated) {
                State().peak_allocated = State().total_allocated;
            }
        }
        if (outGpuAddress) {
            *outGpuAddress = existing->second.gpu_va;
        }
        return kSuccess;
    }

    const uint64_t aligned_base = (State().next_gpu_va + (kGpuAlign - 1ull)) & ~(kGpuAlign - 1ull);
    const uint64_t low_offset = static_cast<uint64_t>(va_key) & (kGpuAlign - 1ull);
    const uint64_t gpu_addr = aligned_base + low_offset;
    const uint64_t span = ((low_offset + size) + (kGpuAlign - 1ull)) & ~(kGpuAlign - 1ull);
    State().next_gpu_va = aligned_base + span;

    State().total_allocated += size;
    if (State().total_allocated > State().peak_allocated) {
        State().peak_allocated = State().total_allocated;
    }

    MemBlock b{};
    b.va = virtualAddress;
    b.gpu_va = gpu_addr;
    b.size = size;
    b.page_size = static_cast<uint32_t>(kGpuAlign);
    b.flags = KERNELX_MEM_GPU_VISIBLE;
    b.is_external = true;
    State().by_va[va_key] = b;
    State().by_gpu_va[gpu_addr] = va_key;

    if (outGpuAddress) {
        *outGpuAddress = gpu_addr;
    }
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXUnregisterExternalAllocation(void* virtualAddress) {
    if (!virtualAddress) return kSuccess;
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().by_va.find(reinterpret_cast<uintptr_t>(virtualAddress));
    if (it == State().by_va.end() || !it->second.is_external) {
        return kInvalidArg;
    }
    State().total_allocated = State().total_allocated >= it->second.size ?
                              State().total_allocated - it->second.size : 0;
    State().by_gpu_va.erase(it->second.gpu_va);
    State().by_va.erase(it);
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXMapVirtualToGpu(const void* virtualAddress, uint64_t* outGpuAddress) {
    if (!virtualAddress || !outGpuAddress) return kInvalidArg;
    const uintptr_t va = reinterpret_cast<uintptr_t>(virtualAddress);
    std::lock_guard<std::mutex> lk(State().mu);
    for (const auto& kv : State().by_va) {
        const uintptr_t base = kv.first;
        if (va >= base && va - base < kv.second.size) {
            *outGpuAddress = kv.second.gpu_va + static_cast<uint64_t>(va - base);
            return kSuccess;
        }
    }
#if defined(_WIN32)
    for (const auto& kv : State().aliases) {
        const uintptr_t base = kv.first;
        if (va >= base && va - base < kv.second.size) {
            *outGpuAddress = kv.second.gpu_va + static_cast<uint64_t>(va - base);
            return kSuccess;
        }
    }
#endif
    return kInvalidArg;
}

int32_t SCARLETTX_KERNELX_CALL KernelXMapGpuToVirtual(uint64_t gpuAddress, void** outVirtualAddress) {
    if (!outVirtualAddress) return kInvalidArg;
    std::lock_guard<std::mutex> lk(State().mu);
    for (const auto& kv : State().by_va) {
        const uint64_t base = kv.second.gpu_va;
        if (gpuAddress >= base && gpuAddress - base < kv.second.size) {
            *outVirtualAddress =
                reinterpret_cast<void*>(kv.first + static_cast<uintptr_t>(gpuAddress - base));
            return kSuccess;
        }
    }
#if defined(_WIN32)
    for (const auto& kv : State().aliases) {
        const uint64_t base = kv.second.gpu_va;
        if (gpuAddress >= base && gpuAddress - base < kv.second.size) {
            *outVirtualAddress =
                reinterpret_cast<void*>(kv.first + static_cast<uintptr_t>(gpuAddress - base));
            return kSuccess;
        }
    }
#endif
    return kInvalidArg;
}

int32_t SCARLETTX_KERNELX_CALL KernelXCreateMemoryAlias(
    void* existingVirtualAddress, uint64_t offset, uint64_t size, void** outAliasVirtualAddress) {
    if (!existingVirtualAddress || !outAliasVirtualAddress || size == 0) return kInvalidArg;
    *outAliasVirtualAddress = nullptr;
#if defined(_WIN32)
    HANDLE dup_section = nullptr;
    uint64_t alias_gpu_va = 0;
    uint32_t page_align = 0;
    const uintptr_t parent_va = reinterpret_cast<uintptr_t>(existingVirtualAddress);
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().by_va.find(parent_va);
        if (it == State().by_va.end() || !it->second.section) return kInvalidArg;
        if (offset > it->second.size || size > it->second.size - offset) return kInvalidArg;
        alias_gpu_va = it->second.gpu_va + offset;
        page_align = it->second.page_size;
        if (!DuplicateHandle(GetCurrentProcess(), it->second.section, GetCurrentProcess(),
                             &dup_section, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return kOutOfMemory;
        }
    }
    void* alias_va = MapSectionWithPlaceholder(dup_section, offset, size, page_align);
    if (!alias_va) {
        CloseHandle(dup_section);
        return kOutOfMemory;
    }
    {
        std::lock_guard<std::mutex> lk(State().mu);
        State().aliases[reinterpret_cast<uintptr_t>(alias_va)] =
            AliasBlock{alias_va, dup_section, parent_va, offset, alias_gpu_va, size};
    }
    *outAliasVirtualAddress = alias_va;
    return kSuccess;
#else
    (void)offset;
    return kInvalidArg;
#endif
}

int32_t SCARLETTX_KERNELX_CALL KernelXFreeMemoryAlias(void* aliasVirtualAddress) {
    if (!aliasVirtualAddress) return kSuccess;
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().aliases.find(reinterpret_cast<uintptr_t>(aliasVirtualAddress));
    if (it == State().aliases.end()) return kInvalidArg;
    UnmapViewOfFile(aliasVirtualAddress);
    if (it->second.section) CloseHandle(it->second.section);
    State().aliases.erase(it);
    return kSuccess;
#else
    return kInvalidArg;
#endif
}

int32_t SCARLETTX_KERNELX_CALL KernelXProtectMemory(void* virtualAddress, uint64_t size, uint32_t protectFlags) {
    if (!virtualAddress || size == 0) return kInvalidArg;
#if defined(_WIN32)
    DWORD old_p = 0;
    DWORD new_p = (protectFlags & KERNELX_MEM_READ_ONLY) ? PAGE_READONLY : PAGE_READWRITE;
    if (!VirtualProtect(virtualAddress, static_cast<SIZE_T>(size), new_p, &old_p)) return kInvalidArg;
#else
    (void)protectFlags;
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXQueryMemoryStatistics(KERNELX_MEMORY_STATS* outStats) {
    if (!outStats) return kInvalidArg;
    const uint64_t total_bytes =
        (IsLockhartMode() ? 10ull : 16ull) * 1024ull * 1024ull * 1024ull;
    std::lock_guard<std::mutex> lk(State().mu);
    outStats->totalUnifiedBytes = total_bytes;
    outStats->allocatedBytes = State().total_allocated;
    outStats->freeBytes =
        (total_bytes > State().total_allocated) ? (total_bytes - State().total_allocated) : 0;
    outStats->peakAllocatedBytes = State().peak_allocated;
    outStats->activeAllocations = static_cast<uint32_t>(State().by_va.size());
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXCreateSynchronizationEvent(
    int32_t manualReset, int32_t initialState, KERNELX_EVENT_HANDLE* outEvent) {
    if (!outEvent) return kInvalidArg;
    *outEvent = nullptr;
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
    if (eventHandle == INVALID_HANDLE_VALUE || !SetEvent(reinterpret_cast<HANDLE>(eventHandle))) {
        return kInvalidArg;
    }
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
    if (eventHandle == INVALID_HANDLE_VALUE || !ResetEvent(reinterpret_cast<HANDLE>(eventHandle))) {
        return kInvalidArg;
    }
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
    if (eventHandle == INVALID_HANDLE_VALUE) return kInvalidArg;
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
    if (eventHandle == INVALID_HANDLE_VALUE || !CloseHandle(reinterpret_cast<HANDLE>(eventHandle))) {
        return kInvalidArg;
    }
#else
    delete static_cast<PosixEvent*>(eventHandle);
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXQueryPerformanceCounter(uint64_t* outCounter) {
    if (!outCounter) return kInvalidArg;
#if defined(_WIN32)
    LARGE_INTEGER li{};
    if (!QueryPerformanceCounter(&li)) return kInvalidArg;
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
    LARGE_INTEGER li{};
    if (!QueryPerformanceFrequency(&li)) return kInvalidArg;
    *outFrequency = static_cast<uint64_t>(li.QuadPart);
#else
    *outFrequency = 1000000000ull;  // nanoseconds
#endif
    return kSuccess;
}

int32_t SCARLETTX_KERNELX_CALL KernelXGetSystemInfo(KERNELX_SYSTEM_INFO* outInfo) {
    if (!outInfo) return kInvalidArg;
    if (IsLockhartMode()) {
        outInfo->consoleType = 2;  // Xbox Series S (Lockhart)
        outInfo->computeUnitCount = 20;  // 20 RDNA2 CUs @ 1.565 GHz
        outInfo->totalPhysicalMemory = 10ull * 1024ull * 1024ull * 1024ull;  // 10 GB GDDR6
        outInfo->memoryBandwidthBytesPerSec = 224ull * 1024ull * 1024ull * 1024ull;  // 224 GB/s
        outInfo->cpuCoreCount = 8;
        outInfo->cpuClockMhz = 3600;  // 3.6 GHz Zen 2 (3.4 GHz SMT)
    } else {
        outInfo->consoleType = 1;  // Xbox Series X (Anaconda)
        outInfo->computeUnitCount = 52;  // 52 RDNA2 CUs active @ 1.825 GHz (12.15 TFLOPS)
        outInfo->totalPhysicalMemory = 16ull * 1024ull * 1024ull * 1024ull;  // 16 GB GDDR6
        outInfo->memoryBandwidthBytesPerSec = 560ull * 1024ull * 1024ull * 1024ull;  // 560 GB/s
        outInfo->cpuCoreCount = 8;
        outInfo->cpuClockMhz = 3800;  // 3.8 GHz Zen 2 (3.66 GHz SMT)
    }
    return kSuccess;
}

}  // extern "C"
