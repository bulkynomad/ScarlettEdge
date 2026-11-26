#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_KERNELX)
#    define SCARLETTX_KERNELX_API __declspec(dllexport)
#  else
#    define SCARLETTX_KERNELX_API __declspec(dllimport)
#  endif
#  define SCARLETTX_KERNELX_CALL __stdcall
#else
#  define SCARLETTX_KERNELX_API __attribute__((visibility("default")))
#  define SCARLETTX_KERNELX_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KERNELX_PAGE_SIZE {
    KERNELX_PAGE_4KB = 0x1000,
    KERNELX_PAGE_64KB = 0x10000,
    KERNELX_PAGE_2MB = 0x200000,
} KERNELX_PAGE_SIZE;

typedef enum KERNELX_MEM_FLAGS {
    KERNELX_MEM_NONE = 0,
    KERNELX_MEM_GPU_VISIBLE = 1 << 0,
    KERNELX_MEM_WRITE_COMBINED = 1 << 1,
    KERNELX_MEM_READ_ONLY = 1 << 2,
} KERNELX_MEM_FLAGS;

typedef struct KERNELX_MEMORY_STATS {
    uint64_t totalUnifiedBytes;
    uint64_t allocatedBytes;
    uint64_t freeBytes;
    uint64_t peakAllocatedBytes;
    uint32_t activeAllocations;
} KERNELX_MEMORY_STATS;

typedef struct KERNELX_SYSTEM_INFO {
    uint32_t consoleType;          // 1 = Anaconda (Series X), 2 = Lockhart (Series S)
    uint32_t computeUnitCount;      // 52 for Anaconda, 20 for Lockhart
    uint64_t totalPhysicalMemory;   // 16 GB for Anaconda, 10 GB for Lockhart
    uint64_t memoryBandwidthBytesPerSec; // 560 GB/s for Anaconda
    uint32_t cpuCoreCount;         // 8 cores, 16 threads
    uint32_t cpuClockMhz;          // 3800 MHz
} KERNELX_SYSTEM_INFO;

typedef void* KERNELX_EVENT_HANDLE;

// Unified CPU/GPU physical & virtual memory management
SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXAllocateMemory(
    uint64_t size, uint32_t pageSize, uint32_t flags, void** outVirtualAddress, uint64_t* outGpuAddress);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXFreeMemory(void* virtualAddress);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXMapVirtualToGpu(
    const void* virtualAddress, uint64_t* outGpuAddress);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXMapGpuToVirtual(
    uint64_t gpuAddress, void** outVirtualAddress);

// Creates a hardware-aliased virtual address view backed by the same physical
// pages as an existing KernelX allocation (via VirtualAlloc2 + MapViewOfFile3).
SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXCreateMemoryAlias(
    void* existingVirtualAddress, uint64_t offset, uint64_t size, void** outAliasVirtualAddress);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXFreeMemoryAlias(
    void* aliasVirtualAddress);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXProtectMemory(
    void* virtualAddress, uint64_t size, uint32_t protectFlags);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXQueryMemoryStatistics(
    KERNELX_MEMORY_STATS* outStats);

// Synchronization primitives
SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXCreateSynchronizationEvent(
    int32_t manualReset, int32_t initialState, KERNELX_EVENT_HANDLE* outEvent);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXSetSynchronizationEvent(
    KERNELX_EVENT_HANDLE eventHandle);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXResetSynchronizationEvent(
    KERNELX_EVENT_HANDLE eventHandle);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXWaitForSynchronizationEvent(
    KERNELX_EVENT_HANDLE eventHandle, uint32_t timeoutMs);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXCloseSynchronizationEvent(
    KERNELX_EVENT_HANDLE eventHandle);

// High-resolution timing & platform info
SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXQueryPerformanceCounter(
    uint64_t* outCounter);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXQueryPerformanceFrequency(
    uint64_t* outFrequency);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXGetSystemInfo(
    KERNELX_SYSTEM_INFO* outInfo);

#ifdef __cplusplus
}
#endif
