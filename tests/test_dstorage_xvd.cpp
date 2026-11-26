#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/xvd_container.h"

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_dstorage_xvd] FAILED: " #cond                   \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define SX_CHECK_EQ(a, b)                                                       \
    do {                                                                        \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b)) {                                                      \
            std::cerr << "[test_dstorage_xvd] FAILED: " #a " == " #b            \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ \
                      << "\n";                                                  \
            return 1;                                                           \
        }                                                                       \
    } while (0)

namespace {

void OnSignal(uint64_t value, void* ctx) {
    auto* flag = static_cast<std::atomic<uint64_t>*>(ctx);
    flag->store(value, std::memory_order_release);
}

struct SignalGate {
    uint64_t queue;
    uint64_t status_arr;
    std::promise<void> entered;
    std::shared_future<void> released;
    int32_t reentrant_result = 0;
};

void HoldSignal(uint64_t, void* ctx) {
    auto& gate = *static_cast<SignalGate*>(ctx);
    gate.entered.set_value();
    gate.released.wait();
    gate.reentrant_result = DStorageEnqueueStatus(gate.queue, gate.status_arr, 5);
}

int TestQueueSubmission(uint64_t factory) {
    DSTORAGE_CONFIGURATION cfg{};
    cfg.NumSubmitThreads = 4;
    SX_CHECK_EQ(DStorageSetConfiguration(&cfg), 0);
    DSTORAGE_CONFIGURATION got_cfg{};
    SX_CHECK_EQ(DStorageGetConfiguration(&got_cfg), 0);
    SX_CHECK_EQ(got_cfg.NumSubmitThreads, 4u);
    using namespace std::chrono_literals;

    DSTORAGE_QUEUE_DESC desc{};
    desc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    desc.Capacity = 4;
    desc.Priority = DSTORAGE_PRIORITY_REALTIME;
    uint64_t queue = 0;
    SX_CHECK_EQ(DStorageCreateQueue(factory, &desc, &queue), 0);
    uint64_t status_arr = 0;
    SX_CHECK_EQ(DStorageCreateStatusArray(factory, 6, "QueueSubmission", &status_arr), 0);

    const std::array<uint8_t, 3> source{7, 11, 23};
    std::array<uint8_t, 3> destination{};
    DSTORAGE_REQUEST read{};
    read.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    read.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    read.SourceSize = 1;
    read.DestinationSize = 1;
    const auto enqueue_read = [&](size_t i) {
        read.SourceMemory = &source[i];
        read.DestinationMemory = &destination[i];
        return DStorageEnqueueRequest(queue, &read);
    };

    SX_CHECK_EQ(enqueue_read(0), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);
    SX_CHECK_EQ(destination[0], 0);
    SX_CHECK_EQ(DStorageEnqueueStatus(queue, status_arr, 0), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);
    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 0), 1u);
    SX_CHECK_EQ(DStorageGetStatusHResult(status_arr, 0), 0);
    SX_CHECK_EQ(destination[0], source[0]);

    uint64_t fence = 0;
    SX_CHECK_EQ(enqueue_read(1), 0);
    SX_CHECK_EQ(DStorageEnqueueSignal(queue, &fence, 9, nullptr, nullptr), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);
    SX_CHECK_EQ(fence, 9u);
    SX_CHECK_EQ(destination[1], source[1]);

    SX_CHECK_EQ(DStorageEnqueueStatus(queue, status_arr, 1), 0);
    SX_CHECK_EQ(enqueue_read(2), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);
    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 1), 1u);
    SX_CHECK(destination == source);

    std::promise<void> release;
    SignalGate gate{queue, status_arr, {}, release.get_future().share()};
    auto entered = gate.entered.get_future();
    SX_CHECK_EQ(DStorageEnqueueSignal(queue, nullptr, 0, HoldSignal, &gate), 0);
    const int submit = DStorageSubmit(queue);
    if (submit != 0 || entered.wait_for(5s) != std::future_status::ready) {
        release.set_value();
        DStorageDestroyQueue(queue);
        SX_CHECK(false);
    }
    const int first = DStorageEnqueueStatus(queue, status_arr, 2);
    const int second = DStorageEnqueueStatus(queue, status_arr, 3);
    const int third = DStorageEnqueueStatus(queue, status_arr, 4);
    std::promise<void> producer_started;
    auto started = producer_started.get_future();
    auto producer = std::async(std::launch::async, [&] {
        producer_started.set_value();
        return DStorageEnqueueStatus(queue, status_arr, 5);
    });
    const bool producer_waited = started.wait_for(5s) == std::future_status::ready &&
                                 producer.wait_for(50ms) == std::future_status::timeout;
    release.set_value();
    const int producer_result = producer.get();
    const int drained = DStorageWaitForIdle(queue, 5000);
    const uint32_t last_status = DStorageIsStatusComplete(status_arr, 5);
    SX_CHECK_EQ(DStorageDestroyQueue(queue), 0);
    SX_CHECK_EQ(DStorageDestroyStatusArray(status_arr), 0);
    SX_CHECK_EQ(first, 0);
    SX_CHECK_EQ(second, 0);
    SX_CHECK_EQ(third, 0);
    SX_CHECK(producer_waited);
    SX_CHECK_EQ(gate.reentrant_result, static_cast<int32_t>(0x80004005u));
    SX_CHECK_EQ(producer_result, 0);
    SX_CHECK_EQ(drained, 0);
    SX_CHECK_EQ(last_status, 1u);
    return 0;
}

}  // namespace

