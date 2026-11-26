#include "sx_check.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "scarlettx/kernel/xmemory.h"
#include "scarlettx/kernel/xruntime.h"
#include "scarlettx/kernel/xstorage.h"
#include "scarlettx/loader/package_vfs.h"

int main() {
    // 1. Unified Memory Manager (64KB & 2MB pages)
    scarlettx::UnifiedMemoryManager mem;
    scarlettx::UnifiedMemoryBlock b1{};
    auto st = mem.Allocate(10000, scarlettx::MemoryPageSize::Page64KB, true, true, b1);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(b1.size == 65536);
    SX_REQUIRE((b1.gpu_va % 65536) == 0);
    SX_REQUIRE(b1.host_ptr != nullptr);
    std::memset(b1.host_ptr, 0xAB, 256);

    scarlettx::UnifiedMemoryBlock found{};
    SX_REQUIRE(mem.LookupByGpuVa(b1.gpu_va + 128, found));
    SX_REQUIRE(found.gpu_va == b1.gpu_va);
    SX_REQUIRE(mem.ActiveAllocationCount() == 1);

    st = mem.Free(b1.gpu_va);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(mem.ActiveAllocationCount() == 0);
    SX_REQUIRE(mem.TotalAllocatedBytes() == 0);

    // 2. XGameRuntime + XTaskQueue
    auto& rt = scarlettx::XGameRuntimeHle::Instance();
    SX_REQUIRE(rt.Initialize(scarlettx::DeviceType::XboxScarlettAnaconda).ok());
    SX_REQUIRE(rt.GetDeviceType() == scarlettx::DeviceType::XboxScarlettAnaconda);

    scarlettx::XUserProfile user;
    SX_REQUIRE(rt.GetDefaultUser(user));
    SX_REQUIRE(user.signed_in && user.xuid != 0);

    const auto qh = rt.CreateTaskQueue();
    auto* q = rt.GetTaskQueue(qh);
    SX_REQUIRE(q != nullptr);
    int counter = 0;
    q->SubmitWork([&]() { counter += 10; });
    q->SubmitCompletion([&]() { counter += 5; });
    SX_REQUIRE(q->DispatchWork() == 1);
    SX_REQUIRE(q->DispatchCompletion() == 1);
    SX_REQUIRE(counter == 15);
    rt.CloseTaskQueue(qh);
    rt.Shutdown();

    // 3. PackageVfs + XStorageQueueHle
    const auto tmp_dir = std::filesystem::path(SCARLETTX_BINARY_DIR) / "tmp_vfs_test";
    std::filesystem::create_directories(tmp_dir);
    const auto data_file = tmp_dir / "asset.bin";
    {
        std::ofstream ofs(data_file, std::ios::binary);
        ofs << "SCARLETTX_DIRECTSTORAGE_PAYLOAD";
    }

    scarlettx::PackageVfs vfs;
    SX_REQUIRE(vfs.MountDirectory("G:", tmp_dir.string()).ok());
    SX_REQUIRE(vfs.Exists("G:\\asset.bin"));

    scarlettx::XStorageQueueHle storage(vfs);
    std::vector<uint8_t> dst(13, 0);
    storage.EnqueueRead({"G:\\asset.bin", 10, 13, dst.data()});
    auto results = storage.SubmitAndWait();
    SX_REQUIRE(results.size() == 1);
    SX_REQUIRE(results[0].status.ok());
    SX_REQUIRE(results[0].bytes_read == 13);
    SX_REQUIRE(std::string(dst.begin(), dst.end()) == "DIRECTSTORAGE");

    // A nested mount shadows the same path on the drive mount in either registration order.
    const auto parent_packs = tmp_dir / "packs";
    const auto nested_packs = tmp_dir / "override";
    std::filesystem::create_directories(parent_packs);
    std::filesystem::create_directories(nested_packs);
    std::ofstream(parent_packs / "asset.bin", std::ios::binary) << "parent";
    std::ofstream(nested_packs / "asset.bin", std::ios::binary) << "nested";

    for (bool nested_first : {false, true}) {
        scarlettx::PackageVfs overlapping;
        if (nested_first) {
            SX_REQUIRE(overlapping.MountDirectory("G:\\packs", nested_packs.string()).ok());
        }
        SX_REQUIRE(overlapping.MountDirectory("G:", tmp_dir.string()).ok());
        if (!nested_first) {
            SX_REQUIRE(overlapping.MountDirectory("G:\\packs", nested_packs.string()).ok());
        }
        std::vector<uint8_t> bytes;
        SX_REQUIRE(overlapping.ReadFileBytes("g:/packs/asset.bin", bytes).ok());
        SX_REQUIRE(std::string(bytes.begin(), bytes.end()) == "nested");
        SX_REQUIRE(overlapping.ReadFileBytes("G:/asset.bin", bytes).ok());
        SX_REQUIRE(std::string(bytes.begin(), bytes.end()) == "SCARLETTX_DIRECTSTORAGE_PAYLOAD");
    }

    std::cout << "[test_kernel_hle] PASSED\n";
    return 0;
}
