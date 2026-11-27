#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/loader/aes_xts_cng.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace scarlettx {

namespace {

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) {
    return static_cast<uint64_t>(ReadU32LE(p)) |
           (static_cast<uint64_t>(ReadU32LE(p + 4)) << 32);
}

void WriteU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void WriteU64LE(uint8_t* p, uint64_t v) {
    WriteU32LE(p, static_cast<uint32_t>(v & 0xFFFFFFFFull));
    WriteU32LE(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
}

// Public XVD header field offsets (relative to start of file; magic at 0x200).
constexpr size_t kOffVolumeFlags = 0x208;
constexpr size_t kOffFormatVersion = 0x20C;
constexpr size_t kOffDriveSize = 0x218;
constexpr size_t kOffContentId = 0x220;
constexpr size_t kOffContentType = 0x240;
constexpr size_t kOffEmbeddedXvdLen = 0x244;
constexpr size_t kOffUserDataOffset = 0x250;
constexpr size_t kOffUserDataLength = 0x258;

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

}  // namespace

Status XvdContainer::SetVolumeKey(const uint8_t* key, size_t key_len) {
    if (!key || key_len < 16) {
        return Status::Error(StatusCode::InvalidArgument, "Volume key must be at least 16 bytes");
    }
    volume_key_.fill(0);
    const size_t copy_len = std::min<size_t>(key_len, volume_key_.size());
    std::memcpy(volume_key_.data(), key, copy_len);
    volume_key_len_ = static_cast<uint32_t>(copy_len);
    auto prep = std::make_shared<crypto::AesXtsPreparedKey>();
    if (!prep->Init(volume_key_.data(), copy_len)) {
        return Status::Error(StatusCode::InvalidArgument, "Failed to initialize XVD AES-XTS key");
    }
    prepared_key_ = std::move(prep);
    has_volume_key_ = true;
    return Status::Ok();
}

Status XvdContainer::SetVolumeKeyHex(const std::string& hex_key) {
    std::vector<uint8_t> bytes;
    bytes.reserve(64);
    size_t i = 0;
    if (hex_key.size() >= 2 && hex_key[0] == '0' && (hex_key[1] == 'x' || hex_key[1] == 'X')) {
        i = 2;
    }
    int high = -1;
    for (; i < hex_key.size(); ++i) {
        const char c = hex_key[i];
        if (c == ' ' || c == '-' || c == ':' || c == '\r' || c == '\n' || c == '\t') {
            continue;
        }
        const int nib = HexNibble(c);
        if (nib < 0) {
            return Status::Error(StatusCode::InvalidArgument, "Invalid hex character in XVD volume key");
        }
        if (high < 0) {
            high = nib;
        } else {
            bytes.push_back(static_cast<uint8_t>((high << 4) | nib));
            high = -1;
        }
    }
    if (high >= 0 || bytes.size() < 16 || bytes.size() > 64) {
        return Status::Error(StatusCode::InvalidArgument, "XVD hex volume key must decode to 16..64 bytes");
    }
    return SetVolumeKey(bytes.data(), bytes.size());
}

