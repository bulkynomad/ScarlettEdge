#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/loader/xvd_container.h"

namespace scarlettx {

class PackageVfs {
public:
    Status MountDirectory(const std::string& virtual_root, const std::string& host_dir);
    Status MountXvd(const std::string& virtual_path, const std::string& xvd_host_path);
    Status ResolvePath(const std::string& virtual_path, std::string& out_host_path) const;
    Status ReadFileBytes(const std::string& virtual_path, std::vector<uint8_t>& out_data) const;
    bool Exists(const std::string& virtual_path) const;

private:
    static std::string NormalizeVirtualPath(const std::string& path);
    static std::string NormalizeKey(const std::string& path);
    std::unordered_map<std::string, std::string> mounts_;
    std::unordered_map<std::string, std::shared_ptr<XvdContainer>> xvd_mounts_;
};

}  // namespace scarlettx
