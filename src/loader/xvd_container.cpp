#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/loader/aes_xts_cng.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <utility>

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

uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

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

void WriteU16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
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

uint64_t AlignUp4K(uint64_t v) {
    return (v + 0xFFFull) & ~uint64_t{0xFFFull};
}

size_t AlignUp8(size_t v) {
    return (v + 7u) & ~size_t{7u};
}

// Common XVD header field offsets (relative to start of file; magic at 0x200).
constexpr size_t kOffVolumeFlags = 0x208;
constexpr size_t kOffFormatVersion = 0x20C;
constexpr size_t kOffDriveSize = 0x218;
constexpr size_t kOffContentId = 0x220;

// Legacy synthetic XVD field offsets.
constexpr size_t kOffLegacyContentType = 0x240;
constexpr size_t kOffLegacyEmbeddedXvdLen = 0x244;
constexpr size_t kOffLegacyUserDataOffset = 0x250;
constexpr size_t kOffLegacyUserDataLength = 0x258;

// Retail msft-xvd v3 field offsets.
constexpr size_t kOffRetailTopHash = 0x240;
constexpr size_t kOffRetailOrigXvcHash = 0x260;
constexpr size_t kOffRetailContentType = 0x284;
constexpr size_t kOffRetailEmbeddedXvdLen = 0x288;
constexpr size_t kOffRetailUserDataLength = 0x28C;
constexpr size_t kOffRetailXvcDataLength = 0x294;
constexpr size_t kOffRetailMutableDataLength = 0x29C;

// XvcInfo + XvcRegionHeader binary constants.
constexpr size_t kXvcInfoSize = 0xDC8;
constexpr size_t kXvcInfoOffVersion = 0xD10;
constexpr size_t kXvcInfoOffRegionCount = 0xD14;
constexpr size_t kXvcInfoOffKeyCount = 0xD1E;
constexpr size_t kXvcRegionHeaderSize = 0x40;

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

Status DecodeHexKeyString(const std::string& hex_key, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    out_bytes.reserve(64);
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
            out_bytes.push_back(static_cast<uint8_t>((high << 4) | nib));
            high = -1;
        }
    }
    if (high >= 0 || out_bytes.size() < 16 || out_bytes.size() > 64) {
        return Status::Error(StatusCode::InvalidArgument, "XVD hex volume key must decode to 16..64 bytes");
    }
    return Status::Ok();
}

uint32_t CalculateMerkleTreeDepth(uint64_t total_pages) {
    if (total_pages == 0) return 0;
    uint32_t levels = 1;
    uint64_t blocks = (total_pages + XvdContainer::kMerkleEntriesPerHashBlock - 1ull) /
                      XvdContainer::kMerkleEntriesPerHashBlock;
    while (blocks > 1ull) {
        ++levels;
        blocks = (blocks + XvdContainer::kMerkleEntriesPerHashBlock - 1ull) /
                 XvdContainer::kMerkleEntriesPerHashBlock;
    }
    return levels;
}

uint64_t DataPageToBackingPage(uint64_t data_page_idx, uint32_t tree_depth) {
    if (tree_depth == 0) return data_page_idx;
    uint64_t hash_pages_before = 0;
    uint64_t span = XvdContainer::kMerkleEntriesPerHashBlock;
    for (uint32_t level = 0; level < tree_depth; ++level) {
        hash_pages_before += 1ull + (data_page_idx / span);
        span *= XvdContainer::kMerkleEntriesPerHashBlock;
    }
    return data_page_idx + hash_pages_before;
}

uint64_t HashBlockToBackingPage(uint32_t level, uint64_t block_idx, uint32_t tree_depth) {
    uint64_t span = XvdContainer::kMerkleEntriesPerHashBlock;
    for (uint32_t i = 0; i < level; ++i) {
        span *= XvdContainer::kMerkleEntriesPerHashBlock;
    }
    const uint64_t first_data_page = block_idx * span;
    return DataPageToBackingPage(first_data_page, tree_depth) - (static_cast<uint64_t>(level) + 1ull);
}

bool IsAnyByteNonZero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != 0) return true;
    }
    return false;
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
            if (ch < 0x20u || ch == ':' || ch == '*' || ch == '?' ||
                ch == '"' || ch == '<' || ch == '>' || ch == '|') {
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

std::string Utf16LeToUtf8(const uint8_t* raw_u16, size_t char_count) {
    std::string out;
    out.reserve(char_count);
    for (size_t i = 0; i < char_count; ++i) {
        uint32_t cp = ReadU16LE(raw_u16 + i * 2u);
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < char_count) {
            const uint32_t low = ReadU16LE(raw_u16 + (i + 1u) * 2u);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                ++i;
            }
        }
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

std::vector<uint16_t> AsciiOrUtf8ToUtf16Le(const std::string& s) {
    std::vector<uint16_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const uint8_t b0 = static_cast<uint8_t>(s[i]);
        if (b0 < 0x80u) {
            out.push_back(b0);
            ++i;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < s.size()) {
            const uint32_t cp = ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3Fu);
            out.push_back(static_cast<uint16_t>(cp));
            i += 2;
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < s.size()) {
            const uint32_t cp = ((b0 & 0x0Fu) << 12) |
                                ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 6) |
                                (static_cast<uint8_t>(s[i + 2]) & 0x3Fu);
            out.push_back(static_cast<uint16_t>(cp));
            i += 3;
        } else {
            out.push_back('?');
            ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Clean-room NTFS 3.1 Read-Only Structures & Helpers
// ---------------------------------------------------------------------------
constexpr uint32_t kNtfsAttrStandardInfo = 0x10u;
constexpr uint32_t kNtfsAttrAttributeList = 0x20u;
constexpr uint32_t kNtfsAttrFileName = 0x30u;
constexpr uint32_t kNtfsAttrData = 0x80u;
constexpr uint32_t kNtfsAttrIndexRoot = 0x90u;
constexpr uint32_t kNtfsAttrIndexAllocation = 0xA0u;
constexpr uint32_t kNtfsAttrEnd = 0xFFFFFFFFu;

struct NtfsBpb {
    uint64_t vol_offset = 0;
    uint16_t bytes_per_sector = 512;
    uint64_t cluster_size = 4096;
    uint64_t mft_lcn = 0;
    uint32_t mft_record_size = 1024;
    uint32_t index_block_size = 4096;
    int8_t raw_clusters_per_index = 1;
};

bool IsPowerOfTwo(uint64_t v) {
    return v != 0 && (v & (v - 1ull)) == 0;
}

bool TryParseNtfsBootSector(const uint8_t* boot, size_t len, uint64_t vol_offset, uint64_t max_vol_end, NtfsBpb& out) {
    if (!boot || len < 512) return false;
    if (std::memcmp(boot + 3, XvdContainer::kNtfsOemMagic, 8) != 0) return false;

    const uint16_t bps = ReadU16LE(boot + 0x0B);
    if (bps < 512u || bps > 4096u || !IsPowerOfTwo(bps)) return false;

    const uint8_t spc = boot[0x0D];
    uint64_t cluster_bytes = 0;
    if (spc <= 0x80u) {
        if (!IsPowerOfTwo(spc)) return false;
        cluster_bytes = static_cast<uint64_t>(bps) * spc;
    } else {
        const int shift = -static_cast<int>(static_cast<int8_t>(spc));
        if (shift < 9 || shift > 21) return false;
        cluster_bytes = uint64_t{1} << static_cast<uint32_t>(shift);
    }
    if (cluster_bytes < bps || cluster_bytes > (2ull << 20)) return false;

    const uint64_t mft_lcn = ReadU64LE(boot + 0x30);
    const int8_t cpm = static_cast<int8_t>(boot[0x40]);
    uint32_t mft_rec_size = 0;
    if (cpm < 0) {
        const int shift = -static_cast<int>(cpm);
        if (shift < 9 || shift > 16) return false;
        mft_rec_size = 1u << static_cast<uint32_t>(shift);
    } else if (cpm > 0) {
        const uint64_t sz = static_cast<uint64_t>(cpm) * cluster_bytes;
        if (sz < 512ull || sz > 65536ull) return false;
        mft_rec_size = static_cast<uint32_t>(sz);
    } else {
        return false;
    }

    const int8_t cpi = static_cast<int8_t>(boot[0x44]);
    uint32_t idx_block_size = 4096;
    if (cpi < 0) {
        const int shift = -static_cast<int>(cpi);
        if (shift >= 9 && shift <= 16) {
            idx_block_size = 1u << static_cast<uint32_t>(shift);
        }
    } else if (cpi > 0) {
        const uint64_t sz = static_cast<uint64_t>(cpi) * cluster_bytes;
        if (sz >= 512ull && sz <= 65536ull) {
            idx_block_size = static_cast<uint32_t>(sz);
        }
    }

    if (mft_lcn > (max_vol_end - vol_offset) / cluster_bytes) return false;
    const uint64_t mft_byte_off = vol_offset + mft_lcn * cluster_bytes;
    if (mft_byte_off + mft_rec_size > max_vol_end) return false;

    out.vol_offset = vol_offset;
    out.bytes_per_sector = bps;
    out.cluster_size = cluster_bytes;
    out.mft_lcn = mft_lcn;
    out.mft_record_size = mft_rec_size;
    out.index_block_size = idx_block_size;
    out.raw_clusters_per_index = cpi;
    return true;
}

// Applies NTFS Update Sequence Array (USA) fixup on FILE or INDX multi-sector records.
Status ApplyNtfsUsaFixup(uint8_t* rec, size_t rec_size, const char expected_sig[4], size_t& out_usa_stride) {
    if (!rec || rec_size < 48) {
        return Status::Error(StatusCode::ParseError, "NTFS record buffer too small");
    }
    if (std::memcmp(rec, "BAAD", 4) == 0) {
        return Status::Error(StatusCode::ParseError, "Corrupt 'BAAD' NTFS multi-sector record");
    }
    if (std::memcmp(rec, expected_sig, 4) != 0) {
        return Status::Error(StatusCode::ParseError, "Unexpected NTFS multi-sector signature");
    }
    const uint16_t usa_off = ReadU16LE(rec + 4);
    const uint16_t usa_count = ReadU16LE(rec + 6);
    if (usa_count < 2u || static_cast<size_t>(usa_off) + static_cast<size_t>(usa_count) * 2u > rec_size) {
        return Status::Error(StatusCode::ParseError, "Invalid NTFS Update Sequence Array bounds");
    }
    const size_t sector_count = static_cast<size_t>(usa_count - 1u);
    size_t stride = 512u;
    if (sector_count * 512u != rec_size) {
        if ((rec_size % sector_count) != 0 || (rec_size / sector_count) < 512u) {
            return Status::Error(StatusCode::ParseError, "Unsupported NTFS USA sector stride");
        }
        stride = rec_size / sector_count;
    }
    out_usa_stride = stride;

    const uint16_t usn = ReadU16LE(rec + usa_off);
    for (size_t s = 1; s <= sector_count; ++s) {
        const size_t check_pos = s * stride - 2u;
        if (ReadU16LE(rec + check_pos) != usn) {
            return Status::Error(StatusCode::ParseError, "NTFS Update Sequence Number tear detected");
        }
        const uint16_t orig_word = ReadU16LE(rec + usa_off + s * 2u);
        WriteU16LE(rec + check_pos, orig_word);
    }
    return Status::Ok();
}

void FinalizeNtfsUsaRecord(uint8_t* rec, size_t rec_size, uint16_t usa_off, uint16_t usn = 0x0001u) {
    const uint16_t usa_count = static_cast<uint16_t>(rec_size / 512u + 1u);
    WriteU16LE(rec + 4, usa_off);
    WriteU16LE(rec + 6, usa_count);
    WriteU16LE(rec + usa_off, usn);
    for (uint16_t s = 1; s < usa_count; ++s) {
        const size_t end_pos = static_cast<size_t>(s) * 512u - 2u;
        const uint16_t orig_word = ReadU16LE(rec + end_pos);
        WriteU16LE(rec + usa_off + static_cast<size_t>(s) * 2u, orig_word);
        WriteU16LE(rec + end_pos, usn);
    }
}

struct NtfsRunExtent {
    uint64_t user_offset = 0;
    uint64_t byte_length = 0;
    bool is_sparse = false;
};

Status DecodeNtfsDataRuns(
    const uint8_t* run_ptr,
    const uint8_t* run_end,
    uint64_t vol_offset,
    uint64_t cluster_size,
    uint64_t max_user_len,
    std::vector<NtfsRunExtent>& out_runs) {
    int64_t current_lcn = 0;
    while (run_ptr < run_end && *run_ptr != 0) {
        const uint8_t hdr = *run_ptr++;
        const uint8_t len_bytes = static_cast<uint8_t>(hdr & 0x0Fu);
        const uint8_t off_bytes = static_cast<uint8_t>((hdr >> 4) & 0x0Fu);
        if (len_bytes == 0 || len_bytes > 8u || off_bytes > 8u) {
            return Status::Error(StatusCode::ParseError, "Invalid NTFS data run field width");
        }
        if (static_cast<size_t>(run_end - run_ptr) < static_cast<size_t>(len_bytes + off_bytes)) {
            return Status::Error(StatusCode::ParseError, "Truncated NTFS data run entry");
        }
        uint64_t cluster_count = 0;
        for (uint8_t b = 0; b < len_bytes; ++b) {
            cluster_count |= static_cast<uint64_t>(run_ptr[b]) << (b * 8u);
        }
        run_ptr += len_bytes;
        if (cluster_count == 0 || cluster_count > (UINT64_MAX / cluster_size)) {
            return Status::Error(StatusCode::ParseError, "Invalid NTFS data run cluster count");
        }
        const uint64_t run_bytes = cluster_count * cluster_size;

        if (off_bytes == 0) {
            out_runs.push_back(NtfsRunExtent{0, run_bytes, true});
        } else {
            uint64_t udelta = 0;
            for (uint8_t b = 0; b < off_bytes; ++b) {
                udelta |= static_cast<uint64_t>(run_ptr[b]) << (b * 8u);
            }
            if (off_bytes < 8u && (run_ptr[off_bytes - 1u] & 0x80u) != 0) {
                udelta |= (~uint64_t{0}) << (off_bytes * 8u);
            }
            run_ptr += off_bytes;

            current_lcn += static_cast<int64_t>(udelta);
            if (current_lcn < 0) {
                return Status::Error(StatusCode::ParseError, "Negative NTFS LCN in data run");
            }
            const uint64_t abs_user_off = vol_offset + static_cast<uint64_t>(current_lcn) * cluster_size;
            if (abs_user_off > max_user_len || run_bytes > max_user_len - abs_user_off) {
                return Status::Error(StatusCode::ParseError, "NTFS data run exceeds user-data partition bounds");
            }
            out_runs.push_back(NtfsRunExtent{abs_user_off, run_bytes, false});
        }
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// GDFX Synthetic Builder Helpers
// ---------------------------------------------------------------------------
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
        const size_t name_len = (std::min)(child.name.size(), size_t{255});
        const size_t rec_bytes = (14u + name_len + 3u) & ~size_t{3u};
        if ((cursor / XvdContainer::kGdfxSectorSize) != ((cursor + rec_bytes - 1u) / XvdContainer::kGdfxSectorSize)) {
            cursor = ((cursor / XvdContainer::kGdfxSectorSize) + 1u) * XvdContainer::kGdfxSectorSize;
        }
        slot_byte_offset[s] = cursor;
        cursor += rec_bytes;
    }

    const size_t total_sectors = (std::max)(size_t{1}, (cursor + XvdContainer::kGdfxSectorSize - 1u) / XvdContainer::kGdfxSectorSize);
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
        const uint8_t name_len = static_cast<uint8_t>((std::min)(child.name.size(), size_t{255}));
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

// ---------------------------------------------------------------------------
// XvdContainer Lifecycle & Persistent Handle Management
// ---------------------------------------------------------------------------

XvdContainer::~XvdContainer() {
    CloseFileHandle();
}

XvdContainer::XvdContainer(XvdContainer&& other) noexcept
    : file_handle_(std::exchange(other.file_handle_, nullptr)),
      file_size_(std::exchange(other.file_size_, 0)),
      path_(std::move(other.path_)),
      backing_bytes_(std::move(other.backing_bytes_)),
      memory_backed_(std::exchange(other.memory_backed_, false)),
      header_(other.header_),
      base_data_offset_(other.base_data_offset_),
      user_data_pages_(other.user_data_pages_),
      total_hashed_pages_(other.total_hashed_pages_),
      merkle_tree_depth_(other.merkle_tree_depth_),
      contiguous_merkle_layout_(other.contiguous_merkle_layout_),
      merkle_root_offset_(other.merkle_root_offset_),
      pdu_offset_(other.pdu_offset_),
      pdu_pages_(other.pdu_pages_),
      xvc_info_offset_(other.xvc_info_offset_),
      verify_integrity_on_read_(other.verify_integrity_on_read_),
      regions_(std::move(other.regions_)),
      volume_key_(other.volume_key_),
      volume_key_len_(other.volume_key_len_),
      has_volume_key_(other.has_volume_key_),
      prepared_key_(std::move(other.prepared_key_)),
      cik_keys_(std::move(other.cik_keys_)) {
    other.header_ = {};
}

XvdContainer& XvdContainer::operator=(XvdContainer&& other) noexcept {
    if (this != &other) {
        CloseFileHandle();
        ResetFsCache();
        file_handle_ = std::exchange(other.file_handle_, nullptr);
        file_size_ = std::exchange(other.file_size_, 0);
        path_ = std::move(other.path_);
        backing_bytes_ = std::move(other.backing_bytes_);
        memory_backed_ = std::exchange(other.memory_backed_, false);
        header_ = other.header_;
        other.header_ = {};
        base_data_offset_ = other.base_data_offset_;
        user_data_pages_ = other.user_data_pages_;
        total_hashed_pages_ = other.total_hashed_pages_;
        merkle_tree_depth_ = other.merkle_tree_depth_;
        contiguous_merkle_layout_ = other.contiguous_merkle_layout_;
        merkle_root_offset_ = other.merkle_root_offset_;
        pdu_offset_ = other.pdu_offset_;
        pdu_pages_ = other.pdu_pages_;
        xvc_info_offset_ = other.xvc_info_offset_;
        verify_integrity_on_read_ = other.verify_integrity_on_read_;
        regions_ = std::move(other.regions_);
        volume_key_ = other.volume_key_;
        volume_key_len_ = other.volume_key_len_;
        has_volume_key_ = other.has_volume_key_;
        prepared_key_ = std::move(other.prepared_key_);
        cik_keys_ = std::move(other.cik_keys_);
    }
    return *this;
}

void XvdContainer::CloseFileHandle() noexcept {
#if defined(_WIN32)
    if (file_handle_) {
        ::CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#else
    file_handle_ = nullptr;
#endif
    file_size_ = 0;
}

void XvdContainer::ResetFsCache() const {
    std::lock_guard<std::mutex> lk(fs_cache_mu_);
    fs_cached_ = false;
    cached_fs_entries_.clear();
    cached_fs_lookup_.clear();
}

Status XvdContainer::ReadRawContainerBytes(uint64_t abs_offset, uint8_t* dst, size_t len) const {
    if (len == 0) {
        return Status::Ok();
    }
    if (!dst) {
        return Status::Error(StatusCode::InvalidArgument, "Null destination buffer in XVD read");
    }
    if (memory_backed_) {
        if (abs_offset > backing_bytes_.size() || len > backing_bytes_.size() - abs_offset) {
            return Status::Error(StatusCode::IoError, "In-memory XVD buffer truncated");
        }
        std::memcpy(dst, backing_bytes_.data() + static_cast<size_t>(abs_offset), len);
        return Status::Ok();
    }

#if defined(_WIN32)
    if (!file_handle_) {
        return Status::Error(StatusCode::IoError, "XVD file handle is not open: " + path_);
    }
    if (abs_offset > file_size_ || len > file_size_ - abs_offset) {
        return Status::Error(StatusCode::IoError, "Short read past EOF from XVD file: " + path_);
    }

    constexpr size_t kMaxReadChunk = 0x40000000u;  // 1 GiB per ReadFile call
    uint64_t cur_off = abs_offset;
    uint8_t* cur_dst = dst;
    size_t remaining = len;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min)(remaining, kMaxReadChunk));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(cur_off & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>((cur_off >> 32) & 0xFFFFFFFFull);
        DWORD bytes_read = 0;
        if (!::ReadFile(static_cast<HANDLE>(file_handle_), cur_dst, chunk, &bytes_read, &ov) ||
            bytes_read != chunk) {
            return Status::Error(StatusCode::IoError, "ReadFile failed on XVD container: " + path_);
        }
        cur_off += chunk;
        cur_dst += chunk;
        remaining -= chunk;
    }
    return Status::Ok();
#else
    std::ifstream ifs(path_, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::IoError, "Failed to open XVD file: " + path_);
    }
    ifs.seekg(static_cast<std::streamoff>(abs_offset), std::ios::beg);
    ifs.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
    if (static_cast<size_t>(ifs.gcount()) != len) {
        return Status::Error(StatusCode::IoError, "Short read from XVD file: " + path_);
    }
    return Status::Ok();
#endif
}