Status XvdContainer::LoadVolumeKeyFile(const std::string& key_file_path) {
    std::ifstream ifs(key_file_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Failed to open XVD volume key file: " + key_file_path);
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (raw.size() == 16 || raw.size() == 32 || raw.size() == 64) {
        return SetVolumeKey(raw.data(), raw.size());
    }
    // Fall back to parsing hex text inside the file (e.g. 32, 64, or 128 hex chars + optional newline).
    std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    return SetVolumeKeyHex(text);
}

Status XvdContainer::ParseHeaderFromBytes(const uint8_t* data, size_t size) {
    header_ = {};
    if (!data || size < kMinHeaderSize) {
        return Status::Error(StatusCode::UnsupportedFormat, "XVD image smaller than 4 KiB header");
    }

    if (std::memcmp(data + kMagicOffset, kMagic, 8) != 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Missing 'msft-xvd' magic at offset 0x200");
    }

    header_.magic.assign(reinterpret_cast<const char*>(data + kMagicOffset), 8);
    header_.volume_flags = ReadU32LE(data + kOffVolumeFlags);
    header_.format_version = ReadU32LE(data + kOffFormatVersion);
    header_.drive_size = ReadU64LE(data + kOffDriveSize);
    std::memcpy(header_.content_id.data(), data + kOffContentId, 16);
    header_.content_type = ReadU32LE(data + kOffContentType);
    header_.embedded_xvd_length = ReadU32LE(data + kOffEmbeddedXvdLen);
    header_.user_data_offset = ReadU64LE(data + kOffUserDataOffset);
    header_.user_data_length = ReadU64LE(data + kOffUserDataLength);

    if (header_.user_data_offset < kMinHeaderSize) {
        header_.user_data_offset = kMinHeaderSize;
    }
    if (header_.user_data_length == 0 && header_.drive_size > header_.user_data_offset) {
        header_.user_data_length = header_.drive_size - header_.user_data_offset;
    }

    header_.is_encrypted = (header_.volume_flags & kFlagEncryptionDisabled) == 0;
    header_.has_data_integrity = (header_.volume_flags & kFlagDataIntegrityDisabled) == 0;
    header_.valid = true;
    return Status::Ok();
}

Status XvdContainer::OpenFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Failed to open XVD file: " + path);
    }

    std::vector<uint8_t> hdr(kMinHeaderSize);
    ifs.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
    if (static_cast<size_t>(ifs.gcount()) < kMinHeaderSize) {
        return Status::Error(StatusCode::UnsupportedFormat, "Truncated XVD header: " + path);
    }

    Status st = ParseHeaderFromBytes(hdr.data(), hdr.size());
    if (!st.ok()) {
        return st;
    }

    path_ = path;
    backing_bytes_.clear();
    memory_backed_ = false;
    return Status::Ok();
}

Status XvdContainer::OpenBytes(std::vector<uint8_t> data) {
    Status st = ParseHeaderFromBytes(data.data(), data.size());
    if (!st.ok()) {
        return st;
    }
    path_.clear();
    backing_bytes_ = std::move(data);
    memory_backed_ = true;
    return Status::Ok();
}

Status XvdContainer::ReadUserData(
    uint64_t offset,
    uint64_t size,
    std::vector<uint8_t>& out_data) const {
    if (!header_.valid) {
        return Status::Error(StatusCode::InvalidArgument, "XVD container is not open");
    }
    if (header_.is_encrypted && !has_volume_key_) {
        return Status::Error(
            StatusCode::NotImplemented,
            "Encrypted retail XVD images require a volume key (supply --volume-key or call SetVolumeKey)");
    }
    if (offset > header_.user_data_length || size > header_.user_data_length - offset) {
        return Status::Error(StatusCode::InvalidArgument, "XVD read out of user-data bounds");
    }

    out_data.resize(static_cast<size_t>(size));
    if (size == 0) {
        return Status::Ok();
    }

    auto read_raw = [&](uint64_t rel_offset, uint8_t* dst, size_t len) -> Status {
        const uint64_t abs_offset = header_.user_data_offset + rel_offset;
        if (memory_backed_) {
            if (abs_offset + len > backing_bytes_.size()) {
                return Status::Error(StatusCode::IoError, "In-memory XVD buffer truncated");
            }
            std::memcpy(dst, backing_bytes_.data() + abs_offset, len);
            return Status::Ok();
        }
        std::ifstream ifs(path_, std::ios::binary);
        if (!ifs) {
            return Status::Error(StatusCode::IoError, "Failed to reopen XVD file: " + path_);
        }
        ifs.seekg(static_cast<std::streamoff>(abs_offset), std::ios::beg);
        ifs.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
        if (static_cast<size_t>(ifs.gcount()) != len) {
            return Status::Error(StatusCode::IoError, "Short read from XVD file: " + path_);
        }
        return Status::Ok();
    };

    if (!header_.is_encrypted) {
        return read_raw(offset, out_data.data(), static_cast<size_t>(size));
    }

    // Encrypted XVD: decrypt sector-by-sector (4 KiB sectors) across arbitrary unaligned ranges.
    const uint64_t first_sector = offset / kSectorSize;
    const uint64_t last_sector = (offset + size - 1) / kSectorSize;
    std::array<uint8_t, kSectorSize> cipher_sector{};
    std::array<uint8_t, kSectorSize> plain_sector{};

    for (uint64_t sector = first_sector; sector <= last_sector; ++sector) {
        const uint64_t sector_start = sector * kSectorSize;
        const size_t sector_avail = static_cast<size_t>(
            std::min<uint64_t>(kSectorSize, header_.user_data_length - sector_start));
        Status st = read_raw(sector_start, cipher_sector.data(), sector_avail);
        if (!st.ok()) {
            return st;
        }
        if (prepared_key_) {
            prepared_key_->TransformSector(false, sector, cipher_sector.data(), plain_sector.data(), sector_avail);
        } else {
            crypto::AesXtsTransformSector(
                false, volume_key_.data(), volume_key_len_ >= 16 ? volume_key_len_ : 32,
                sector, cipher_sector.data(), plain_sector.data(), sector_avail);
        }

        const uint64_t copy_start = std::max<uint64_t>(offset, sector_start);
        const uint64_t copy_end = std::min<uint64_t>(offset + size, sector_start + sector_avail);
        const size_t src_off = static_cast<size_t>(copy_start - sector_start);
        const size_t dst_off = static_cast<size_t>(copy_start - offset);
        const size_t copy_len = static_cast<size_t>(copy_end - copy_start);
        std::memcpy(out_data.data() + dst_off, plain_sector.data() + src_off, copy_len);
    }
    return Status::Ok();
}

