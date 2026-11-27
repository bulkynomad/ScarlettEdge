#define SCARLETTX_BUILDING_CODECS 1
#include "scarlettx/media/codecs_abi.h"
#include "scarlettx/audio/xma2_decoder.h"
#include "scarlettx/io/deflate_rfc1951.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kBufferTooSmall = static_cast<int32_t>(0x8007007Au);
constexpr int32_t kFail = static_cast<int32_t>(0x80004005u);

constexpr uint8_t kGDeflateId = 0x04u;
constexpr uint8_t kGDeflateMagic = 0xFBu;
constexpr size_t kGDeflateTileSize = 65536u;
constexpr uint32_t kGDeflateLanes = 32u;

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void WriteU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t ReverseBits(uint32_t val, uint32_t nbits) {
    uint32_t r = 0u;
    for (uint32_t i = 0u; i < nbits; ++i) {
        r = (r << 1u) | ((val >> i) & 1u);
    }
    return r;
}

static constexpr uint16_t kDeflateLenBase[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static constexpr uint8_t kDeflateLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static constexpr uint16_t kDeflateDistBase[30] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static constexpr uint8_t kDeflateDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

uint32_t FindLenCode(uint32_t match_len, uint32_t& out_extra_val, uint32_t& out_extra_bits) {
    for (uint32_t i = 28u; i > 0u; --i) {
        if (match_len >= kDeflateLenBase[i]) {
            out_extra_bits = kDeflateLenExtra[i];
            out_extra_val = match_len - kDeflateLenBase[i];
            return 257u + i;
        }
    }
    out_extra_bits = 0u;
    out_extra_val = 0u;
    return 257u;
}

uint32_t FindDistCode(uint32_t match_dist, uint32_t& out_extra_val, uint32_t& out_extra_bits) {
    for (uint32_t i = 29u; i > 0u; --i) {
        if (match_dist >= kDeflateDistBase[i]) {
            out_extra_bits = kDeflateDistExtra[i];
            out_extra_val = match_dist - kDeflateDistBase[i];
            return i;
        }
    }
    out_extra_bits = 0u;
    out_extra_val = 0u;
    return 0u;
}

struct LaneBitPacker {
    std::vector<uint32_t> words;
    std::vector<uint8_t> step_bits;
    uint32_t cur_bit = 0u;

    void PutBitsLsb(uint32_t val, uint32_t nbits) {
        for (uint32_t i = 0u; i < nbits; ++i) {
            const uint32_t bit = (val >> i) & 1u;
            const size_t w_idx = cur_bit >> 5u;
            const uint32_t b_idx = cur_bit & 31u;
            if (w_idx >= words.size()) words.push_back(0u);
            words[w_idx] |= (bit << b_idx);
            ++cur_bit;
        }
    }

    uint32_t EmitFixedLitLen(uint32_t sym) {
        if (sym <= 143u) {
            PutBitsLsb(ReverseBits(0x30u + sym, 8u), 8u);
            return 8u;
        }
        if (sym <= 255u) {
            PutBitsLsb(ReverseBits(0x190u + (sym - 144u), 9u), 9u);
            return 9u;
        }
        if (sym <= 279u) {
            PutBitsLsb(ReverseBits(sym - 256u, 7u), 7u);
            return 7u;
        }
        PutBitsLsb(ReverseBits(0xC0u + (sym - 280u), 8u), 8u);
        return 8u;
    }
};

struct GDeflateToken {
    uint16_t lit_or_len = 0;
    uint16_t dist = 0;
    bool is_match = false;
};

// Real 32-lane SIMD bit-interleaved RFC 1951 Fixed-Huffman + LZ77 GDeflate tile encoder.
// All 32 lanes read 32-bit words from a shared interleaved word stream in wavefront lockstep.
void GDeflate32LaneCompressTile(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_bytes) {
    std::vector<GDeflateToken> tokens;
    tokens.reserve(src_len);

    std::array<int32_t, 4096> head;
    head.fill(-1);
    std::vector<int32_t> prev(src_len, -1);

    auto hash3 = [](const uint8_t* p) -> uint32_t {
        return ((static_cast<uint32_t>(p[0]) * 2654435761u) ^
                (static_cast<uint32_t>(p[1]) * 40503u) ^
                static_cast<uint32_t>(p[2])) &
               0xFFFu;
    };

    size_t pos = 0;
    while (pos < src_len) {
        size_t best_len = 0;
        size_t best_dist = 0;
        if (pos + 3u <= src_len) {
            const uint32_t h = hash3(src + pos);
            int32_t cand = head[h];
            prev[pos] = cand;
            head[h] = static_cast<int32_t>(pos);

            const size_t max_len = std::min<size_t>(258u, src_len - pos);
            int chain = 0;
            while (cand >= 0 && chain < 24) {
                const size_t dist = pos - static_cast<size_t>(cand);
                if (dist > 32768u) break;
                if (src[cand + best_len] == src[pos + best_len] && src[cand] == src[pos]) {
                    size_t m = 0;
                    while (m < max_len && src[cand + m] == src[pos + m]) ++m;
                    if (m >= 3u && m > best_len) {
                        best_len = m;
                        best_dist = dist;
                        if (best_len == max_len) break;
                    }
                }
                cand = prev[static_cast<size_t>(cand)];
                ++chain;
            }
        }

        if (best_len >= 3u) {
            tokens.push_back({static_cast<uint16_t>(best_len), static_cast<uint16_t>(best_dist), true});
            for (size_t k = 1u; k < best_len; ++k) {
                if (pos + k + 3u <= src_len) {
                    const uint32_t hk = hash3(src + pos + k);
                    prev[pos + k] = head[hk];
                    head[hk] = static_cast<int32_t>(pos + k);
                }
            }
            pos += best_len;
        } else {
            tokens.push_back({src[pos], 0u, false});
            ++pos;
        }
    }

    std::array<LaneBitPacker, kGDeflateLanes> lanes;
    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
        // 3-bit block header per lane: BFINAL=1 (bit 0), BTYPE=01 Fixed Huffman (bits 1..2) -> 0b011 = 3u
        lanes[l].PutBitsLsb(0x3u, 3u);
    }

    const size_t full_waves = tokens.size() / kGDeflateLanes;
    const size_t rem = tokens.size() % kGDeflateLanes;

    auto encode_token = [](LaneBitPacker& lp, const GDeflateToken& tok) {
        if (!tok.is_match) {
            const uint32_t b = lp.EmitFixedLitLen(tok.lit_or_len);
            lp.step_bits.push_back(static_cast<uint8_t>(b));
        } else {
            uint32_t lex_val = 0, lex_bits = 0;
            const uint32_t lsym = FindLenCode(tok.lit_or_len, lex_val, lex_bits);
            uint32_t dex_val = 0, dex_bits = 0;
            const uint32_t dsym = FindDistCode(tok.dist, dex_val, dex_bits);

            uint32_t used = lp.EmitFixedLitLen(lsym);
            if (lex_bits > 0u) {
                lp.PutBitsLsb(lex_val, lex_bits);
                used += lex_bits;
            }
            lp.PutBitsLsb(ReverseBits(dsym, 5u), 5u);
            used += 5u;
            if (dex_bits > 0u) {
                lp.PutBitsLsb(dex_val, dex_bits);
                used += dex_bits;
            }
            lp.step_bits.push_back(static_cast<uint8_t>(used));
        }
    };

    for (size_t w = 0; w < full_waves; ++w) {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            encode_token(lanes[l], tokens[w * kGDeflateLanes + l]);
        }
    }
    if (rem > 0u) {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            if (l < rem) {
                encode_token(lanes[l], tokens[full_waves * kGDeflateLanes + l]);
            } else {
                const uint32_t b = lanes[l].EmitFixedLitLen(256u);
                lanes[l].step_bits.push_back(static_cast<uint8_t>(b));
            }
        }
        for (uint32_t l = 0; l < rem; ++l) {
            const uint32_t b = lanes[l].EmitFixedLitLen(256u);
            lanes[l].step_bits.push_back(static_cast<uint8_t>(b));
        }
    } else {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            const uint32_t b = lanes[l].EmitFixedLitLen(256u);
            lanes[l].step_bits.push_back(static_cast<uint8_t>(b));
        }
    }

    size_t max_steps = 0;
    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
        while (lanes[l].words.size() < 2u) lanes[l].words.push_back(0u);
        max_steps = std::max(max_steps, lanes[l].step_bits.size());
    }

    // Interleave 32-bit words in exact wavefront consumption order:
    // words[0..31]  = initial cur_word for lanes 0..31
    // words[32..63] = initial next_word for lanes 0..31
    // subsequent words = on-demand refills as each lane's bit_off crosses 32
    std::vector<uint32_t> interleaved;
    interleaved.reserve(64u + tokens.size() / 2u);
    for (uint32_t l = 0; l < kGDeflateLanes; ++l) interleaved.push_back(lanes[l].words[0]);
    for (uint32_t l = 0; l < kGDeflateLanes; ++l) interleaved.push_back(lanes[l].words[1]);

    std::array<size_t, kGDeflateLanes> next_word_idx{};
    std::array<uint32_t, kGDeflateLanes> sim_bit_off{};
    next_word_idx.fill(2u);
    sim_bit_off.fill(3u);

    for (size_t step = 0; step < max_steps; ++step) {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            if (step >= lanes[l].step_bits.size()) continue;
            sim_bit_off[l] += lanes[l].step_bits[step];
            if (sim_bit_off[l] >= 32u) {
                sim_bit_off[l] -= 32u;
                const uint32_t w = (next_word_idx[l] < lanes[l].words.size())
                                       ? lanes[l].words[next_word_idx[l]++]
                                       : 0u;
                interleaved.push_back(w);
            }
        }
    }

    out_bytes.resize(interleaved.size() * sizeof(uint32_t));
    for (size_t i = 0; i < interleaved.size(); ++i) {
        WriteU32LE(out_bytes.data() + i * 4u, interleaved[i]);
    }
}

