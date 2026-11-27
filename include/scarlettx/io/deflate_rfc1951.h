#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace scarlettx::io {

// Clean-room, bounds-checked RFC 1951 DEFLATE & RFC 1950 Zlib (with Adler-32) decompressor.
// Supports Stored (BTYPE=0), Fixed Huffman (BTYPE=1), and Dynamic Huffman (BTYPE=2) blocks
// with 32 KiB sliding-window LZ77 back-references.

inline uint32_t ComputeAdler32(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 1u;
    constexpr uint32_t kMod = 65521u;
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    size_t pos = 0;
    while (pos < len) {
        const size_t chunk = std::min<size_t>(5552u, len - pos);
        for (size_t i = 0; i < chunk; ++i) {
            s1 += data[pos + i];
            s2 += s1;
        }
        s1 %= kMod;
        s2 %= kMod;
        pos += chunk;
    }
    return (s2 << 16) | s1;
}

inline bool LooksLikeRfc1950ZlibHeader(const uint8_t* src, size_t len) {
    if (!src || len < 6) return false;
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0Fu) != 8u) return false;       // Compression method 8 = DEFLATE
    if ((cmf >> 4) > 7u) return false;           // Window <= 32 KiB
    if ((flg & 0x20u) != 0u) return false;       // Preset dictionary (FDICT) not used in DirectStorage
    return (((static_cast<uint32_t>(cmf) << 8) | flg) % 31u) == 0u;
}

struct Rfc1951BitReader {
    const uint8_t* src = nullptr;
    size_t len = 0;
    size_t byte_pos = 0;
    uint32_t bit_buf = 0;
    uint32_t bit_cnt = 0;

    bool EnsureBits(uint32_t n) {
        while (bit_cnt < n) {
            if (byte_pos >= len) return false;
            bit_buf |= static_cast<uint32_t>(src[byte_pos++]) << bit_cnt;
            bit_cnt += 8u;
        }
        return true;
    }

    int GetBit() {
        if (!EnsureBits(1u)) return -1;
        const int b = static_cast<int>(bit_buf & 1u);
        bit_buf >>= 1u;
        --bit_cnt;
        return b;
    }

    bool ReadBitsLsb(uint32_t n, uint32_t& out) {
        if (n == 0) {
            out = 0;
            return true;
        }
        if (!EnsureBits(n)) return false;
        out = bit_buf & ((1u << n) - 1u);
        bit_buf >>= n;
        bit_cnt -= n;
        return true;
    }

    void AlignToByte() {
        const uint32_t drop = bit_cnt & 7u;
        bit_buf >>= drop;
        bit_cnt -= drop;
    }

    bool ReadAlignedByte(uint8_t& out) {
        if (bit_cnt >= 8u) {
            out = static_cast<uint8_t>(bit_buf & 0xFFu);
            bit_buf >>= 8u;
            bit_cnt -= 8u;
            return true;
        }
        if (byte_pos >= len) return false;
        out = src[byte_pos++];
        return true;
    }

    size_t ConsumedBytes() const {
        return byte_pos - (bit_cnt / 8u);
    }
};

struct CanonicalHuffman {
    std::array<int16_t, 16> count{};
    std::array<int16_t, 288> symbol{};

    bool Build(const uint8_t* lengths, size_t num_syms) {
        count.fill(0);
        symbol.fill(0);
        for (size_t i = 0; i < num_syms; ++i) {
            if (lengths[i] > 15u) return false;
            if (lengths[i] > 0u) ++count[lengths[i]];
        }
        int32_t left = 1;
        for (size_t bits = 1; bits <= 15; ++bits) {
            left <<= 1;
            left -= count[bits];
            if (left < 0) return false;  // Oversubscribed tree
        }
        std::array<int16_t, 16> offs{};
        offs[1] = 0;
        for (size_t bits = 1; bits < 15; ++bits) {
            offs[bits + 1] = static_cast<int16_t>(offs[bits] + count[bits]);
        }
        for (size_t sym = 0; sym < num_syms; ++sym) {
            const uint8_t l = lengths[sym];
            if (l != 0u) {
                symbol[static_cast<size_t>(offs[l]++)] = static_cast<int16_t>(sym);
            }
        }
        return true;
    }