// ---------------------------------------------------------------------------
// Volume Key & Multi-Key CIK Ring Management
// ---------------------------------------------------------------------------

Status XvdContainer::SetVolumeKey(const uint8_t* key, size_t key_len) {
    if (!key || key_len < 16) {
        return Status::Error(StatusCode::InvalidArgument, "Volume key must be at least 16 bytes");
    }
    volume_key_.fill(0);
    const size_t copy_len = (std::min)(key_len, volume_key_.size());
    std::memcpy(volume_key_.data(), key, copy_len);
    volume_key_len_ = static_cast<uint32_t>(copy_len);
    auto prep = std::make_shared<crypto::AesXtsPreparedKey>();
    if (!prep->Init(volume_key_.data(), copy_len)) {
        return Status::Error(StatusCode::InvalidArgument, "Failed to initialize XVD AES-XTS key");
    }
    prepared_key_ = prep;
    if (cik_keys_.find(0) == cik_keys_.end()) {
        cik_keys_[0] = prep;
    }
    has_volume_key_ = true;
    ResetFsCache();
    return Status::Ok();
}

Status XvdContainer::SetVolumeKeyHex(const std::string& hex_key) {
    std::vector<uint8_t> bytes;
    Status st = DecodeHexKeyString(hex_key, bytes);
    if (!st.ok()) return st;
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
    // Standard 48-byte binary .cik file: [0..15] = 16-byte KeyId GUID, [16..47] = 32-byte AES-128-XTS key.
    if (raw.size() == 48) {
        return SetVolumeKey(raw.data() + 16, 32);
    }

    std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    // Support text .cik / keyring lines formatted as "<GUID> = <HEXKEY>", "<GUID>:<HEXKEY>", or multi-line key lists.
    uint16_t slot_idx = 0;
    bool loaded_any = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find_first_of("\r\n", pos);
        std::string line = (eol == std::string::npos) ? text.substr(pos) : text.substr(pos, eol - pos);
        pos = (eol == std::string::npos) ? text.size() : (eol + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        const size_t eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            line = line.substr(eq_pos + 1);
        } else {
            // Handle "<GUID>:<64-hex-key>" where GUID itself has no colons
            const size_t colon_pos = line.rfind(':');
            if (colon_pos != std::string::npos && colon_pos >= 32u) {
                line = line.substr(colon_pos + 1);
            }
        }
        std::vector<uint8_t> decoded;
        if (DecodeHexKeyString(line, decoded).ok()) {
            // If 48 bytes of hex (16B GUID + 32B key) were concatenated on one line, slice the 32B key.
            const uint8_t* kptr = (decoded.size() == 48u) ? (decoded.data() + 16u) : decoded.data();
            const size_t klen = (decoded.size() == 48u) ? 32u : decoded.size();
            if (SetCikKey(slot_idx, kptr, klen).ok()) {
                ++slot_idx;
                loaded_any = true;
            }
        }
    }
    if (loaded_any) {
        return Status::Ok();
    }
    return SetVolumeKeyHex(text);
}

Status XvdContainer::SetCikKey(uint16_t slot, const uint8_t* key, size_t key_len) {
    if (slot == kUnencryptedKeyId) {
        return Status::Error(StatusCode::InvalidArgument, "CIK slot 0xFFFF is reserved for unencrypted XVC regions");
    }
    if (!key || key_len < 16) {
        return Status::Error(StatusCode::InvalidArgument, "CIK key must be at least 16 bytes");
    }
    const size_t copy_len = (std::min)(key_len, size_t{64});
    auto prep = std::make_shared<crypto::AesXtsPreparedKey>();
    if (!prep->Init(key, copy_len)) {
        return Status::Error(StatusCode::InvalidArgument, "Failed to initialize XVC CIK AES-XTS key");
    }
    cik_keys_[slot] = prep;
    if (slot == 0 && !has_volume_key_) {
        volume_key_.fill(0);
        std::memcpy(volume_key_.data(), key, copy_len);
        volume_key_len_ = static_cast<uint32_t>(copy_len);
        prepared_key_ = prep;
        has_volume_key_ = true;
    }
    ResetFsCache();
    return Status::Ok();
}