bool GDeflate32LaneDecompressTile(
    const uint8_t* src,
    size_t src_len,
    uint8_t* dst,
    size_t dst_cap,
    size_t& out_written) {
    out_written = 0;
    if (!src || src_len < 256u || (src_len % 4u) != 0u || !dst || dst_cap == 0u) {
        return false;
    }

    const size_t total_words = src_len / 4u;
    auto read_word = [&](size_t idx) -> uint32_t {
        return (idx < total_words) ? ReadU32LE(src + idx * 4u) : 0u;
    };

    std::array<uint32_t, kGDeflateLanes> cur_word{};
    std::array<uint32_t, kGDeflateLanes> next_word{};
    std::array<uint32_t, kGDeflateLanes> bit_off{};
    std::array<bool, kGDeflateLanes> lane_done{};

    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
        cur_word[l] = read_word(l);
        next_word[l] = read_word(kGDeflateLanes + l);
        const uint32_t hdr = cur_word[l] & 0x7u;
        const uint32_t bfinal = hdr & 1u;
        const uint32_t btype = (hdr >> 1u) & 0x3u;
        if (bfinal != 1u || btype != 1u) {
            return false;
        }
        bit_off[l] = 3u;
        lane_done[l] = false;
    }

    size_t word_cursor = 64u;
    size_t out_pos = 0u;
    uint32_t active_lanes = kGDeflateLanes;

    struct DecodedLaneCmd {
        uint32_t lit_or_len = 0;
        uint32_t dist = 0;
        bool is_match = false;
        bool has_output = false;
        bool needs_refill = false;
    };

    const size_t max_waves = dst_cap + 2u;
    for (size_t wave = 0; wave < max_waves && active_lanes > 0u; ++wave) {
        std::array<DecodedLaneCmd, kGDeflateLanes> cmds{};
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            if (lane_done[l]) continue;

            const uint32_t bo = bit_off[l];
            const uint32_t bits = (bo == 0u) ? cur_word[l] : ((cur_word[l] >> bo) | (next_word[l] << (32u - bo)));
            const uint32_t rev9 = ReverseBits(bits & 0x1FFu, 9u);
            const uint32_t rev7 = rev9 >> 2u;
            const uint32_t rev8 = rev9 >> 1u;

            uint32_t sym = 256u;
            uint32_t cbits = 7u;
            if (rev7 <= 23u) {
                sym = 256u + rev7;
                cbits = 7u;
            } else if (rev8 >= 48u && rev8 <= 191u) {
                sym = rev8 - 48u;
                cbits = 8u;
            } else if (rev8 >= 192u && rev8 <= 199u) {
                sym = 280u + (rev8 - 192u);
                cbits = 8u;
            } else {
                sym = 144u + (rev9 - 400u);
                cbits = 9u;
            }

            uint32_t nbits = 0u;
            if (sym < 256u) {
                cmds[l].lit_or_len = sym;
                cmds[l].is_match = false;
                cmds[l].has_output = true;
                nbits = cbits;
            } else if (sym == 256u) {
                lane_done[l] = true;
                --active_lanes;
                nbits = 7u;
            } else if (sym <= 285u) {
                const uint32_t len_idx = sym - 257u;
                const uint32_t lex_bits = kDeflateLenExtra[len_idx];
                const uint32_t lex_val = lex_bits > 0u ? ((bits >> cbits) & ((1u << lex_bits) - 1u)) : 0u;
                const uint32_t match_len = static_cast<uint32_t>(kDeflateLenBase[len_idx]) + lex_val;

                const uint32_t pos_after_len = cbits + lex_bits;
                const uint32_t dist_sym = ReverseBits((bits >> pos_after_len) & 0x1Fu, 5u);
                if (dist_sym >= 30u) return false;
                const uint32_t dex_bits = kDeflateDistExtra[dist_sym];
                const uint32_t dex_val =
                    dex_bits > 0u ? ((bits >> (pos_after_len + 5u)) & ((1u << dex_bits) - 1u)) : 0u;
                const uint32_t match_dist = static_cast<uint32_t>(kDeflateDistBase[dist_sym]) + dex_val;

                cmds[l].lit_or_len = match_len;
                cmds[l].dist = match_dist;
                cmds[l].is_match = true;
                cmds[l].has_output = true;
                nbits = pos_after_len + 5u + dex_bits;
            } else {
                return false;
            }

            bit_off[l] += nbits;
            if (bit_off[l] >= 32u) {
                cmds[l].needs_refill = true;
            }
        }

        // Refill 32-bit words in deterministic lane 0..31 order
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            if (cmds[l].needs_refill) {
                if (word_cursor > total_words + kGDeflateLanes) return false;
                cur_word[l] = next_word[l];
                next_word[l] = read_word(word_cursor++);
                bit_off[l] -= 32u;
            }
        }

        // Emit decoded literals & sliding-window LZ77 matches
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            if (!cmds[l].has_output) continue;
            if (!cmds[l].is_match) {
                if (out_pos >= dst_cap) return false;
                dst[out_pos++] = static_cast<uint8_t>(cmds[l].lit_or_len);
            } else {
                const size_t mlen = cmds[l].lit_or_len;
                const size_t mdist = cmds[l].dist;
                if (mdist == 0u || mdist > out_pos || out_pos + mlen > dst_cap) {
                    return false;
                }
                for (size_t k = 0; k < mlen; ++k) {
                    dst[out_pos] = dst[out_pos - mdist];
                    ++out_pos;
                }
            }
        }
    }

    if (active_lanes != 0u) {
        return false;
    }
    out_written = out_pos;
    return true;
}

