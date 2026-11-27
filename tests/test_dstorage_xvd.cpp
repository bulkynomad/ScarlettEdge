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
#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/xvd_container.h"

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#endif

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

        // 1b. Encrypted GDFX ("MICROSOFT*XBOX*MEDIA") .xvc hierarchy + PackageVfs file-level mounting + extraction
        const std::string cfg_xml = "<Game configVersion=\"1\"><Identity Name=\"GdfxTest\" /></Game>";
        std::vector<uint8_t> pak_payload(6144);
        for (size_t i = 0; i < pak_payload.size(); ++i) {
            pak_payload[i] = static_cast<uint8_t>((i * 29 + 7) & 0xFFu);
        }
        std::vector<scarlettx::XvdInputFile> gdfx_inputs = {
            {"MicrosoftGame.Config", std::vector<uint8_t>(cfg_xml.begin(), cfg_xml.end())},
            {"Content/Paks/pakchunk0-xsx.pak", pak_payload},
            {"DLC/Season1/manifest.bin", {0xDE, 0xAD, 0xBE, 0xEF}},
        };
        auto gdfx_xvc_bytes = scarlettx::XvdContainer::BuildEncryptedGdfxXvc(
            gdfx_inputs, vol_key.data(), vol_key.size(), 1);
        const auto gdfx_xvc_path = (tmp_dir / "title_gdfx.xvc").string();
        {
            std::ofstream ofs(gdfx_xvc_path, std::ios::binary);
            SX_CHECK(ofs.good());
            ofs.write(reinterpret_cast<const char*>(gdfx_xvc_bytes.data()),
                      static_cast<std::streamsize>(gdfx_xvc_bytes.size()));
        }

        scarlettx::XvdContainer gdfx_xvd;
        SX_CHECK(gdfx_xvd.OpenFile(gdfx_xvc_path).ok());
        SX_CHECK(gdfx_xvd.Header().is_encrypted);
        SX_CHECK(!gdfx_xvd.HasEmbeddedFilesystem());  // Must reject without volume key
        SX_CHECK(gdfx_xvd.SetVolumeKeyHex(hex_key).ok());
        SX_CHECK(gdfx_xvd.HasEmbeddedFilesystem());

        std::vector<scarlettx::XvdEmbeddedFile> gdfx_list;
        SX_CHECK(gdfx_xvd.ListEmbeddedFiles(gdfx_list).ok());
        SX_CHECK(gdfx_list.size() >= 3u);

        std::vector<uint8_t> read_pak;
        SX_CHECK(gdfx_xvd.ReadEmbeddedFile("content\\paks\\pakchunk0-xsx.pak", read_pak).ok());
        SX_CHECK_EQ(read_pak.size(), pak_payload.size());
        SX_CHECK(std::memcmp(read_pak.data(), pak_payload.data(), pak_payload.size()) == 0);

        scarlettx::PackageVfs gdfx_vfs;
        SX_CHECK(gdfx_vfs.MountXvd("G:", gdfx_xvc_path, hex_key).ok());
        SX_CHECK(gdfx_vfs.Exists("G:\\MicrosoftGame.Config"));
        SX_CHECK(gdfx_vfs.Exists("G:\\Content\\Paks\\pakchunk0-xsx.pak"));
        SX_CHECK(gdfx_vfs.Exists("G:/DLC/Season1/manifest.bin"));
        std::vector<uint8_t> vfs_cfg;
        SX_CHECK(gdfx_vfs.ReadFileBytes("G:\\MicrosoftGame.Config", vfs_cfg).ok());
        SX_CHECK_EQ(std::string(vfs_cfg.begin(), vfs_cfg.end()), cfg_xml);

        const auto extract_dir = tmp_dir / "gdfx_extracted";
        SX_CHECK(gdfx_xvd.ExtractEmbeddedFilesystem(extract_dir.string()).ok());
        SX_CHECK(std::filesystem::exists(extract_dir / "MicrosoftGame.Config"));
        SX_CHECK(std::filesystem::exists(extract_dir / "Content" / "Paks" / "pakchunk0-xsx.pak"));
        SX_CHECK_EQ(std::filesystem::file_size(extract_dir / "Content" / "Paks" / "pakchunk0-xsx.pak"),
                    pak_payload.size());
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
    std::fill(dst.begin(), dst.end(), uint8_t{0});
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