std::vector<uint8_t> XvdContainer::BuildSyntheticXvd(
    const std::vector<uint8_t>& user_data,
    uint32_t content_type,
    bool mark_encrypted) {
    const uint64_t total_size = kMinHeaderSize + user_data.size();
    std::vector<uint8_t> img(static_cast<size_t>(total_size), 0);

    std::memcpy(img.data() + kMagicOffset, kMagic, 8);
    uint32_t flags = kFlagReadOnly | kFlagDataIntegrityDisabled;
    if (!mark_encrypted) {
        flags |= kFlagEncryptionDisabled;
    }
    WriteU32LE(img.data() + kOffVolumeFlags, flags);
    WriteU32LE(img.data() + kOffFormatVersion, 3);
    WriteU64LE(img.data() + kOffDriveSize, total_size);
    for (size_t i = 0; i < 16; ++i) {
        img[kOffContentId + i] = static_cast<uint8_t>(0xA0u + i);
    }
    WriteU32LE(img.data() + kOffContentType, content_type);
    WriteU32LE(img.data() + kOffEmbeddedXvdLen, 0);
    WriteU64LE(img.data() + kOffUserDataOffset, kMinHeaderSize);
    WriteU64LE(img.data() + kOffUserDataLength, user_data.size());

    if (!user_data.empty()) {
        std::memcpy(img.data() + kMinHeaderSize, user_data.data(), user_data.size());
    }
    return img;
}

std::vector<uint8_t> XvdContainer::BuildEncryptedSyntheticXvd(
    const std::vector<uint8_t>& user_data,
    const uint8_t* volume_key,
    size_t key_len,
    uint32_t content_type) {
    std::array<uint8_t, 64> key{};
    const size_t effective_len = (volume_key && key_len >= 16) ? std::min<size_t>(key_len, key.size()) : 32;
    if (volume_key && key_len > 0) {
        std::memcpy(key.data(), volume_key, std::min<size_t>(key_len, key.size()));
    }
    crypto::AesXtsPreparedKey prep;
    prep.Init(key.data(), effective_len);
    std::vector<uint8_t> encrypted(user_data.size());
    const uint64_t num_sectors = (user_data.size() + kSectorSize - 1) / kSectorSize;
    for (uint64_t s = 0; s < num_sectors; ++s) {
        const size_t start = static_cast<size_t>(s * kSectorSize);
        const size_t len = std::min<size_t>(kSectorSize, user_data.size() - start);
        prep.TransformSector(true, s, user_data.data() + start, encrypted.data() + start, len);
    }
    return BuildSyntheticXvd(encrypted, content_type, true);
}

namespace {

uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

void WriteU16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool SplitSafeRelativePath(const std::string& raw, std::vector<std::string>& out_parts) {
    out_parts.clear();
    std::string cur;
    auto flush_part = [&]() -> bool {
        if (cur.empty()) return true;
        if (cur == "." || cur == "..") return false;
        for (unsigned char ch : cur) {
            if (ch < 0x20u || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
                return false;
            }
        }
        out_parts.push_back(cur);
        cur.clear();
        return true;
    };
    for (char c : raw) {
        if (c == '/' || c == '\\') {
            if (!flush_part()) return false;
        } else {
            cur.push_back(c);
        }
    }
    if (!flush_part()) return false;
    return !out_parts.empty();
}

std::string NormalizeRelativeLookupKey(const std::string& path) {
    std::vector<std::string> parts;
    if (!SplitSafeRelativePath(path, parts)) {
        return {};
    }
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) joined.push_back('/');
        joined += ToLowerAscii(parts[i]);
    }
    return joined;
}

