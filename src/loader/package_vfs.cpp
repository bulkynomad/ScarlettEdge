#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/xvd_container.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scarlettx {

namespace {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

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

// Strips trailing "\." segments and collapses duplicate slashes cleanly.
std::string CleanNormalizedVirtualPath(const std::string& raw) {
    std::string s = raw;
    std::replace(s.begin(), s.end(), '/', '\\');
    // Strip \\?\ or \\.\ prefix if present
    if (s.size() >= 6 && s[0] == '\\' && s[1] == '\\' &&
        (s[2] == '?' || s[2] == '.') && s[3] == '\\') {
        s.erase(0, 4);
    }
    std::string prefix;
    size_t pos = 0;
    if (s.size() >= 2 && s[1] == ':') {
        prefix = s.substr(0, 2);
        pos = 2;
    }
    std::vector<std::string> parts;
    while (pos < s.size()) {
        while (pos < s.size() && s[pos] == '\\') ++pos;
        const size_t start = pos;
        while (pos < s.size() && s[pos] != '\\') ++pos;
        if (pos > start) {
            const std::string seg = s.substr(start, pos - start);
            if (seg != ".") {
                parts.push_back(seg);
            }
        }
    }
    std::string out = prefix;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!out.empty() || i > 0) out.push_back('\\');
        out += parts[i];
    }
    return out;
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

    const std::string lower = ToLowerAscii(norm_virtual_path);

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
    out_host_path = rel.empty() ? *host_dir : (std::filesystem::path(*host_dir) / rel).string();
    return Status::Ok();
}

void AddDirChildDedup(std::vector<VfsDirEntry>& vec, const VfsDirEntry& item) {
    const std::string want = ToLowerAscii(item.name);
    for (auto& existing : vec) {
        if (ToLowerAscii(existing.name) == want) {
            if (item.is_directory) existing.is_directory = true;
            if (item.size_bytes > existing.size_bytes) existing.size_bytes = item.size_bytes;
            return;
        }
    }
    vec.push_back(item);
}

}  // namespace

PackageVfs::PackageVfs() = default;

PackageVfs::~PackageVfs() {
    std::lock_guard<std::mutex> clk(cache_mu_);
    CloseAllHostHandlesUnlocked();
    ClearBlockCacheUnlocked();
}

PackageVfs& PackageVfs::Global() {
    static PackageVfs instance;
    return instance;
}

void PackageVfs::CloseAllHostHandlesUnlocked() const noexcept {
#if defined(_WIN32)
    for (auto& item : host_handle_lru_) {
        if (item.handle) {
            ::CloseHandle(static_cast<HANDLE>(item.handle));
            item.handle = nullptr;
        }
    }
#endif
    host_handle_map_.clear();
    host_handle_lru_.clear();
}

void PackageVfs::ClearBlockCacheUnlocked() const noexcept {
    block_map_.clear();
    block_lru_.clear();
}

void PackageVfs::Reset() {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    mounts_.clear();
    xvd_mounts_.clear();
    xvd_embedded_files_.clear();
    xvd_directories_.clear();
    xvd_dir_children_.clear();
    std::lock_guard<std::mutex> clk(cache_mu_);
    CloseAllHostHandlesUnlocked();
    ClearBlockCacheUnlocked();
}

std::string PackageVfs::NormalizeVirtualPath(const std::string& path) {
    return CleanNormalizedVirtualPath(path);
}

std::string PackageVfs::NormalizeKey(const std::string& path) {
    std::string key = CleanNormalizedVirtualPath(path);
    while (!key.empty() && key.back() == '\\') {
        key.pop_back();
    }
    return ToLowerAscii(key);
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
    std::unique_lock<std::shared_mutex> lk(mutex_);
    mounts_[NormalizeKey(virtual_root)] = host_dir;
    std::lock_guard<std::mutex> clk(cache_mu_);
    CloseAllHostHandlesUnlocked();
    ClearBlockCacheUnlocked();
    return Status::Ok();
}

