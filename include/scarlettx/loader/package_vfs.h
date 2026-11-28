#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

class XvdContainer;

struct VfsEntryInfo {
    bool exists = false;
    bool is_directory = false;
    bool is_xvd_backed = false;
    uint64_t size_bytes = 0;
    std::string host_path;
};

struct VfsDirEntry {
    std::string name;
    uint64_t size_bytes = 0;
    bool is_directory = false;
};

class PackageVfs {
public:
    static constexpr size_t kSectorSize = 4096;
    static constexpr size_t kMaxCachedBlocks = 512;  // 2 MiB bounded LRU cache
    static constexpr size_t kMaxCachedHostHandles = 64;

    PackageVfs();
    ~PackageVfs();

    static PackageVfs& Global();

    void Reset();
    Status MountDirectory(const std::string& virtual_root, const std::string& host_dir);
    Status MountXvd(
        const std::string& virtual_path,
        const std::string& xvd_host_path,
        const std::string& volume_key_hex = "");
    Status MountXvdContainer(
        const std::string& virtual_path,
        std::shared_ptr<XvdContainer> container);

    Status ResolvePath(const std::string& virtual_path, std::string& out_host_path) const;
    bool Stat(const std::string& virtual_path, VfsEntryInfo& out_info) const;
    Status ListDirectory(const std::string& virtual_dir, std::vector<VfsDirEntry>& out_entries) const;

    Status ReadFileBytes(const std::string& virtual_path, std::vector<uint8_t>& out_data) const;
    Status ReadFileRange(const std::string& virtual_path,
                         uint64_t offset,
                         uint64_t size,
                         std::vector<uint8_t>& out_data) const;
    Status ReadFileRangeToBuffer(const std::string& virtual_path,
                                 uint64_t offset,
                                 size_t size,
                                 void* dst,
                                 size_t& out_bytes_read) const;

    bool GetFileSize(const std::string& virtual_path, uint64_t& out_size) const;
    bool Exists(const std::string& virtual_path) const;

private:
    struct EmbeddedFileRef {
        std::shared_ptr<XvdContainer> container;
        uint64_t user_data_offset = 0;
        uint64_t size_bytes = 0;
        std::string relative_path;
    };

    struct CachedHostFile {
        std::string key;
        std::string host_path;
        void* handle = nullptr;
        uint64_t size_bytes = 0;
    };

    struct CachedBlockKey {
        std::string file_key;
        uint64_t block_idx = 0;
        bool operator==(const CachedBlockKey& o) const noexcept {
            return block_idx == o.block_idx && file_key == o.file_key;
        }
    };

    struct CachedBlockKeyHash {
        size_t operator()(const CachedBlockKey& k) const noexcept {
            const size_t h1 = std::hash<std::string>{}(k.file_key);
            const size_t h2 = std::hash<uint64_t>{}(k.block_idx);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
        }
    };

    struct CachedBlock {
        CachedBlockKey key;
        size_t valid_len = 0;
        std::array<uint8_t, kSectorSize> bytes{};
    };

    void CloseAllHostHandlesUnlocked() const noexcept;
    void ClearBlockCacheUnlocked() const noexcept;
    void IndexContainerEntriesUnlocked(const std::string& mount_key,
                                       const std::shared_ptr<XvdContainer>& container);
    Status ReadHostRangeCached(const std::string& vfs_key,
                               const std::string& host_path,
                               uint64_t offset,
                               uint64_t size,
                               void* dst,
                               size_t& out_got) const;

    static std::string NormalizeVirtualPath(const std::string& path);
    static std::string NormalizeKey(const std::string& path);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> mounts_;
    std::unordered_map<std::string, std::shared_ptr<XvdContainer>> xvd_mounts_;
    std::unordered_map<std::string, EmbeddedFileRef> xvd_embedded_files_;
    std::unordered_set<std::string> xvd_directories_;
    std::unordered_map<std::string, std::vector<VfsDirEntry>> xvd_dir_children_;

    mutable std::mutex cache_mu_;
    mutable std::list<CachedHostFile> host_handle_lru_;
    mutable std::unordered_map<std::string, std::list<CachedHostFile>::iterator> host_handle_map_;
    mutable std::list<CachedBlock> block_lru_;
    mutable std::unordered_map<CachedBlockKey, std::list<CachedBlock>::iterator, CachedBlockKeyHash> block_map_;
};

}  // namespace scarlettx