struct GdfxBuildNode {
    std::string name;
    bool is_dir = false;
    std::vector<uint8_t> file_data;
    std::vector<std::unique_ptr<GdfxBuildNode>> children;
    uint32_t assigned_sector = 0;
    uint32_t assigned_size = 0;
    std::vector<uint8_t> encoded_dir_bytes;
};

void BuildPreorderBstOrder(size_t lo, size_t hi, std::vector<size_t>& out_order,
                           std::vector<int>& out_left, std::vector<int>& out_right) {
    if (lo > hi) return;
    const size_t mid = lo + (hi - lo) / 2;
    const size_t cur_slot = out_order.size();
    out_order.push_back(mid);
    out_left.push_back(-1);
    out_right.push_back(-1);
    if (mid > lo) {
        out_left[cur_slot] = static_cast<int>(out_order.size());
        BuildPreorderBstOrder(lo, mid - 1, out_order, out_left, out_right);
    }
    if (mid < hi) {
        out_right[cur_slot] = static_cast<int>(out_order.size());
        BuildPreorderBstOrder(mid + 1, hi, out_order, out_left, out_right);
    }
}

void AssignAndEncodeGdfxDirectories(GdfxBuildNode& dir, uint32_t& next_sector) {
    std::sort(dir.children.begin(), dir.children.end(),
              [](const std::unique_ptr<GdfxBuildNode>& a, const std::unique_ptr<GdfxBuildNode>& b) {
                  return ToLowerAscii(a->name) < ToLowerAscii(b->name);
              });
    for (auto& child : dir.children) {
        if (child->is_dir) {
            AssignAndEncodeGdfxDirectories(*child, next_sector);
        }
    }

    if (dir.children.empty()) {
        dir.encoded_dir_bytes.assign(XvdContainer::kGdfxSectorSize, 0xFFu);
        dir.assigned_sector = next_sector;
        dir.assigned_size = static_cast<uint32_t>(dir.encoded_dir_bytes.size());
        next_sector += 1;
        return;
    }

    std::vector<size_t> order;
    std::vector<int> left_slot;
    std::vector<int> right_slot;
    BuildPreorderBstOrder(0, dir.children.size() - 1, order, left_slot, right_slot);

    std::vector<size_t> slot_byte_offset(order.size(), 0);
    size_t cursor = 0;
    for (size_t s = 0; s < order.size(); ++s) {
        const auto& child = *dir.children[order[s]];
        const size_t name_len = std::min<size_t>(child.name.size(), 255u);
        const size_t rec_bytes = (14u + name_len + 3u) & ~size_t(3u);
        if ((cursor / XvdContainer::kGdfxSectorSize) != ((cursor + rec_bytes - 1u) / XvdContainer::kGdfxSectorSize)) {
            cursor = ((cursor / XvdContainer::kGdfxSectorSize) + 1u) * XvdContainer::kGdfxSectorSize;
        }
        slot_byte_offset[s] = cursor;
        cursor += rec_bytes;
    }

    const size_t total_sectors = std::max<size_t>(1u, (cursor + XvdContainer::kGdfxSectorSize - 1u) / XvdContainer::kGdfxSectorSize);
    dir.encoded_dir_bytes.assign(total_sectors * XvdContainer::kGdfxSectorSize, 0xFFu);

    for (size_t s = 0; s < order.size(); ++s) {
        const auto& child = *dir.children[order[s]];
        const size_t off = slot_byte_offset[s];
        const uint16_t left_dw = (left_slot[s] >= 0)
                                     ? static_cast<uint16_t>(slot_byte_offset[static_cast<size_t>(left_slot[s])] / 4u)
                                     : 0xFFFFu;
        const uint16_t right_dw = (right_slot[s] >= 0)
                                      ? static_cast<uint16_t>(slot_byte_offset[static_cast<size_t>(right_slot[s])] / 4u)
                                      : 0xFFFFu;
        const uint8_t name_len = static_cast<uint8_t>(std::min<size_t>(child.name.size(), 255u));
        uint8_t* p = dir.encoded_dir_bytes.data() + off;
        WriteU16LE(p + 0, left_dw);
        WriteU16LE(p + 2, right_dw);
        WriteU32LE(p + 4, child.assigned_sector);
        WriteU32LE(p + 8, child.assigned_size);
        p[12] = child.is_dir ? 0x10u : 0x80u;
        p[13] = name_len;
        std::memcpy(p + 14, child.name.data(), name_len);
    }

    dir.assigned_sector = next_sector;
    dir.assigned_size = static_cast<uint32_t>(dir.encoded_dir_bytes.size());
    next_sector += static_cast<uint32_t>(total_sectors);
}