Status XvdContainer::SetCikKeyHex(uint16_t slot, const std::string& hex_key) {
    std::vector<uint8_t> bytes;
    Status st = DecodeHexKeyString(hex_key, bytes);
    if (!st.ok()) return st;
    return SetCikKey(slot, bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// Dual Header Parsing (Retail msft-xvd v3 vs Legacy Synthetic) & Merkle Tree
// ---------------------------------------------------------------------------

Status XvdContainer::ParseHeaderFromBytes(const uint8_t* data, size_t size, uint64_t total_container_size) {
    header_ = {};
    regions_.clear();
    base_data_offset_ = kMinHeaderSize;
    user_data_pages_ = 0;
    total_hashed_pages_ = 0;
    merkle_tree_depth_ = 0;
    contiguous_merkle_layout_ = false;
    merkle_root_offset_ = 0;
    pdu_offset_ = 0;
    pdu_pages_ = 0;
    xvc_info_offset_ = 0;

    if (!data || size < kMinHeaderSize || total_container_size < kMinHeaderSize) {
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
    header_.is_encrypted = (header_.volume_flags & kFlagEncryptionDisabled) == 0;
    header_.has_data_integrity = (header_.volume_flags & kFlagDataIntegrityDisabled) == 0;

    const uint32_t syn_ct = ReadU32LE(data + kOffLegacyContentType);
    const uint64_t syn_uoff = ReadU64LE(data + kOffLegacyUserDataOffset);
    const uint64_t syn_ulen = ReadU64LE(data + kOffLegacyUserDataLength);
    const bool range_280_2af_zero = !IsAnyByteNonZero(data + 0x280, 0x30);
    const bool range_248_24f_zero = !IsAnyByteNonZero(data + 0x248, 0x08);

    const bool is_legacy_synthetic =
        range_280_2af_zero &&
        range_248_24f_zero &&
        (syn_ct <= 0xFFu) &&
        (syn_uoff >= kMinHeaderSize) &&
        ((syn_uoff % kSectorSize) == 0) &&
        (syn_uoff <= total_container_size) &&
        (syn_ulen <= total_container_size - syn_uoff);

    if (!is_legacy_synthetic) {
        if (total_container_size < kRetailHeaderSize) {
            return Status::Error(StatusCode::UnsupportedFormat, "Truncated retail v3 XVD header (< 0x3000 bytes)");
        }
        header_.is_retail_v3_layout = true;
        std::memcpy(header_.top_hash_block_hash.data(), data + kOffRetailTopHash, 32);
        std::memcpy(header_.original_xvc_data_hash.data(), data + kOffRetailOrigXvcHash, 32);
        header_.content_type = ReadU32LE(data + kOffRetailContentType);
        header_.embedded_xvd_length = ReadU32LE(data + kOffRetailEmbeddedXvdLen);

        // Authentic Microsoft makepkg (.msixvc / .xvc) containers store 32-bit PDU/XvcData lengths
        // at 0x28C / 0x290, BlockSize = 0xAA000 (170 * 4096) at 0x298, and a contiguous Merkle hash tree
        // preceding PDU + XvcInfo + DriveSize.
        const uint32_t makepkg_block_size = ReadU32LE(data + 0x298);
        if (makepkg_block_size == 0x000AA000u && header_.drive_size > 0) {
            contiguous_merkle_layout_ = true;
            const uint32_t pdu_len = ReadU32LE(data + 0x28C);
            header_.xvc_data_length = ReadU32LE(data + 0x290);
            header_.user_data_length = header_.drive_size;

            pdu_pages_ = (static_cast<uint64_t>(pdu_len) + kSectorSize - 1ull) / kSectorSize;
            const uint64_t xvc_pages = (header_.xvc_data_length + kSectorSize - 1ull) / kSectorSize;
            user_data_pages_ = (header_.user_data_length + kSectorSize - 1ull) / kSectorSize;
            total_hashed_pages_ = pdu_pages_ + xvc_pages + user_data_pages_;

            uint64_t contiguous_hash_pages = 0;
            if (header_.has_data_integrity && total_hashed_pages_ > 0) {
                uint64_t cur = (total_hashed_pages_ + kMerkleEntriesPerHashBlock - 1ull) / kMerkleEntriesPerHashBlock;
                contiguous_hash_pages += cur;
                uint32_t levels = 1;
                while (cur > 1ull) {
                    cur = (cur + kMerkleEntriesPerHashBlock - 1ull) / kMerkleEntriesPerHashBlock;
                    contiguous_hash_pages += cur;
                    ++levels;
                }
                merkle_tree_depth_ = levels;
            }

            const uint64_t tail_bytes = (contiguous_hash_pages + total_hashed_pages_) * kSectorSize;
            if (total_container_size >= kRetailHeaderSize + tail_bytes) {
                merkle_root_offset_ = total_container_size - tail_bytes;
                header_.mutable_data_length = merkle_root_offset_ - kRetailHeaderSize;
            } else {
                merkle_root_offset_ = kRetailHeaderSize + AlignUp4K(header_.embedded_xvd_length);
            }
            pdu_offset_ = merkle_root_offset_ + contiguous_hash_pages * kSectorSize;
            xvc_info_offset_ = pdu_offset_ + pdu_pages_ * kSectorSize;
            base_data_offset_ = xvc_info_offset_ + xvc_pages * kSectorSize;
            header_.user_data_offset = base_data_offset_;
        } else {
            header_.user_data_length = ReadU64LE(data + kOffRetailUserDataLength);
            header_.xvc_data_length = ReadU64LE(data + kOffRetailXvcDataLength);
            header_.mutable_data_length = ReadU64LE(data + kOffRetailMutableDataLength);

            if (header_.user_data_length == 0 && header_.drive_size > 0) {
                header_.user_data_length = header_.drive_size;
            }
            base_data_offset_ = kRetailHeaderSize +
                                AlignUp4K(header_.embedded_xvd_length) +
                                AlignUp4K(header_.mutable_data_length);
            header_.user_data_offset = base_data_offset_;
            user_data_pages_ = (header_.user_data_length + kSectorSize - 1ull) / kSectorSize;
            const uint64_t xvc_pages = (header_.xvc_data_length + kSectorSize - 1ull) / kSectorSize;
            total_hashed_pages_ = user_data_pages_ + xvc_pages;
            if (header_.has_data_integrity) {
                merkle_tree_depth_ = CalculateMerkleTreeDepth(total_hashed_pages_);
            }
        }
    } else {
        header_.is_retail_v3_layout = false;
        header_.content_type = syn_ct;
        header_.embedded_xvd_length = ReadU32LE(data + kOffLegacyEmbeddedXvdLen);
        header_.user_data_offset = syn_uoff;
        header_.user_data_length = syn_ulen;
        if (header_.user_data_offset < kMinHeaderSize) {
            header_.user_data_offset = kMinHeaderSize;
        }
        if (header_.user_data_length == 0 && header_.drive_size > header_.user_data_offset) {
            header_.user_data_length = header_.drive_size - header_.user_data_offset;
        }
        base_data_offset_ = header_.user_data_offset;
        user_data_pages_ = (header_.user_data_length + kSectorSize - 1ull) / kSectorSize;
        total_hashed_pages_ = user_data_pages_;
    }

    header_.valid = true;
    return Status::Ok();
}

Status XvdContainer::ReadLogicalBackingPage(uint64_t logical_page_idx, uint8_t out_page[kSectorSize]) const {
    uint64_t phys_off = 0;
    if (contiguous_merkle_layout_) {
        phys_off = pdu_offset_ + logical_page_idx * kSectorSize;
    } else if (header_.is_retail_v3_layout) {
        const uint64_t backing_idx = header_.has_data_integrity
                                         ? DataPageToBackingPage(logical_page_idx, merkle_tree_depth_)
                                         : logical_page_idx;
        phys_off = base_data_offset_ + backing_idx * kSectorSize;
    } else {
        phys_off = header_.user_data_offset + logical_page_idx * kSectorSize;
    }
    return ReadRawContainerBytes(phys_off, out_page, kSectorSize);
}

Status XvdContainer::FinalizeOpenMetadata() {
    if (!header_.is_retail_v3_layout) {
        return Status::Ok();
    }

    const uint64_t total_size = memory_backed_ ? static_cast<uint64_t>(backing_bytes_.size()) : file_size_;

    if (contiguous_merkle_layout_) {
        header_.user_data_offset = base_data_offset_;
        constexpr size_t kMakepkgXvcInfoSize = 0xDA8u;
        constexpr size_t kMakepkgRegionHeaderSize = 0x80u;
        if (header_.xvc_data_length >= kMakepkgXvcInfoSize && header_.xvc_data_length <= (16ull << 20)) {
            const uint64_t xvc_pages = (header_.xvc_data_length + kSectorSize - 1ull) / kSectorSize;
            std::vector<uint8_t> xvc_buf(static_cast<size_t>(xvc_pages * kSectorSize), 0);
            if (ReadRawContainerBytes(xvc_info_offset_, xvc_buf.data(), xvc_buf.size()).ok()) {
                const uint32_t region_count = ReadU32LE(xvc_buf.data() + kXvcInfoOffRegionCount);
                const size_t avail = xvc_buf.size() > kMakepkgXvcInfoSize ? (xvc_buf.size() - kMakepkgXvcInfoSize) : 0u;
                const uint32_t max_regions = static_cast<uint32_t>(avail / kMakepkgRegionHeaderSize);
                const uint32_t clamped_count = (std::min)((std::min)(region_count, max_regions), 4096u);
                const uint64_t drive_end = base_data_offset_ + header_.user_data_length;
                for (uint32_t r = 0; r < clamped_count; ++r) {
                    const uint8_t* rp = xvc_buf.data() + kMakepkgXvcInfoSize + static_cast<size_t>(r) * kMakepkgRegionHeaderSize;
                    const uint32_t reg_id = ReadU32LE(rp + 0x00);
                    const uint16_t key_id = ReadU16LE(rp + 0x04);
                    const uint32_t reg_flags = ReadU32LE(rp + 0x08);
                    const uint64_t phys_off = ReadU64LE(rp + 0x50);
                    const uint64_t reg_len = ReadU64LE(rp + 0x58);
                    if (reg_len == 0 || phys_off >= drive_end || phys_off + reg_len <= base_data_offset_) {
                        continue;
                    }
                    const uint64_t ov_start = (std::max)(phys_off, base_data_offset_);
                    const uint64_t ov_end = (std::min)(phys_off + reg_len, drive_end);
                    if (ov_end > ov_start) {
                        regions_.push_back(XvcRegionInfo{
                            reg_id,
                            key_id,
                            reg_flags,
                            ov_start - base_data_offset_,
                            ov_end - ov_start,
                        });
                    }
                }
            }
        }
        return Status::Ok();
    }

    // Auto-detect fixed 4-level retail makexvc hash trees on small images when present.
    if (header_.has_data_integrity && total_hashed_pages_ > 0 && merkle_tree_depth_ < 4u &&
        total_size >= base_data_offset_ + 5ull * kSectorSize) {
        std::array<uint8_t, kSectorSize> p0{}, p1{}, p2{}, p3{};
        if (ReadRawContainerBytes(base_data_offset_ + 0ull * kSectorSize, p0.data(), kSectorSize).ok() &&
            ReadRawContainerBytes(base_data_offset_ + 1ull * kSectorSize, p1.data(), kSectorSize).ok() &&
            ReadRawContainerBytes(base_data_offset_ + 2ull * kSectorSize, p2.data(), kSectorSize).ok() &&
            ReadRawContainerBytes(base_data_offset_ + 3ull * kSectorSize, p3.data(), kSectorSize).ok()) {
            uint8_t d1[32]{}, d2[32]{}, d3[32]{};
            if (crypto::ComputeSha256(p1.data(), kSectorSize, d1) &&
                crypto::ComputeSha256(p2.data(), kSectorSize, d2) &&
                crypto::ComputeSha256(p3.data(), kSectorSize, d3) &&
                std::memcmp(p0.data(), d1, kMerkleHashEntrySize) == 0 &&
                std::memcmp(p1.data(), d2, kMerkleHashEntrySize) == 0 &&
                std::memcmp(p2.data(), d3, kMerkleHashEntrySize) == 0) {
                merkle_tree_depth_ = 4u;
            }
        }
    }

    header_.user_data_offset = LogicalUserPageToPhysicalOffset(0);

    // Parse unencrypted XvcInfo + XvcRegionHeader[] starting immediately after UserData pages.
    if (header_.xvc_data_length >= kXvcInfoSize && header_.xvc_data_length <= (16ull << 20)) {
        const uint64_t xvc_pages = (header_.xvc_data_length + kSectorSize - 1ull) / kSectorSize;
        std::vector<uint8_t> xvc_buf(static_cast<size_t>(xvc_pages * kSectorSize), 0);
        bool xvc_ok = true;
        for (uint64_t i = 0; i < xvc_pages; ++i) {
            if (!ReadLogicalBackingPage(user_data_pages_ + i, xvc_buf.data() + static_cast<size_t>(i * kSectorSize)).ok()) {
                xvc_ok = false;
                break;
            }
        }
        if (xvc_ok) {
            const uint32_t region_count = ReadU32LE(xvc_buf.data() + kXvcInfoOffRegionCount);
            const size_t avail_for_regions = (static_cast<size_t>(header_.xvc_data_length) > kXvcInfoSize)
                                                 ? (static_cast<size_t>(header_.xvc_data_length) - kXvcInfoSize)
                                                 : 0u;
            const uint32_t max_regions = static_cast<uint32_t>(avail_for_regions / kXvcRegionHeaderSize);
            const uint32_t clamped_count = (std::min)((std::min)(region_count, max_regions), 4096u);

            for (uint32_t r = 0; r < clamped_count; ++r) {
                const uint8_t* rp = xvc_buf.data() + kXvcInfoSize + static_cast<size_t>(r) * kXvcRegionHeaderSize;
                const uint32_t reg_id = ReadU32LE(rp + 0x00);
                const uint16_t key_id = ReadU16LE(rp + 0x04);
                const uint32_t reg_flags = ReadU32LE(rp + 0x08);
                uint64_t reg_off = ReadU64LE(rp + 0x20);
                const uint64_t reg_len = ReadU64LE(rp + 0x28);

                // Ignore container header metadata pseudo-regions (0x40000001..0x40000005).
                if (reg_id >= 0x40000000u || reg_len == 0) {
                    continue;
                }
                if (reg_off > header_.user_data_length && reg_off >= base_data_offset_) {
                    const uint64_t cand = reg_off - base_data_offset_;
                    if (cand <= AlignUp4K(header_.user_data_length)) {
                        reg_off = cand;
                    }
                }
                regions_.push_back(XvcRegionInfo{reg_id, key_id, reg_flags, reg_off, reg_len});
            }
        }
    }

    return Status::Ok();
}

Status XvdContainer::OpenFile(const std::string& path) {
    CloseFileHandle();
    ResetFsCache();
    header_ = {};
    regions_.clear();
    backing_bytes_.clear();
    memory_backed_ = false;
    path_.clear();

#if defined(_WIN32)
    const std::wstring wpath = std::filesystem::path(path).wstring();
    HANDLE h = ::CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        h = ::CreateFileW(
            wpath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) {
        return Status::Error(StatusCode::NotFound, "Failed to open XVD file: " + path);
    }

    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h, &li) || li.QuadPart < 0) {
        ::CloseHandle(h);
        return Status::Error(StatusCode::IoError, "Failed to query XVD file size: " + path);
    }
    const uint64_t fsize = static_cast<uint64_t>(li.QuadPart);
    if (fsize < kMinHeaderSize) {
        ::CloseHandle(h);
        return Status::Error(StatusCode::UnsupportedFormat, "Truncated XVD header: " + path);
    }

    file_handle_ = static_cast<void*>(h);
    file_size_ = fsize;
    path_ = path;

    const size_t hdr_read = static_cast<size_t>((std::min)(fsize, uint64_t{kRetailHeaderSize}));
    std::vector<uint8_t> hdr(hdr_read, 0);
    Status st = ReadRawContainerBytes(0, hdr.data(), hdr.size());
    if (!st.ok()) {
        CloseFileHandle();
        path_.clear();
        return st;
    }

    st = ParseHeaderFromBytes(hdr.data(), hdr.size(), fsize);
    if (!st.ok()) {
        CloseFileHandle();
        path_.clear();
        return st;
    }
    return FinalizeOpenMetadata();
#else
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Failed to open XVD file: " + path);
    }
    const auto end_pos = ifs.tellg();
    if (end_pos < static_cast<std::streamoff>(kMinHeaderSize)) {
        return Status::Error(StatusCode::UnsupportedFormat, "Truncated XVD header: " + path);
    }
    const uint64_t fsize = static_cast<uint64_t>(end_pos);
    ifs.seekg(0, std::ios::beg);
    const size_t hdr_read = static_cast<size_t>((std::min)(fsize, uint64_t{kRetailHeaderSize}));
    std::vector<uint8_t> hdr(hdr_read, 0);
    ifs.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));

    file_size_ = fsize;
    path_ = path;
    Status st = ParseHeaderFromBytes(hdr.data(), hdr.size(), fsize);
    if (!st.ok()) {
        path_.clear();
        file_size_ = 0;
        return st;
    }
    return FinalizeOpenMetadata();
#endif
}

Status XvdContainer::OpenBytes(std::vector<uint8_t> data) {
    CloseFileHandle();
    ResetFsCache();
    path_.clear();
    const uint64_t total_sz = static_cast<uint64_t>(data.size());
    Status st = ParseHeaderFromBytes(data.data(), data.size(), total_sz);
    if (!st.ok()) {
        return st;
    }
    backing_bytes_ = std::move(data);
    memory_backed_ = true;
    return FinalizeOpenMetadata();
}

uint64_t XvdContainer::LogicalUserPageToPhysicalOffset(uint64_t user_page_idx) const {
    if (!header_.is_retail_v3_layout) {
        return header_.user_data_offset + user_page_idx * kSectorSize;
    }
    if (contiguous_merkle_layout_ || !header_.has_data_integrity) {
        return base_data_offset_ + user_page_idx * kSectorSize;
    }
    const uint64_t backing_page = DataPageToBackingPage(user_page_idx, merkle_tree_depth_);
    return base_data_offset_ + backing_page * kSectorSize;
}

Status XvdContainer::VerifyDataPageHash(uint64_t user_page_idx) const {
    if (!header_.valid) {
        return Status::Error(StatusCode::InvalidArgument, "XVD container is not open");
    }
    if (!header_.is_retail_v3_layout || !header_.has_data_integrity || merkle_tree_depth_ == 0) {
        return Status::Error(StatusCode::InvalidArgument, "XVD container does not have Merkle data integrity enabled");
    }
    if (user_page_idx >= user_data_pages_) {
        return Status::Error(StatusCode::InvalidArgument, "XVD page index out of user-data bounds");
    }

    std::array<uint8_t, kSectorSize> cur_page{};
    Status st = ReadRawContainerBytes(LogicalUserPageToPhysicalOffset(user_page_idx), cur_page.data(), kSectorSize);
    if (!st.ok()) return st;

    uint8_t child_digest[32]{};
    if (!crypto::ComputeSha256(cur_page.data(), kSectorSize, child_digest)) {
        return Status::Error(StatusCode::IoError, "CNG SHA-256 failed while verifying XVD Merkle page");
    }

    if (contiguous_merkle_layout_) {
        std::vector<uint64_t> level_counts;
        uint64_t cur = (total_hashed_pages_ + kMerkleEntriesPerHashBlock - 1ull) / kMerkleEntriesPerHashBlock;
        level_counts.push_back(cur);
        while (cur > 1ull) {
            cur = (cur + kMerkleEntriesPerHashBlock - 1ull) / kMerkleEntriesPerHashBlock;
            level_counts.push_back(cur);
        }
        std::vector<uint64_t> level_offsets(level_counts.size(), 0);
        uint64_t running_off = merkle_root_offset_;
        for (size_t i = level_counts.size(); i-- > 0;) {
            level_offsets[i] = running_off;
            running_off += level_counts[i] * kSectorSize;
        }

        const uint64_t xvc_pages = (header_.xvc_data_length + kSectorSize - 1ull) / kSectorSize;
        uint64_t child_idx = pdu_pages_ + xvc_pages + user_page_idx;
        std::array<uint8_t, kSectorSize> hash_page{};
        for (size_t level = 0; level < level_counts.size(); ++level) {
            const uint64_t block_idx = child_idx / kMerkleEntriesPerHashBlock;
            const size_t entry_idx = static_cast<size_t>(child_idx % kMerkleEntriesPerHashBlock);
            st = ReadRawContainerBytes(level_offsets[level] + block_idx * kSectorSize, hash_page.data(), kSectorSize);
            if (!st.ok()) return st;
            if (std::memcmp(hash_page.data() + entry_idx * kMerkleHashEntrySize,
                            child_digest, kMerkleHashEntrySize) != 0) {
                return Status::Error(StatusCode::ParseError,
                                     "XVD contiguous Merkle hash tree verification failed at level " +
                                         std::to_string(level));
            }
            if (!crypto::ComputeSha256(hash_page.data(), kSectorSize, child_digest)) {
                return Status::Error(StatusCode::IoError, "CNG SHA-256 failed on XVD Merkle hash block");
            }
            child_idx = block_idx;
        }
        if (IsAnyByteNonZero(header_.top_hash_block_hash.data(), 32) &&
            std::memcmp(child_digest, header_.top_hash_block_hash.data(), 32) != 0) {
            return Status::Error(StatusCode::ParseError, "XVD TopHashBlockHash root digest mismatch");
        }
        return Status::Ok();
    }

    uint64_t child_idx = user_page_idx;
    std::array<uint8_t, kSectorSize> hash_page{};
    for (uint32_t level = 0; level < merkle_tree_depth_; ++level) {
        const uint64_t block_idx = child_idx / kMerkleEntriesPerHashBlock;
        const size_t entry_idx = static_cast<size_t>(child_idx % kMerkleEntriesPerHashBlock);
        const uint64_t hash_backing = HashBlockToBackingPage(level, block_idx, merkle_tree_depth_);
        st = ReadRawContainerBytes(base_data_offset_ + hash_backing * kSectorSize, hash_page.data(), kSectorSize);
        if (!st.ok()) return st;

        if (std::memcmp(hash_page.data() + entry_idx * kMerkleHashEntrySize,
                        child_digest, kMerkleHashEntrySize) != 0) {
            return Status::Error(StatusCode::ParseError, "XVD Merkle hash tree verification failed at level " + std::to_string(level));
        }

        if (!crypto::ComputeSha256(hash_page.data(), kSectorSize, child_digest)) {
            return Status::Error(StatusCode::IoError, "CNG SHA-256 failed on XVD Merkle hash block");
        }
        child_idx = block_idx;
    }

    if (IsAnyByteNonZero(header_.top_hash_block_hash.data(), 32)) {
        if (std::memcmp(child_digest, header_.top_hash_block_hash.data(), 32) != 0) {
            return Status::Error(StatusCode::ParseError, "XVD TopHashBlockHash root digest mismatch");
        }
    }
    return Status::Ok();
}