void PackageVfs::IndexContainerEntriesUnlocked(
    const std::string& mount_key,
    const std::shared_ptr<XvdContainer>& container) {
    std::vector<XvdEmbeddedFile> embedded;
    const bool has_embedded = container && container->ListEmbeddedFiles(embedded).ok();
    if (!has_embedded) {
        // Raw container file mounted inside a virtual parent directory (e.g. "G:\dlc.xvd")
        const size_t sep = mount_key.find_last_of('\\');
        if (sep != std::string::npos) {
            const std::string parent_key = mount_key.substr(0, sep);
            const std::string leaf_name = mount_key.substr(sep + 1);
            if (!parent_key.empty() && !leaf_name.empty() && container) {
                xvd_directories_.insert(parent_key);
                AddDirChildDedup(
                    xvd_dir_children_[parent_key],
                    VfsDirEntry{leaf_name, container->Header().user_data_length, false});
            }
        }
        return;
    }

    xvd_directories_.insert(mount_key);
    for (const auto& entry : embedded) {
        std::string norm_rel = entry.relative_path;
        std::replace(norm_rel.begin(), norm_rel.end(), '/', '\\');
        while (!norm_rel.empty() && norm_rel.front() == '\\') norm_rel.erase(norm_rel.begin());
        while (!norm_rel.empty() && norm_rel.back() == '\\') norm_rel.pop_back();
        if (norm_rel.empty()) continue;

        // Walk segments to register every intermediate directory and its immediate children.
        std::vector<std::string> segs;
        size_t pos = 0;
        while (pos < norm_rel.size()) {
            const size_t next = norm_rel.find('\\', pos);
            if (next == std::string::npos) {
                segs.push_back(norm_rel.substr(pos));
                break;
            }
            if (next > pos) {
                segs.push_back(norm_rel.substr(pos, next - pos));
            }
            pos = next + 1;
        }
        if (segs.empty()) continue;

        std::string parent_key = mount_key;
        for (size_t i = 0; i + 1 < segs.size(); ++i) {
            const std::string child_dir_key = NormalizeKey(parent_key + "\\" + segs[i]);
            xvd_directories_.insert(child_dir_key);
            AddDirChildDedup(xvd_dir_children_[parent_key], VfsDirEntry{segs[i], 0, true});
            parent_key = child_dir_key;
        }

        const std::string leaf_key = NormalizeKey(parent_key + "\\" + segs.back());
        if (entry.is_directory) {
            xvd_directories_.insert(leaf_key);
            AddDirChildDedup(xvd_dir_children_[parent_key], VfsDirEntry{segs.back(), 0, true});
        } else {
            xvd_embedded_files_[leaf_key] = EmbeddedFileRef{
                container,
                entry.user_data_offset,
                entry.size_bytes,
                entry.relative_path,
            };
            AddDirChildDedup(
                xvd_dir_children_[parent_key],
                VfsDirEntry{segs.back(), entry.size_bytes, false});
        }
    }
}

Status PackageVfs::MountXvdContainer(
    const std::string& virtual_path,
    std::shared_ptr<XvdContainer> container) {
    if (virtual_path.empty() || !container || !container->IsOpen()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid virtual_path or XvdContainer");
    }
    if (HasTraversalSegment(virtual_path)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Invalid traversal in virtual_path: " + virtual_path);
    }
    if (container->Header().is_encrypted && !container->HasVolumeKey()) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Encrypted XVD containers cannot be mounted without a volume key");
    }

    const std::string mount_key = NormalizeKey(virtual_path);
    std::unique_lock<std::shared_mutex> lk(mutex_);
    IndexContainerEntriesUnlocked(mount_key, container);
    xvd_mounts_[mount_key] = std::move(container);
    std::lock_guard<std::mutex> clk(cache_mu_);
    ClearBlockCacheUnlocked();
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
        if (std::filesystem::is_directory(volume_key_hex, ec)) {
            st = container->LoadKeyringDirectory(volume_key_hex);
        } else if (std::filesystem::exists(volume_key_hex, ec)) {
            st = container->LoadVolumeKeyFile(volume_key_hex);
        } else {
            st = container->SetVolumeKeyHex(volume_key_hex);
        }
        if (!st.ok()) {
            return st;
        }
    }
    return MountXvdContainer(virtual_path, std::move(container));
}

Status PackageVfs::ResolvePath(const std::string& virtual_path, std::string& out_host_path) const {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return ResolvePathUnlocked(mounts_, NormalizeVirtualPath(virtual_path), virtual_path,
                               out_host_path);
}