void WriteGdfxNodePayloads(const GdfxBuildNode& node, std::vector<uint8_t>& volume) {
    if (node.is_dir) {
        if (!node.encoded_dir_bytes.empty()) {
            const size_t dst = static_cast<size_t>(node.assigned_sector) * XvdContainer::kGdfxSectorSize;
            std::memcpy(volume.data() + dst, node.encoded_dir_bytes.data(), node.encoded_dir_bytes.size());
        }
        for (const auto& child : node.children) {
            WriteGdfxNodePayloads(*child, volume);
        }
    } else if (!node.file_data.empty()) {
        const size_t dst = static_cast<size_t>(node.assigned_sector) * XvdContainer::kGdfxSectorSize;
        std::memcpy(volume.data() + dst, node.file_data.data(), node.file_data.size());
    }
}

}  // namespace

bool XvdContainer::HasEmbeddedFilesystem() const {
    std::vector<XvdEmbeddedFile> files;
    return ListEmbeddedFiles(files).ok();
}

Status XvdContainer::ListEmbeddedFiles(std::vector<XvdEmbeddedFile>& out_files) const {
    out_files.clear();
    if (!header_.valid) {
        return Status::Error(StatusCode::InvalidArgument, "XVD container is not open");
    }

    uint64_t vol_base = 0;
    std::vector<uint8_t> desc;
    bool found_magic = false;
    for (uint64_t candidate : {kGdfxDescriptorOffset, uint64_t{0}}) {
        if (header_.user_data_length >= candidate + kGdfxSectorSize) {
            Status st = ReadUserData(candidate, kGdfxSectorSize, desc);
            if (!st.ok()) {
                return st;
            }
            if (std::memcmp(desc.data(), kGdfxMagic, 20) == 0 &&
                std::memcmp(desc.data() + 2028, kGdfxMagic, 20) == 0) {
                found_magic = true;
                // Sector indices in GDFX are relative to the start of the user-data partition (0) when descriptor is at 0x10000.
                vol_base = 0;
                break;
            }
        }
    }
    if (!found_magic) {
        return Status::Error(StatusCode::UnsupportedFormat, "No embedded GDFX volume descriptor found in XVD user-data");
    }

    const uint32_t root_sector = ReadU32LE(desc.data() + 20);
    const uint32_t root_size = ReadU32LE(desc.data() + 24);
    if (root_size == 0 || root_size > 64u * 1024u * 1024u) {
        return Status::Error(StatusCode::ParseError, "Invalid GDFX root directory size");
    }

    struct DirWorkItem {
        uint32_t sector;
        uint32_t size;
        std::string prefix;
        uint32_t depth;
    };
    std::unordered_set<uint32_t> visited_dir_sectors;
    std::vector<DirWorkItem> dir_stack;
    dir_stack.push_back({root_sector, root_size, "", 0});

    while (!dir_stack.empty()) {
        const DirWorkItem cur_dir = std::move(dir_stack.back());
        dir_stack.pop_back();

        if (cur_dir.depth > 32u) {
            return Status::Error(StatusCode::ParseError, "GDFX directory depth limit exceeded");
        }
        if (!visited_dir_sectors.insert(cur_dir.sector).second) {
            return Status::Error(StatusCode::ParseError, "Cycle detected in GDFX directory sectors");
        }

        const uint64_t dir_byte_off = vol_base + static_cast<uint64_t>(cur_dir.sector) * kGdfxSectorSize;
        if (dir_byte_off > header_.user_data_length || cur_dir.size > header_.user_data_length - dir_byte_off) {
            return Status::Error(StatusCode::ParseError, "GDFX directory out of XVD user-data bounds");
        }

        std::vector<uint8_t> dir_bytes;
        Status st = ReadUserData(dir_byte_off, cur_dir.size, dir_bytes);
        if (!st.ok()) {
            return st;
        }
        if (dir_bytes.size() < 14u) {
            continue;
        }
        // Empty root directory padded with 0xFF
        if (ReadU16LE(dir_bytes.data()) == 0xFFFFu && ReadU16LE(dir_bytes.data() + 2) == 0xFFFFu &&
            dir_bytes[12] == 0xFFu && dir_bytes[13] == 0xFFu) {
            continue;
        }

        std::unordered_set<uint32_t> visited_dwords;
        std::vector<uint32_t> node_stack = {0u};
        while (!node_stack.empty()) {
            const uint32_t dw_off = node_stack.back();
            node_stack.pop_back();
            if (dw_off == 0xFFFFu) continue;
            if (!visited_dwords.insert(dw_off).second) {
                return Status::Error(StatusCode::ParseError, "Cycle detected in GDFX directory binary tree");
            }
            const size_t byte_off = static_cast<size_t>(dw_off) * 4u;
            if (byte_off + 14u > dir_bytes.size()) {
                return Status::Error(StatusCode::ParseError, "GDFX directory entry header truncated");
            }

            const uint16_t left_dw = ReadU16LE(dir_bytes.data() + byte_off + 0);
            const uint16_t right_dw = ReadU16LE(dir_bytes.data() + byte_off + 2);
            const uint32_t start_sector = ReadU32LE(dir_bytes.data() + byte_off + 4);
            const uint32_t entry_size = ReadU32LE(dir_bytes.data() + byte_off + 8);
            const uint8_t attr = dir_bytes[byte_off + 12];
            const uint8_t name_len = dir_bytes[byte_off + 13];
            if (name_len == 0 || name_len == 0xFFu) {
                continue;
            }
            const size_t rec_end = (byte_off + 14u + name_len + 3u) & ~size_t(3u);
            if (byte_off + 14u + name_len > dir_bytes.size() || rec_end > dir_bytes.size()) {
                return Status::Error(StatusCode::ParseError, "GDFX directory entry filename exceeds table bounds");
            }

            const std::string raw_name(reinterpret_cast<const char*>(dir_bytes.data() + byte_off + 14), name_len);
            std::vector<std::string> name_parts;
            if (!SplitSafeRelativePath(raw_name, name_parts) || name_parts.size() != 1u) {
                return Status::Error(StatusCode::ParseError, "Unsafe filename in GDFX directory entry");
            }

            const std::string rel_path = cur_dir.prefix.empty() ? name_parts[0] : (cur_dir.prefix + "/" + name_parts[0]);
            const uint64_t data_off = vol_base + static_cast<uint64_t>(start_sector) * kGdfxSectorSize;
            if (entry_size > 0 &&
                (data_off > header_.user_data_length || entry_size > header_.user_data_length - data_off)) {
                return Status::Error(StatusCode::ParseError, "GDFX file payload out of XVD user-data bounds");
            }

            const bool is_dir = (attr & 0x10u) != 0;
            out_files.push_back(XvdEmbeddedFile{
                rel_path,
                data_off,
                entry_size,
                is_dir,
            });

            if (is_dir && entry_size > 0) {
                dir_stack.push_back({start_sector, entry_size, rel_path, cur_dir.depth + 1u});
            }
            if (right_dw != 0xFFFFu) node_stack.push_back(right_dw);
            if (left_dw != 0xFFFFu) node_stack.push_back(left_dw);
        }
    }

    return Status::Ok();
}