Status XvdContainer::ReadUserData(
    uint64_t offset,
    uint64_t size,
    std::vector<uint8_t>& out_data) const {
    if (!header_.valid) {
        return Status::Error(StatusCode::InvalidArgument, "XVD container is not open");
    }
    if (offset > header_.user_data_length || size > header_.user_data_length - offset) {
        return Status::Error(StatusCode::InvalidArgument, "XVD read out of user-data bounds");
    }

    // Fast rejection before resizing out_data when container has no regions and is encrypted without a key.
    if (header_.is_encrypted && regions_.empty() && !HasVolumeKey()) {
        return Status::Error(
            StatusCode::NotImplemented,
            "Encrypted retail XVD images require a volume key (supply --volume-key or call SetVolumeKey)");
    }

    out_data.resize(static_cast<size_t>(size));
    if (size == 0) {
        return Status::Ok();
    }

    // Fast contiguous raw read path ONLY for unencrypted containers without Merkle interleaving.
    if (!header_.is_encrypted && (!header_.is_retail_v3_layout || !header_.has_data_integrity)) {
        return ReadRawContainerBytes(base_data_offset_ + offset, out_data.data(), static_cast<size_t>(size));
    }

    auto resolve_sector_key = [&](uint64_t sector_byte_start, const crypto::AesXtsPreparedKey*& out_key) -> Status {
        out_key = nullptr;
        if (!header_.is_encrypted) {
            return Status::Ok();
        }
        for (const auto& reg : regions_) {
            if (sector_byte_start >= reg.user_data_offset &&
                sector_byte_start < reg.user_data_offset + reg.length) {
                if (reg.key_id == kUnencryptedKeyId) {
                    return Status::Ok();  // Unencrypted region (KeyId == 0xFFFF)
                }
                auto it = cik_keys_.find(reg.key_id);
                if (it != cik_keys_.end() && it->second) {
                    out_key = it->second.get();
                    return Status::Ok();
                }
                if (reg.key_id == 0u && prepared_key_) {
                    out_key = prepared_key_.get();
                    return Status::Ok();
                }
                return Status::Error(
                    StatusCode::NotImplemented,
                    "Missing CIK key for encrypted XVC region slot " + std::to_string(reg.key_id));
            }
        }
        if (prepared_key_) {
            out_key = prepared_key_.get();
            return Status::Ok();
        }
        auto it0 = cik_keys_.find(0);
        if (it0 != cik_keys_.end() && it0->second) {
            out_key = it0->second.get();
            return Status::Ok();
        }
        return Status::Error(
            StatusCode::NotImplemented,
            "Encrypted retail XVD images require a volume key (supply --volume-key or call SetVolumeKey)");
    };

    const uint64_t first_sector = offset / kSectorSize;
    const uint64_t last_sector = (offset + size - 1ull) / kSectorSize;
    std::array<uint8_t, kSectorSize> raw_sector{};
    std::array<uint8_t, kSectorSize> plain_sector{};

    for (uint64_t sector = first_sector; sector <= last_sector; ++sector) {
        const uint64_t sector_start = sector * kSectorSize;
        const size_t sector_avail = static_cast<size_t>(
            (std::min)(uint64_t{kSectorSize}, header_.user_data_length - sector_start));

        const crypto::AesXtsPreparedKey* sector_key = nullptr;
        Status st = resolve_sector_key(sector_start, sector_key);
        if (!st.ok()) {
            return st;
        }

        if (verify_integrity_on_read_ && header_.is_retail_v3_layout && header_.has_data_integrity) {
            st = VerifyDataPageHash(sector);
            if (!st.ok()) {
                return st;
            }
        }

        const size_t phys_read_len = header_.is_retail_v3_layout ? kSectorSize : sector_avail;
        st = ReadRawContainerBytes(LogicalUserPageToPhysicalOffset(sector), raw_sector.data(), phys_read_len);
        if (!st.ok()) {
            return st;
        }

        const uint8_t* src_sector = raw_sector.data();
        if (sector_key != nullptr) {
            const uint8_t* tweak_seed = header_.is_retail_v3_layout ? header_.content_id.data() : nullptr;
            const size_t xts_len = header_.is_retail_v3_layout ? kSectorSize : sector_avail;
            if (!sector_key->TransformSectorWithTweakSeed(
                    false, sector, tweak_seed, raw_sector.data(), plain_sector.data(), xts_len)) {
                return Status::Error(StatusCode::IoError, "CNG AES-XTS sector decryption failed");
            }
            src_sector = plain_sector.data();
        }

        const uint64_t copy_start = (std::max)(offset, sector_start);
        const uint64_t copy_end = (std::min)(offset + size, sector_start + sector_avail);
        const size_t src_off = static_cast<size_t>(copy_start - sector_start);
        const size_t dst_off = static_cast<size_t>(copy_start - offset);
        const size_t copy_len = static_cast<size_t>(copy_end - copy_start);
        std::memcpy(out_data.data() + dst_off, src_sector + src_off, copy_len);
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Embedded NTFS & GDFX Filesystem Discovery, Multi-Run Reading & 4 MiB Extraction
// ---------------------------------------------------------------------------

Status XvdContainer::EnsureFilesystemParsed() const {
    std::lock_guard<std::mutex> lk(fs_cache_mu_);
    if (fs_cached_) {
        return Status::Ok();
    }
    if (!header_.valid) {
        return Status::Error(StatusCode::InvalidArgument, "XVD container is not open");
    }

    std::vector<XvdEmbeddedFile> parsed_entries;
    std::unordered_map<std::string, CachedEmbeddedFileInfo> parsed_lookup;

    // 1. Check Xbox GDFX ("MICROSOFT*XBOX*MEDIA") volume descriptor first.
    bool found_gdfx = false;
    std::vector<uint8_t> gdfx_desc;
    for (uint64_t candidate : {kGdfxDescriptorOffset, uint64_t{0}}) {
        if (header_.user_data_length >= candidate + kGdfxSectorSize) {
            if (ReadUserData(candidate, kGdfxSectorSize, gdfx_desc).ok() &&
                std::memcmp(gdfx_desc.data(), kGdfxMagic, 20) == 0 &&
                std::memcmp(gdfx_desc.data() + 2028, kGdfxMagic, 20) == 0) {
                found_gdfx = true;
                break;
            }
        }
    }

    if (found_gdfx) {
        const uint32_t root_sector = ReadU32LE(gdfx_desc.data() + 20);
        const uint32_t root_size = ReadU32LE(gdfx_desc.data() + 24);
        if (root_size == 0 || root_size > 64u * 1024u * 1024u) {
            return Status::Error(StatusCode::ParseError, "Invalid GDFX root directory size");
        }

        struct GdfxDirWork {
            uint32_t sector;
            uint32_t size;
            std::string prefix;
            uint32_t depth;
        };
        std::unordered_set<uint32_t> visited_dir_sectors;
        std::vector<GdfxDirWork> dir_stack;
        dir_stack.push_back({root_sector, root_size, "", 0});

        while (!dir_stack.empty()) {
            const GdfxDirWork cur_dir = std::move(dir_stack.back());
            dir_stack.pop_back();

            if (cur_dir.depth > 32u) {
                return Status::Error(StatusCode::ParseError, "GDFX directory depth limit exceeded");
            }
            if (!visited_dir_sectors.insert(cur_dir.sector).second) {
                return Status::Error(StatusCode::ParseError, "Cycle detected in GDFX directory sectors");
            }

            const uint64_t dir_byte_off = static_cast<uint64_t>(cur_dir.sector) * kGdfxSectorSize;
            if (dir_byte_off > header_.user_data_length || cur_dir.size > header_.user_data_length - dir_byte_off) {
                return Status::Error(StatusCode::ParseError, "GDFX directory out of XVD user-data bounds");
            }

            std::vector<uint8_t> dir_bytes;
            Status st = ReadUserData(dir_byte_off, cur_dir.size, dir_bytes);
            if (!st.ok()) return st;
            if (dir_bytes.size() < 14u) continue;
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
                if (name_len == 0 || name_len == 0xFFu) continue;

                const size_t rec_end = (byte_off + 14u + name_len + 3u) & ~size_t{3u};
                if (byte_off + 14u + name_len > dir_bytes.size() || rec_end > dir_bytes.size()) {
                    return Status::Error(StatusCode::ParseError, "GDFX directory entry filename exceeds table bounds");
                }

                const std::string raw_name(reinterpret_cast<const char*>(dir_bytes.data() + byte_off + 14), name_len);
                std::vector<std::string> name_parts;
                if (!SplitSafeRelativePath(raw_name, name_parts) || name_parts.size() != 1u) {
                    return Status::Error(StatusCode::ParseError, "Unsafe filename in GDFX directory entry");
                }

                const std::string rel_path = cur_dir.prefix.empty() ? name_parts[0] : (cur_dir.prefix + "/" + name_parts[0]);
                const uint64_t data_off = static_cast<uint64_t>(start_sector) * kGdfxSectorSize;
                if (entry_size > 0 &&
                    (data_off > header_.user_data_length || entry_size > header_.user_data_length - data_off)) {
                    return Status::Error(StatusCode::ParseError, "GDFX file payload out of XVD user-data bounds");
                }

                const bool is_dir = (attr & 0x10u) != 0;
                XvdEmbeddedFile meta{rel_path, data_off, entry_size, is_dir};
                parsed_entries.push_back(meta);
                if (!is_dir) {
                    CachedEmbeddedFileInfo info{};
                    info.meta = meta;
                    info.is_resident = false;
                    info.initialized_size = entry_size;
                    if (entry_size > 0) {
                        info.extents.push_back(FileExtent{data_off, entry_size, false});
                    }
                    parsed_lookup[NormalizeRelativeLookupKey(rel_path)] = std::move(info);
                }

                if (is_dir && entry_size > 0) {
                    dir_stack.push_back({start_sector, entry_size, rel_path, cur_dir.depth + 1u});
                }
                if (right_dw != 0xFFFFu) node_stack.push_back(right_dw);
                if (left_dw != 0xFFFFu) node_stack.push_back(left_dw);
            }
        }

        cached_fs_entries_ = std::move(parsed_entries);
        cached_fs_lookup_ = std::move(parsed_lookup);
        fs_cached_ = true;
        return Status::Ok();
    }

    // 2. Check Gen9 NTFS volume at user-data offset 0 or MBR / GPT partition entry.
    NtfsBpb bpb{};
    bool found_ntfs = false;
    std::vector<uint8_t> sec0;
    if (header_.user_data_length >= 512) {
        Status st = ReadUserData(0, (std::min)(header_.user_data_length, uint64_t{4096}), sec0);
        if (!st.ok()) return st;

        if (TryParseNtfsBootSector(sec0.data(), sec0.size(), 0, header_.user_data_length, bpb)) {
            found_ntfs = true;
        } else if (sec0.size() >= 512 && sec0[510] == 0x55u && sec0[511] == 0xAAu) {
            std::vector<uint64_t> candidates;
            // Check GPT header at LBA 1 (512B and 4096B sector sizes).
            for (uint64_t lba_sz : {uint64_t{512}, uint64_t{4096}}) {
                if (header_.user_data_length >= lba_sz * 3ull) {
                    std::vector<uint8_t> gpt_hdr;
                    if (ReadUserData(lba_sz, 512, gpt_hdr).ok() &&
                        std::memcmp(gpt_hdr.data(), "EFI PART", 8) == 0) {
                        const uint64_t entry_lba = ReadU64LE(gpt_hdr.data() + 0x48);
                        const uint32_t num_entries = (std::min)(ReadU32LE(gpt_hdr.data() + 0x50), 128u);
                        const uint32_t entry_sz = ReadU32LE(gpt_hdr.data() + 0x54);
                        if (entry_sz >= 128u && entry_sz <= 1024u) {
                            const uint64_t table_off = entry_lba * lba_sz;
                            const uint64_t table_bytes = static_cast<uint64_t>(num_entries) * entry_sz;
                            if (table_off <= header_.user_data_length &&
                                table_bytes <= header_.user_data_length - table_off) {
                                std::vector<uint8_t> gpt_tbl;
                                if (ReadUserData(table_off, table_bytes, gpt_tbl).ok()) {
                                    for (uint32_t e = 0; e < num_entries; ++e) {
                                        const uint8_t* ep = gpt_tbl.data() + static_cast<size_t>(e) * entry_sz;
                                        if (!IsAnyByteNonZero(ep, 16)) continue;
                                        const uint64_t first_lba = ReadU64LE(ep + 0x20);
                                        if (first_lba > 0 && first_lba <= header_.user_data_length / lba_sz) {
                                            candidates.push_back(first_lba * lba_sz);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // Check MBR 4 primary partition entries at 0x1BE.
            for (size_t p = 0; p < 4u; ++p) {
                const uint32_t start_lba = ReadU32LE(sec0.data() + 0x1BEu + p * 16u + 8u);
                if (start_lba > 0) {
                    candidates.push_back(static_cast<uint64_t>(start_lba) * 512ull);
                    candidates.push_back(static_cast<uint64_t>(start_lba) * 4096ull);
                }
            }
            for (uint64_t cand : candidates) {
                if (cand + 512ull <= header_.user_data_length) {
                    std::vector<uint8_t> boot_cand;
                    if (ReadUserData(cand, 512, boot_cand).ok() &&
                        TryParseNtfsBootSector(boot_cand.data(), boot_cand.size(), cand, header_.user_data_length, bpb)) {
                        found_ntfs = true;
                        break;
                    }
                }
            }
        }
    }

    if (!found_ntfs) {
        return Status::Error(StatusCode::UnsupportedFormat, "No embedded NTFS or GDFX filesystem found in XVD user-data");
    }

    auto read_from_runs = [&](const std::vector<NtfsRunExtent>& runs,
                              uint64_t stream_off,
                              uint64_t req_size,
                              std::vector<uint8_t>& out_buf,
                              uint64_t* out_direct_user_off = nullptr) -> Status {
        out_buf.assign(static_cast<size_t>(req_size), 0);
        if (out_direct_user_off) *out_direct_user_off = 0;
        if (req_size == 0) return Status::Ok();

        uint64_t cur_logical = 0;
        uint64_t written = 0;
        bool single_direct_set = false;
        for (const auto& run : runs) {
            const uint64_t run_end = cur_logical + run.byte_length;
            if (run_end > stream_off && cur_logical < stream_off + req_size) {
                const uint64_t ov_start = (std::max)(stream_off, cur_logical);
                const uint64_t ov_end = (std::min)(stream_off + req_size, run_end);
                const uint64_t ov_len = ov_end - ov_start;
                const size_t dst_pos = static_cast<size_t>(ov_start - stream_off);

                if (!run.is_sparse) {
                    const uint64_t src_user_off = run.user_offset + (ov_start - cur_logical);
                    if (out_direct_user_off && ov_start == stream_off && ov_len == req_size && !single_direct_set) {
                        *out_direct_user_off = src_user_off;
                        single_direct_set = true;
                    }
                    std::vector<uint8_t> slice;
                    Status st = ReadUserData(src_user_off, ov_len, slice);
                    if (!st.ok()) return st;
                    std::memcpy(out_buf.data() + dst_pos, slice.data(), slice.size());
                }
                written += ov_len;
                if (written >= req_size) break;
            }
            cur_logical = run_end;
        }
        if (written < req_size) {
            return Status::Error(StatusCode::ParseError, "Short NTFS run-list stream read");
        }
        return Status::Ok();
    };

    // Parse MFT Record #0 ($MFT) so fragmented $MFT tables work seamlessly.
    std::vector<NtfsRunExtent> mft_runs;
    {
        const uint64_t mft0_off = bpb.vol_offset + bpb.mft_lcn * bpb.cluster_size;
        std::vector<uint8_t> mft0;
        Status st = ReadUserData(mft0_off, bpb.mft_record_size, mft0);
        if (!st.ok()) return st;
        size_t usa_stride = 512;
        st = ApplyNtfsUsaFixup(mft0.data(), mft0.size(), "FILE", usa_stride);
        if (!st.ok()) return st;

        const uint16_t attr_off0 = ReadU16LE(mft0.data() + 0x14);
        const uint32_t used0 = (std::min)(ReadU32LE(mft0.data() + 0x18), bpb.mft_record_size);
        size_t pos = attr_off0;
        while (pos + 16u <= used0) {
            const uint32_t atype = ReadU32LE(mft0.data() + pos);
            if (atype == kNtfsAttrEnd) break;
            const uint32_t alen = ReadU32LE(mft0.data() + pos + 4);
            if (alen < 16u || pos + alen > used0) break;
            const uint8_t non_res = mft0[pos + 8];
            const uint8_t nlen = mft0[pos + 9];
            if (atype == kNtfsAttrData && non_res == 1u && nlen == 0u && alen >= 64u) {
                const uint16_t run_off = ReadU16LE(mft0.data() + pos + 32);
                if (run_off < alen) {
                    st = DecodeNtfsDataRuns(
                        mft0.data() + pos + run_off,
                        mft0.data() + pos + alen,
                        bpb.vol_offset,
                        bpb.cluster_size,
                        header_.user_data_length,
                        mft_runs);
                    if (!st.ok()) return st;
                }
            }
            pos += alen;
        }
    }

    auto read_mft_record = [&](uint64_t mft_num,
                               std::vector<uint8_t>& out_rec,
                               uint64_t& out_raw_user_off,
                               size_t& out_usa_stride) -> Status {
        out_raw_user_off = 0;
        const uint64_t byte_index = mft_num * static_cast<uint64_t>(bpb.mft_record_size);
        Status st;
        if (!mft_runs.empty()) {
            st = read_from_runs(mft_runs, byte_index, bpb.mft_record_size, out_rec, &out_raw_user_off);
        } else {
            const uint64_t direct_off = bpb.vol_offset + bpb.mft_lcn * bpb.cluster_size + byte_index;
            if (direct_off > header_.user_data_length || bpb.mft_record_size > header_.user_data_length - direct_off) {
                return Status::Error(StatusCode::ParseError, "NTFS MFT record out of volume bounds");
            }
            out_raw_user_off = direct_off;
            st = ReadUserData(direct_off, bpb.mft_record_size, out_rec);
        }
        if (!st.ok()) return st;
        return ApplyNtfsUsaFixup(out_rec.data(), out_rec.size(), "FILE", out_usa_stride);
    };

    struct NtfsChildEntry {
        uint64_t mft_num = 0;
        std::string name;
        uint8_t name_namespace = 0;
        bool is_dir_hint = false;
    };

    auto parse_index_entries = [&](const uint8_t* node_hdr,
                                   size_t max_node_bytes,
                                   std::vector<NtfsChildEntry>& out_children,
                                   std::vector<uint64_t>& out_subnode_vcns) -> Status {
        if (max_node_bytes < 16u) {
            return Status::Error(StatusCode::ParseError, "NTFS INDEX_HEADER truncated");
        }
        const uint32_t entries_off = ReadU32LE(node_hdr + 0);
        const uint32_t index_len = ReadU32LE(node_hdr + 4);
        if (entries_off < 16u || index_len < entries_off || index_len > max_node_bytes) {
            return Status::Error(StatusCode::ParseError, "Invalid NTFS INDEX_HEADER entry bounds");
        }
        size_t cur = entries_off;
        while (cur + 16u <= index_len) {
            const uint64_t mft_ref = ReadU64LE(node_hdr + cur + 0);
            const uint16_t entry_len = ReadU16LE(node_hdr + cur + 8);
            const uint16_t key_len = ReadU16LE(node_hdr + cur + 10);
            const uint16_t entry_flags = ReadU16LE(node_hdr + cur + 12);
            if (entry_len < 16u || cur + entry_len > index_len) {
                return Status::Error(StatusCode::ParseError, "Invalid NTFS INDEX_ENTRY size");
            }

            // Always inspect INDEX_ENTRY_NODE (0x01) BEFORE breaking on INDEX_ENTRY_END (0x02)!
            if ((entry_flags & 0x0001u) != 0) {
                if (entry_len < 24u) {
                    return Status::Error(StatusCode::ParseError, "Truncated NTFS INDEX_ENTRY subnode VCN");
                }
                const uint64_t sub_vcn = ReadU64LE(node_hdr + cur + entry_len - 8u);
                out_subnode_vcns.push_back(sub_vcn);
            }

            if ((entry_flags & 0x0002u) != 0) {
                break;
            }

            if (key_len >= 66u && 16u + static_cast<size_t>(key_len) <= entry_len) {
                const uint8_t* fn = node_hdr + cur + 16u;
                const uint32_t fn_flags = ReadU32LE(fn + 0x38);
                const uint8_t fn_chars = fn[0x40];
                const uint8_t fn_ns = fn[0x41];
                if (0x42u + static_cast<size_t>(fn_chars) * 2u <= key_len && fn_chars > 0) {
                    NtfsChildEntry child{};
                    child.mft_num = mft_ref & 0x0000FFFFFFFFFFFFull;
                    child.name = Utf16LeToUtf8(fn + 0x42u, fn_chars);
                    child.name_namespace = fn_ns;
                    child.is_dir_hint = (fn_flags & 0x10000000u) != 0 || (fn_flags & 0x10u) != 0;
                    out_children.push_back(std::move(child));
                }
            }

            cur += entry_len;
        }
        return Status::Ok();
    };

    struct NtfsDirWork {
        uint64_t mft_num;
        std::string prefix;
        uint32_t depth;
    };
    std::unordered_set<uint64_t> visited_dir_mfts;
    std::vector<NtfsDirWork> dir_stack;
    dir_stack.push_back({5ull, "", 0u});  // Root directory "." is MFT record #5

    while (!dir_stack.empty()) {
        const NtfsDirWork cur_dir = std::move(dir_stack.back());
        dir_stack.pop_back();

        if (cur_dir.depth > 64u) {
            return Status::Error(StatusCode::ParseError, "NTFS directory depth limit exceeded");
        }
        if (!visited_dir_mfts.insert(cur_dir.mft_num).second) {
            continue;
        }

        std::vector<uint8_t> dir_rec;
        uint64_t dir_raw_off = 0;
        size_t dir_usa_stride = 512;
        Status st = read_mft_record(cur_dir.mft_num, dir_rec, dir_raw_off, dir_usa_stride);
        if (!st.ok()) return st;

        const uint16_t rec_flags = ReadU16LE(dir_rec.data() + 0x16);
        if ((rec_flags & 0x0001u) == 0 || (rec_flags & 0x0002u) == 0) {
            continue;
        }

        std::vector<NtfsChildEntry> raw_children;
        std::vector<uint64_t> pending_vcns;
        std::vector<NtfsRunExtent> index_alloc_runs;
        uint32_t dir_idx_block_size = bpb.index_block_size;

        const uint16_t first_attr = ReadU16LE(dir_rec.data() + 0x14);
        const uint32_t used_bytes = (std::min)(ReadU32LE(dir_rec.data() + 0x18), bpb.mft_record_size);
        size_t pos = first_attr;
        while (pos + 16u <= used_bytes) {
            const uint32_t atype = ReadU32LE(dir_rec.data() + pos);
            if (atype == kNtfsAttrEnd) break;
            const uint32_t alen = ReadU32LE(dir_rec.data() + pos + 4);
            if (alen < 16u || pos + alen > used_bytes) break;
            const uint8_t non_res = dir_rec[pos + 8];

            if (atype == kNtfsAttrIndexRoot && non_res == 0u && alen >= 24u) {
                const uint32_t val_len = ReadU32LE(dir_rec.data() + pos + 16);
                const uint16_t val_off = ReadU16LE(dir_rec.data() + pos + 20);
                if (val_off <= alen && val_len <= alen - val_off && val_len >= 32u) {
                    const uint8_t* ir = dir_rec.data() + pos + val_off;
                    const uint32_t indexed_type = ReadU32LE(ir + 0);
                    if (indexed_type == kNtfsAttrFileName) {
                        const uint32_t ir_blk_sz = ReadU32LE(ir + 8);
                        if (ir_blk_sz >= 512u && ir_blk_sz <= 65536u) {
                            dir_idx_block_size = ir_blk_sz;
                        }
                        st = parse_index_entries(ir + 16u, val_len - 16u, raw_children, pending_vcns);
                        if (!st.ok()) return st;
                    }
                }
            } else if (atype == kNtfsAttrIndexAllocation && non_res == 1u && alen >= 64u) {
                const uint16_t run_off = ReadU16LE(dir_rec.data() + pos + 32);
                if (run_off < alen) {
                    st = DecodeNtfsDataRuns(
                        dir_rec.data() + pos + run_off,
                        dir_rec.data() + pos + alen,
                        bpb.vol_offset,
                        bpb.cluster_size,
                        header_.user_data_length,
                        index_alloc_runs);
                    if (!st.ok()) return st;
                }
            }
            pos += alen;
        }

        // Traverse $INDEX_ALLOCATION INDX B-tree subnodes with VCN cycle protection.
        std::unordered_set<uint64_t> visited_vcns;
        const uint64_t vcn_unit = (dir_idx_block_size >= bpb.cluster_size)
                                      ? bpb.cluster_size
                                      : static_cast<uint64_t>(bpb.bytes_per_sector);
        while (!pending_vcns.empty()) {
            const uint64_t vcn = pending_vcns.back();
            pending_vcns.pop_back();
            if (!visited_vcns.insert(vcn).second) {
                continue;
            }
            if (index_alloc_runs.empty()) {
                return Status::Error(StatusCode::ParseError, "NTFS subnode VCN present without $INDEX_ALLOCATION");
            }
            std::vector<uint8_t> indx_buf;
            st = read_from_runs(index_alloc_runs, vcn * vcn_unit, dir_idx_block_size, indx_buf);
            if (!st.ok()) return st;

            size_t indx_stride = 512;
            st = ApplyNtfsUsaFixup(indx_buf.data(), indx_buf.size(), "INDX", indx_stride);
            if (!st.ok()) return st;
            if (indx_buf.size() < 0x28u) {
                return Status::Error(StatusCode::ParseError, "Truncated NTFS INDX header");
            }
            st = parse_index_entries(indx_buf.data() + 0x18u, indx_buf.size() - 0x18u, raw_children, pending_vcns);
            if (!st.ok()) return st;
        }

        // Suppress DOS 8.3 namespace (2) duplicates when a Win32/POSIX name exists for the same MFT record.
        std::unordered_set<uint64_t> mfts_with_long_name;
        for (const auto& ch : raw_children) {
            if (ch.name_namespace != 2u) {
                mfts_with_long_name.insert(ch.mft_num);
            }
        }

        std::unordered_set<std::string> seen_names_in_dir;
        for (const auto& ch : raw_children) {
            if (ch.mft_num < 16ull) continue;  // Skip NTFS internal metadata files ($MFT, $Boot, ., $Extend, etc.)
            if (cur_dir.mft_num == 5ull && !ch.name.empty() && ch.name[0] == '$' && ch.mft_num < 24ull) {
                continue;
            }
            if (ch.name == "." || ch.name == "..") continue;
            if (ch.name_namespace == 2u && mfts_with_long_name.count(ch.mft_num) > 0) {
                continue;
            }

            std::vector<std::string> name_parts;
            if (!SplitSafeRelativePath(ch.name, name_parts) || name_parts.size() != 1u) {
                continue;
            }
            const std::string lower_name = ToLowerAscii(name_parts[0]);
            if (!seen_names_in_dir.insert(lower_name).second) {
                continue;
            }

            // Inspect child MFT record for authoritative flags and $DATA attribute(s).
            std::vector<uint8_t> child_rec;
            uint64_t child_raw_off = 0;
            size_t child_usa_stride = 512;
            st = read_mft_record(ch.mft_num, child_rec, child_raw_off, child_usa_stride);
            if (!st.ok()) return st;

            const uint16_t child_flags = ReadU16LE(child_rec.data() + 0x16);
            if ((child_flags & 0x0001u) == 0) continue;  // Deleted / unused record
            const bool is_dir = (child_flags & 0x0002u) != 0;

            const std::string rel_path = cur_dir.prefix.empty() ? name_parts[0] : (cur_dir.prefix + "/" + name_parts[0]);
            if (is_dir) {
                parsed_entries.push_back(XvdEmbeddedFile{rel_path, 0, 0, true});
                dir_stack.push_back({ch.mft_num, rel_path, cur_dir.depth + 1u});
                continue;
            }

            // Parse unnamed $DATA (0x80) attribute(s), including $ATTRIBUTE_LIST (0x20) extension records if present.
            CachedEmbeddedFileInfo file_info{};
            bool found_data_attr = false;

            auto parse_data_attrs_from_record = [&](const std::vector<uint8_t>& rec_buf,
                                                    uint64_t rec_raw_user_off,
                                                    size_t rec_usa_stride,
                                                    std::vector<uint64_t>* out_ext_mfts) -> Status {
                const uint16_t a_start = ReadU16LE(rec_buf.data() + 0x14);
                const uint32_t a_used = (std::min)(ReadU32LE(rec_buf.data() + 0x18), bpb.mft_record_size);
                size_t apos = a_start;
                while (apos + 16u <= a_used) {
                    const uint32_t atype = ReadU32LE(rec_buf.data() + apos);
                    if (atype == kNtfsAttrEnd) break;
                    const uint32_t alen = ReadU32LE(rec_buf.data() + apos + 4);
                    if (alen < 16u || apos + alen > a_used) break;
                    const uint8_t non_res = rec_buf[apos + 8];
                    const uint8_t nlen = rec_buf[apos + 9];

                    if (atype == kNtfsAttrAttributeList && out_ext_mfts != nullptr) {
                        std::vector<uint8_t> alist_bytes;
                        if (non_res == 0u && alen >= 24u) {
                            const uint32_t vlen = ReadU32LE(rec_buf.data() + apos + 16);
                            const uint16_t voff = ReadU16LE(rec_buf.data() + apos + 20);
                            if (voff <= alen && vlen <= alen - voff) {
                                alist_bytes.assign(rec_buf.data() + apos + voff, rec_buf.data() + apos + voff + vlen);
                            }
                        } else if (non_res == 1u && alen >= 64u) {
                            const uint16_t roff = ReadU16LE(rec_buf.data() + apos + 32);
                            const uint64_t rsize = ReadU64LE(rec_buf.data() + apos + 48);
                            std::vector<NtfsRunExtent> aruns;
                            if (roff < alen && rsize <= (1ull << 20) &&
                                DecodeNtfsDataRuns(rec_buf.data() + apos + roff, rec_buf.data() + apos + alen,
                                                   bpb.vol_offset, bpb.cluster_size, header_.user_data_length, aruns).ok()) {
                                (void)read_from_runs(aruns, 0, rsize, alist_bytes);
                            }
                        }
                        size_t lpos = 0;
                        while (lpos + 26u <= alist_bytes.size()) {
                            const uint32_t latype = ReadU32LE(alist_bytes.data() + lpos);
                            const uint16_t lreclen = ReadU16LE(alist_bytes.data() + lpos + 4);
                            const uint8_t lnlen = alist_bytes[lpos + 6];
                            if (lreclen < 26u || lpos + lreclen > alist_bytes.size()) break;
                            if (latype == kNtfsAttrData && lnlen == 0u) {
                                const uint64_t ext_ref = ReadU64LE(alist_bytes.data() + lpos + 16) & 0x0000FFFFFFFFFFFFull;
                                if (ext_ref != ch.mft_num) {
                                    out_ext_mfts->push_back(ext_ref);
                                }
                            }
                            lpos += lreclen;
                        }
                    } else if (atype == kNtfsAttrData && nlen == 0u) {
                        if (non_res == 0u && alen >= 24u) {
                            const uint32_t vlen = ReadU32LE(rec_buf.data() + apos + 16);
                            const uint16_t voff = ReadU16LE(rec_buf.data() + apos + 20);
                            if (voff > alen || vlen > alen - voff) {
                                return Status::Error(StatusCode::ParseError, "Invalid NTFS resident $DATA bounds");
                            }
                            found_data_attr = true;
                            file_info.is_resident = true;
                            file_info.meta.size_bytes = vlen;
                            file_info.initialized_size = vlen;
                            file_info.resident_bytes.assign(
                                rec_buf.data() + apos + voff,
                                rec_buf.data() + apos + voff + vlen);

                            // Provide direct user_data_offset when resident bytes do not cross a USA fixup word.
                            const size_t rec_val_start = apos + voff;
                            const size_t rec_val_end = rec_val_start + vlen;
                            if (rec_raw_user_off != 0 &&
                                (vlen == 0 ||
                                 ((rec_val_start / rec_usa_stride) == ((rec_val_end - 1u) / rec_usa_stride) &&
                                  (rec_val_end % rec_usa_stride) != 0 &&
                                  (rec_val_end % rec_usa_stride) <= (rec_usa_stride - 2u)))) {
                                file_info.meta.user_data_offset = rec_raw_user_off + rec_val_start;
                            }
                        } else if (non_res == 1u && alen >= 64u) {
                            const uint64_t start_vcn = ReadU64LE(rec_buf.data() + apos + 16);
                            const uint16_t roff = ReadU16LE(rec_buf.data() + apos + 32);
                            const uint64_t real_sz = ReadU64LE(rec_buf.data() + apos + 48);
                            const uint64_t init_sz = ReadU64LE(rec_buf.data() + apos + 56);
                            if (roff >= alen) {
                                return Status::Error(StatusCode::ParseError, "Invalid NTFS non-resident $DATA run offset");
                            }
                            std::vector<NtfsRunExtent> druns;
                            Status rst = DecodeNtfsDataRuns(
                                rec_buf.data() + apos + roff,
                                rec_buf.data() + apos + alen,
                                bpb.vol_offset,
                                bpb.cluster_size,
                                header_.user_data_length,
                                druns);
                            if (!rst.ok()) return rst;

                            found_data_attr = true;
                            file_info.is_resident = false;
                            if (start_vcn == 0) {
                                file_info.meta.size_bytes = real_sz;
                                file_info.initialized_size = (std::min)(init_sz, real_sz);
                            }
                            for (const auto& dr : druns) {
                                file_info.extents.push_back(FileExtent{dr.user_offset, dr.byte_length, dr.is_sparse});
                            }
                        }
                    }
                    apos += alen;
                }
                return Status::Ok();
            };

            std::vector<uint64_t> ext_mfts;
            st = parse_data_attrs_from_record(child_rec, child_raw_off, child_usa_stride, &ext_mfts);
            if (!st.ok()) return st;

            std::unordered_set<uint64_t> visited_ext;
            for (uint64_t ext_num : ext_mfts) {
                if (!visited_ext.insert(ext_num).second) continue;
                std::vector<uint8_t> ext_rec;
                uint64_t ext_raw = 0;
                size_t ext_stride = 512;
                if (read_mft_record(ext_num, ext_rec, ext_raw, ext_stride).ok()) {
                    st = parse_data_attrs_from_record(ext_rec, ext_raw, ext_stride, nullptr);
                    if (!st.ok()) return st;
                }
            }

            if (!found_data_attr) {
                file_info.is_resident = true;
                file_info.meta.size_bytes = 0;
                file_info.initialized_size = 0;
            }
            if (!file_info.is_resident && !file_info.extents.empty() && !file_info.extents[0].is_sparse) {
                file_info.meta.user_data_offset = file_info.extents[0].user_offset;
            }
            file_info.meta.relative_path = rel_path;
            file_info.meta.is_directory = false;

            parsed_entries.push_back(file_info.meta);
            parsed_lookup[NormalizeRelativeLookupKey(rel_path)] = std::move(file_info);
        }
    }

    cached_fs_entries_ = std::move(parsed_entries);
    cached_fs_lookup_ = std::move(parsed_lookup);
    fs_cached_ = true;
    return Status::Ok();
}

bool XvdContainer::HasEmbeddedFilesystem() const {
    return EnsureFilesystemParsed().ok();
}

Status XvdContainer::ListEmbeddedFiles(std::vector<XvdEmbeddedFile>& out_files) const {
    out_files.clear();
    Status st = EnsureFilesystemParsed();
    if (!st.ok()) return st;
    std::lock_guard<std::mutex> lk(fs_cache_mu_);
    out_files = cached_fs_entries_;
    return Status::Ok();
}

Status XvdContainer::ReadEmbeddedFile(
    const std::string& relative_path,
    uint64_t offset,
    uint64_t size,
    std::vector<uint8_t>& out_data) const {
    const std::string target_key = NormalizeRelativeLookupKey(relative_path);
    if (target_key.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid embedded file path: " + relative_path);
    }

    Status st = EnsureFilesystemParsed();
    if (!st.ok()) return st;

    CachedEmbeddedFileInfo info;
    {
        std::lock_guard<std::mutex> lk(fs_cache_mu_);
        auto it = cached_fs_lookup_.find(target_key);
        if (it == cached_fs_lookup_.end()) {
            return Status::Error(StatusCode::NotFound, "Embedded file not found in XVD volume: " + relative_path);
        }
        info = it->second;
    }

    if (offset > info.meta.size_bytes || size > info.meta.size_bytes - offset) {
        return Status::Error(StatusCode::InvalidArgument, "Embedded file read range out of bounds: " + relative_path);
    }
    out_data.assign(static_cast<size_t>(size), 0);
    if (size == 0) {
        return Status::Ok();
    }

    if (info.is_resident) {
        if (offset + size > info.resident_bytes.size()) {
            return Status::Error(StatusCode::ParseError, "Resident NTFS file payload truncated");
        }
        std::memcpy(out_data.data(), info.resident_bytes.data() + static_cast<size_t>(offset), static_cast<size_t>(size));
        return Status::Ok();
    }

    // Clamp physical cluster reads to initialized_size; any tail in [initialized_size, size_bytes) stays 0.
    const uint64_t init_end = (std::min)(offset + size, info.initialized_size);
    if (init_end <= offset) {
        return Status::Ok();
    }
    const uint64_t need_bytes = init_end - offset;

    uint64_t cur_logical = 0;
    uint64_t copied = 0;
    std::vector<uint8_t> run_slice;
    for (const auto& ext : info.extents) {
        const uint64_t ext_end = cur_logical + ext.byte_length;
        if (ext_end > offset && cur_logical < init_end) {
            const uint64_t ov_start = (std::max)(offset, cur_logical);
            const uint64_t ov_end = (std::min)(init_end, ext_end);
            const uint64_t ov_len = ov_end - ov_start;
            const size_t dst_off = static_cast<size_t>(ov_start - offset);

            if (!ext.is_sparse) {
                const uint64_t src_user_off = ext.user_offset + (ov_start - cur_logical);
                st = ReadUserData(src_user_off, ov_len, run_slice);
                if (!st.ok()) return st;
                std::memcpy(out_data.data() + dst_off, run_slice.data(), run_slice.size());
            }
            copied += ov_len;
            if (copied >= need_bytes) break;
        }
        cur_logical = ext_end;
    }

    if (copied < need_bytes) {
        return Status::Error(StatusCode::ParseError, "NTFS non-resident data runs shorter than file size: " + relative_path);
    }
    return Status::Ok();
}

Status XvdContainer::ReadEmbeddedFile(const std::string& relative_path, std::vector<uint8_t>& out_data) const {
    const std::string target_key = NormalizeRelativeLookupKey(relative_path);
    if (target_key.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid embedded file path: " + relative_path);
    }
    Status st = EnsureFilesystemParsed();
    if (!st.ok()) return st;

    uint64_t total_size = 0;
    {
        std::lock_guard<std::mutex> lk(fs_cache_mu_);
        auto it = cached_fs_lookup_.find(target_key);
        if (it == cached_fs_lookup_.end()) {
            return Status::Error(StatusCode::NotFound, "Embedded file not found in XVD volume: " + relative_path);
        }
        total_size = it->second.meta.size_bytes;
    }
    return ReadEmbeddedFile(relative_path, 0, total_size, out_data);
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

    constexpr uint64_t kStreamChunkSize = 4ull * 1024ull * 1024ull;  // Bounded 4 MiB streaming buffer
    std::vector<uint8_t> chunk_buf;

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
        std::ofstream ofs(full_path, std::ios::binary);
        if (!ofs) {
            return Status::Error(StatusCode::IoError, "Failed to write extracted file: " + full_path.string());
        }

        uint64_t file_pos = 0;
        while (file_pos < entry.size_bytes) {
            const uint64_t to_read = (std::min)(kStreamChunkSize, entry.size_bytes - file_pos);
            st = ReadEmbeddedFile(entry.relative_path, file_pos, to_read, chunk_buf);
            if (!st.ok()) {
                return st;
            }
            ofs.write(reinterpret_cast<const char*>(chunk_buf.data()),
                      static_cast<std::streamsize>(chunk_buf.size()));
            if (!ofs) {
                return Status::Error(StatusCode::IoError, "Disk write error while extracting: " + full_path.string());
            }
            file_pos += to_read;
        }
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Synthetic Legacy, GDFX, Retail v3 & Clean-Room NTFS Builders
// ---------------------------------------------------------------------------

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
    WriteU32LE(img.data() + kOffLegacyContentType, content_type);
    WriteU32LE(img.data() + kOffLegacyEmbeddedXvdLen, 0);
    WriteU64LE(img.data() + kOffLegacyUserDataOffset, kMinHeaderSize);
    WriteU64LE(img.data() + kOffLegacyUserDataLength, user_data.size());

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
    const size_t effective_len = (volume_key && key_len >= 16) ? (std::min)(key_len, key.size()) : 32;
    if (volume_key && key_len > 0) {
        std::memcpy(key.data(), volume_key, (std::min)(key_len, key.size()));
    }
    crypto::AesXtsPreparedKey prep;
    prep.Init(key.data(), effective_len);
    std::vector<uint8_t> encrypted(user_data.size());
    const uint64_t num_sectors = (user_data.size() + kSectorSize - 1) / kSectorSize;
    for (uint64_t s = 0; s < num_sectors; ++s) {
        const size_t start = static_cast<size_t>(s * kSectorSize);
        const size_t len = (std::min)(kSectorSize, user_data.size() - start);
        prep.TransformSector(true, s, user_data.data() + start, encrypted.data() + start, len);
    }
    return BuildSyntheticXvd(encrypted, content_type, true);
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

std::vector<uint8_t> XvdContainer::BuildRetailV3XvcMultiKey(
    const std::vector<uint8_t>& user_data,
    const std::vector<std::vector<uint8_t>>& cik_keys,
    const std::vector<XvcRegionInfo>& regions,
    bool enable_merkle_tree,
    uint32_t content_type) {
    const bool is_encrypted = !cik_keys.empty();
    std::array<uint8_t, 16> vduid{};
    for (size_t i = 0; i < 16; ++i) {
        vduid[i] = static_cast<uint8_t>(0xB0u + i);
    }

    std::vector<std::unique_ptr<crypto::AesXtsPreparedKey>> prepped_keys;
    prepped_keys.reserve(cik_keys.size());
    for (const auto& k : cik_keys) {
        auto pk = std::make_unique<crypto::AesXtsPreparedKey>();
        if (k.size() >= 16) {
            pk->Init(k.data(), k.size());
        }
        prepped_keys.push_back(std::move(pk));
    }

    std::vector<XvcRegionInfo> eff_regions = regions;
    if (eff_regions.empty() && !user_data.empty()) {
        eff_regions.push_back(XvcRegionInfo{
            1u,
            is_encrypted ? uint16_t{0} : kUnencryptedKeyId,
            1u,
            0ull,
            static_cast<uint64_t>(user_data.size()),
        });
    }

    const size_t user_pages = (user_data.size() + kSectorSize - 1u) / kSectorSize;
    std::vector<uint8_t> padded_user(user_pages * kSectorSize, 0);
    if (!user_data.empty()) {
        std::memcpy(padded_user.data(), user_data.data(), user_data.size());
    }

    // Encrypt user-data 4 KiB pages according to XvcRegionHeader KeyId & VDUID tweak seed.
    if (is_encrypted) {
        for (size_t p = 0; p < user_pages; ++p) {
            const uint64_t page_byte_off = static_cast<uint64_t>(p) * kSectorSize;
            uint16_t key_slot = 0u;
            for (const auto& reg : eff_regions) {
                if (page_byte_off >= reg.user_data_offset &&
                    page_byte_off < reg.user_data_offset + reg.length) {
                    key_slot = reg.key_id;
                    break;
                }
            }
            if (key_slot == kUnencryptedKeyId) {
                continue;
            }
            if (key_slot < prepped_keys.size() && prepped_keys[key_slot]) {
                uint8_t cipher_page[kSectorSize]{};
                prepped_keys[key_slot]->TransformSectorWithTweakSeed(
                    true,
                    static_cast<uint64_t>(p),
                    vduid.data(),
                    padded_user.data() + p * kSectorSize,
                    cipher_page,
                    kSectorSize);
                std::memcpy(padded_user.data() + p * kSectorSize, cipher_page, kSectorSize);
            }
        }
    }

    // Build XvcInfo + XvcRegionHeader[] table (4 KiB page-aligned).
    const size_t raw_xvc_len = kXvcInfoSize + eff_regions.size() * kXvcRegionHeaderSize;
    const size_t xvc_pages = (raw_xvc_len + kSectorSize - 1u) / kSectorSize;
    std::vector<uint8_t> xvc_bytes(xvc_pages * kSectorSize, 0);
    std::memcpy(xvc_bytes.data() + 0x00, vduid.data(), 16);
    WriteU32LE(xvc_bytes.data() + kXvcInfoOffVersion, 2u);
    WriteU32LE(xvc_bytes.data() + kXvcInfoOffRegionCount, static_cast<uint32_t>(eff_regions.size()));
    WriteU16LE(xvc_bytes.data() + kXvcInfoOffKeyCount, static_cast<uint16_t>(cik_keys.size()));
    for (size_t r = 0; r < eff_regions.size(); ++r) {
        uint8_t* rp = xvc_bytes.data() + kXvcInfoSize + r * kXvcRegionHeaderSize;
        WriteU32LE(rp + 0x00, eff_regions[r].id);
        WriteU16LE(rp + 0x04, eff_regions[r].key_id);
        WriteU32LE(rp + 0x08, eff_regions[r].flags);
        WriteU64LE(rp + 0x20, eff_regions[r].user_data_offset);
        WriteU64LE(rp + 0x28, eff_regions[r].length);
    }

    uint8_t orig_xvc_sha[32]{};
    crypto::ComputeSha256(xvc_bytes.data(), xvc_bytes.size(), orig_xvc_sha);

    const uint64_t total_data_pages = static_cast<uint64_t>(user_pages + xvc_pages);
    const uint32_t tree_depth = enable_merkle_tree ? CalculateMerkleTreeDepth(total_data_pages) : 0u;
    const uint64_t total_backing_pages =
        (total_data_pages == 0)
            ? 0ull
            : (enable_merkle_tree ? (DataPageToBackingPage(total_data_pages - 1ull, tree_depth) + 1ull)
                                  : total_data_pages);

    const size_t img_size = kRetailHeaderSize + static_cast<size_t>(total_backing_pages) * kSectorSize;
    std::vector<uint8_t> img(img_size, 0);

    // Place logical data pages (UserData followed by XvcData) into interleaved backing slots.
    for (size_t p = 0; p < user_pages; ++p) {
        const uint64_t bpage = enable_merkle_tree ? DataPageToBackingPage(p, tree_depth) : static_cast<uint64_t>(p);
        std::memcpy(
            img.data() + kRetailHeaderSize + static_cast<size_t>(bpage) * kSectorSize,
            padded_user.data() + p * kSectorSize,
            kSectorSize);
    }
    for (size_t p = 0; p < xvc_pages; ++p) {
        const uint64_t lpage = static_cast<uint64_t>(user_pages + p);
        const uint64_t bpage = enable_merkle_tree ? DataPageToBackingPage(lpage, tree_depth) : lpage;
        std::memcpy(
            img.data() + kRetailHeaderSize + static_cast<size_t>(bpage) * kSectorSize,
            xvc_bytes.data() + p * kSectorSize,
            kSectorSize);
    }

    // Populate Merkle SHA-256 hash blocks bottom-up (L0 -> L1 -> ... -> TopHashBlockHash).
    uint8_t top_hash_sha[32]{};
    if (enable_merkle_tree && tree_depth > 0 && total_data_pages > 0) {
        uint64_t child_count = total_data_pages;
        for (uint32_t level = 0; level < tree_depth; ++level) {
            const uint64_t parent_blocks =
                (child_count + kMerkleEntriesPerHashBlock - 1ull) / kMerkleEntriesPerHashBlock;
            for (uint64_t c = 0; c < child_count; ++c) {
                const uint64_t child_backing =
                    (level == 0) ? DataPageToBackingPage(c, tree_depth)
                                 : HashBlockToBackingPage(level - 1u, c, tree_depth);
                const uint8_t* child_ptr =
                    img.data() + kRetailHeaderSize + static_cast<size_t>(child_backing) * kSectorSize;
                uint8_t digest[32]{};
                crypto::ComputeSha256(child_ptr, kSectorSize, digest);

                const uint64_t pblock = c / kMerkleEntriesPerHashBlock;
                const size_t pentry = static_cast<size_t>(c % kMerkleEntriesPerHashBlock);
                const uint64_t parent_backing = HashBlockToBackingPage(level, pblock, tree_depth);
                uint8_t* parent_ptr =
                    img.data() + kRetailHeaderSize + static_cast<size_t>(parent_backing) * kSectorSize;
                std::memcpy(parent_ptr + pentry * kMerkleHashEntrySize, digest, kMerkleHashEntrySize);
            }
            child_count = parent_blocks;
        }
        const uint64_t root_backing = HashBlockToBackingPage(tree_depth - 1u, 0, tree_depth);
        crypto::ComputeSha256(
            img.data() + kRetailHeaderSize + static_cast<size_t>(root_backing) * kSectorSize,
            kSectorSize,
            top_hash_sha);
    }

    // Populate retail msft-xvd v3 header at 0x200..0x2A3.
    std::memcpy(img.data() + kMagicOffset, kMagic, 8);
    uint32_t vol_flags = kFlagReadOnly;
    if (!is_encrypted) vol_flags |= kFlagEncryptionDisabled;
    if (!enable_merkle_tree) vol_flags |= kFlagDataIntegrityDisabled;

    WriteU32LE(img.data() + kOffVolumeFlags, vol_flags);
    WriteU32LE(img.data() + kOffFormatVersion, 3u);
    WriteU64LE(img.data() + kOffDriveSize, static_cast<uint64_t>(user_pages) * kSectorSize);
    std::memcpy(img.data() + kOffContentId, vduid.data(), 16);
    std::memcpy(img.data() + kOffRetailTopHash, top_hash_sha, 32);
    std::memcpy(img.data() + kOffRetailOrigXvcHash, orig_xvc_sha, 32);
    WriteU32LE(img.data() + kOffRetailContentType, content_type);
    WriteU32LE(img.data() + kOffRetailEmbeddedXvdLen, 0u);
    WriteU64LE(img.data() + kOffRetailUserDataLength, user_data.size());
    WriteU64LE(img.data() + kOffRetailXvcDataLength, xvc_bytes.size());
    WriteU64LE(img.data() + kOffRetailMutableDataLength, 0ull);

    // Embed SHA-256 header digest at 0x000..0x01F for container signature verification.
    crypto::ComputeSha256(img.data() + kMagicOffset, kRetailHeaderSize - kMagicOffset, img.data());
    return img;
}

std::vector<uint8_t> XvdContainer::BuildRetailV3Xvc(
    const std::vector<uint8_t>& user_data,
    const uint8_t* volume_key,
    size_t key_len,
    bool enable_merkle_tree,
    const std::vector<XvcRegionInfo>& regions,
    uint32_t content_type) {
    std::vector<std::vector<uint8_t>> cik_keys;
    if (volume_key && key_len >= 16) {
        cik_keys.emplace_back(volume_key, volume_key + (std::min)(key_len, size_t{64}));
    }
    return BuildRetailV3XvcMultiKey(user_data, cik_keys, regions, enable_merkle_tree, content_type);
}

namespace {

struct NtfsBuildNode {
    std::string name;
    bool is_dir = false;
    uint64_t mft_num = 0;
    uint64_t parent_mft_num = 5;
    std::vector<uint8_t> file_data;
    std::vector<std::unique_ptr<NtfsBuildNode>> children;

    // Assigned cluster layout for non-resident files or directory $INDEX_ALLOCATION:
    bool non_resident_data = false;
    uint64_t data_start_lcn = 0;
    uint64_t data_cluster_count = 0;

    bool use_index_alloc = false;
    uint64_t idx_start_lcn = 0;
    uint64_t idx_block_count = 0;
    std::vector<std::vector<uint8_t>> idx_blocks;
    std::vector<uint8_t> idx_root_entries;
};

std::vector<uint8_t> BuildNtfsFileNameValue(
    uint64_t parent_mft_num,
    uint64_t alloc_size,
    uint64_t real_size,
    bool is_dir,
    const std::string& utf8_name,
    uint8_t name_ns) {
    const auto u16 = AsciiOrUtf8ToUtf16Le(utf8_name);
    const uint8_t nchars = static_cast<uint8_t>((std::min)(u16.size(), size_t{255}));
    std::vector<uint8_t> fn(0x42u + static_cast<size_t>(nchars) * 2u, 0);
    WriteU64LE(fn.data() + 0x00, parent_mft_num | (uint64_t{1} << 48));
    WriteU64LE(fn.data() + 0x28, alloc_size);
    WriteU64LE(fn.data() + 0x30, real_size);
    WriteU32LE(fn.data() + 0x38, is_dir ? 0x10000000u : 0x80u);
    fn[0x40] = nchars;
    fn[0x41] = name_ns;
    for (size_t i = 0; i < nchars; ++i) {
        WriteU16LE(fn.data() + 0x42u + i * 2u, u16[i]);
    }
    return fn;
}

std::vector<uint8_t> BuildNtfsIndexEntry(
    uint64_t child_mft_num,
    const std::vector<uint8_t>& fn_val,
    bool has_subnode,
    uint64_t subnode_vcn) {
    const size_t base_len = AlignUp8(16u + fn_val.size()) + (has_subnode ? 8u : 0u);
    std::vector<uint8_t> ent(base_len, 0);
    WriteU64LE(ent.data() + 0x00, child_mft_num | (uint64_t{1} << 48));
    WriteU16LE(ent.data() + 0x08, static_cast<uint16_t>(base_len));
    WriteU16LE(ent.data() + 0x0A, static_cast<uint16_t>(fn_val.size()));
    WriteU16LE(ent.data() + 0x0C, has_subnode ? 0x0001u : 0x0000u);
    std::memcpy(ent.data() + 0x10, fn_val.data(), fn_val.size());
    if (has_subnode) {
        WriteU64LE(ent.data() + base_len - 8u, subnode_vcn);
    }
    return ent;
}

std::vector<uint8_t> BuildNtfsIndexEndEntry(bool has_subnode, uint64_t subnode_vcn) {
    const size_t len = has_subnode ? 24u : 16u;
    std::vector<uint8_t> ent(len, 0);
    WriteU16LE(ent.data() + 0x08, static_cast<uint16_t>(len));
    WriteU16LE(ent.data() + 0x0C, static_cast<uint16_t>(0x0002u | (has_subnode ? 0x0001u : 0x0000u)));
    if (has_subnode) {
        WriteU64LE(ent.data() + 16u, subnode_vcn);
    }
    return ent;
}

void AppendResidentAttr(
    std::vector<uint8_t>& rec,
    size_t& cursor,
    uint32_t attr_type,
    const char* ascii_name,
    const uint8_t* val,
    size_t val_len) {
    const size_t nlen = ascii_name ? std::strlen(ascii_name) : 0u;
    const size_t val_off = AlignUp8(24u + nlen * 2u);
    const size_t attr_len = AlignUp8(val_off + val_len);

    uint8_t* p = rec.data() + cursor;
    WriteU32LE(p + 0, attr_type);
    WriteU32LE(p + 4, static_cast<uint32_t>(attr_len));
    p[8] = 0u;  // Resident
    p[9] = static_cast<uint8_t>(nlen);
    WriteU16LE(p + 10, nlen > 0 ? 24u : 0u);
    WriteU32LE(p + 16, static_cast<uint32_t>(val_len));
    WriteU16LE(p + 20, static_cast<uint16_t>(val_off));
    for (size_t i = 0; i < nlen; ++i) {
        WriteU16LE(p + 24u + i * 2u, static_cast<uint8_t>(ascii_name[i]));
    }
    if (val && val_len > 0) {
        std::memcpy(p + val_off, val, val_len);
    }
    cursor += attr_len;
}

void AppendNonResidentSingleRunAttr(
    std::vector<uint8_t>& rec,
    size_t& cursor,
    uint32_t attr_type,
    const char* ascii_name,
    uint64_t start_lcn,
    uint64_t cluster_count,
    uint64_t real_size) {
    const size_t nlen = ascii_name ? std::strlen(ascii_name) : 0u;
    const size_t run_off = AlignUp8(64u + nlen * 2u);
    // 1B run header (0x44 = 4B len + 4B signed LCN) + 4B len + 4B LCN + 1B 0x00 terminator = 10B
    const size_t attr_len = AlignUp8(run_off + 10u);

    uint8_t* p = rec.data() + cursor;
    WriteU32LE(p + 0, attr_type);
    WriteU32LE(p + 4, static_cast<uint32_t>(attr_len));
    p[8] = 1u;  // Non-resident
    p[9] = static_cast<uint8_t>(nlen);
    WriteU16LE(p + 10, nlen > 0 ? 64u : 0u);
    WriteU64LE(p + 16, 0ull);  // start VCN
    WriteU64LE(p + 24, cluster_count > 0 ? (cluster_count - 1ull) : 0ull);
    WriteU16LE(p + 32, static_cast<uint16_t>(run_off));
    WriteU64LE(p + 40, cluster_count * 4096ull);
    WriteU64LE(p + 48, real_size);
    WriteU64LE(p + 56, real_size);
    for (size_t i = 0; i < nlen; ++i) {
        WriteU16LE(p + 64u + i * 2u, static_cast<uint8_t>(ascii_name[i]));
    }

    uint8_t* r = p + run_off;
    r[0] = 0x44u;
    WriteU32LE(r + 1, static_cast<uint32_t>(cluster_count));
    WriteU32LE(r + 5, static_cast<uint32_t>(start_lcn));
    r[9] = 0x00u;

    cursor += attr_len;
}

}  // namespace

std::vector<uint8_t> XvdContainer::BuildNtfsVolumeBytes(const std::vector<XvdInputFile>& files) {
    constexpr uint32_t kClusterSize = 4096u;
    constexpr uint32_t kMftRecSize = 1024u;
    constexpr uint64_t kMftStartLcn = 4ull;

    NtfsBuildNode root;
    root.name = ".";
    root.is_dir = true;
    root.mft_num = 5ull;
    root.parent_mft_num = 5ull;

    uint64_t next_user_mft = 24ull;
    std::vector<NtfsBuildNode*> all_user_nodes;

    for (const auto& f : files) {
        std::vector<std::string> parts;
        if (!SplitSafeRelativePath(f.relative_path, parts)) {
            continue;
        }
        NtfsBuildNode* cur = &root;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            NtfsBuildNode* found = nullptr;
            for (auto& ch : cur->children) {
                if (ch->is_dir && ToLowerAscii(ch->name) == ToLowerAscii(parts[i])) {
                    found = ch.get();
                    break;
                }
            }
            if (!found) {
                auto sub = std::make_unique<NtfsBuildNode>();
                sub->name = parts[i];
                sub->is_dir = true;
                sub->mft_num = next_user_mft++;
                sub->parent_mft_num = cur->mft_num;
                found = sub.get();
                all_user_nodes.push_back(found);
                cur->children.push_back(std::move(sub));
            }
            cur = found;
        }
        auto leaf = std::make_unique<NtfsBuildNode>();
        leaf->name = parts.back();
        leaf->is_dir = false;
        leaf->mft_num = next_user_mft++;
        leaf->parent_mft_num = cur->mft_num;
        leaf->file_data = f.data;
        all_user_nodes.push_back(leaf.get());
        cur->children.push_back(std::move(leaf));
    }

    const uint64_t total_mft_records = (std::max)(next_user_mft, uint64_t{24});
    const uint64_t mft_total_bytes = total_mft_records * kMftRecSize;
    const uint64_t mft_clusters = (mft_total_bytes + kClusterSize - 1u) / kClusterSize;
    uint64_t next_free_lcn = kMftStartLcn + mft_clusters;

    // Encode directory indices & assign clusters for non-resident files and INDX blocks.
    auto prepare_tree = [&](auto& self, NtfsBuildNode& node) -> void {
        if (!node.is_dir) {
            // Files <= 192B use resident $DATA inside MFT record; larger files use non-resident cluster runs.
            if (node.file_data.size() > 192u) {
                node.non_resident_data = true;
                node.data_cluster_count = (node.file_data.size() + kClusterSize - 1u) / kClusterSize;
                node.data_start_lcn = next_free_lcn;
                next_free_lcn += node.data_cluster_count;
            }
            return;
        }

        std::sort(node.children.begin(), node.children.end(),
                  [](const std::unique_ptr<NtfsBuildNode>& a, const std::unique_ptr<NtfsBuildNode>& b) {
                      return ToLowerAscii(a->name) < ToLowerAscii(b->name);
                  });
        for (auto& ch : node.children) {
            self(self, *ch);
        }

        std::vector<std::vector<uint8_t>> leaf_entries;
        bool added_dos_alias = false;
        for (const auto& ch : node.children) {
            const uint64_t alloc_sz = ch->non_resident_data ? (ch->data_cluster_count * kClusterSize) : AlignUp8(ch->file_data.size());
            if (!added_dos_alias && ch->name.size() > 12u) {
                // Include a synthetic DOS 8.3 namespace=2 alias alongside Win32 namespace=1 for the same MFT record.
                const auto dos_fn = BuildNtfsFileNameValue(node.mft_num, alloc_sz, ch->file_data.size(), ch->is_dir, "SHORT8~1.BIN", 2u);
                leaf_entries.push_back(BuildNtfsIndexEntry(ch->mft_num, dos_fn, false, 0));
                added_dos_alias = true;
            }
            const auto win_fn = BuildNtfsFileNameValue(node.mft_num, alloc_sz, ch->file_data.size(), ch->is_dir, ch->name, 1u);
            leaf_entries.push_back(BuildNtfsIndexEntry(ch->mft_num, win_fn, false, 0));
        }

        // Root directory (when non-empty) and larger directories use $INDEX_ALLOCATION INDX B-tree nodes.
        if (!leaf_entries.empty() && (node.mft_num == 5ull || leaf_entries.size() > 2u)) {
            node.use_index_alloc = true;
            // Pack leaf entries into 4096-byte INDX blocks (header + USA up to 0x40, leave 24B for end marker).
            size_t idx = 0;
            while (idx < leaf_entries.size()) {
                std::vector<uint8_t> blk(kClusterSize, 0);
                std::memcpy(blk.data(), "INDX", 4);
                WriteU64LE(blk.data() + 0x10, static_cast<uint64_t>(node.idx_blocks.size()));  // index_block_vcn
                // INDEX_HEADER at +0x18; entries start at +0x40 (offset 0x28 relative to +0x18, clears 9-word USA at +0x28..0x39).
                constexpr size_t kEntriesStart = 0x40u;
                size_t bcur = kEntriesStart;
                while (idx < leaf_entries.size() &&
                       bcur + leaf_entries[idx].size() + 24u <= kClusterSize - 16u) {
                    std::memcpy(blk.data() + bcur, leaf_entries[idx].data(), leaf_entries[idx].size());
                    bcur += leaf_entries[idx].size();
                    ++idx;
                }
                const auto end_ent = BuildNtfsIndexEndEntry(false, 0);
                std::memcpy(blk.data() + bcur, end_ent.data(), end_ent.size());
                bcur += end_ent.size();

                WriteU32LE(blk.data() + 0x18 + 0x00, static_cast<uint32_t>(kEntriesStart - 0x18u));
                WriteU32LE(blk.data() + 0x18 + 0x04, static_cast<uint32_t>(bcur - 0x18u));
                WriteU32LE(blk.data() + 0x18 + 0x08, static_cast<uint32_t>(kClusterSize - 0x18u));
                FinalizeNtfsUsaRecord(blk.data(), blk.size(), 0x28u, 0x0001u);

                node.idx_blocks.push_back(std::move(blk));

                // If more leaf entries remain, promote one separator entry into $INDEX_ROOT pointing to this block!
                if (idx < leaf_entries.size()) {
                    const uint64_t sub_vcn = static_cast<uint64_t>(node.idx_blocks.size() - 1u);
                    if (node.idx_root_entries.size() + leaf_entries[idx].size() + 64u <= 512u) {
                        const uint64_t sep_mft = ReadU64LE(leaf_entries[idx].data()) & 0x0000FFFFFFFFFFFFull;
                        const uint16_t sep_klen = ReadU16LE(leaf_entries[idx].data() + 10);
                        std::vector<uint8_t> sep_fn(
                            leaf_entries[idx].data() + 16,
                            leaf_entries[idx].data() + 16 + sep_klen);
                        const auto root_sep = BuildNtfsIndexEntry(sep_mft, sep_fn, true, sub_vcn);
                        node.idx_root_entries.insert(node.idx_root_entries.end(), root_sep.begin(), root_sep.end());
                        ++idx;
                    } else {
                        // Compact 24B branch pointer so large directories never overflow a 1024B MFT record.
                        std::vector<uint8_t> branch(24u, 0);
                        WriteU16LE(branch.data() + 0x08, 24u);
                        WriteU16LE(branch.data() + 0x0C, 0x0001u);
                        WriteU64LE(branch.data() + 0x10, sub_vcn);
                        node.idx_root_entries.insert(node.idx_root_entries.end(), branch.begin(), branch.end());
                    }
                }
            }
            // If that separator consumed the very last leaf entry, emit an empty trailing INDX block for the right-most branch.
            if (node.idx_blocks.empty() || !node.idx_root_entries.empty()) {
                // Ensure final end entry in $INDEX_ROOT points to the last INDX block; if separator consumed all remaining leaves, add empty right-most block.
                // Check how many subnode pointers we already emitted vs idx_blocks.size():
                size_t root_ptrs = 0;
                size_t rpos = 0;
                while (rpos + 16u <= node.idx_root_entries.size()) {
                    ++root_ptrs;
                    rpos += ReadU16LE(node.idx_root_entries.data() + rpos + 8);
                }
                if (root_ptrs == node.idx_blocks.size()) {
                    std::vector<uint8_t> empty_blk(kClusterSize, 0);
                    std::memcpy(empty_blk.data(), "INDX", 4);
                    WriteU64LE(empty_blk.data() + 0x10, static_cast<uint64_t>(node.idx_blocks.size()));
                    const auto end_ent = BuildNtfsIndexEndEntry(false, 0);
                    std::memcpy(empty_blk.data() + 0x40u, end_ent.data(), end_ent.size());
                    WriteU32LE(empty_blk.data() + 0x18 + 0x00, 0x28u);
                    WriteU32LE(empty_blk.data() + 0x18 + 0x04, static_cast<uint32_t>(0x28u + end_ent.size()));
                    WriteU32LE(empty_blk.data() + 0x18 + 0x08, static_cast<uint32_t>(kClusterSize - 0x18u));
                    FinalizeNtfsUsaRecord(empty_blk.data(), empty_blk.size(), 0x28u, 0x0001u);
                    node.idx_blocks.push_back(std::move(empty_blk));
                }
            }
            const auto root_end = BuildNtfsIndexEndEntry(true, static_cast<uint64_t>(node.idx_blocks.size() - 1u));
            node.idx_root_entries.insert(node.idx_root_entries.end(), root_end.begin(), root_end.end());

            node.idx_block_count = node.idx_blocks.size();
            node.idx_start_lcn = next_free_lcn;
            next_free_lcn += node.idx_block_count;
        } else {
            for (const auto& le : leaf_entries) {
                node.idx_root_entries.insert(node.idx_root_entries.end(), le.begin(), le.end());
            }
            const auto root_end = BuildNtfsIndexEndEntry(false, 0);
            node.idx_root_entries.insert(node.idx_root_entries.end(), root_end.begin(), root_end.end());
        }
    };
    prepare_tree(prepare_tree, root);

    const uint64_t total_clusters = (std::max)(next_free_lcn + 2ull, uint64_t{16});
    std::vector<uint8_t> vol(static_cast<size_t>(total_clusters * kClusterSize), 0);

    // 1. Write NTFS Boot Sector at cluster 0.
    {
        uint8_t* bs = vol.data();
        bs[0] = 0xEBu;
        bs[1] = 0x52u;
        bs[2] = 0x90u;
        std::memcpy(bs + 3, kNtfsOemMagic, 8);
        WriteU16LE(bs + 0x0B, 512u);                // bytes_per_sector = 512
        bs[0x0D] = 8u;                              // sectors_per_cluster = 8 (4096B)
        bs[0x15] = 0xF8u;                           // media descriptor (fixed disk)
        WriteU16LE(bs + 0x18, 63u);
        WriteU16LE(bs + 0x1A, 255u);
        WriteU64LE(bs + 0x28, total_clusters * 8ull - 1ull);
        WriteU64LE(bs + 0x30, kMftStartLcn);        // $MFT start LCN
        WriteU64LE(bs + 0x38, 1ull);                // $MFTMirr LCN
        bs[0x40] = static_cast<uint8_t>(-10);       // clusters_per_mft_record = -10 -> 1024B
        bs[0x44] = 1u;                              // clusters_per_index_block = 1 -> 4096B
        WriteU64LE(bs + 0x48, 0x534341524C455454ull);
        bs[510] = 0x55u;
        bs[511] = 0xAAu;
    }

    auto build_mft_record_base = [&](uint64_t mft_num, uint16_t rec_flags) {
        std::vector<uint8_t> rec(kMftRecSize, 0);
        std::memcpy(rec.data(), "FILE", 4);
        WriteU16LE(rec.data() + 0x10, 1u);          // sequence_number
        WriteU16LE(rec.data() + 0x12, 1u);          // hard_link_count
        WriteU16LE(rec.data() + 0x14, 0x38u);       // first_attr_offset = 56
        WriteU16LE(rec.data() + 0x16, rec_flags);
        WriteU32LE(rec.data() + 0x1C, kMftRecSize); // bytes_allocated
        WriteU32LE(rec.data() + 0x2C, static_cast<uint32_t>(mft_num));
        return rec;
    };

    auto finalize_mft_record = [&](uint64_t mft_num, std::vector<uint8_t>& rec, size_t cursor) {
        WriteU32LE(rec.data() + cursor, kNtfsAttrEnd);
        cursor += 8u;
        WriteU32LE(rec.data() + 0x18, static_cast<uint32_t>(cursor));
        FinalizeNtfsUsaRecord(rec.data(), rec.size(), 0x30u, 0x0001u);
        const size_t dst = static_cast<size_t>(kMftStartLcn * kClusterSize + mft_num * kMftRecSize);
        std::memcpy(vol.data() + dst, rec.data(), rec.size());
    };

    // 2. Write MFT Record #0 ($MFT).
    {
        auto rec = build_mft_record_base(0ull, 0x0001u);
        size_t cur = 0x38u;
        uint8_t std_info[48]{};
        WriteU32LE(std_info + 0x20, 0x06u);  // Hidden | System
        AppendResidentAttr(rec, cur, kNtfsAttrStandardInfo, nullptr, std_info, sizeof(std_info));
        const auto fn = BuildNtfsFileNameValue(5ull, mft_clusters * kClusterSize, mft_total_bytes, false, "$MFT", 3u);
        AppendResidentAttr(rec, cur, kNtfsAttrFileName, nullptr, fn.data(), fn.size());
        AppendNonResidentSingleRunAttr(rec, cur, kNtfsAttrData, nullptr, kMftStartLcn, mft_clusters, mft_total_bytes);
        finalize_mft_record(0ull, rec, cur);
    }

    // 3. Write minimal valid system MFT records #1..#4 and #6..#15 so MFT table is contiguous.
    for (uint64_t sys_mft = 1ull; sys_mft < 16ull; ++sys_mft) {
        if (sys_mft == 5ull) continue;
        auto rec = build_mft_record_base(sys_mft, 0x0001u);
        size_t cur = 0x38u;
        uint8_t std_info[48]{};
        WriteU32LE(std_info + 0x20, 0x06u);
        AppendResidentAttr(rec, cur, kNtfsAttrStandardInfo, nullptr, std_info, sizeof(std_info));
        finalize_mft_record(sys_mft, rec, cur);
    }

    // 4. Write MFT Record #5 (Root Directory ".") and all user directory & file MFT records + cluster payloads.
    auto emit_node = [&](NtfsBuildNode& node) {
        auto rec = build_mft_record_base(node.mft_num, node.is_dir ? 0x0003u : 0x0001u);
        size_t cur = 0x38u;

        uint8_t std_info[48]{};
        WriteU32LE(std_info + 0x20, node.is_dir ? 0x10u : 0x80u);
        AppendResidentAttr(rec, cur, kNtfsAttrStandardInfo, nullptr, std_info, sizeof(std_info));

        const uint64_t alloc_sz = node.non_resident_data
                                      ? (node.data_cluster_count * kClusterSize)
                                      : AlignUp8(node.file_data.size());
        const auto fn = BuildNtfsFileNameValue(
            node.parent_mft_num, alloc_sz, node.file_data.size(), node.is_dir, node.name, 1u);
        AppendResidentAttr(rec, cur, kNtfsAttrFileName, nullptr, fn.data(), fn.size());

        if (node.is_dir) {
            std::vector<uint8_t> ir_val(32u + node.idx_root_entries.size(), 0);
            WriteU32LE(ir_val.data() + 0x00, kNtfsAttrFileName);
            WriteU32LE(ir_val.data() + 0x04, 1u);  // COLLATION_FILENAME
            WriteU32LE(ir_val.data() + 0x08, kClusterSize);
            ir_val[0x0C] = 1u;
            // Embedded INDEX_HEADER at +0x10
            WriteU32LE(ir_val.data() + 0x10 + 0x00, 16u);
            WriteU32LE(ir_val.data() + 0x10 + 0x04, static_cast<uint32_t>(16u + node.idx_root_entries.size()));
            WriteU32LE(ir_val.data() + 0x10 + 0x08, static_cast<uint32_t>(16u + node.idx_root_entries.size()));
            ir_val[0x10 + 0x0C] = node.use_index_alloc ? 0x01u : 0x00u;
            std::memcpy(ir_val.data() + 0x20, node.idx_root_entries.data(), node.idx_root_entries.size());

            AppendResidentAttr(rec, cur, kNtfsAttrIndexRoot, "$I30", ir_val.data(), ir_val.size());

            if (node.use_index_alloc) {
                AppendNonResidentSingleRunAttr(
                    rec, cur, kNtfsAttrIndexAllocation, "$I30",
                    node.idx_start_lcn, node.idx_block_count, node.idx_block_count * kClusterSize);
                for (size_t b = 0; b < node.idx_blocks.size(); ++b) {
                    const size_t dst = static_cast<size_t>((node.idx_start_lcn + b) * kClusterSize);
                    std::memcpy(vol.data() + dst, node.idx_blocks[b].data(), kClusterSize);
                }
            }
        } else {
            if (node.non_resident_data) {
                AppendNonResidentSingleRunAttr(
                    rec, cur, kNtfsAttrData, nullptr,
                    node.data_start_lcn, node.data_cluster_count, node.file_data.size());
                const size_t dst = static_cast<size_t>(node.data_start_lcn * kClusterSize);
                std::memcpy(vol.data() + dst, node.file_data.data(), node.file_data.size());
            } else {
                AppendResidentAttr(rec, cur, kNtfsAttrData, nullptr, node.file_data.data(), node.file_data.size());
            }
        }

        finalize_mft_record(node.mft_num, rec, cur);
    };

    emit_node(root);
    for (NtfsBuildNode* n : all_user_nodes) {
        emit_node(*n);
    }

    return vol;
}

std::vector<uint8_t> XvdContainer::BuildSyntheticNtfsXvc(
    const std::vector<XvdInputFile>& files,
    const uint8_t* key,
    size_t key_len,
    bool enable_merkle_tree) {
    const std::vector<uint8_t> ntfs_vol = BuildNtfsVolumeBytes(files);
    const bool has_key = (key != nullptr && key_len >= 16);

    std::vector<XvcRegionInfo> regions;
    if (has_key && ntfs_vol.size() > kSectorSize) {
        // Region 1: unencrypted NTFS Boot Sector page (KeyId = 0xFFFF).
        regions.push_back(XvcRegionInfo{1u, kUnencryptedKeyId, 1u, 0ull, kSectorSize});
        // Region 2: AES-XTS encrypted $MFT, INDX directories, and file clusters (KeyId = 0).
        regions.push_back(XvcRegionInfo{2u, 0u, 3u, kSectorSize, ntfs_vol.size() - kSectorSize});
    } else {
        regions.push_back(XvcRegionInfo{
            1u,
            has_key ? uint16_t{0} : kUnencryptedKeyId,
            1u,
            0ull,
            ntfs_vol.size(),
        });
    }

    return BuildRetailV3Xvc(
        ntfs_vol,
        has_key ? key : nullptr,
        has_key ? key_len : 0u,
        enable_merkle_tree,
        regions,
        1u);
}

}  // namespace scarlettx