    int Decode(Rfc1951BitReader& br) const {
        int32_t code = 0;
        int32_t first = 0;
        int32_t index = 0;
        for (size_t len = 1; len <= 15; ++len) {
            const int bit = br.GetBit();
            if (bit < 0) return -1;
            code |= bit;
            const int32_t cnt = count[len];
            if (code - cnt < first) {
                return symbol[static_cast<size_t>(index + (code - first))];
            }
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

inline bool InflateRfc1951(const uint8_t* src,
                           size_t src_len,
                           uint8_t* dst,
                           size_t dst_cap,
                           size_t& out_written,
                           size_t* out_consumed_bytes = nullptr) {
    out_written = 0;
    if (!src || src_len == 0 || (!dst && dst_cap > 0)) return false;

    static constexpr uint16_t kLenBase[29] = {
        3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr uint16_t kDistBase[30] = {
        1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
        33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static constexpr uint8_t kCodeLenOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    Rfc1951BitReader br{src, src_len, 0, 0, 0};
    size_t out_pos = 0;

    uint32_t bfinal = 0;
    do {
        uint32_t btype = 0;
        if (!br.ReadBitsLsb(1u, bfinal) || !br.ReadBitsLsb(2u, btype)) return false;
        if (btype == 3u) return false;

        if (btype == 0u) {
            br.AlignToByte();
            uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
            if (!br.ReadAlignedByte(b0) || !br.ReadAlignedByte(b1) ||
                !br.ReadAlignedByte(b2) || !br.ReadAlignedByte(b3)) {
                return false;
            }
            const uint16_t blk_len = static_cast<uint16_t>(b0 | (static_cast<uint16_t>(b1) << 8));
            const uint16_t blk_nlen = static_cast<uint16_t>(b2 | (static_cast<uint16_t>(b3) << 8));
            if ((blk_len ^ blk_nlen) != 0xFFFFu) return false;
            if (out_pos + blk_len > dst_cap) return false;
            for (uint16_t i = 0; i < blk_len; ++i) {
                uint8_t v = 0;
                if (!br.ReadAlignedByte(v)) return false;
                dst[out_pos++] = v;
            }
        } else {
            CanonicalHuffman lit_tree{};
            CanonicalHuffman dist_tree{};

            if (btype == 1u) {
                uint8_t lit_lens[288]{};
                for (size_t i = 0; i <= 143; ++i) lit_lens[i] = 8u;
                for (size_t i = 144; i <= 255; ++i) lit_lens[i] = 9u;
                for (size_t i = 256; i <= 279; ++i) lit_lens[i] = 7u;
                for (size_t i = 280; i <= 287; ++i) lit_lens[i] = 8u;
                uint8_t dist_lens[32]{};
                std::fill_n(dist_lens, 32, uint8_t{5u});
                if (!lit_tree.Build(lit_lens, 288) || !dist_tree.Build(dist_lens, 32)) return false;
            } else {
                uint32_t hlit = 0, hdist = 0, hclen = 0;
                if (!br.ReadBitsLsb(5u, hlit) || !br.ReadBitsLsb(5u, hdist) || !br.ReadBitsLsb(4u, hclen)) {
                    return false;
                }
                hlit += 257u;
                hdist += 1u;
                hclen += 4u;
                if (hlit > 286u || hdist > 30u) return false;

                uint8_t cl_lens[19]{};
                for (uint32_t i = 0; i < hclen; ++i) {
                    uint32_t v = 0;
                    if (!br.ReadBitsLsb(3u, v)) return false;
                    cl_lens[kCodeLenOrder[i]] = static_cast<uint8_t>(v);
                }
                CanonicalHuffman cl_tree{};
                if (!cl_tree.Build(cl_lens, 19)) return false;

                uint8_t combined_lens[320]{};
                const uint32_t total_lens = hlit + hdist;
                uint32_t idx = 0;
                while (idx < total_lens) {
                    const int sym = cl_tree.Decode(br);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        combined_lens[idx++] = static_cast<uint8_t>(sym);
                    } else {
                        uint8_t rep_val = 0;
                        uint32_t rep_cnt = 0;
                        if (sym == 16) {
                            if (idx == 0) return false;
                            uint32_t ex = 0;
                            if (!br.ReadBitsLsb(2u, ex)) return false;
                            rep_val = combined_lens[idx - 1];
                            rep_cnt = 3u + ex;
                        } else if (sym == 17) {
                            uint32_t ex = 0;
                            if (!br.ReadBitsLsb(3u, ex)) return false;
                            rep_cnt = 3u + ex;
                        } else if (sym == 18) {
                            uint32_t ex = 0;
                            if (!br.ReadBitsLsb(7u, ex)) return false;
                            rep_cnt = 11u + ex;
                        } else {
                            return false;
                        }
                        if (idx + rep_cnt > total_lens) return false;
                        for (uint32_t r = 0; r < rep_cnt; ++r) {
                            combined_lens[idx++] = rep_val;
                        }
                    }
                }
                if (combined_lens[256] == 0u) return false;  // End-of-block symbol must be present
                if (!lit_tree.Build(combined_lens, hlit) ||
                    !dist_tree.Build(combined_lens + hlit, hdist)) {
                    return false;
                }
            }

            for (;;) {
                const int sym = lit_tree.Decode(br);
                if (sym < 0) return false;
                if (sym < 256) {
                    if (out_pos >= dst_cap) return false;
                    dst[out_pos++] = static_cast<uint8_t>(sym);
                } else if (sym == 256) {
                    break;
                } else {
                    const int len_idx = sym - 257;
                    if (len_idx < 0 || len_idx >= 29) return false;
                    uint32_t len_extra = 0;
                    if (!br.ReadBitsLsb(kLenExtra[len_idx], len_extra)) return false;
                    const size_t match_len = static_cast<size_t>(kLenBase[len_idx]) + len_extra;

                    const int dist_sym = dist_tree.Decode(br);
                    if (dist_sym < 0 || dist_sym >= 30) return false;
                    uint32_t dist_extra = 0;
                    if (!br.ReadBitsLsb(kDistExtra[dist_sym], dist_extra)) return false;
                    const size_t match_dist = static_cast<size_t>(kDistBase[dist_sym]) + dist_extra;

                    if (match_dist == 0 || match_dist > out_pos || out_pos + match_len > dst_cap) {
                        return false;
                    }
                    for (size_t k = 0; k < match_len; ++k) {
                        dst[out_pos] = dst[out_pos - match_dist];
                        ++out_pos;
                    }
                }
            }
        }
    } while (bfinal == 0u);

    br.AlignToByte();
    out_written = out_pos;
    if (out_consumed_bytes) *out_consumed_bytes = br.ConsumedBytes();
    return true;
}

inline bool InflateZlibRfc1950(const uint8_t* src,
                               size_t src_len,
                               uint8_t* dst,
                               size_t dst_cap,
                               size_t& out_written) {
    out_written = 0;
    if (!LooksLikeRfc1950ZlibHeader(src, src_len)) return false;

    size_t deflate_consumed = 0;
    if (!InflateRfc1951(src + 2, src_len - 2, dst, dst_cap, out_written, &deflate_consumed)) {
        return false;
    }
    const size_t trailer_off = 2u + deflate_consumed;
    if (trailer_off + 4u > src_len) return false;

    const uint32_t expected_adler =
        (static_cast<uint32_t>(src[trailer_off]) << 24) |
        (static_cast<uint32_t>(src[trailer_off + 1]) << 16) |
        (static_cast<uint32_t>(src[trailer_off + 2]) << 8) |
        static_cast<uint32_t>(src[trailer_off + 3]);
    return ComputeAdler32(dst, out_written) == expected_adler;
}

}  // namespace scarlettx::io
