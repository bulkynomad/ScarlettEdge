#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx::crypto {
struct AesXtsPreparedKey;
}

namespace scarlettx {

// Xbox Virtual Disk (XVD / XVC) header metadata.
// Supports both ScarlettX synthetic headers and retail msft-xvd v3 containers
// (0x3000 header, Merkle SHA-256 hash tree, XvcInfo/XvcRegionHeader[], and NTFS/GDFX volumes).
struct XvdHeaderInfo {
    bool valid = false;
    uint32_t format_version = 0;
    uint32_t content_type = 0;
    uint32_t volume_flags = 0;
    uint64_t drive_size = 0;
    uint64_t user_data_offset = 0;
    uint64_t user_data_length = 0;
    uint32_t embedded_xvd_length = 0;
    bool is_encrypted = false;
    bool has_data_integrity = false;
    std::array<uint8_t, 16> content_id{};
    std::string magic;
    bool is_retail_v3_layout = false;
    uint64_t xvc_data_length = 0;
    uint64_t mutable_data_length = 0;
    std::array<uint8_t, 32> top_hash_block_hash{};
    std::array<uint8_t, 32> original_xvc_data_hash{};
};

struct XvcRegionInfo {
    uint32_t id = 0;
    uint16_t key_id = 0xFFFFu;  // 0xFFFF = unencrypted region even when container is encrypted
    uint32_t flags = 0;
    uint64_t user_data_offset = 0;
    uint64_t length = 0;
};

struct XvdEmbeddedFile {
    std::string relative_path;
    uint64_t user_data_offset = 0;
    uint64_t size_bytes = 0;
    bool is_directory = false;
};

struct XvdInputFile {
    std::string relative_path;
    std::vector<uint8_t> data;
};

class XvdContainer {
public:
    static constexpr size_t kMagicOffset = 0x200;
    static constexpr size_t kMinHeaderSize = 0x1000;
    static constexpr size_t kRetailHeaderSize = 0x3000;
    static constexpr size_t kSectorSize = 0x1000;
    static constexpr uint32_t kMerkleEntriesPerHashBlock = 0xAAu;  // 170 entries * 24B = 4080B
    static constexpr size_t kMerkleHashEntrySize = 24;
    static constexpr size_t kGdfxSectorSize = 2048;
    static constexpr uint64_t kGdfxDescriptorOffset = 0x10000ull;
    static constexpr const char* kMagic = "msft-xvd";
    static constexpr const char* kGdfxMagic = "MICROSOFT*XBOX*MEDIA";
    static constexpr const char* kNtfsOemMagic = "NTFS    ";
    static constexpr uint16_t kUnencryptedKeyId = 0xFFFFu;

    // Volume flag bits (publicly documented XVD header flags).
    static constexpr uint32_t kFlagReadOnly = 1u << 0;
    static constexpr uint32_t kFlagEncryptionDisabled = 1u << 1;
    static constexpr uint32_t kFlagDataIntegrityDisabled = 1u << 2;

    XvdContainer() = default;
    ~XvdContainer();

    XvdContainer(const XvdContainer&) = delete;
    XvdContainer& operator=(const XvdContainer&) = delete;
    XvdContainer(XvdContainer&& other) noexcept;
    XvdContainer& operator=(XvdContainer&& other) noexcept;

    Status OpenFile(const std::string& path);
    Status OpenBytes(std::vector<uint8_t> data);

    // Loads a 16-byte, 32-byte (AES-128-XTS), or 64-byte (AES-256-XTS) volume key for encrypted XVD streaming.
    Status SetVolumeKey(const uint8_t* key, size_t key_len);
    Status SetVolumeKeyHex(const std::string& hex_key);
    Status LoadVolumeKeyFile(const std::string& key_file_path);

    // Multi-key CIK ring slot management for retail XVC regions (KeyId != 0xFFFF).
    Status SetCikKey(uint16_t slot, const uint8_t* key, size_t key_len);
    Status SetCikKeyHex(uint16_t slot, const std::string& hex_key);

    bool HasVolumeKey() const { return has_volume_key_ || !cik_keys_.empty(); }

    bool IsOpen() const { return header_.valid; }
    const XvdHeaderInfo& Header() const { return header_; }
    const std::string& Path() const { return path_; }
    const std::vector<XvcRegionInfo>& Regions() const { return regions_; }

    // Merkle hash tree translation & optional page integrity verification.
    uint64_t LogicalUserPageToPhysicalOffset(uint64_t user_page_idx) const;
    Status VerifyDataPageHash(uint64_t user_page_idx) const;
    void SetVerifyIntegrityOnRead(bool enable) { verify_integrity_on_read_ = enable; }

    // Reads bytes from the user-data region of the XVD (automatically de-interleaving
    // Merkle hash blocks and decrypting 4 KiB sectors via XTS when encrypted).
    Status ReadUserData(uint64_t offset, uint64_t size, std::vector<uint8_t>& out_data) const;

