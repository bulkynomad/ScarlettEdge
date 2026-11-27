#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

class XvdContainer;

class PackageVfs {
public:
    PackageVfs();
    ~PackageVfs();

    static PackageVfs& Global();

    void Reset();
    Status MountDirectory(const std::string& virtual_root, const std::string& host_dir);
    Status MountXvd(
        const std::string& virtual_path,
        const std::string& xvd_host_path,
        const std::string& volume_key_hex = "");
    Status ResolvePath(const std::string& virtual_path, std::string& out_host_path) const;
    Status ReadFileBytes(const std::string& virtual_path, std::vector<uint8_t>& out_data) const;
    Status ReadFileRange(const std::string& virtual_path,
                         uint64_t offset,
                         uint64_t size,
                         std::vector<uint8_t>& out_data) const;
    bool GetFileSize(const std::string& virtual_path, uint64_t& out_size) const;
    bool Exists(const std::string& virtual_path) const;

private:
    struct EmbeddedFileRef {
        std::shared_ptr<XvdContainer> container;
        uint64_t user_data_offset = 0;
        uint64_t size_bytes = 0;
        std::string relative_path;
    };

    static std::string NormalizeVirtualPath(const std::string& path);
    static std::string NormalizeKey(const std::string& path);
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> mounts_;
    std::unordered_map<std::string, std::shared_ptr<XvdContainer>> xvd_mounts_;
    std::unordered_map<std::string, EmbeddedFileRef> xvd_embedded_files_;
};

}  // namespace scarlettx