int main() {
    const std::filesystem::path tmp_dir =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "test_dstorage_tmp";
    std::filesystem::create_directories(tmp_dir);

    // 1. Synthetic XVD parsing & encrypted rejection
    {
        std::vector<uint8_t> payload(2048);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFFu);
        }

        auto unenc_bytes = scarlettx::XvdContainer::BuildSyntheticXvd(payload, 1, false);
        scarlettx::XvdContainer xvd;
        SX_CHECK(xvd.OpenBytes(unenc_bytes).ok());
        SX_CHECK(xvd.IsOpen());
        SX_CHECK_EQ(xvd.Header().magic, std::string("msft-xvd"));
        SX_CHECK(!xvd.Header().is_encrypted);
        SX_CHECK_EQ(xvd.Header().user_data_length, payload.size());

        std::vector<uint8_t> slice;
        SX_CHECK(xvd.ReadUserData(128, 256, slice).ok());
        SX_CHECK_EQ(slice.size(), 256u);
        SX_CHECK(std::memcmp(slice.data(), payload.data() + 128, 256) == 0);

        // Encrypted XVD must parse header cleanly but reject user-data reads without a key
        auto enc_bytes = scarlettx::XvdContainer::BuildSyntheticXvd(payload, 1, true);
        scarlettx::XvdContainer enc_xvd;
        SX_CHECK(enc_xvd.OpenBytes(enc_bytes).ok());
        SX_CHECK(enc_xvd.Header().is_encrypted);
        std::vector<uint8_t> denied;
        SX_CHECK(!enc_xvd.ReadUserData(0, 64, denied).ok());

        // Encrypted XVD with 4 KiB sector XTS decryption across multi-sector unaligned boundaries
        std::vector<uint8_t> large_payload(10000);
        for (size_t i = 0; i < large_payload.size(); ++i) {
            large_payload[i] = static_cast<uint8_t>((i * 17 + 31) & 0xFFu);
        }
        std::array<uint8_t, 32> vol_key{};
        for (size_t i = 0; i < vol_key.size(); ++i) {
            vol_key[i] = static_cast<uint8_t>(0x30u + i);
        }
        auto xts_img = scarlettx::XvdContainer::BuildEncryptedSyntheticXvd(
            large_payload, vol_key.data(), vol_key.size(), 1);
        scarlettx::XvdContainer xts_xvd;
        SX_CHECK(xts_xvd.OpenBytes(xts_img).ok());
        SX_CHECK(xts_xvd.Header().is_encrypted);
        SX_CHECK(!xts_xvd.ReadUserData(0, 64, denied).ok());
        SX_CHECK(xts_xvd.SetVolumeKey(vol_key.data(), vol_key.size()).ok());
        SX_CHECK(xts_xvd.HasVolumeKey());

        std::vector<uint8_t> decrypted_slice;
        SX_CHECK(xts_xvd.ReadUserData(137, 5000, decrypted_slice).ok());
        SX_CHECK_EQ(decrypted_slice.size(), 5000u);
        SX_CHECK(std::memcmp(decrypted_slice.data(), large_payload.data() + 137, 5000) == 0);

        // Verify PackageVfs::MountXvd with hex volume key
        const auto enc_xvd_path = (tmp_dir / "encrypted_assets.xvd").string();
        {
            std::ofstream ofs(enc_xvd_path, std::ios::binary);
            SX_CHECK(ofs.good());
            ofs.write(reinterpret_cast<const char*>(xts_img.data()), static_cast<std::streamsize>(xts_img.size()));
        }
        std::string hex_key;
        static const char kHex[] = "0123456789abcdef";
        for (uint8_t b : vol_key) {
            hex_key.push_back(kHex[b >> 4]);
            hex_key.push_back(kHex[b & 0xF]);
        }
        scarlettx::PackageVfs enc_vfs;
        SX_CHECK(!enc_vfs.MountXvd("G:\\enc_assets", enc_xvd_path).ok());
        SX_CHECK(enc_vfs.MountXvd("G:\\enc_assets", enc_xvd_path, hex_key).ok());
        std::vector<uint8_t> vfs_plain;
        SX_CHECK(enc_vfs.ReadFileBytes("G:\\enc_assets", vfs_plain).ok());
        SX_CHECK_EQ(vfs_plain.size(), large_payload.size());
        SX_CHECK(std::memcmp(vfs_plain.data(), large_payload.data(), large_payload.size()) == 0);
    }

    // 2. DirectStorage configuration, factory, queue, status array
    DSTORAGE_CONFIGURATION cfg{};
    cfg.NumSubmitThreads = 1;
    cfg.NumBuiltInCpuDecompressionThreads = 1;
    SX_CHECK_EQ(DStorageSetConfiguration(&cfg), 0);

    uint64_t factory = 0;
    SX_CHECK_EQ(DStorageGetFactory(&factory), 0);
    SX_CHECK(factory != 0);
    SX_CHECK_EQ(TestQueueSubmission(factory), 0);

    DSTORAGE_QUEUE_DESC qdesc{};
    qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qdesc.Capacity = 64;
    qdesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qdesc.Name = "ScarlettX_TestQueue";
    uint64_t queue = 0;
    SX_CHECK_EQ(DStorageCreateQueue(factory, &qdesc, &queue), 0);

    uint64_t status_arr = 0;
    SX_CHECK_EQ(DStorageCreateStatusArray(factory, 8, "TestStatus", &status_arr), 0);
    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 0), 0u);

    // 3. DirectStorage async file read + GDeflate/LZ-RLE decompression from disk
    std::vector<uint8_t> original(4096);
    for (size_t i = 0; i < 2048; ++i) original[i] = 0x5Au;  // highly compressible run
    for (size_t i = 2048; i < 4096; ++i) original[i] = static_cast<uint8_t>(i & 0xFFu);

    std::vector<uint8_t> compressed(8192);
    uint32_t comp_size = 0;
    SX_CHECK_EQ(
        DStorageCompressBuffer(
            DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
            original.data(),
            static_cast<uint32_t>(original.size()),
            compressed.data(),
            static_cast<uint32_t>(compressed.size()),
            &comp_size),
        0);
    SX_CHECK(comp_size < original.size());

    const auto xvd_path = (tmp_dir / "assets.xvd").string();
    {
        auto xvd_img = scarlettx::XvdContainer::BuildSyntheticXvd(
            std::vector<uint8_t>(compressed.begin(), compressed.begin() + comp_size),
            1,
            false);
        std::ofstream ofs(xvd_path, std::ios::binary);
        SX_CHECK(ofs.good());
        ofs.write(reinterpret_cast<const char*>(xvd_img.data()), static_cast<std::streamsize>(xvd_img.size()));
    }

    // Verify PackageVfs::MountXvd mounts the unencrypted XVD and serves its user data
    {
        scarlettx::PackageVfs vfs;
        SX_CHECK(vfs.MountXvd("G:\\mounted_assets", xvd_path).ok());
        SX_CHECK(vfs.Exists("G:/mounted_assets"));
        std::vector<uint8_t> mounted_bytes;
        SX_CHECK(vfs.ReadFileBytes("G:\\mounted_assets", mounted_bytes).ok());
        SX_CHECK_EQ(mounted_bytes.size(), comp_size);
        SX_CHECK(std::memcmp(mounted_bytes.data(), compressed.data(), comp_size) == 0);
    }

    uint64_t xvd_file = 0;
    SX_CHECK_EQ(DStorageOpenXvdFile(factory, xvd_path.c_str(), &xvd_file), 0);
    uint64_t file_sz = 0;
    SX_CHECK_EQ(DStorageGetFileSize(xvd_file, &file_sz), 0);
    SX_CHECK_EQ(file_sz, comp_size);

    std::vector<uint8_t> dst(original.size(), 0);
    DSTORAGE_REQUEST req{};
    req.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    req.SourceFileHandle = xvd_file;
    req.SourceFileOffset = 0;
    req.SourceSize = comp_size;
    req.DestinationMemory = dst.data();
    req.DestinationSize = static_cast<uint32_t>(dst.size());
    req.UncompressedSize = static_cast<uint32_t>(original.size());
    req.CancellationTag = 0x1001;
    req.Name = "ReadCompressedXvdAsset";

    uint64_t fence_val = 0;
    std::atomic<uint64_t> cb_val{0};
    SX_CHECK_EQ(DStorageEnqueueRequest(queue, &req), 0);
    SX_CHECK_EQ(DStorageEnqueueStatus(queue, status_arr, 0), 0);
    SX_CHECK_EQ(DStorageEnqueueSignal(queue, &fence_val, 42, OnSignal, &cb_val), 0);
    SX_CHECK_EQ(DStorageSubmit(queue), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);

    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 0), 1u);
    SX_CHECK_EQ(DStorageGetStatusHResult(status_arr, 0), 0);
    SX_CHECK_EQ(fence_val, 42ull);
    SX_CHECK_EQ(cb_val.load(), 42ull);
    SX_CHECK(std::memcmp(dst.data(), original.data(), original.size()) == 0);

    // 4. Cancellation leaves the destination untouched and reports no error.
    constexpr uint64_t cancel_mask = 0xFFFF0000ull;
    constexpr uint64_t cancel_value = 0xBEEF0000ull;
    std::vector<uint8_t> cancel_dst(original.size(), 0xCC);
    DSTORAGE_REQUEST cancel_req = req;
    cancel_req.DestinationMemory = cancel_dst.data();
    cancel_req.CancellationTag = cancel_value | 1;
    cancel_req.Name = "CancelledRead";

    SX_CHECK_EQ(DStorageEnqueueRequest(queue, &cancel_req), 0);
    SX_CHECK_EQ(DStorageCancelRequestsWithTag(queue, cancel_mask, cancel_value), 0);
    // Requests added after the cancellation still run, even with the same tag.
    std::fill(dst.begin(), dst.end(), 0);
    DSTORAGE_REQUEST later_req = cancel_req;
    later_req.DestinationMemory = dst.data();
    later_req.Name = "ReadAfterCancellation";
    SX_CHECK_EQ(DStorageEnqueueRequest(queue, &later_req), 0);
    SX_CHECK_EQ(DStorageEnqueueStatus(queue, status_arr, 1), 0);
    SX_CHECK_EQ(DStorageSubmit(queue), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);

    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 1), 1u);
    SX_CHECK_EQ(DStorageGetStatusHResult(status_arr, 1), 0);
    SX_CHECK(std::all_of(cancel_dst.begin(), cancel_dst.end(), [](uint8_t byte) { return byte == 0xCC; }));
    SX_CHECK(dst == original);

    DSTORAGE_ERROR_RECORD err{};
    SX_CHECK_EQ(DStorageRetrieveErrorRecord(queue, &err), 0);
    SX_CHECK_EQ(err.FailureCount, 0u);

    // 5. A cancellation must preserve an actual failure earlier in the batch.
    DSTORAGE_REQUEST failed_req = req;
    failed_req.SourceFileOffset = file_sz;
    failed_req.Name = "ReadPastEof";
    SX_CHECK_EQ(DStorageEnqueueRequest(queue, &failed_req), 0);
    SX_CHECK_EQ(DStorageEnqueueRequest(queue, &cancel_req), 0);
    SX_CHECK_EQ(DStorageCancelRequestsWithTag(queue, cancel_mask, cancel_value), 0);
    SX_CHECK_EQ(DStorageEnqueueStatus(queue, status_arr, 2), 0);
    SX_CHECK_EQ(DStorageSubmit(queue), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);

    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 2), 1u);
    constexpr auto invalid_arg = static_cast<int32_t>(0x80070057u);
    SX_CHECK_EQ(DStorageGetStatusHResult(status_arr, 2), invalid_arg);
    SX_CHECK_EQ(DStorageRetrieveErrorRecord(queue, &err), 0);
    SX_CHECK_EQ(err.FailureCount, 1u);
    SX_CHECK_EQ(err.FirstFailureHResult, invalid_arg);
    SX_CHECK_EQ(std::string(err.FailedRequestName), std::string("ReadPastEof"));

    SX_CHECK_EQ(DStorageCloseFile(xvd_file), 0);
    SX_CHECK_EQ(DStorageDestroyStatusArray(status_arr), 0);
    SX_CHECK_EQ(DStorageDestroyQueue(queue), 0);
    SX_CHECK_EQ(DStorageDestroyFactory(factory), 0);

    std::cout << "[test_dstorage_xvd] All DirectStorage + XVD HLE checks passed.\n";
    return 0;
}