bool PackageVfs::Stat(const std::string& virtual_path, VfsEntryInfo& out_info) const {
    out_info = VfsEntryInfo{};
    if (virtual_path.empty() || HasTraversalSegment(virtual_path)) {
        return false;
    }
    const std::string norm = NormalizeVirtualPath(virtual_path);
    const std::string key = NormalizeKey(virtual_path);

    std::shared_lock<std::shared_mutex> lk(mutex_);
    auto eit = xvd_embedded_files_.find(key);
    if (eit != xvd_embedded_files_.end()) {
        out_info.exists = true;
        out_info.is_directory = false;
        out_info.is_xvd_backed = true;
        out_info.size_bytes = eit->second.size_bytes;
        return true;
    }
    if (xvd_directories_.find(key) != xvd_directories_.end()) {
        out_info.exists = true;
        out_info.is_directory = true;
        out_info.is_xvd_backed = true;
        out_info.size_bytes = 0;
        return true;
    }
    auto xit = xvd_mounts_.find(key);
    if (xit != xvd_mounts_.end()) {
        out_info.exists = true;
        out_info.is_directory = false;
        out_info.is_xvd_backed = true;
        out_info.size_bytes = xit->second->Header().user_data_length;
        return true;
    }

    std::string host_path;
    if (ResolvePathUnlocked(mounts_, norm, virtual_path, host_path).ok()) {
        out_info.host_path = host_path;
        std::error_code ec;
        const auto status = std::filesystem::status(host_path, ec);
        if (!ec && std::filesystem::exists(status)) {
            out_info.exists = true;
            out_info.is_directory = std::filesystem::is_directory(status);
            out_info.is_xvd_backed = false;
            if (!out_info.is_directory) {
                const uintmax_t sz = std::filesystem::file_size(host_path, ec);
                if (!ec) out_info.size_bytes = static_cast<uint64_t>(sz);
            }
            return true;
        }
    }
    return false;
}

Status PackageVfs::ListDirectory(
    const std::string& virtual_dir,
    std::vector<VfsDirEntry>& out_entries) const {
    out_entries.clear();
    if (virtual_dir.empty() || HasTraversalSegment(virtual_dir)) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid virtual directory path");
    }

    const std::string norm = NormalizeVirtualPath(virtual_dir);
    const std::string key = NormalizeKey(virtual_dir);
    bool found_dir = false;

    std::shared_lock<std::shared_mutex> lk(mutex_);
    if (xvd_directories_.find(key) != xvd_directories_.end()) {
        found_dir = true;
        auto dit = xvd_dir_children_.find(key);
        if (dit != xvd_dir_children_.end()) {
            out_entries = dit->second;
        }
    }

    std::string host_dir;
    if (ResolvePathUnlocked(mounts_, norm, virtual_dir, host_dir).ok()) {
        std::error_code ec;
        if (std::filesystem::exists(host_dir, ec) && std::filesystem::is_directory(host_dir, ec)) {
            found_dir = true;
            for (const auto& dent : std::filesystem::directory_iterator(host_dir, ec)) {
                const std::string fname = dent.path().filename().string();
                if (fname.empty()) continue;
                const bool is_dir = dent.is_directory(ec);
                uint64_t sz = 0;
                if (!is_dir) {
                    const uintmax_t fsz = dent.file_size(ec);
                    if (!ec) sz = static_cast<uint64_t>(fsz);
                }
                AddDirChildDedup(out_entries, VfsDirEntry{fname, sz, is_dir});
            }
        }
    }

    if (!found_dir) {
        return Status::Error(StatusCode::NotFound, "Virtual directory not found: " + virtual_dir);
    }
    return Status::Ok();
}

