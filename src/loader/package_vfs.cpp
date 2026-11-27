#include "scarlettx/loader/package_vfs.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>

namespace scarlettx {

namespace {

bool HasTraversalSegment(const std::string& path) {
    if (path.find('\0') != std::string::npos) {
        return true;
    }
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == '/' || path[i] == '\\')) {
            ++i;
        }
        const size_t start = i;
        while (i < path.size() && path[i] != '/' && path[i] != '\\') {
            ++i;
        }
        if (i - start == 2 && path[start] == '.' && path[start + 1] == '.') {
            return true;
        }
    }
    return false;
}

Status ResolvePathUnlocked(const std::unordered_map<std::string, std::string>& mounts,
                           const std::string& norm_virtual_path,
                           const std::string& raw_virtual_path,
                           std::string& out_host_path) {
    if (HasTraversalSegment(raw_virtual_path)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Path traversal ('..') is not permitted in virtual path: " +
                                 raw_virtual_path);
    }

    std::string lower = norm_virtual_path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // A nested directory mount takes precedence over its parent drive mount.
    const std::string* host_dir = nullptr;
    size_t root_length = 0;
    for (const auto& [root, directory] : mounts) {
        if ((!host_dir || root.size() > root_length) && lower.starts_with(root) &&
            (lower.size() == root.size() || lower[root.size()] == '\\')) {
            host_dir = &directory;
            root_length = root.size();
        }
    }
    if (!host_dir) {
        return Status::Error(StatusCode::NotFound,
                             "No mount point for virtual path: " + raw_virtual_path);
    }

    const size_t rel_start = norm_virtual_path.find_first_not_of('\\', root_length);
    const std::string rel =
        rel_start == std::string::npos ? std::string{} : norm_virtual_path.substr(rel_start);
    out_host_path = (std::filesystem::path(*host_dir) / rel).string();
    return Status::Ok();
}

}  // namespace

PackageVfs& PackageVfs::Global() {
    static PackageVfs instance;
    return instance;
}

void PackageVfs::Reset() {
    std::lock_guard<std::mutex> lk(mutex_);
    mounts_.clear();
    xvd_mounts_.clear();
    xvd_embedded_files_.clear();
}

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
    if (HasTraversalSegment(virtual_root)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Invalid traversal in virtual_root: " + virtual_root);
    }
    std::error_code ec;
    if (!std::filesystem::exists(host_dir, ec)) {
        return Status::Error(StatusCode::NotFound, "Host directory does not exist: " + host_dir);
    }
    std::lock_guard<std::mutex> lk(mutex_);
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
    if (HasTraversalSegment(virtual_path)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Invalid traversal in virtual_path: " + virtual_path);
    }
    auto container = std::make_shared<XvdContainer>();
    Status st = container->OpenFile(xvd_host_path);
    if (!st.ok()) {
        return st;
    }
    if (!volume_key_hex.empty()) {
        std::error_code ec;
        st = std::filesystem::exists(volume_key_hex, ec)
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
    const bool has_embedded = container->ListEmbeddedFiles(embedded).ok();

    std::lock_guard<std::mutex> lk(mutex_);
    if (has_embedded) {
        for (const auto& entry : embedded) {
            if (entry.is_directory) continue;
            const std::string file_key = NormalizeKey(mount_key + "\\" + entry.relative_path);
            xvd_embedded_files_[file_key] = EmbeddedFileRef{
                container,
                entry.user_data_offset,
                entry.size_bytes,
                entry.relative_path,
            };
        }
    }
    xvd_mounts_[mount_key] = std::move(container);
    return Status::Ok();
}

Status PackageVfs::ResolvePath(const std::string& virtual_path, std::string& out_host_path) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return ResolvePathUnlocked(mounts_, NormalizeVirtualPath(virtual_path), virtual_path,
                               out_host_path);
}

Status PackageVfs::ReadFileBytes(const std::string& virtual_path,
                                 std::vector<uint8_t>& out_data) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (HasTraversalSegment(virtual_path)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Path traversal ('..') is not permitted in virtual path: " +
                                 virtual_path);
    }
    const std::string key = NormalizeKey(virtual_path);
    auto eit = xvd_embedded_files_.find(key);
    if (eit != xvd_embedded_files_.end()) {
        if (!eit->second.relative_path.empty()) {
            Status est = eit->second.container->ReadEmbeddedFile(eit->second.relative_path, out_data);
            if (est.ok()) return est;
        }
        return eit->second.container->ReadUserData(
            eit->second.user_data_offset, eit->second.size_bytes, out_data);
    }
    auto xit = xvd_mounts_.find(key);
    if (xit != xvd_mounts_.end()) {
        const auto& hdr = xit->second->Header();
        return xit->second->ReadUserData(0, hdr.user_data_length, out_data);
    }
    std::string host_path;
    Status st =
        ResolvePathUnlocked(mounts_, NormalizeVirtualPath(virtual_path), virtual_path, host_path);
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