#if defined(_WIN32)
    // 6. GPU Compute GDeflate Decompression (DSTORAGE_REQUEST_DESTINATION_BUFFER & DStorageDecompressGpuCompute)
    //    and Direct DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION GPU CopyTextureRegion Upload!
    {
        scarlettx::D3D12Backend gpu_backend;
        SX_CHECK(gpu_backend.Initialize(true /* WARP-safe */, false).ok());
        auto* d3d_dev = static_cast<ID3D12Device*>(gpu_backend.NativeDevice());
        SX_CHECK(d3d_dev != nullptr);

        D3D12_HEAP_PROPERTIES def_hp{};
        def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES rb_hp{};
        rb_hp.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = original.size() + 256ull;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource* gpu_dst_buf = nullptr;
        ID3D12Resource* gpu_rb_buf = nullptr;
        SX_CHECK(SUCCEEDED(d3d_dev->CreateCommittedResource(
            &def_hp, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpu_dst_buf))));
        SX_CHECK(SUCCEEDED(d3d_dev->CreateCommittedResource(
            &rb_hp, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu_rb_buf))));

        // Test 6a: Enqueue DSTORAGE_REQUEST_DESTINATION_BUFFER directly from XVD into DEFAULT-heap ID3D12Resource*
        DSTORAGE_REQUEST gpu_buf_req{};
        gpu_buf_req.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        gpu_buf_req.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
        gpu_buf_req.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        gpu_buf_req.SourceFileHandle = xvd_file;
        gpu_buf_req.SourceFileOffset = 0;
        gpu_buf_req.SourceSize = comp_size;
        gpu_buf_req.DestinationMemory = gpu_dst_buf;
        gpu_buf_req.DestinationSize = static_cast<uint32_t>(original.size());
        gpu_buf_req.UncompressedSize = static_cast<uint32_t>(original.size());
        gpu_buf_req.Name = "GpuComputeGDeflateFromXvd";

        SX_CHECK_EQ(DStorageEnqueueRequest(queue, &gpu_buf_req), 0);
        SX_CHECK_EQ(DStorageEnqueueStatus(queue, status_arr, 3), 0);
        SX_CHECK_EQ(DStorageSubmit(queue), 0);
        SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);
        SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 3), 1u);
        SX_CHECK_EQ(DStorageGetStatusHResult(status_arr, 3), 0);

        // Also test standalone DStorageDecompressGpuCompute at non-zero offset (64)
        std::vector<uint8_t> mini_plain(253);  // non-multiple-of-4 size
        for (size_t i = 0; i < mini_plain.size(); ++i) {
            mini_plain[i] = (i < 100) ? 0xE7u : static_cast<uint8_t>((i * 19u + 5u) & 0xFFu);
        }
        std::vector<uint8_t> mini_comp(512);
        uint32_t mini_comp_sz = 0;
        SX_CHECK_EQ(DStorageCompressBuffer(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, mini_plain.data(),
                                           static_cast<uint32_t>(mini_plain.size()), mini_comp.data(),
                                           static_cast<uint32_t>(mini_comp.size()), &mini_comp_sz), 0);

        // Read back gpu_dst_buf to verify GPU compute GDeflate output matches `original` byte-for-byte!
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* d3d_q = nullptr;
        ID3D12CommandAllocator* d3d_alloc = nullptr;
        ID3D12GraphicsCommandList* d3d_cmd = nullptr;
        ID3D12Fence* d3d_fence = nullptr;
        SX_CHECK(SUCCEEDED(d3d_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&d3d_q))));
        SX_CHECK(SUCCEEDED(d3d_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d3d_alloc))));
        SX_CHECK(SUCCEEDED(d3d_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d_alloc, nullptr, IID_PPV_ARGS(&d3d_cmd))));
        SX_CHECK(SUCCEEDED(d3d_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d3d_fence))));
        HANDLE d3d_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        SX_CHECK(d3d_ev != nullptr);
        uint64_t fval = 0;

        auto copy_and_wait = [&](auto&& record_fn) {
            d3d_alloc->Reset();
            d3d_cmd->Reset(d3d_alloc, nullptr);
            record_fn(d3d_cmd);
            d3d_cmd->Close();
            ID3D12CommandList* lists[] = {d3d_cmd};
            d3d_q->ExecuteCommandLists(1, lists);
            const uint64_t v = ++fval;
            d3d_q->Signal(d3d_fence, v);
            if (d3d_fence->GetCompletedValue() < v) {
                d3d_fence->SetEventOnCompletion(v, d3d_ev);
                WaitForSingleObject(d3d_ev, 5000);
            }
        };

        copy_and_wait([&](ID3D12GraphicsCommandList* cl) {
            cl->CopyBufferRegion(gpu_rb_buf, 0, gpu_dst_buf, 0, original.size());
        });
        void* mapped = nullptr;
        SX_CHECK(SUCCEEDED(gpu_rb_buf->Map(0, nullptr, &mapped)) && mapped != nullptr);
        SX_CHECK(std::memcmp(mapped, original.data(), original.size()) == 0);
        gpu_rb_buf->Unmap(0, nullptr);

        // Verify DStorageDecompressGpuCompute with non-zero destination offset (64) & odd byte length (253)
        SX_CHECK_EQ(DStorageDecompressGpuCompute(
                        d3d_dev, d3d_q, DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
                        mini_comp.data(), mini_comp_sz, gpu_dst_buf, 64ull,
                        static_cast<uint32_t>(mini_plain.size())),
                    0);
        copy_and_wait([&](ID3D12GraphicsCommandList* cl) {
            cl->CopyBufferRegion(gpu_rb_buf, 0, gpu_dst_buf, 64ull, mini_plain.size());
        });
        mapped = nullptr;
        SX_CHECK(SUCCEEDED(gpu_rb_buf->Map(0, nullptr, &mapped)) && mapped != nullptr);
        SX_CHECK(std::memcmp(mapped, mini_plain.data(), mini_plain.size()) == 0);
        gpu_rb_buf->Unmap(0, nullptr);

        // Test 6b: Direct DStorageEnqueueTextureRegion (GDeflate-compressed + 4KB_S swizzled 32x32 RGBA8 texture!)
        XG_TEXTURE2D_DESC xgt{};
        xgt.Width = 32;
        xgt.Height = 32;
        xgt.MipLevels = 1;
        xgt.ArraySize = 1;
        xgt.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        xgt.SampleDesc.Count = 1;
        xgt.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
        XG_RESOURCE_LAYOUT xgl{};
        scarlettx::gpu::xg::ComputeTexture2DLayout(xgt, &xgl);

        std::vector<uint8_t> linear_rgba(32u * 32u * 4u);
        for (size_t i = 0; i < linear_rgba.size(); ++i) {
            linear_rgba[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
        }
        std::vector<uint8_t> swizzled_rgba(static_cast<size_t>(xgl.SizeBytes), 0);
        SX_CHECK(scarlettx::gpu::xg::CopyIntoSubresource(xgl, swizzled_rgba.data(), 0u, 0u, linear_rgba.data(), 32u * 4u));

        std::vector<uint8_t> comp_tex(8192);
        uint32_t comp_tex_sz = 0;
        SX_CHECK_EQ(DStorageCompressBuffer(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, swizzled_rgba.data(),
                                           static_cast<uint32_t>(swizzled_rgba.size()), comp_tex.data(),
                                           static_cast<uint32_t>(comp_tex.size()), &comp_tex_sz), 0);

        D3D12_RESOURCE_DESC tex_desc{};
        tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex_desc.Width = 32;
        tex_desc.Height = 32;
        tex_desc.DepthOrArraySize = 1;
        tex_desc.MipLevels = 1;
        tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex_desc.SampleDesc.Count = 1;
        ID3D12Resource* gpu_tex = nullptr;
        SX_CHECK(SUCCEEDED(d3d_dev->CreateCommittedResource(
            &def_hp, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpu_tex))));

        DSTORAGE_QUEUE_DESC mem_qd{};
        mem_qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        mem_qd.Capacity = 16;
        mem_qd.Name = "GpuTexUploadQueue";
        uint64_t mem_q = 0;
        SX_CHECK_EQ(DStorageCreateQueue(factory, &mem_qd, &mem_q), 0);

        DSTORAGE_TEXTURE_REGION_DESC tex_req{};
        tex_req.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        tex_req.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        tex_req.SwizzleMode = static_cast<uint16_t>(XG_SWIZZLE_MODE_4KB_S);
        tex_req.SourceMemory = comp_tex.data();
        tex_req.SourceSize = comp_tex_sz;
        tex_req.UncompressedSize = static_cast<uint32_t>(swizzled_rgba.size());
        tex_req.D3D12Device = d3d_dev;
        tex_req.D3D12CommandQueue = d3d_q;
        tex_req.DestinationTexture = gpu_tex;
        tex_req.SubresourceIndex = 0;
        tex_req.InitialResourceState = D3D12_RESOURCE_STATE_COMMON;
        tex_req.Name = "UploadSwizzledGDeflateTex";

        SX_CHECK_EQ(DStorageEnqueueTextureRegion(mem_q, &tex_req), 0);
        SX_CHECK_EQ(DStorageEnqueueStatus(mem_q, status_arr, 4), 0);
        SX_CHECK_EQ(DStorageSubmit(mem_q), 0);
        SX_CHECK_EQ(DStorageWaitForIdle(mem_q, 5000), 0);
        SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 4), 1u);
        SX_CHECK_EQ(DStorageGetStatusHResult(status_arr, 4), 0);
        SX_CHECK_EQ(DStorageDestroyQueue(mem_q), 0);

        // Read back gpu_tex via CopyTextureRegion (256-byte row pitch) and verify every texel matches linear_rgba!
        D3D12_RESOURCE_DESC tex_rb_desc = buf_desc;
        tex_rb_desc.Width = 256ull * 32ull;
        ID3D12Resource* tex_rb = nullptr;
        SX_CHECK(SUCCEEDED(d3d_dev->CreateCommittedResource(
            &rb_hp, D3D12_HEAP_FLAG_NONE, &tex_rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex_rb))));

        copy_and_wait([&](ID3D12GraphicsCommandList* cl) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = gpu_tex;
            b.Transition.Subresource = 0;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1, &b);

            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = tex_rb;
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst_loc.PlacedFootprint.Footprint.Width = 32;
            dst_loc.PlacedFootprint.Footprint.Height = 32;
            dst_loc.PlacedFootprint.Footprint.Depth = 1;
            dst_loc.PlacedFootprint.Footprint.RowPitch = 256;

            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = gpu_tex;
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src_loc.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        });

        mapped = nullptr;
        SX_CHECK(SUCCEEDED(tex_rb->Map(0, nullptr, &mapped)) && mapped != nullptr);
        for (uint32_t y = 0; y < 32u; ++y) {
            SX_CHECK(std::memcmp(static_cast<const uint8_t*>(mapped) + y * 256u,
                                 linear_rgba.data() + y * 128u,
                                 128u) == 0);
        }
        tex_rb->Unmap(0, nullptr);

        tex_rb->Release();
        gpu_tex->Release();
        CloseHandle(d3d_ev);
        d3d_fence->Release();
        d3d_cmd->Release();
        d3d_alloc->Release();
        d3d_q->Release();
        gpu_rb_buf->Release();
        gpu_dst_buf->Release();
    }
#endif

    SX_CHECK_EQ(DStorageCloseFile(xvd_file), 0);
    SX_CHECK_EQ(DStorageDestroyStatusArray(status_arr), 0);
    SX_CHECK_EQ(DStorageDestroyQueue(queue), 0);
    SX_CHECK_EQ(DStorageDestroyFactory(factory), 0);

    std::cout << "[test_dstorage_xvd] All DirectStorage + XVD HLE checks passed.\n";
    return 0;
}