Status PackageVfs::ReadHostRangeCached(
    const std::string& vfs_key,
    const std::string& host_path,
    uint64_t offset,
    uint64_t size,
    void* dst,
    size_t& out_got) const {
    out_got = 0;
#if defined(_WIN32)
    HANDLE dup_or_cached = INVALID_HANDLE_VALUE;
    uint64_t file_size = 0;
    {
        std::lock_guard<std::mutex> clk(cache_mu_);
        auto hit = host_handle_map_.find(vfs_key);
        if (hit != host_handle_map_.end() && hit->second->handle != nullptr) {
            host_handle_lru_.splice(host_handle_lru_.begin(), host_handle_lru_, hit->second);
            file_size = hit->second->size_bytes;
            if (::DuplicateHandle(GetCurrentProcess(),
                                  static_cast<HANDLE>(hit->second->handle),
                                  GetCurrentProcess(),
                                  &dup_or_cached,
                                  0,
                                  FALSE,
                                  DUPLICATE_SAME_ACCESS)) {
                // Held across stateless OVERLAPPED ReadFile outside cache_mu_
            } else {
                dup_or_cached = INVALID_HANDLE_VALUE;
            }
        }
    }

    bool close_after = true;
    if (dup_or_cached == INVALID_HANDLE_VALUE) {
        const std::wstring wpath = std::filesystem::path(host_path).wstring();
        HANDLE opened = ::CreateFileW(
            wpath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (opened == INVALID_HANDLE_VALUE) {
            return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
        }
        LARGE_INTEGER li{};
        if (!::GetFileSizeEx(opened, &li) || li.QuadPart < 0) {
            ::CloseHandle(opened);
            return Status::Error(StatusCode::IoError, "Failed to query mounted file size: " + host_path);
        }
        file_size = static_cast<uint64_t>(li.QuadPart);

        HANDLE cache_copy = INVALID_HANDLE_VALUE;
        if (::DuplicateHandle(GetCurrentProcess(), opened, GetCurrentProcess(),
                              &cache_copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            std::lock_guard<std::mutex> clk(cache_mu_);
            auto existing = host_handle_map_.find(vfs_key);
            if (existing != host_handle_map_.end()) {
                ::CloseHandle(cache_copy);
            } else {
                if (host_handle_lru_.size() >= kMaxCachedHostHandles) {
                    if (host_handle_lru_.back().handle) {
                        ::CloseHandle(static_cast<HANDLE>(host_handle_lru_.back().handle));
                    }
                    host_handle_map_.erase(host_handle_lru_.back().key);
                    host_handle_lru_.pop_back();
                }
                host_handle_lru_.push_front(
                    CachedHostFile{vfs_key, host_path, static_cast<void*>(cache_copy), file_size});
                host_handle_map_[vfs_key] = host_handle_lru_.begin();
            }
        }
        dup_or_cached = opened;
    }

    struct HandleCloser {
        HANDLE h = INVALID_HANDLE_VALUE;
        bool active = true;
        ~HandleCloser() {
            if (active && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        }
    } guard{dup_or_cached, close_after};

    if (offset > file_size) {
        return Status::Error(StatusCode::InvalidArgument, "Read offset past EOF: " + host_path);
    }
    const uint64_t avail = file_size - offset;
    const uint64_t clamped = (std::min)(size, avail);
    if (clamped == 0) {
        return Status::Ok();
    }
    if (clamped > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return Status::Error(StatusCode::OutOfMemory, "Requested read range exceeds host address space");
    }

    auto* out_ptr = static_cast<uint8_t*>(dst);
    // Use bounded 4 KiB LRU sector cache for small reads (<= 64 KiB) to avoid repeated OS syscalls.
    if (clamped <= 65536ull) {
        const uint64_t first_blk = offset / kSectorSize;
        const uint64_t last_blk = (offset + clamped - 1ull) / kSectorSize;
        for (uint64_t blk = first_blk; blk <= last_blk; ++blk) {
            const uint64_t blk_start = blk * kSectorSize;
            const size_t blk_avail = static_cast<size_t>(
                (std::min)(uint64_t{kSectorSize}, file_size - blk_start));
            const uint64_t cp_start = (std::max)(offset, blk_start);
            const uint64_t cp_end = (std::min)(offset + clamped, blk_start + blk_avail);
            const size_t src_off = static_cast<size_t>(cp_start - blk_start);
            const size_t dst_off = static_cast<size_t>(cp_start - offset);
            const size_t cp_len = static_cast<size_t>(cp_end - cp_start);

            CachedBlockKey bkey{vfs_key, blk};
            bool hit = false;
            {
                std::lock_guard<std::mutex> clk(cache_mu_);
                auto bit = block_map_.find(bkey);
                if (bit != block_map_.end() && bit->second->valid_len >= src_off + cp_len) {
                    block_lru_.splice(block_lru_.begin(), block_lru_, bit->second);
                    std::memcpy(out_ptr + dst_off, bit->second->bytes.data() + src_off, cp_len);
                    hit = true;
                }
            }
            if (hit) continue;

            std::array<uint8_t, kSectorSize> sector_buf{};
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(blk_start & 0xFFFFFFFFull);
            ov.OffsetHigh = static_cast<DWORD>((blk_start >> 32) & 0xFFFFFFFFull);
            DWORD got_dw = 0;
            if (!::ReadFile(dup_or_cached, sector_buf.data(), static_cast<DWORD>(blk_avail), &got_dw, &ov) ||
                got_dw != blk_avail) {
                return Status::Error(StatusCode::IoError, "Short read from mounted file: " + host_path);
            }
            std::memcpy(out_ptr + dst_off, sector_buf.data() + src_off, cp_len);

            {
                std::lock_guard<std::mutex> clk(cache_mu_);
                if (block_map_.find(bkey) == block_map_.end()) {
                    if (block_lru_.size() >= kMaxCachedBlocks) {
                        block_map_.erase(block_lru_.back().key);
                        block_lru_.pop_back();
                    }
                    CachedBlock cb{};
                    cb.key = std::move(bkey);
                    cb.valid_len = blk_avail;
                    cb.bytes = sector_buf;
                    block_lru_.push_front(std::move(cb));
                    block_map_[block_lru_.front().key] = block_lru_.begin();
                }
            }
        }
        out_got = static_cast<size_t>(clamped);
        return Status::Ok();
    }

    constexpr size_t kMaxChunk = 0x40000000u;
    size_t remaining = static_cast<size_t>(clamped);
    uint64_t cur_off = offset;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min)(remaining, kMaxChunk));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(cur_off & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>((cur_off >> 32) & 0xFFFFFFFFull);
        DWORD got_dw = 0;
        if (!::ReadFile(dup_or_cached, out_ptr, chunk, &got_dw, &ov) || got_dw != chunk) {
            return Status::Error(StatusCode::IoError, "Short read from mounted file: " + host_path);
        }
        out_ptr += chunk;
        cur_off += chunk;
        remaining -= chunk;
    }
    out_got = static_cast<size_t>(clamped);
    return Status::Ok();
#else
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
    const uint64_t clamped = (std::min)(size, file_size - offset);
    if (clamped == 0) return Status::Ok();
    ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    ifs.read(static_cast<char*>(dst), static_cast<std::streamsize>(clamped));
    out_got = static_cast<size_t>(ifs.gcount());
    if (out_got != clamped) {
        return Status::Error(StatusCode::IoError, "Short read from mounted file: " + host_path);
    }
    return Status::Ok();
#endif
}

Status PackageVfs::ReadFileBytes(const std::string& virtual_path,
                                 std::vector<uint8_t>& out_data) const {
    if (HasTraversalSegment(virtual_path)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Path traversal ('..') is not permitted in virtual path: " +
                                 virtual_path);
    }

    const std::string norm = NormalizeVirtualPath(virtual_path);
    const std::string key = NormalizeKey(virtual_path);
    EmbeddedFileRef embedded_ref{};
    bool has_embedded = false;
    std::shared_ptr<XvdContainer> raw_xvd;
    std::string host_path;

    {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        auto eit = xvd_embedded_files_.find(key);
        if (eit != xvd_embedded_files_.end()) {
            embedded_ref = eit->second;
            has_embedded = true;
        } else {
            auto xit = xvd_mounts_.find(key);
            if (xit != xvd_mounts_.end()) {
                raw_xvd = xit->second;
            } else {
                Status st = ResolvePathUnlocked(mounts_, norm, virtual_path, host_path);
                if (!st.ok()) {
                    return st;
                }
            }
        }
    }

    if (has_embedded) {
        if (!embedded_ref.relative_path.empty()) {
            Status est = embedded_ref.container->ReadEmbeddedFile(embedded_ref.relative_path, out_data);
            if (est.ok()) return est;
        }
        return embedded_ref.container->ReadUserData(
            embedded_ref.user_data_offset, embedded_ref.size_bytes, out_data);
    }
    if (raw_xvd) {
        const auto& hdr = raw_xvd->Header();
        return raw_xvd->ReadUserData(0, hdr.user_data_length, out_data);
    }

    std::error_code ec;
    const uintmax_t fsz = std::filesystem::file_size(host_path, ec);
    if (ec) {
        return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
    }
    out_data.resize(static_cast<size_t>(fsz));
    if (fsz == 0) {
        return Status::Ok();
    }
    size_t got = 0;
    Status st = ReadHostRangeCached(key, host_path, 0, static_cast<uint64_t>(fsz), out_data.data(), got);
    out_data.resize(got);
    return st;
}