Status XvdContainer::ReadEmbeddedFile(const std::string& relative_path, std::vector<uint8_t>& out_data) const {
    const std::string target_key = NormalizeRelativeLookupKey(relative_path);
    if (target_key.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid embedded file path: " + relative_path);
    }
    std::vector<XvdEmbeddedFile> entries;
    Status st = ListEmbeddedFiles(entries);
    if (!st.ok()) {
        return st;
    }
    for (const auto& entry : entries) {
        if (!entry.is_directory && NormalizeRelativeLookupKey(entry.relative_path) == target_key) {
            return ReadUserData(entry.user_data_offset, entry.size_bytes, out_data);
        }
    }
    return Status::Error(StatusCode::NotFound, "Embedded GDFX file not found: " + relative_path);
}

Status XvdContainer::ExtractEmbeddedFilesystem(const std::string& dest_dir) const {
    if (dest_dir.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Destination directory is empty");
    }
    std::vector<XvdEmbeddedFile> entries;
    Status st = ListEmbeddedFiles(entries);
    if (!st.ok()) {
        return st;
    }
    const std::filesystem::path root = std::filesystem::absolute(dest_dir);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return Status::Error(StatusCode::IoError, "Failed to create extraction directory: " + dest_dir);
    }

    for (const auto& entry : entries) {
        std::vector<std::string> parts;
        if (!SplitSafeRelativePath(entry.relative_path, parts)) {
            return Status::Error(StatusCode::ParseError, "Unsafe embedded relative path rejected");
        }
        std::filesystem::path full_path = root;
        for (const auto& part : parts) {
            full_path /= part;
        }
        if (entry.is_directory) {
            std::filesystem::create_directories(full_path, ec);
            continue;
        }
        std::filesystem::create_directories(full_path.parent_path(), ec);
        std::vector<uint8_t> bytes;
        st = ReadUserData(entry.user_data_offset, entry.size_bytes, bytes);
        if (!st.ok()) {
            return st;
        }
        std::ofstream ofs(full_path, std::ios::binary);
        if (!ofs) {
            return Status::Error(StatusCode::IoError, "Failed to write extracted file: " + full_path.string());
        }
        if (!bytes.empty()) {
            ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
    }
    return Status::Ok();
}

