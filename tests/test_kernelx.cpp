#include "scarlettx/kernel/kernelx_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <atomic>
#include <thread>
#include <atomic>
#include <thread>

int main() {
    std::cout << "[test_kernelx] Testing KernelX unified memory & Game OS services...\n";

    // 1. Unified memory allocation (64KB and 2MB pages)
    void* va1 = nullptr;
    uint64_t gpu_va1 = 0;
    SX_CHECK(KernelXAllocateMemory(65536, KERNELX_PAGE_64KB, KERNELX_MEM_GPU_VISIBLE, &va1, &gpu_va1) == 0);
    SX_CHECK(va1 != nullptr);
    SX_CHECK(gpu_va1 != 0);

    // Write test data
    std::memset(va1, 0xAB, 65536);
    SX_CHECK(static_cast<uint8_t*>(va1)[0] == 0xAB);

    // 1b. Hardware physical-virtual page aliasing via VirtualAlloc2 + MapViewOfFile3
    void* alias_va = nullptr;
    SX_CHECK(KernelXCreateMemoryAlias(va1, 0, 65536, &alias_va) == 0);
    SX_CHECK(alias_va != nullptr);
    SX_CHECK(alias_va != va1);
    // Data written to va1 is instantaneously visible in alias_va (same physical pages)
    SX_CHECK(static_cast<uint8_t*>(alias_va)[0] == 0xAB);
    SX_CHECK(static_cast<uint8_t*>(alias_va)[65535] == 0xAB);
    // Writing through alias_va is instantaneously visible in va1
    static_cast<uint8_t*>(alias_va)[1234] = 0x5E;
    SX_CHECK(static_cast<uint8_t*>(va1)[1234] == 0x5E);
    uint64_t alias_gpu = 0;
    SX_CHECK(KernelXMapVirtualToGpu(alias_va, &alias_gpu) == 0);
    SX_CHECK(alias_gpu == gpu_va1);
    SX_CHECK(KernelXFreeMemoryAlias(alias_va) == 0);

    // 1c. Verify guaranteed 2 MB virtual address alignment on KERNELX_PAGE_2MB
    void* va_2mb = nullptr;
    uint64_t gpu_2mb = 0;
    SX_CHECK(KernelXAllocateMemory(0x200000, KERNELX_PAGE_2MB, KERNELX_MEM_GPU_VISIBLE, &va_2mb, &gpu_2mb) == 0);
    SX_CHECK(va_2mb != nullptr);
    SX_CHECK((reinterpret_cast<uintptr_t>(va_2mb) & (0x200000ull - 1ull)) == 0);
    SX_CHECK(KernelXFreeMemory(va_2mb) == 0);

    // 2. Dual VA <-> GPU VA mapping lookup
    uint64_t mapped_gpu = 0;
    SX_CHECK(KernelXMapVirtualToGpu(va1, &mapped_gpu) == 0);
    SX_CHECK(mapped_gpu == gpu_va1);

    void* mapped_va = nullptr;
    SX_CHECK(KernelXMapGpuToVirtual(gpu_va1, &mapped_va) == 0);
    SX_CHECK(mapped_va == va1);

    // 3. Memory statistics
    KERNELX_MEMORY_STATS stats{};
    SX_CHECK(KernelXQueryMemoryStatistics(&stats) == 0);
    SX_CHECK(stats.allocatedBytes >= 65536);
    SX_CHECK(stats.activeAllocations >= 1);

    // 4. Synchronization event (manual reset)
    KERNELX_EVENT_HANDLE ev = nullptr;
    SX_CHECK(KernelXCreateSynchronizationEvent(1, 0, &ev) == 0);
    SX_CHECK(ev != nullptr);

    // Initial wait with 10ms timeout should time out
    SX_CHECK(KernelXWaitForSynchronizationEvent(ev, 10) != 0);

    // Signal event
    SX_CHECK(KernelXSetSynchronizationEvent(ev) == 0);
    SX_CHECK(KernelXWaitForSynchronizationEvent(ev, 100) == 0);

    // Reset event
    SX_CHECK(KernelXResetSynchronizationEvent(ev) == 0);
    SX_CHECK(KernelXWaitForSynchronizationEvent(ev, 10) != 0);
    SX_CHECK(KernelXCloseSynchronizationEvent(ev) == 0);

    // 5. High-resolution timing
    uint64_t counter = 0;
    uint64_t freq = 0;
    SX_CHECK(KernelXQueryPerformanceCounter(&counter) == 0);
    SX_CHECK(KernelXQueryPerformanceFrequency(&freq) == 0);
    SX_CHECK(counter > 0);
    SX_CHECK(freq > 0);

    // 6. System info
    KERNELX_SYSTEM_INFO sys_info{};
    SX_CHECK(KernelXGetSystemInfo(&sys_info) == 0);
    SX_CHECK(sys_info.consoleType == 1);
    SX_CHECK(sys_info.computeUnitCount == 52);
    SX_CHECK(sys_info.cpuCoreCount == 8);

    // 6b. Retail GameOS TitleMemoryStatus / JobTitleMemoryStatus & WaitOnAddress
    SX_CHECK(TitleMemoryStatus(nullptr) == 0);
    XBOX_TITLE_MEMORY_STATUS title_mem{};
    title_mem.dwLength = sizeof(XBOX_TITLE_MEMORY_STATUS);
    SX_CHECK(TitleMemoryStatus(&title_mem) == 1);
    SX_CHECK(title_mem.ullTotalMem == 14495514624ull);
    SX_CHECK(title_mem.ullTitleUsed >= 65536ull);
    SX_CHECK(title_mem.ullTitleAvail == title_mem.ullTotalMem - title_mem.ullTitleUsed);

    XBOX_TITLE_MEMORY_STATUS job_mem{};
    SX_CHECK(JobTitleMemoryStatus(&job_mem) == 1);
    SX_CHECK(job_mem.ullTotalMem == title_mem.ullTotalMem);

    alignas(4) std::atomic<uint32_t> futex_word{0u};
    std::thread waker([&futex_word]() {
        futex_word.store(0x42u, std::memory_order_release);
        KernelXWakeByAddressSingle(&futex_word);
    });
    while (futex_word.load(std::memory_order_acquire) == 0u) {
        uint32_t expected_zero = 0u;
        (void)KernelXWaitOnAddress(&futex_word, &expected_zero, sizeof(uint32_t), 2000u);
    }
    waker.join();
    SX_CHECK(futex_word.load(std::memory_order_relaxed) == 0x42u);

    // 7. Cleanup
    SX_CHECK(KernelXFreeMemory(va1) == 0);

    std::cout << "[test_kernelx] All 19 KernelX & GameOS APIs verified successfully!\n";
    return 0;
}