Status PackageVfs::ReadFileRange(const std::string& virtual_path,
                                 uint64_t offset,
                                 uint64_t size,
                                 std::vector<uint8_t>& out_data) const {
    out_data.clear();
    if (HasTraversalSegment(virtual_path)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Path traversal ('..') is not permitted in virtual path: " +
                                 virtual_path);
    }

    const std::string norm = NormalizeVirtualPath(virtual_path);
    const std::string key = NormalizeKey(virtual_path);
    EmbeddedFileRef embedded_ref{};
    bool has_embedded = false;
    std::shared_ptr<XvdContainer> raw_xvd;
    std::string host_path;

    {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        auto eit = xvd_embedded_files_.find(key);
        if (eit != xvd_embedded_files_.end()) {
            embedded_ref = eit->second;
            has_embedded = true;
        } else {
            auto xit = xvd_mounts_.find(key);
            if (xit != xvd_mounts_.end()) {
                raw_xvd = xit->second;
            } else {
                Status st = ResolvePathUnlocked(mounts_, norm, virtual_path, host_path);
                if (!st.ok()) {
                    return st;
                }
            }
        }
    }

    if (has_embedded) {
        if (offset > embedded_ref.size_bytes) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "Read offset past end of embedded XVD file: " + virtual_path);
        }
        const uint64_t avail = embedded_ref.size_bytes - offset;
        const uint64_t clamped_size = (std::min)(size, avail);
        if (clamped_size == 0) {
            return Status::Ok();
        }
        if (!embedded_ref.relative_path.empty()) {
            Status est = embedded_ref.container->ReadEmbeddedFile(
                embedded_ref.relative_path, offset, clamped_size, out_data);
            if (est.ok()) return est;
        }
        return embedded_ref.container->ReadUserData(
            embedded_ref.user_data_offset + offset, clamped_size, out_data);
    }

    if (raw_xvd) {
        const auto& hdr = raw_xvd->Header();
        if (offset > hdr.user_data_length) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "Read offset past end of mounted XVD container: " + virtual_path);
        }
        const uint64_t avail = hdr.user_data_length - offset;
        const uint64_t clamped_size = (std::min)(size, avail);
        if (clamped_size == 0) {
            return Status::Ok();
        }
        return raw_xvd->ReadUserData(offset, clamped_size, out_data);
    }

    std::error_code ec;
    const uintmax_t fsz = std::filesystem::file_size(host_path, ec);
    if (ec) {
        return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
    }
    const uint64_t file_size = static_cast<uint64_t>(fsz);
    if (offset > file_size) {
        return Status::Error(StatusCode::InvalidArgument, "Read offset past EOF: " + host_path);
    }
    const uint64_t clamped_size = (std::min)(size, file_size - offset);
    if (clamped_size == 0) {
        return Status::Ok();
    }
    if (clamped_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return Status::Error(StatusCode::OutOfMemory, "Requested read range exceeds host address space");
    }
    out_data.resize(static_cast<size_t>(clamped_size));
    size_t got = 0;
    Status st = ReadHostRangeCached(key, host_path, offset, clamped_size, out_data.data(), got);
    out_data.resize(got);
    return st;
}

