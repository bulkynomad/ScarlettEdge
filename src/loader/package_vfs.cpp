#include "scarlettx/loader/package_vfs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace scarlettx {

std::string PackageVfs::NormalizeVirtualPath(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

std::string PackageVfs::NormalizeKey(const std::string& path) {
    std::string key = NormalizeVirtualPath(path);
    while (!key.empty() && key.back() == '\\') {
        key.pop_back();
    }
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return key;
}

Status PackageVfs::MountDirectory(const std::string& virtual_root, const std::string& host_dir) {
    if (virtual_root.empty() || host_dir.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Empty virtual_root or host_dir");
    }
    if (!std::filesystem::exists(host_dir)) {
        return Status::Error(StatusCode::NotFound, "Host directory does not exist: " + host_dir);
    }
    mounts_[NormalizeKey(virtual_root)] = host_dir;
    return Status::Ok();
}

Status PackageVfs::MountXvd(
    const std::string& virtual_path,
    const std::string& xvd_host_path,
    const std::string& volume_key_hex) {
    if (virtual_path.empty() || xvd_host_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Empty virtual_path or xvd_host_path");
    }
    auto container = std::make_shared<XvdContainer>();
    Status st = container->OpenFile(xvd_host_path);
    if (!st.ok()) {
        return st;
    }
    if (!volume_key_hex.empty()) {
        st = std::filesystem::exists(volume_key_hex)
                 ? container->LoadVolumeKeyFile(volume_key_hex)
                 : container->SetVolumeKeyHex(volume_key_hex);
        if (!st.ok()) {
            return st;
        }
    }
    if (container->Header().is_encrypted && !container->HasVolumeKey()) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Encrypted XVD containers cannot be mounted without a volume key");
    }
    const std::string mount_key = NormalizeKey(virtual_path);
    std::vector<XvdEmbeddedFile> embedded;
    if (container->ListEmbeddedFiles(embedded).ok()) {
        for (const auto& entry : embedded) {
            if (entry.is_directory) continue;
            const std::string file_key = NormalizeKey(mount_key + "\\" + entry.relative_path);
            xvd_embedded_files_[file_key] = EmbeddedFileRef{
                container,
                entry.user_data_offset,
                entry.size_bytes,
            };
        }
    }
    xvd_mounts_[mount_key] = std::move(container);
    return Status::Ok();
}

Status PackageVfs::ResolvePath(const std::string& virtual_path, std::string& out_host_path) const {
    std::string norm = NormalizeVirtualPath(virtual_path);
    std::string lower = norm;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // A nested directory mount takes precedence over its parent drive mount.
    const std::string* host_dir = nullptr;
    size_t root_length = 0;
    for (const auto& [root, directory] : mounts_) {
        if ((!host_dir || root.size() > root_length) && lower.starts_with(root) &&
            (lower.size() == root.size() || lower[root.size()] == '\\')) {
            host_dir = &directory;
            root_length = root.size();
        }
    }
    if (!host_dir) {
        return Status::Error(StatusCode::NotFound, "No mount point for virtual path: " + virtual_path);
    }

    const size_t rel_start = norm.find_first_not_of('\\', root_length);
    const std::string rel = rel_start == std::string::npos ? std::string{} : norm.substr(rel_start);
    out_host_path = (std::filesystem::path(*host_dir) / rel).string();
    return Status::Ok();
}

Status PackageVfs::ReadFileBytes(const std::string& virtual_path,
                                 std::vector<uint8_t>& out_data) const {
    const std::string key = NormalizeKey(virtual_path);
    auto eit = xvd_embedded_files_.find(key);
    if (eit != xvd_embedded_files_.end()) {
        return eit->second.container->ReadUserData(eit->second.user_data_offset, eit->second.size_bytes, out_data);
    }
    auto xit = xvd_mounts_.find(key);
    if (xit != xvd_mounts_.end()) {
        const auto& hdr = xit->second->Header();
        return xit->second->ReadUserData(0, hdr.user_data_length, out_data);
    }
    std::string host_path;
    Status st = ResolvePath(virtual_path, host_path);
    if (!st.ok()) {
        return st;
    }
    std::ifstream ifs(host_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
    }
    out_data.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return Status::Ok();
}

bool PackageVfs::Exists(const std::string& virtual_path) const {
    const std::string key = NormalizeKey(virtual_path);
    if (xvd_embedded_files_.find(key) != xvd_embedded_files_.end()) {
        return true;
    }
    if (xvd_mounts_.find(key) != xvd_mounts_.end()) {
        return true;
    }
    std::string host_path;
    if (!ResolvePath(virtual_path, host_path).ok()) {
        return false;
    }
    return std::filesystem::exists(host_path);
}

}  // namespace scarlettx