    // Embedded Xbox NTFS (Gen9 .xvc / .msixvc) & GDFX ("MICROSOFT*XBOX*MEDIA") filesystem
    // inspection, multi-run / resident file reading, and bounded 4 MiB streaming extraction.
    bool HasEmbeddedFilesystem() const;
    Status ListEmbeddedFiles(std::vector<XvdEmbeddedFile>& out_files) const;
    Status ReadEmbeddedFile(const std::string& relative_path, std::vector<uint8_t>& out_data) const;
    Status ReadEmbeddedFile(
        const std::string& relative_path,
        uint64_t offset,
        uint64_t size,
        std::vector<uint8_t>& out_data) const;
    Status ExtractEmbeddedFilesystem(const std::string& dest_dir) const;

    // Builds an unencrypted synthetic XVD image in memory for autonomous testing.
    static std::vector<uint8_t> BuildSyntheticXvd(
        const std::vector<uint8_t>& user_data,
        uint32_t content_type = 1,
        bool mark_encrypted = false);

    // Builds an encrypted synthetic XVD image using 4 KiB sector XTS encryption.
    static std::vector<uint8_t> BuildEncryptedSyntheticXvd(
        const std::vector<uint8_t>& user_data,
        const uint8_t* volume_key,
        size_t key_len,
        uint32_t content_type = 1);

    // Builds a raw Xbox GDFX filesystem volume and an AES-XTS encrypted GDFX .xvc image.
    static std::vector<uint8_t> BuildGdfxVolumeBytes(const std::vector<XvdInputFile>& files);
    static std::vector<uint8_t> BuildEncryptedGdfxXvc(
        const std::vector<XvdInputFile>& files,
        const uint8_t* volume_key,
        size_t key_len,
        uint32_t content_type = 1);

    // Builds a retail msft-xvd v3 .xvc container (0x3000 header, 0x284+ offsets,
    // interleaved 0xAA Merkle hash tree, XvcInfo + XvcRegionHeader[], and VDUID-seeded AES-XTS).
    static std::vector<uint8_t> BuildRetailV3Xvc(
        const std::vector<uint8_t>& user_data,
        const uint8_t* volume_key = nullptr,
        size_t key_len = 0,
        bool enable_merkle_tree = true,
        const std::vector<XvcRegionInfo>& regions = {},
        uint32_t content_type = 1);

    static std::vector<uint8_t> BuildRetailV3XvcMultiKey(
        const std::vector<uint8_t>& user_data,
        const std::vector<std::vector<uint8_t>>& cik_keys,
        const std::vector<XvcRegionInfo>& regions,
        bool enable_merkle_tree = true,
        uint32_t content_type = 1);

    // Builds a clean-room NTFS 3.1 user partition and a retail-v3 Merkle + NTFS .xvc container.
    static std::vector<uint8_t> BuildNtfsVolumeBytes(const std::vector<XvdInputFile>& files);
    static std::vector<uint8_t> BuildSyntheticNtfsXvc(
        const std::vector<XvdInputFile>& files,
        const uint8_t* key,
        size_t key_len,
        bool enable_merkle_tree = true);

private:
    struct FileExtent {
        uint64_t user_offset = 0;
        uint64_t byte_length = 0;
        bool is_sparse = false;
    };

    struct CachedEmbeddedFileInfo {
        XvdEmbeddedFile meta;
        bool is_resident = false;
        uint64_t initialized_size = 0;
        std::vector<uint8_t> resident_bytes;
        std::vector<FileExtent> extents;
    };

    void CloseFileHandle() noexcept;
    void ResetFsCache() const;
    Status ReadRawContainerBytes(uint64_t abs_offset, uint8_t* dst, size_t len) const;
    Status ParseHeaderFromBytes(const uint8_t* data, size_t size, uint64_t total_container_size);
    Status FinalizeOpenMetadata();
    Status ReadLogicalBackingPage(uint64_t logical_page_idx, uint8_t out_page[kSectorSize]) const;
    Status EnsureFilesystemParsed() const;

    void* file_handle_ = nullptr;
    uint64_t file_size_ = 0;
    std::string path_;
    std::vector<uint8_t> backing_bytes_;
    bool memory_backed_ = false;
    XvdHeaderInfo header_{};

    uint64_t base_data_offset_ = kMinHeaderSize;
    uint64_t user_data_pages_ = 0;
    uint64_t total_hashed_pages_ = 0;
    uint32_t merkle_tree_depth_ = 0;
    bool contiguous_merkle_layout_ = false;
    uint64_t merkle_root_offset_ = 0;
    uint64_t pdu_offset_ = 0;
    uint64_t pdu_pages_ = 0;
    uint64_t xvc_info_offset_ = 0;
    bool verify_integrity_on_read_ = false;
    std::vector<XvcRegionInfo> regions_;

    std::array<uint8_t, 64> volume_key_{};
    uint32_t volume_key_len_ = 0;
    bool has_volume_key_ = false;
    std::shared_ptr<crypto::AesXtsPreparedKey> prepared_key_;
    std::unordered_map<uint16_t, std::shared_ptr<crypto::AesXtsPreparedKey>> cik_keys_;

    mutable std::mutex fs_cache_mu_;
    mutable bool fs_cached_ = false;
    mutable std::vector<XvdEmbeddedFile> cached_fs_entries_;
    mutable std::unordered_map<std::string, CachedEmbeddedFileInfo> cached_fs_lookup_;
};

}  // namespace scarlettx
