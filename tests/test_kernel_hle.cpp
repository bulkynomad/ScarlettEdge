#include "sx_check.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/kernel/xruntime.h"
#include "scarlettx/loader/package_vfs.h"

int main() {
    // 1. XGameRuntimeHle singleton lifecycle & device profiles
    auto& rt = scarlettx::XGameRuntimeHle::Instance();
    rt.Shutdown();
    SX_REQUIRE(!rt.IsInitialized());

    scarlettx::XUserProfile user{};
    SX_REQUIRE(!rt.GetDefaultUser(user));
    SX_REQUIRE(!rt.SignInDefaultUser(user).ok());

    SX_REQUIRE(rt.Initialize(scarlettx::DeviceType::XboxScarlettAnaconda).ok());
    SX_REQUIRE(rt.IsInitialized());
    SX_REQUIRE(rt.GetDeviceType() == scarlettx::DeviceType::XboxScarlettAnaconda);
    SX_REQUIRE(!rt.GetConsoleOsVersion().empty());
    SX_REQUIRE(rt.GetDefaultUser(user));
    SX_REQUIRE(user.signed_in && user.xuid != 0 && !user.gamertag.empty());

    SX_REQUIRE(rt.Initialize(scarlettx::DeviceType::XboxScarlettLockhart).ok());
    SX_REQUIRE(rt.GetDeviceType() == scarlettx::DeviceType::XboxScarlettLockhart);
    rt.Shutdown();
    SX_REQUIRE(!rt.IsInitialized());

    // 2. PackageVfs isolated temp directory mount, range read, and nested mount precedence
    sx_test::ScopedTempDir sandbox("test_kernel_hle");
    const auto& tmp_dir = sandbox.path();
    const auto data_file = tmp_dir / "asset.bin";
    {
        std::ofstream ofs(data_file, std::ios::binary);
        ofs << "SCARLETTX_DIRECTSTORAGE_PAYLOAD";
    }

    scarlettx::PackageVfs vfs;
    SX_REQUIRE(vfs.MountDirectory("G:", tmp_dir.string()).ok());
    SX_REQUIRE(vfs.Exists("G:\\asset.bin"));
    SX_REQUIRE(!vfs.Exists("G:\\..\\asset.bin"));

    uint64_t fsize = 0;
    SX_REQUIRE(vfs.GetFileSize("G:\\asset.bin", fsize) && fsize == 31);

    std::vector<uint8_t> dst;
    SX_REQUIRE(vfs.ReadFileRange("G:\\asset.bin", 10, 13, dst).ok());
    SX_REQUIRE(dst.size() == 13);
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