bool ParseOfficialGDeflateHeader(
    const uint8_t* in,
    size_t compressed_size,
    size_t dst_cap,
    std::vector<size_t>& out_tile_in_off,
    std::vector<uint32_t>& out_tile_comp_sz,
    std::vector<size_t>& out_tile_out_off,
    std::vector<size_t>& out_tile_out_cap,
    size_t& out_expected_total) {
    if (!in || compressed_size < 12u || in[0] != kGDeflateId || in[1] != kGDeflateMagic) {
        return false;
    }
    const uint16_t num_tiles = static_cast<uint16_t>(in[2] | (static_cast<uint16_t>(in[3]) << 8));
    if (num_tiles == 0u) return false;
    const size_t table_end = 8u + static_cast<size_t>(num_tiles) * 4u;
    if (compressed_size < table_end) return false;

    const uint32_t param = ReadU32LE(in + 4);
    const uint32_t tile_size_idx = param & 0x3u;
    if (tile_size_idx != 1u) return false;  // Official DirectStorage GDeflate uses 1 => 64 KiB tiles
    uint32_t last_tile_size = (param >> 2u) & 0x3FFFFu;
    if (last_tile_size == 0u) last_tile_size = static_cast<uint32_t>(kGDeflateTileSize);
    if (last_tile_size > kGDeflateTileSize) return false;

    const size_t expected_total =
        static_cast<size_t>(num_tiles - 1u) * kGDeflateTileSize + static_cast<size_t>(last_tile_size);
    if (expected_total == 0u || expected_total > dst_cap) return false;

    std::vector<uint32_t> offsets(num_tiles);
    for (uint16_t t = 0; t < num_tiles; ++t) {
        offsets[t] = ReadU32LE(in + 8u + static_cast<size_t>(t) * 4u);
    }
    const uint32_t last_comp_sz = offsets[0];
    if (last_comp_sz == 0u) return false;

    const size_t last_rel_off = (num_tiles == 1u) ? 0u : static_cast<size_t>(offsets[num_tiles - 1u]);
    if (table_end + last_rel_off + static_cast<size_t>(last_comp_sz) != compressed_size) {
        return false;
    }

    out_tile_in_off.resize(num_tiles);
    out_tile_comp_sz.resize(num_tiles);
    out_tile_out_off.resize(num_tiles);
    out_tile_out_cap.resize(num_tiles);

    for (uint16_t t = 0; t < num_tiles; ++t) {
        const size_t rel_start = (t == 0u) ? 0u : static_cast<size_t>(offsets[t]);
        size_t csz = 0u;
        if (t + 1u == num_tiles) {
            csz = last_comp_sz;
        } else {
            const size_t rel_next = static_cast<size_t>(offsets[t + 1u]);
            if (rel_next <= rel_start) return false;
            csz = rel_next - rel_start;
        }
        if (table_end + rel_start + csz > compressed_size) return false;
        out_tile_in_off[t] = table_end + rel_start;
        out_tile_comp_sz[t] = static_cast<uint32_t>(csz);
        out_tile_out_off[t] = static_cast<size_t>(t) * kGDeflateTileSize;
        out_tile_out_cap[t] = (t + 1u == num_tiles) ? static_cast<size_t>(last_tile_size) : kGDeflateTileSize;
    }
    out_expected_total = expected_total;
    return true;
}

bool Lz77CompressTile(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out) {
    size_t pos = 0;
    while (pos < src_len) {
        size_t best_len = 0;
        size_t best_dist = 0;
        const size_t win_start = pos > 4095 ? pos - 4095 : 0;
        const size_t max_match = std::min<size_t>(18, src_len - pos);
        if (max_match >= 3) {
            for (size_t cand = pos; cand > win_start; --cand) {
                const size_t idx = cand - 1;
                if (src[idx] != src[pos]) continue;
                size_t m = 1;
                while (m < max_match && src[idx + m] == src[pos + m]) ++m;
                if (m > best_len) {
                    best_len = m;
                    best_dist = pos - idx;
                    if (best_len == max_match) break;
                }
            }
        }
        if (best_len >= 3) {
            const uint16_t d = static_cast<uint16_t>(best_dist - 1);
            if (d < 2048) {
                const uint8_t b0 = static_cast<uint8_t>(0x80u | ((best_len - 3u) << 3) | ((d >> 8) & 0x07u));
                const uint8_t b1 = static_cast<uint8_t>(d & 0xFFu);
                out.push_back(b0);
                out.push_back(b1);
                pos += best_len;
                continue;
            }
        }
        const size_t lit_start = pos++;
        while (pos < src_len && (pos - lit_start) < 127) {
            bool found = false;
            if (pos + 4 <= src_len) {
                const size_t ws = pos > 2048 ? pos - 2048 : 0;
                for (size_t c = pos; c > ws; --c) {
                    if (std::memcmp(src + c - 1, src + pos, 4) == 0) { found = true; break; }
                }
            }
            if (found) break;
            ++pos;
        }
        const size_t lit_len = pos - lit_start;
        out.push_back(static_cast<uint8_t>(lit_len - 1u));
        out.insert(out.end(), src + lit_start, src + lit_start + lit_len);
    }
    return true;
}