Status PackageVfs::ReadFileRange(const std::string& virtual_path,
                                 uint64_t offset,
                                 uint64_t size,
                                 std::vector<uint8_t>& out_data) const {
    std::lock_guard<std::mutex> lk(mutex_);
    out_data.clear();
    if (HasTraversalSegment(virtual_path)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Path traversal ('..') is not permitted in virtual path: " +
                                 virtual_path);
    }

    const std::string key = NormalizeKey(virtual_path);
    auto eit = xvd_embedded_files_.find(key);
    if (eit != xvd_embedded_files_.end()) {
        const EmbeddedFileRef& ref = eit->second;
        if (offset > ref.size_bytes) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "Read offset past end of embedded XVD file: " + virtual_path);
        }
        const uint64_t avail = ref.size_bytes - offset;
        const uint64_t clamped_size = std::min<uint64_t>(size, avail);
        if (clamped_size == 0) {
            return Status::Ok();
        }
        if (!ref.relative_path.empty()) {
            Status est = ref.container->ReadEmbeddedFile(ref.relative_path, offset, clamped_size, out_data);
            if (est.ok()) return est;
        }
        return ref.container->ReadUserData(ref.user_data_offset + offset, clamped_size, out_data);
    }

    auto xit = xvd_mounts_.find(key);
    if (xit != xvd_mounts_.end()) {
        const auto& hdr = xit->second->Header();
        if (offset > hdr.user_data_length) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "Read offset past end of mounted XVD container: " + virtual_path);
        }
        const uint64_t avail = hdr.user_data_length - offset;
        const uint64_t clamped_size = std::min<uint64_t>(size, avail);
        if (clamped_size == 0) {
            return Status::Ok();
        }
        return xit->second->ReadUserData(offset, clamped_size, out_data);
    }

    std::string host_path;
    Status st =
        ResolvePathUnlocked(mounts_, NormalizeVirtualPath(virtual_path), virtual_path, host_path);
    if (!st.ok()) {
        return st;
    }

    std::ifstream ifs(host_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
    }
    const std::streampos end_pos = ifs.tellg();
    if (end_pos < 0) {
        return Status::Error(StatusCode::IoError, "Failed to query mounted file size: " + host_path);
    }
    const uint64_t file_size = static_cast<uint64_t>(end_pos);
    if (offset > file_size) {
        return Status::Error(StatusCode::InvalidArgument, "Read offset past EOF: " + host_path);
    }
    const uint64_t avail = file_size - offset;
    const uint64_t clamped_size = std::min<uint64_t>(size, avail);
    if (clamped_size == 0) {
        return Status::Ok();
    }
    if (clamped_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return Status::Error(StatusCode::OutOfMemory, "Requested read range exceeds host address space");
    }

    ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!ifs) {
        return Status::Error(StatusCode::IoError, "Failed to seek mounted file: " + host_path);
    }
    out_data.resize(static_cast<size_t>(clamped_size));
    ifs.read(reinterpret_cast<char*>(out_data.data()), static_cast<std::streamsize>(clamped_size));
    const size_t got = static_cast<size_t>(ifs.gcount());
    if (got != out_data.size()) {
        out_data.resize(got);
        return Status::Error(StatusCode::IoError, "Short read from mounted file: " + host_path);
    }
    return Status::Ok();
}

bool PackageVfs::GetFileSize(const std::string& virtual_path, uint64_t& out_size) const {
    out_size = 0;
    if (HasTraversalSegment(virtual_path)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    const std::string key = NormalizeKey(virtual_path);
    auto eit = xvd_embedded_files_.find(key);
    if (eit != xvd_embedded_files_.end()) {
        out_size = eit->second.size_bytes;
        return true;
    }
    auto xit = xvd_mounts_.find(key);
    if (xit != xvd_mounts_.end()) {
        out_size = xit->second->Header().user_data_length;
        return true;
    }
    std::string host_path;
    if (!ResolvePathUnlocked(mounts_, NormalizeVirtualPath(virtual_path), virtual_path, host_path)
             .ok()) {
        return false;
    }
    std::error_code ec;
    const uintmax_t sz = std::filesystem::file_size(host_path, ec);
    if (ec) {
        return false;
    }
    out_size = static_cast<uint64_t>(sz);
    return true;
}

bool PackageVfs::Exists(const std::string& virtual_path) const {
    if (HasTraversalSegment(virtual_path)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    const std::string key = NormalizeKey(virtual_path);
    if (xvd_embedded_files_.find(key) != xvd_embedded_files_.end()) {
        return true;
    }
    if (xvd_mounts_.find(key) != xvd_mounts_.end()) {
        return true;
    }
    std::string host_path;
    if (!ResolvePathUnlocked(mounts_, NormalizeVirtualPath(virtual_path), virtual_path, host_path)
             .ok()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(host_path, ec);
}

}  // namespace scarlettx