Status PackageVfs::ReadFileRangeToBuffer(const std::string& virtual_path,
                                         uint64_t offset,
                                         size_t size,
                                         void* dst,
                                         size_t& out_bytes_read) const {
    out_bytes_read = 0;
    if (size == 0) {
        return Status::Ok();
    }
    if (!dst) {
        return Status::Error(StatusCode::InvalidArgument, "Null destination buffer in ReadFileRangeToBuffer");
    }
    std::vector<uint8_t> tmp;
    Status st = ReadFileRange(virtual_path, offset, static_cast<uint64_t>(size), tmp);
    if (!st.ok()) {
        return st;
    }
    if (!tmp.empty()) {
        std::memcpy(dst, tmp.data(), tmp.size());
    }
    out_bytes_read = tmp.size();
    return Status::Ok();
}

bool PackageVfs::GetFileSize(const std::string& virtual_path, uint64_t& out_size) const {
    out_size = 0;
    VfsEntryInfo info{};
    if (!Stat(virtual_path, info) || info.is_directory) {
        return false;
    }
    out_size = info.size_bytes;
    return true;
}

bool PackageVfs::Exists(const std::string& virtual_path) const {
    VfsEntryInfo info{};
    return Stat(virtual_path, info) && info.exists;
}

}  // namespace scarlettx
