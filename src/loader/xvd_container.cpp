#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/loader/aes_xts_cng.h"

#include <algorithm>
#include <cstring>
#include <fstream>

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

}  // namespace scarlettx