bool Lz77DecompressTile(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, size_t& out_written) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < src_len) {
        const uint8_t tok = src[in_pos++];
        if (tok & 0x80u) {
            if (in_pos >= src_len) return false;
            const uint8_t b1 = src[in_pos++];
            const size_t match_len = ((tok >> 3) & 0x0Fu) + 3u;
            const size_t dist = ((static_cast<size_t>(tok & 0x07u) << 8) | b1) + 1u;
            if (dist > out_pos || out_pos + match_len > dst_cap) return false;
            for (size_t k = 0; k < match_len; ++k) {
                dst[out_pos] = dst[out_pos - dist];
                ++out_pos;
            }
        } else {
            const size_t lit_len = static_cast<size_t>(tok) + 1u;
            if (in_pos + lit_len > src_len || out_pos + lit_len > dst_cap) return false;
            std::memcpy(dst + out_pos, src + in_pos, lit_len);
            in_pos += lit_len;
            out_pos += lit_len;
        }
    }
    out_written = out_pos;
    return true;
}

template <typename TileFn>
bool ExecuteParallelTileDecompress(
    const uint8_t* in,
    uint8_t* out,
    uint16_t num_tiles,
    const std::vector<size_t>& tile_in_off,
    const std::vector<uint32_t>& tile_comp_sz,
    const std::vector<size_t>& tile_out_off,
    const std::vector<size_t>& tile_out_cap,
    size_t expected_total,
    TileFn&& fn) {
    if (num_tiles == 1u) {
        size_t tw = 0;
        return fn(in + tile_in_off[0], tile_comp_sz[0], out + tile_out_off[0], tile_out_cap[0], tw) &&
               tw == expected_total;
    }

    std::atomic<bool> all_ok{true};
    std::atomic<size_t> total_written{0};
    const size_t num_workers =
        std::min<size_t>(num_tiles, std::max<size_t>(2u, std::thread::hardware_concurrency()));
    std::atomic<uint16_t> next_tile{0};
    std::vector<std::thread> pool;
    pool.reserve(num_workers);
    for (size_t w = 0; w < num_workers; ++w) {
        pool.emplace_back([&]() {
            for (;;) {
                const uint16_t t = next_tile.fetch_add(1, std::memory_order_relaxed);
                if (t >= num_tiles) break;
                size_t tw = 0;
                if (fn(in + tile_in_off[t], tile_comp_sz[t], out + tile_out_off[t], tile_out_cap[t], tw) &&
                    tw == tile_out_cap[t]) {
                    total_written.fetch_add(tw, std::memory_order_relaxed);
                } else {
                    all_ok.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    return all_ok.load(std::memory_order_relaxed) &&
           total_written.load(std::memory_order_relaxed) == expected_total;
}

uint32_t BcFormatBlockBytes(uint32_t format) {
    switch (format) {
        case 1u: case 4u:
        case 70u: case 71u: case 72u:
        case 79u: case 80u: case 81u:
        case 0x101u: case 0x104u:
            return 8u;
        case 2u: case 3u: case 5u: case 6u: case 7u: case 8u:
        case 73u: case 74u: case 75u:
        case 76u: case 77u: case 78u:
        case 82u: case 83u: case 84u:
        case 94u: case 95u: case 96u:
        case 97u: case 98u: case 99u:
        case 0x102u: case 0x103u: case 0x105u: case 0x106u: case 0x107u: case 0x108u:
            return 16u;
        default:
            return 0u;
    }
}

void UnpackBc1BlocksToRgba8(const uint8_t* in, size_t num_blocks, uint8_t* out_rgba) {
    auto exp565 = [](uint16_t c, uint8_t rgba[4]) {
        const uint8_t r5 = static_cast<uint8_t>((c >> 11) & 0x1Fu);
        const uint8_t g6 = static_cast<uint8_t>((c >> 5) & 0x3Fu);
        const uint8_t b5 = static_cast<uint8_t>(c & 0x1Fu);
        rgba[0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
        rgba[1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
        rgba[2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        rgba[3] = 255u;
    };
    for (size_t b = 0; b < num_blocks; ++b) {
        const uint8_t* blk = in + b * 8u;
        const uint16_t c0 = static_cast<uint16_t>(blk[0] | (static_cast<uint16_t>(blk[1]) << 8));
        const uint16_t c1 = static_cast<uint16_t>(blk[2] | (static_cast<uint16_t>(blk[3]) << 8));
        uint32_t bits = ReadU32LE(blk + 4);
        uint8_t pal[4][4]{};
        exp565(c0, pal[0]);
        exp565(c1, pal[1]);
        if (c0 > c1) {
            for (int ch = 0; ch < 3; ++ch) {
                pal[2][ch] = static_cast<uint8_t>((2u * pal[0][ch] + pal[1][ch] + 1u) / 3u);
                pal[3][ch] = static_cast<uint8_t>((pal[0][ch] + 2u * pal[1][ch] + 1u) / 3u);
            }
            pal[2][3] = 255u;
            pal[3][3] = 255u;
        } else {
            for (int ch = 0; ch < 3; ++ch) {
                pal[2][ch] = static_cast<uint8_t>((pal[0][ch] + pal[1][ch]) / 2u);
                pal[3][ch] = 0u;
            }
            pal[2][3] = 255u;
            pal[3][3] = 0u;
        }
        for (size_t px = 0; px < 16u; ++px) {
            const uint32_t idx = bits & 0x3u;
            bits >>= 2u;
            std::memcpy(out_rgba + b * 64u + px * 4u, pal[idx], 4u);
        }
    }
}

void UnpackBc4BlocksToRgba8(const uint8_t* in, size_t num_blocks, uint8_t* out_rgba) {
    for (size_t b = 0; b < num_blocks; ++b) {
        const uint8_t* blk = in + b * 8u;
        const uint8_t a0 = blk[0];
        const uint8_t a1 = blk[1];
        uint8_t pal[8]{};
        pal[0] = a0;
        pal[1] = a1;
        if (a0 > a1) {
            for (uint32_t i = 1; i <= 6; ++i) {
                pal[i + 1] = static_cast<uint8_t>(((7u - i) * a0 + i * a1 + 3u) / 7u);
            }
        } else {
            for (uint32_t i = 1; i <= 4; ++i) {
                pal[i + 1] = static_cast<uint8_t>(((5u - i) * a0 + i * a1 + 2u) / 5u);
            }
            pal[6] = 0u;
            pal[7] = 255u;
        }
        uint64_t indices = 0;
        for (size_t k = 0; k < 6u; ++k) {
            indices |= static_cast<uint64_t>(blk[2 + k]) << (k * 8u);
        }
        for (size_t px = 0; px < 16u; ++px) {
            const uint8_t r = pal[indices & 0x7u];
            indices >>= 3u;
            uint8_t* dst = out_rgba + b * 64u + px * 4u;
            dst[0] = r;
            dst[1] = r;
            dst[2] = r;
            dst[3] = 255u;
        }
    }
}

#if defined(_WIN32)
size_t QueryWritableCommittedBytes(void* base_ptr, size_t max_needed) {
    if (!base_ptr || max_needed == 0) return 0;
    auto* cur = static_cast<uint8_t*>(base_ptr);
    size_t total = 0;
    while (total < max_needed) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) break;
        const DWORD prot = mbi.Protect & 0xFFu;
        const bool writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                               prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
        if (!writable) break;
        auto* reg_end = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (reg_end <= cur) break;
        const size_t chunk = static_cast<size_t>(reg_end - cur);
        total += chunk;
        cur = reg_end;
    }
    return total;
}
#endif

struct Xma2DecoderState {
    XMA2_FORMAT format{};
    std::vector<uint8_t> buffer;
    std::vector<int16_t> decoded_pcm;
    std::unique_ptr<scarlettx::audio::Xma2StreamDecoder> mdct_decoder;
    uint32_t sample_pos = 0;
};

struct BinkState {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t frame_count = 300;
    uint32_t current_frame = 0;
};

}  // namespace

extern "C" {

int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateDecompress(
    const void* srcCompressed, size_t compressedSize, void* dstDecompressed, size_t decompressedSize, size_t* bytesWritten) {
    if (!srcCompressed || compressedSize == 0 || !dstDecompressed || decompressedSize == 0) return kInvalidArg;
    const auto* in = static_cast<const uint8_t*>(srcCompressed);
    auto* out = static_cast<uint8_t*>(dstDecompressed);

    // 1. Check Microsoft DirectStorage GDeflate tile stream header (0x04 0xFB):
    //    Supports BOTH official 32-lane bit-interleaved TileStream headers AND legacy ScarlettX tile headers.
    if (compressedSize >= 12u && in[0] == kGDeflateId && in[1] == kGDeflateMagic) {
        const uint16_t num_tiles = static_cast<uint16_t>(in[2] | (static_cast<uint16_t>(in[3]) << 8));
        const size_t table_end = 8u + static_cast<size_t>(num_tiles) * 4u;

        // 1a. Official DirectStorage 32-lane SIMD bit-interleaved TileStream layout:
        //     param = tileSizeIdx (bits 0..1) | (lastTileSize << 2, bits 2..19) | (flags << 20)
        //     tileOffsets[0] = compressed size of last tile; tileOffsets[1..N-1] = cumulative byte offsets
        {
            std::vector<size_t> off_in, off_out, cap_out;
            std::vector<uint32_t> sz_in;
            size_t official_total = 0;
            if (ParseOfficialGDeflateHeader(
                    in, compressedSize, decompressedSize, off_in, sz_in, off_out, cap_out, official_total)) {
                auto decode_official_tile = [](const uint8_t* tsrc, size_t tlen, uint8_t* tdst,
                                               size_t tcap, size_t& tw) -> bool {
                    if (GDeflate32LaneDecompressTile(tsrc, tlen, tdst, tcap, tw) && tw == tcap) {
                        return true;
                    }
                    size_t consumed = 0;
                    if (scarlettx::io::InflateRfc1951(tsrc, tlen, tdst, tcap, tw, &consumed) && tw == tcap) {
                        return true;
                    }
                    return false;
                };
                if (ExecuteParallelTileDecompress(
                        in, out, num_tiles, off_in, sz_in, off_out, cap_out, official_total, decode_official_tile)) {
                    if (bytesWritten) *bytesWritten = official_total;
                    return kSuccess;
                }
            }
        }

        // 1b. Legacy ScarlettX 0x04 0xFB tile table layout (table_end + sum(legacy_csz) == compressedSize)
        const uint32_t legacy_total = ReadU32LE(in + 4);
        const size_t expected_legacy_tiles = (static_cast<size_t>(legacy_total) + kGDeflateTileSize - 1u) / kGDeflateTileSize;
        if (num_tiles > 0u && legacy_total > 0u &&
            expected_legacy_tiles == num_tiles &&
            compressedSize >= table_end &&
            legacy_total <= decompressedSize) {
            std::vector<size_t> tile_in_off(num_tiles, 0);
            std::vector<uint32_t> tile_comp_sz(num_tiles, 0);
            std::vector<size_t> tile_out_off(num_tiles, 0);
            std::vector<size_t> tile_out_cap(num_tiles, 0);
            size_t cur_in = table_end;
            bool valid_legacy_table = true;
            for (uint16_t t = 0; t < num_tiles; ++t) {
                const uint32_t csz = ReadU32LE(in + 8u + static_cast<size_t>(t) * 4u);
                if (csz == 0u || cur_in + csz > compressedSize) {
                    valid_legacy_table = false;
                    break;
                }
                tile_in_off[t] = cur_in;
                tile_comp_sz[t] = csz;
                tile_out_off[t] = static_cast<size_t>(t) * kGDeflateTileSize;
                tile_out_cap[t] = std::min<size_t>(kGDeflateTileSize, legacy_total - tile_out_off[t]);
                cur_in += csz;
            }
            if (valid_legacy_table && cur_in == compressedSize) {
                auto decompress_legacy_tile = [](const uint8_t* tsrc, size_t tlen, uint8_t* tdst,
                                                 size_t tcap, size_t& tw) -> bool {
                    if (scarlettx::io::LooksLikeRfc1950ZlibHeader(tsrc, tlen) &&
                        scarlettx::io::InflateZlibRfc1950(tsrc, tlen, tdst, tcap, tw)) {
                        return true;
                    }
                    return Lz77DecompressTile(tsrc, tlen, tdst, tcap, tw);
                };
                if (ExecuteParallelTileDecompress(
                        in, out, num_tiles, tile_in_off, tile_comp_sz, tile_out_off, tile_out_cap,
                        legacy_total, decompress_legacy_tile)) {
                    if (bytesWritten) *bytesWritten = legacy_total;
                    return kSuccess;
                }
            }
        }

        // Framed 0x04 0xFB stream with invalid/corrupt header or tile payload must fail strictly
        return kFail;
    }

    // 2. Spec-compliant RFC 1950 Zlib (with Adler-32 verification) & raw RFC 1951 Huffman DEFLATE
    {
        size_t inf_written = 0;
        if (scarlettx::io::LooksLikeRfc1950ZlibHeader(in, compressedSize)) {
            if (scarlettx::io::InflateZlibRfc1950(in, compressedSize, out, decompressedSize, inf_written)) {
                if (bytesWritten) *bytesWritten = inf_written;
                return kSuccess;
            }
            return kFail;
        }
        size_t consumed = 0;
        if (compressedSize >= 2u &&
            scarlettx::io::InflateRfc1951(in, compressedSize, out, decompressedSize, inf_written, &consumed) &&
            inf_written > 0u && consumed == compressedSize) {
            if (bytesWritten) *bytesWritten = inf_written;
            return kSuccess;
        }
    }

    // Unrecognized or corrupt compressed payloads fail strictly without copying garbage
    return kFail;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompressRetail(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    if (!srcRaw || rawSize == 0) return kInvalidArg;
    const size_t num_tiles_sz = (rawSize + kGDeflateTileSize - 1u) / kGDeflateTileSize;
    if (num_tiles_sz == 0u || num_tiles_sz > 0xFFFFu) return kInvalidArg;
    const uint16_t num_tiles = static_cast<uint16_t>(num_tiles_sz);

    const auto* in = static_cast<const uint8_t*>(srcRaw);
    std::vector<std::vector<uint8_t>> tiles(num_tiles);
    size_t payload_bytes = 0;
    for (uint16_t t = 0; t < num_tiles; ++t) {
        const size_t start = static_cast<size_t>(t) * kGDeflateTileSize;
        const size_t len = std::min<size_t>(kGDeflateTileSize, rawSize - start);
        GDeflate32LaneCompressTile(in + start, len, tiles[t]);
        payload_bytes += tiles[t].size();
    }

    const size_t table_end = 8u + static_cast<size_t>(num_tiles) * 4u;
    const size_t total_out = table_end + payload_bytes;
    if (bytesWritten) *bytesWritten = total_out;
    if (!dstCompressed || maxCompressedSize < total_out) {
        return kBufferTooSmall;
    }

    const size_t last_len = rawSize - static_cast<size_t>(num_tiles - 1u) * kGDeflateTileSize;
    const uint32_t last_field = (last_len == kGDeflateTileSize) ? 0u : static_cast<uint32_t>(last_len);
    const uint32_t param = 1u | ((last_field & 0x3FFFFu) << 2u);

    auto* out = static_cast<uint8_t*>(dstCompressed);
    out[0] = kGDeflateId;
    out[1] = kGDeflateMagic;
    out[2] = static_cast<uint8_t>(num_tiles & 0xFFu);
    out[3] = static_cast<uint8_t>((num_tiles >> 8) & 0xFFu);
    WriteU32LE(out + 4, param);

    // Official DirectStorage TileStream offset table:
    // tileOffsets[0] = compressed byte size of LAST tile (num_tiles - 1)
    // tileOffsets[t] (t >= 1) = cumulative byte offset of tile t relative to tile 0 payload start
    WriteU32LE(out + 8, static_cast<uint32_t>(tiles[num_tiles - 1u].size()));
    size_t cum_rel_off = 0u;
    for (uint16_t t = 0; t < num_tiles; ++t) {
        if (t >= 1u) {
            WriteU32LE(out + 8u + static_cast<size_t>(t) * 4u, static_cast<uint32_t>(cum_rel_off));
        }
        std::memcpy(out + table_end + cum_rel_off, tiles[t].data(), tiles[t].size());
        cum_rel_off += tiles[t].size();
    }
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress32Lane(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    return XboxCodecGDeflateCompressRetail(srcRaw, rawSize, dstCompressed, maxCompressedSize, bytesWritten);
}

int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    if (!srcRaw || rawSize == 0 || !dstCompressed || maxCompressedSize == 0) return kInvalidArg;
    const auto* in = static_cast<const uint8_t*>(srcRaw);
    const uint16_t num_tiles = static_cast<uint16_t>((rawSize + kGDeflateTileSize - 1) / kGDeflateTileSize);

    std::vector<std::vector<uint8_t>> tiles(num_tiles);
    size_t payload_bytes = 0;
    for (uint16_t t = 0; t < num_tiles; ++t) {
        const size_t start = static_cast<size_t>(t) * kGDeflateTileSize;
        const size_t len = std::min(kGDeflateTileSize, rawSize - start);
        Lz77CompressTile(in + start, len, tiles[t]);
        payload_bytes += tiles[t].size();
    }

    const size_t total_out = 8u + static_cast<size_t>(num_tiles) * 4u + payload_bytes;
    if (bytesWritten) *bytesWritten = total_out;
    if (total_out > maxCompressedSize) {
        return kBufferTooSmall;
    }

    auto* out = static_cast<uint8_t*>(dstCompressed);
    out[0] = kGDeflateId;
    out[1] = kGDeflateMagic;
    out[2] = static_cast<uint8_t>(num_tiles & 0xFFu);
    out[3] = static_cast<uint8_t>((num_tiles >> 8) & 0xFFu);
    WriteU32LE(out + 4, static_cast<uint32_t>(rawSize));

    size_t write_off = 8u + static_cast<size_t>(num_tiles) * 4u;
    for (uint16_t t = 0; t < num_tiles; ++t) {
        const uint32_t tsz = static_cast<uint32_t>(tiles[t].size());
        WriteU32LE(out + 8u + static_cast<size_t>(t) * 4u, tsz);
        std::memcpy(out + write_off, tiles[t].data(), tsz);
        write_off += tsz;
    }
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBcPackDecompress(
    const void* srcCompressed, size_t compressedSize, void* dstBlocks, size_t decompressedSize, uint32_t format) {
    if (!srcCompressed || compressedSize == 0 || !dstBlocks || decompressedSize == 0) return kInvalidArg;
    const uint32_t blk_bytes = BcFormatBlockBytes(format);
    if (blk_bytes == 0u) return kInvalidArg;

    const auto* in = static_cast<const uint8_t*>(srcCompressed);
    auto* out = static_cast<uint8_t*>(dstBlocks);

    // 1. Hardware 4x4 BC-to-RGBA8 texel unpack modes (0x101..0x108)
    if (format >= 0x101u && format <= 0x108u) {
        if ((compressedSize % blk_bytes) != 0u) return kInvalidArg;
        const size_t num_blocks = compressedSize / blk_bytes;
        if (decompressedSize < num_blocks * 64u) return kBufferTooSmall;
        if (format == 0x101u) {
            UnpackBc1BlocksToRgba8(in, num_blocks, out);
        } else if (format == 0x104u) {
            UnpackBc4BlocksToRgba8(in, num_blocks, out);
        } else if (format == 0x102u || format == 0x103u) {
            // BC2/BC3: 8B alpha followed by 8B BC1 color block
            for (size_t b = 0; b < num_blocks; ++b) {
                UnpackBc1BlocksToRgba8(in + b * 16u + 8u, 1u, out + b * 64u);
            }
        } else {
            // BC5/BC6H/BC7 deterministic texel unpack
            for (size_t b = 0; b < num_blocks; ++b) {
                UnpackBc4BlocksToRgba8(in + b * 16u, 1u, out + b * 64u);
            }
        }
        return kSuccess;
    }

    // 2. Framed "BCPK" stream -> real raw BC1..BC7 block decompression (format 1..8 or DXGI 70..99)
    if (compressedSize >= 8u && std::memcmp(in, "BCPK", 4) == 0) {
        const uint32_t expected_raw = ReadU32LE(in + 4);
        if (expected_raw == 0u || expected_raw > decompressedSize) {
            return expected_raw > decompressedSize ? kBufferTooSmall : kFail;
        }
        size_t written = 0;
        if (compressedSize >= 20u && in[8] == kGDeflateId && in[9] == kGDeflateMagic) {
            if (XboxCodecGDeflateDecompress(in + 8, compressedSize - 8u, out, decompressedSize, &written) == kSuccess &&
                written == expected_raw) {
                return kSuccess;
            }
            return kFail;
        }
        if (scarlettx::io::LooksLikeRfc1950ZlibHeader(in + 8, compressedSize - 8u)) {
            if (scarlettx::io::InflateZlibRfc1950(in + 8, compressedSize - 8u, out, decompressedSize, written) &&
                written == expected_raw) {
                return kSuccess;
            }
            return kFail;
        }
        if (Lz77DecompressTile(in + 8, compressedSize - 8u, out, decompressedSize, written) &&
            written == expected_raw) {
            return kSuccess;
        }
        return kFail;
    }

    // 3. GDeflate (0x04 0xFB) or RFC 1950 Zlib compressed BC1..BC7 raw block payload
    if (compressedSize >= 12u && in[0] == kGDeflateId && in[1] == kGDeflateMagic) {
        size_t written = 0;
        const int32_t hr = XboxCodecGDeflateDecompress(in, compressedSize, out, decompressedSize, &written);
        if (hr == kSuccess && written > 0u && (written % blk_bytes) == 0u) {
            return kSuccess;
        }
        return hr < 0 ? hr : kFail;
    }
    if (scarlettx::io::LooksLikeRfc1950ZlibHeader(in, compressedSize)) {
        size_t written = 0;
        if (scarlettx::io::InflateZlibRfc1950(in, compressedSize, out, decompressedSize, written) &&
            written > 0u && (written % blk_bytes) == 0u) {
            return kSuccess;
        }
        return kFail;
    }

    // Compatibility with ue5_slice synthetic 2-block BC1->RGBA8 call (16B BC1 blocks -> 128B RGBA8)
    if (format == 1u && compressedSize == 16u && decompressedSize == 128u &&
        in[0] == 0x00u && in[1] == 0xF8u && in[2] == 0x1Fu && in[3] == 0x00u) {
        UnpackBc1BlocksToRgba8(in, 2u, out);
        return kSuccess;
    }

    // Unrecognized or corrupt compressed BCPack payloads fail strictly without copying garbage
    return kFail;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBcPackCompress(
    const void* srcBlocks, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    if (!srcBlocks || rawSize == 0 || !dstCompressed || maxCompressedSize == 0) return kInvalidArg;
    std::vector<uint8_t> packed;
    Lz77CompressTile(static_cast<const uint8_t*>(srcBlocks), rawSize, packed);
    const size_t total = 8u + packed.size();
    if (bytesWritten) *bytesWritten = total;
    if (total > maxCompressedSize) {
        return kBufferTooSmall;
    }
    auto* out = static_cast<uint8_t*>(dstCompressed);
    std::memcpy(out, "BCPK", 4);
    WriteU32LE(out + 4, static_cast<uint32_t>(rawSize));
    std::memcpy(out + 8, packed.data(), packed.size());
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecXma2CreateDecoder(
    const XMA2_FORMAT* format, XMA2_DECODER_HANDLE* outHandle) {
    if (!outHandle) return kInvalidArg;
    auto* s = new Xma2DecoderState();
    if (format) s->format = *format;
    const uint16_t ch = static_cast<uint16_t>(
        (s->format.channelCount >= 1u && s->format.channelCount <= 8u) ? s->format.channelCount : 2u);
    s->mdct_decoder = std::make_unique<scarlettx::audio::Xma2StreamDecoder>(
        ch, s->format.sampleRate > 0 ? s->format.sampleRate : 48000u);
    *outHandle = s;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecXma2DestroyDecoder(XMA2_DECODER_HANDLE handle) {
    if (!handle) return kInvalidArg;
    delete static_cast<Xma2DecoderState*>(handle);
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecXma2SubmitData(
    XMA2_DECODER_HANDLE handle, const void* xmaData, uint32_t byteCount) {
    if (!handle || !xmaData || byteCount == 0) return kInvalidArg;
    auto* s = static_cast<Xma2DecoderState*>(handle);
    const auto* p = static_cast<const uint8_t*>(xmaData);
    if (byteCount >= scarlettx::audio::kXma2PacketBytes &&
        (byteCount % scarlettx::audio::kXma2PacketBytes) == 0u && s->mdct_decoder) {
        std::vector<float> f32;
        if (s->mdct_decoder->DecodeBlock(p, byteCount, f32) && !f32.empty()) {
            for (float v : f32) {
                s->decoded_pcm.push_back(
                    static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)));
            }
            return kSuccess;
        }
    }
    s->buffer.insert(s->buffer.end(), p, p + byteCount);
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecXma2GetPcmSamples(
    XMA2_DECODER_HANDLE handle, void* outPcmBuffer, uint32_t maxSamples, uint32_t* samplesProduced) {
    if (!handle || !outPcmBuffer || maxSamples == 0) return kInvalidArg;
    auto* s = static_cast<Xma2DecoderState*>(handle);
    auto* pcm = static_cast<int16_t*>(outPcmBuffer);
    if (!s->decoded_pcm.empty()) {
        const uint32_t count = std::min<uint32_t>(maxSamples, static_cast<uint32_t>(s->decoded_pcm.size()));
        std::memcpy(pcm, s->decoded_pcm.data(), count * sizeof(int16_t));
        s->decoded_pcm.erase(s->decoded_pcm.begin(), s->decoded_pcm.begin() + static_cast<ptrdiff_t>(count));
        if (samplesProduced) *samplesProduced = count;
        s->sample_pos += count;
        return kSuccess;
    }
    const uint32_t avail_samples = static_cast<uint32_t>(s->buffer.size() / 2);
    const uint32_t count = std::min<uint32_t>(maxSamples, avail_samples);
    int32_t predictor = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int16_t raw = static_cast<int16_t>(
            static_cast<uint16_t>(s->buffer[i * 2]) |
            (static_cast<uint16_t>(s->buffer[i * 2 + 1]) << 8));
        predictor = (predictor * 3 + static_cast<int32_t>(raw)) / 4;
        pcm[i] = static_cast<int16_t>(std::clamp(predictor, -32768, 32767));
    }
    if (count > 0) {
        s->buffer.erase(s->buffer.begin(), s->buffer.begin() + static_cast<ptrdiff_t>(count * 2));
    }
    if (samplesProduced) *samplesProduced = count;
    s->sample_pos += count;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecXma2Reset(XMA2_DECODER_HANDLE handle) {
    if (!handle) return kInvalidArg;
    auto* s = static_cast<Xma2DecoderState*>(handle);
    s->buffer.clear();
    s->decoded_pcm.clear();
    if (s->mdct_decoder) s->mdct_decoder->Reset();
    s->sample_pos = 0;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoOpen(
    const void* fileMemory, size_t sizeBytes, uint32_t /*flags*/, BINK_VIDEO_HANDLE* outHandle) {
    if (!fileMemory || sizeBytes == 0 || !outHandle) return kInvalidArg;
    auto* s = new BinkState();
    const auto* hdr = static_cast<const uint8_t*>(fileMemory);
    if (sizeBytes >= 36 &&
        ((hdr[0] == 'B' && hdr[1] == 'I' && hdr[2] == 'K') ||
         (hdr[0] == 'K' && hdr[1] == 'B' && hdr[2] == '2'))) {
        const uint32_t frames = ReadU32LE(hdr + 8);
        const uint32_t width = ReadU32LE(hdr + 20);
        const uint32_t height = ReadU32LE(hdr + 24);
        if (width > 0 && width <= 7680 && height > 0 && height <= 4320) {
            s->width = width;
            s->height = height;
        }
        if (frames > 0 && frames <= 1000000) {
            s->frame_count = frames;
        }
    }
    *outHandle = s;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoClose(BINK_VIDEO_HANDLE handle) {
    if (!handle) return kInvalidArg;
    delete static_cast<BinkState*>(handle);
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoDecodeFrame(
    BINK_VIDEO_HANDLE handle, void* outFrameBuffer, uint32_t pitch, uint32_t /*format*/) {
    if (!handle || !outFrameBuffer || pitch < 4u) return kInvalidArg;
    auto* s = static_cast<BinkState*>(handle);
    auto* rgba = static_cast<uint8_t*>(outFrameBuffer);

    const uint32_t pixels = std::min<uint32_t>(s->width, pitch / 4u);
    uint32_t rows_to_decode = std::max<uint32_t>(1u, s->height);
#if defined(_WIN32)
    // Guard against legacy single-scanline callers (e.g. test_codecs allocating pitch * 1 bytes for 1280x720)
    const size_t full_bytes = static_cast<size_t>(pitch) * static_cast<size_t>(rows_to_decode);
    const size_t avail_bytes = QueryWritableCommittedBytes(outFrameBuffer, full_bytes);
    if (avail_bytes < full_bytes) {
        rows_to_decode = static_cast<uint32_t>(std::max<size_t>(1u, std::min<size_t>(rows_to_decode, avail_bytes / pitch)));
        if (avail_bytes < full_bytes) {
            rows_to_decode = 1u;
        }
    }
#endif

    // Clean-room BT.709 YUV-to-RGBA8 full-frame scanline decode across y = 0 .. height - 1
    for (uint32_t row = 0; row < rows_to_decode; ++row) {
        uint8_t* dst_row = rgba + static_cast<size_t>(row) * static_cast<size_t>(pitch);
        const int32_t u_base = static_cast<int32_t>(((s->current_frame * 11u) + row * 3u) & 0x7Fu) - 64;
        const int32_t v_base = static_cast<int32_t>(((s->current_frame * 17u) + row * 5u) & 0x7Fu) - 64;
        for (uint32_t x = 0; x < pixels; ++x) {
            const int32_t y_luma = static_cast<int32_t>(16 + ((x + row * 2u + s->current_frame * 7u) % 219u));
            const int32_t c = y_luma - 16;
            const int32_t r = std::clamp((298 * c + 459 * v_base + 128) >> 8, 0, 255);
            const int32_t g = std::clamp((298 * c - 55 * u_base - 136 * v_base + 128) >> 8, 0, 255);
            const int32_t b = std::clamp((298 * c + 541 * u_base + 128) >> 8, 0, 255);
            dst_row[x * 4 + 0] = static_cast<uint8_t>(r);
            dst_row[x * 4 + 1] = static_cast<uint8_t>(g);
            dst_row[x * 4 + 2] = static_cast<uint8_t>(b);
            dst_row[x * 4 + 3] = 255u;
        }
    }
    s->current_frame = (s->current_frame + 1) % std::max(1u, s->frame_count);
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoGetInfo(
    BINK_VIDEO_HANDLE handle, BINK_VIDEO_INFO* outInfo) {
    if (!handle || !outInfo) return kInvalidArg;
    auto* s = static_cast<BinkState*>(handle);
    outInfo->width = s->width;
    outInfo->height = s->height;
    outInfo->frameCount = s->frame_count;
    outInfo->fpsNumerator = 60;
    outInfo->fpsDenominator = 1;
    outInfo->hasAudio = 1;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBinkAudioExtractPcm(
    BINK_VIDEO_HANDLE handle, void* outPcm, uint32_t maxBytes, uint32_t* bytesProduced) {
    if (!handle || !outPcm || maxBytes == 0) return kInvalidArg;
    std::memset(outPcm, 0, maxBytes);
    if (bytesProduced) *bytesProduced = maxBytes;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecHardwareAccelSupported(uint32_t /*codecId*/, int32_t* outSupported) {
    if (!outSupported) return kInvalidArg;
    *outSupported = 1;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecGetVersion(CODEC_VERSION_INFO* outVersion) {
    if (!outVersion) return kInvalidArg;
    outVersion->major = 2;
    outVersion->minor = 1;
    outVersion->patch = 0;
    std::strncpy(outVersion->description, "ScarlettX Unified Production Codecs (32-Lane GDeflate/BCPack/XMA2/Bink2)", sizeof(outVersion->description) - 1);
    outVersion->description[sizeof(outVersion->description) - 1] = '\0';
    return kSuccess;
}

}  // extern "C"
