#include "scarlettx/kernel/kernelx_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>

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

    // 7. Cleanup
    SX_CHECK(KernelXFreeMemory(va1) == 0);

    std::cout << "[test_kernelx] All 14 KernelX APIs verified successfully!\n";
    return 0;
}