std::vector<uint8_t> XvdContainer::BuildGdfxVolumeBytes(const std::vector<XvdInputFile>& files) {
    GdfxBuildNode root;
    root.is_dir = true;

    for (const auto& f : files) {
        std::vector<std::string> parts;
        if (!SplitSafeRelativePath(f.relative_path, parts)) {
            continue;
        }
        GdfxBuildNode* cur = &root;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            GdfxBuildNode* found = nullptr;
            for (auto& ch : cur->children) {
                if (ch->is_dir && ToLowerAscii(ch->name) == ToLowerAscii(parts[i])) {
                    found = ch.get();
                    break;
                }
            }
            if (!found) {
                auto sub = std::make_unique<GdfxBuildNode>();
                sub->name = parts[i];
                sub->is_dir = true;
                found = sub.get();
                cur->children.push_back(std::move(sub));
            }
            cur = found;
        }
        auto leaf = std::make_unique<GdfxBuildNode>();
        leaf->name = parts.back();
        leaf->is_dir = false;
        leaf->file_data = f.data;
        cur->children.push_back(std::move(leaf));
    }

    // Sector 32 (offset 0x10000) holds the GDFX volume descriptor; sectors 33+ hold payloads & directory tables.
    uint32_t next_sector = 33u;
    auto assign_files = [&](auto& self, GdfxBuildNode& node) -> void {
        if (!node.is_dir) {
            node.assigned_size = static_cast<uint32_t>(node.file_data.size());
            if (!node.file_data.empty()) {
                node.assigned_sector = next_sector;
                next_sector += static_cast<uint32_t>((node.file_data.size() + kGdfxSectorSize - 1u) / kGdfxSectorSize);
            }
            return;
        }
        for (auto& ch : node.children) {
            self(self, *ch);
        }
    };
    assign_files(assign_files, root);
    AssignAndEncodeGdfxDirectories(root, next_sector);

    // Pad total volume size to 4 KiB AES-XTS sector alignment.
    const size_t raw_bytes = static_cast<size_t>(next_sector) * kGdfxSectorSize;
    const size_t aligned_bytes = (raw_bytes + kSectorSize - 1u) & ~(kSectorSize - 1u);
    std::vector<uint8_t> volume(aligned_bytes, 0u);

    uint8_t* desc = volume.data() + kGdfxDescriptorOffset;
    std::memcpy(desc + 0, kGdfxMagic, 20);
    WriteU32LE(desc + 20, root.assigned_sector);
    WriteU32LE(desc + 24, root.assigned_size);
    std::memcpy(desc + 2028, kGdfxMagic, 20);

    WriteGdfxNodePayloads(root, volume);
    return volume;
}

std::vector<uint8_t> XvdContainer::BuildEncryptedGdfxXvc(
    const std::vector<XvdInputFile>& files,
    const uint8_t* volume_key,
    size_t key_len,
    uint32_t content_type) {
    const std::vector<uint8_t> gdfx_vol = BuildGdfxVolumeBytes(files);
    return BuildEncryptedSyntheticXvd(gdfx_vol, volume_key, key_len, content_type);
}

}  // namespace scarlettx
