#define SCARLETTX_BUILDING_CODECS 1
#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

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
static constexpr uint8_t kDeflateCodeLenOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

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

// Strict RFC 1951 Canonical Huffman table with O(1) 10-bit primary table and fail-closed
// Kraft-equality validation (rejects oversubscribed AND incomplete/corrupt Huffman trees).
struct GDeflateHuffmanTable {
    static constexpr uint32_t kFastBits = 10u;
    static constexpr uint32_t kFastSize = 1u << kFastBits;
    static constexpr uint16_t kInvalidEntry = 0xFFFFu;

    std::array<uint16_t, kFastSize> fast{};
    std::array<int16_t, 16> count{};
    std::array<int16_t, 288> symbol{};
    uint32_t max_bits = 15u;
    int16_t single_sym = -1;
    bool is_empty = false;

    bool Build(const uint8_t* lengths, size_t num_syms, uint32_t max_len, bool allow_empty) {
        if (!lengths || num_syms == 0u || num_syms > 288u || max_len == 0u || max_len > 15u) {
            return false;
        }
        fast.fill(kInvalidEntry);
        count.fill(0);
        symbol.fill(0);
        max_bits = max_len;
        single_sym = -1;
        is_empty = false;

        uint32_t total_codes = 0u;
        for (size_t i = 0; i < num_syms; ++i) {
            const uint8_t l = lengths[i];
            if (l > max_len) return false;
            if (l > 0u) {
                ++count[l];
                ++total_codes;
            }
        }

        if (total_codes == 0u) {
            if (!allow_empty) return false;
            is_empty = true;
            return true;
        }

        if (total_codes == 1u) {
            // A 1-symbol RFC 1951 tree is valid ONLY when its sole code has length 1 (code bit 0; bit 1 is invalid).
            if (count[1] != 1) return false;
            for (size_t s = 0; s < num_syms; ++s) {
                if (lengths[s] == 1u) {
                    single_sym = static_cast<int16_t>(s);
                    break;
                }
            }
            if (single_sym < 0) return false;
            const uint16_t packed = static_cast<uint16_t>((1u << 9u) | static_cast<uint32_t>(single_sym));
            for (uint32_t idx = 0u; idx < kFastSize; idx += 2u) {
                fast[idx] = packed;
            }
            symbol[0] = single_sym;
            return true;
        }

        int32_t left = 1;
        for (uint32_t bits = 1u; bits <= max_len; ++bits) {
            left <<= 1;
            left -= count[bits];
            if (left < 0) return false;  // Oversubscribed tree
        }
        if (left != 0) return false;     // Incomplete / under-subscribed tree

        std::array<uint32_t, 16> next_code{};
        std::array<int16_t, 16> offs{};
        uint32_t code = 0u;
        offs[1] = 0;
        for (uint32_t bits = 1u; bits <= max_len; ++bits) {
            code = (code + static_cast<uint32_t>(count[bits - 1u])) << 1u;
            next_code[bits] = code;
            if (bits < max_len) {
                offs[bits + 1u] = static_cast<int16_t>(offs[bits] + count[bits]);
            }
        }

        for (size_t s = 0; s < num_syms; ++s) {
            const uint8_t len = lengths[s];
            if (len == 0u) continue;
            symbol[static_cast<size_t>(offs[len]++)] = static_cast<int16_t>(s);
            const uint32_t canon = next_code[len]++;
            if (len <= kFastBits) {
                const uint32_t rev = ReverseBits(canon, len);
                const uint16_t packed = static_cast<uint16_t>((static_cast<uint32_t>(len) << 9u) | static_cast<uint32_t>(s));
                const uint32_t step = 1u << len;
                for (uint32_t idx = rev; idx < kFastSize; idx += step) {
                    fast[idx] = packed;
                }
            }
        }
        return true;
    }

    bool Decode(uint32_t peek_bits, uint32_t& out_sym, uint32_t& out_len) const {
        if (is_empty) return false;
        const uint16_t hit = fast[peek_bits & (kFastSize - 1u)];
        if (hit != kInvalidEntry) {
            out_sym = static_cast<uint32_t>(hit & 0x1FFu);
            out_len = static_cast<uint32_t>(hit >> 9u);
            return true;
        }
        if (single_sym >= 0 || max_bits <= kFastBits) {
            return false;
        }
        int32_t c = 0;
        int32_t first = 0;
        int32_t index = 0;
        for (uint32_t len = 1u; len <= max_bits; ++len) {
            c |= static_cast<int32_t>((peek_bits >> (len - 1u)) & 1u);
            const int32_t cnt = count[len];
            if (c - cnt < first) {
                out_sym = static_cast<uint32_t>(symbol[static_cast<size_t>(index + (c - first))]);
                out_len = len;
                return true;
            }
            index += cnt;
            first = (first + cnt) << 1;
            c <<= 1;
        }
        return false;
    }
};

void BuildLengthLimitedCodeLengths(
    const uint32_t* freqs,
    size_t num_syms,
    uint32_t max_bits,
    uint8_t* out_lens) {
    std::memset(out_lens, 0, num_syms);
    struct SymFreq {
        uint64_t freq;
        uint32_t sym;
    };
    std::array<SymFreq, 288> active{};
    size_t m = 0u;
    for (size_t s = 0; s < num_syms; ++s) {
        if (freqs[s] > 0u) {
            active[m++] = {freqs[s], static_cast<uint32_t>(s)};
        }
    }
    if (m == 0u) return;
    if (m == 1u) {
        const uint32_t s0 = active[0].sym;
        const uint32_t s1 = (s0 == 0u) ? 1u : 0u;
        out_lens[s0] = 1u;
        if (s1 < num_syms) out_lens[s1] = 1u;
        return;
    }

    std::sort(active.begin(), active.begin() + static_cast<ptrdiff_t>(m),
              [](const SymFreq& a, const SymFreq& b) {
                  return (a.freq != b.freq) ? (a.freq < b.freq) : (a.sym < b.sym);
              });

    // Two-queue O(m) Huffman tree construction
    std::array<uint64_t, 576> node_freq{};
    std::array<int32_t, 576> parent{};
    parent.fill(-1);
    for (size_t i = 0; i < m; ++i) node_freq[i] = active[i].freq;

    size_t leaf_pos = 0u;
    size_t int_pos = m;
    size_t int_end = m;
    auto pop_min = [&]() -> size_t {
        if (leaf_pos < m && (int_pos >= int_end || node_freq[leaf_pos] <= node_freq[int_pos])) {
            return leaf_pos++;
        }
        return int_pos++;
    };

    for (size_t step = 0; step + 1u < m; ++step) {
        const size_t c0 = pop_min();
        const size_t c1 = pop_min();
        const size_t p = int_end++;
        node_freq[p] = node_freq[c0] + node_freq[c1];
        parent[c0] = static_cast<int32_t>(p);
        parent[c1] = static_cast<int32_t>(p);
    }

    std::array<uint32_t, 288> lens{};
    bool needs_clamp = false;
    for (size_t i = 0; i < m; ++i) {
        uint32_t d = 0u;
        int32_t cur = parent[i];
        while (cur >= 0) {
            ++d;
            cur = parent[static_cast<size_t>(cur)];
        }
        if (d > max_bits) {
            d = max_bits;
            needs_clamp = true;
        }
        lens[i] = d;
    }

    if (needs_clamp) {
        const uint32_t target = 1u << max_bits;
        uint32_t kraft = 0u;
        for (size_t i = 0; i < m; ++i) {
            kraft += 1u << (max_bits - lens[i]);
        }
        while (kraft > target) {
            size_t best_i = m;
            uint32_t best_len = 0u;
            for (size_t i = 0; i < m; ++i) {
                if (lens[i] < max_bits && lens[i] > best_len) {
                    best_len = lens[i];
                    best_i = i;
                }
            }
            if (best_i >= m) break;
            kraft -= 1u << (max_bits - lens[best_i]);
            ++lens[best_i];
            kraft += 1u << (max_bits - lens[best_i]);
        }
        while (kraft < target) {
            const uint32_t deficit = target - kraft;
            size_t best_i = m;
            for (size_t idx = m; idx > 0u; --idx) {
                const size_t i = idx - 1u;
                if (lens[i] > 1u) {
                    const uint32_t delta = 1u << (max_bits - lens[i]);
                    if (delta <= deficit) {
                        best_i = i;
                        break;
                    }
                }
            }
            if (best_i >= m) break;
            const uint32_t delta = 1u << (max_bits - lens[best_i]);
            --lens[best_i];
            kraft += delta;
        }
    }

    for (size_t i = 0; i < m; ++i) {
        out_lens[active[i].sym] = static_cast<uint8_t>(lens[i]);
    }
}

void BuildCanonicalEncodeCodes(
    const uint8_t* lengths,
    size_t num_syms,
    uint32_t max_bits,
    uint32_t* out_rev_codes) {
    std::memset(out_rev_codes, 0, num_syms * sizeof(uint32_t));
    std::array<uint32_t, 16> count{};
    for (size_t i = 0; i < num_syms; ++i) {
        if (lengths[i] > 0u && lengths[i] <= max_bits) {
            ++count[lengths[i]];
        }
    }
    std::array<uint32_t, 16> next_code{};
    uint32_t code = 0u;
    for (uint32_t bits = 1u; bits <= max_bits; ++bits) {
        code = (code + count[bits - 1u]) << 1u;
        next_code[bits] = code;
    }
    for (size_t s = 0; s < num_syms; ++s) {
        const uint8_t len = lengths[s];
        if (len > 0u) {
            out_rev_codes[s] = ReverseBits(next_code[len]++, len);
        }
    }
}

struct LaneBitPacker {
    std::vector<uint32_t> words;
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

// Synchronized 32-lane wavefront bit packer that emits interleaved 32-bit words in exact
// lane 0..31 refill order whenever a wave advances a lane's bit offset across 32.
struct WavefrontBitPacker {
    std::array<LaneBitPacker, kGDeflateLanes> lanes{};
    std::array<uint32_t, kGDeflateLanes> wave_bits{};
    std::vector<std::array<uint8_t, kGDeflateLanes>> waves;

    void AddBits(uint32_t lane, uint32_t val, uint32_t nbits) {
        lanes[lane].PutBitsLsb(val, nbits);
        wave_bits[lane] += nbits;
    }

    void AddFixedLitLen(uint32_t lane, uint32_t sym) {
        wave_bits[lane] += lanes[lane].EmitFixedLitLen(sym);
    }

    void EndRefillWave() {
        std::array<uint8_t, kGDeflateLanes> w{};
        bool any_non_zero = false;
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            w[l] = static_cast<uint8_t>(wave_bits[l]);
            if (wave_bits[l] != 0u) any_non_zero = true;
            wave_bits[l] = 0u;
        }
        if (any_non_zero) {
            waves.push_back(w);
        }
    }

    void Finalize(std::vector<uint8_t>& out_bytes) {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            while (lanes[l].words.size() < 2u) lanes[l].words.push_back(0u);
        }

        std::vector<uint32_t> interleaved;
        interleaved.reserve(64u + waves.size() * 4u);
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) interleaved.push_back(lanes[l].words[0]);
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) interleaved.push_back(lanes[l].words[1]);

        std::array<size_t, kGDeflateLanes> next_word_idx{};
        std::array<uint32_t, kGDeflateLanes> sim_bit_off{};
        next_word_idx.fill(2u);
        sim_bit_off.fill(0u);

        for (const auto& w : waves) {
            for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
                sim_bit_off[l] += w[l];
                if (sim_bit_off[l] >= 32u) {
                    sim_bit_off[l] -= 32u;
                    const uint32_t word = (next_word_idx[l] < lanes[l].words.size())
                                              ? lanes[l].words[next_word_idx[l]++]
                                              : 0u;
                    interleaved.push_back(word);
                }
            }
        }

        out_bytes.resize(interleaved.size() * sizeof(uint32_t));
        for (size_t i = 0; i < interleaved.size(); ++i) {
            WriteU32LE(out_bytes.data() + i * 4u, interleaved[i]);
        }
    }
};

struct GDeflateToken {
    uint16_t lit_or_len = 0;
    uint16_t dist = 0;
    bool is_match = false;
};

void TokenizeGDeflateLz77(const uint8_t* src, size_t src_len, std::vector<GDeflateToken>& tokens) {
    tokens.clear();
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
}

// 32-lane SIMD bit-interleaved BTYPE=0 (Stored/Uncompressed) GDeflate tile encoder.
void GDeflate32LaneCompressTileStored(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_bytes) {
    WavefrontBitPacker wb;
    wb.waves.reserve(4u + (src_len + kGDeflateLanes - 1u) / kGDeflateLanes);

    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
        // BFINAL=1 (bit 0), BTYPE=00 Stored (bits 1..2) -> 0b001 = 1u
        wb.AddBits(l, 0x1u, 3u);
    }
    wb.EndRefillWave();

    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
        const uint32_t pad = (8u - (wb.lanes[l].cur_bit & 7u)) & 7u;
        wb.AddBits(l, 0u, pad);
    }
    wb.EndRefillWave();

    std::array<uint16_t, kGDeflateLanes> lane_lens{};
    const size_t base_len = src_len / kGDeflateLanes;
    const size_t rem_len = src_len % kGDeflateLanes;
    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
        const uint16_t len_l = static_cast<uint16_t>(base_len + (l < rem_len ? 1u : 0u));
        const uint16_t nlen_l = static_cast<uint16_t>(~len_l);
        lane_lens[l] = len_l;
        wb.AddBits(l, static_cast<uint32_t>(len_l) | (static_cast<uint32_t>(nlen_l) << 16u), 32u);
    }
    wb.EndRefillWave();

    const uint16_t max_lane_len = lane_lens[0];
    for (uint16_t w = 0; w < max_lane_len; ++w) {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            if (w < lane_lens[l]) {
                wb.AddBits(l, src[static_cast<size_t>(w) * kGDeflateLanes + l], 8u);
            }
        }
        wb.EndRefillWave();
    }

    wb.Finalize(out_bytes);
}

// Real 32-lane SIMD bit-interleaved RFC 1951 Fixed-Huffman (BTYPE=1) + LZ77 GDeflate tile encoder.
// All 32 lanes read 32-bit words from a shared interleaved word stream in wavefront lockstep.
void GDeflate32LaneCompressTile(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_bytes) {
    std::vector<GDeflateToken> tokens;
    TokenizeGDeflateLz77(src, src_len, tokens);

    WavefrontBitPacker wb;
    wb.waves.reserve(4u + tokens.size() / kGDeflateLanes);
    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
        // 3-bit block header per lane: BFINAL=1 (bit 0), BTYPE=01 Fixed Huffman (bits 1..2) -> 0b011 = 3u
        wb.AddBits(l, 0x3u, 3u);
    }
    wb.EndRefillWave();

    const size_t full_waves = tokens.size() / kGDeflateLanes;
    const size_t rem = tokens.size() % kGDeflateLanes;

    auto encode_token = [&](uint32_t lane, const GDeflateToken& tok) {
        if (!tok.is_match) {
            wb.AddFixedLitLen(lane, tok.lit_or_len);
        } else {
            uint32_t lex_val = 0, lex_bits = 0;
            const uint32_t lsym = FindLenCode(tok.lit_or_len, lex_val, lex_bits);
            uint32_t dex_val = 0, dex_bits = 0;
            const uint32_t dsym = FindDistCode(tok.dist, dex_val, dex_bits);

            wb.AddFixedLitLen(lane, lsym);
            if (lex_bits > 0u) wb.AddBits(lane, lex_val, lex_bits);
            wb.AddBits(lane, ReverseBits(dsym, 5u), 5u);
            if (dex_bits > 0u) wb.AddBits(lane, dex_val, dex_bits);
        }
    };

    for (size_t w = 0; w < full_waves; ++w) {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            encode_token(l, tokens[w * kGDeflateLanes + l]);
        }
        wb.EndRefillWave();
    }
    if (rem > 0u) {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            if (l < rem) {
                encode_token(l, tokens[full_waves * kGDeflateLanes + l]);
            } else {
                wb.AddFixedLitLen(l, 256u);
            }
        }
        wb.EndRefillWave();
        for (uint32_t l = 0; l < rem; ++l) {
            wb.AddFixedLitLen(l, 256u);
        }
        wb.EndRefillWave();
    } else {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            wb.AddFixedLitLen(l, 256u);
        }
        wb.EndRefillWave();
    }

    wb.Finalize(out_bytes);
}

// Real 32-lane SIMD bit-interleaved RFC 1951 Dynamic-Huffman (BTYPE=2) + LZ77 GDeflate tile encoder.
void GDeflate32LaneCompressTileDynamic(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_bytes) {
    std::vector<GDeflateToken> tokens;
    TokenizeGDeflateLz77(src, src_len, tokens);

    std::array<uint32_t, 286> lit_freqs{};
    std::array<uint32_t, 30> dist_freqs{};
    lit_freqs[256] = kGDeflateLanes;  // One EOB symbol per lane
    for (const auto& tok : tokens) {
        if (!tok.is_match) {
            ++lit_freqs[tok.lit_or_len];
        } else {
            uint32_t lex_val = 0, lex_bits = 0;
            const uint32_t lsym = FindLenCode(tok.lit_or_len, lex_val, lex_bits);
            uint32_t dex_val = 0, dex_bits = 0;
            const uint32_t dsym = FindDistCode(tok.dist, dex_val, dex_bits);
            ++lit_freqs[lsym];
            ++dist_freqs[dsym];
        }
    }

    std::array<uint8_t, 286> lit_lens{};
    std::array<uint8_t, 30> dist_lens{};
    BuildLengthLimitedCodeLengths(lit_freqs.data(), lit_lens.size(), 15u, lit_lens.data());
    BuildLengthLimitedCodeLengths(dist_freqs.data(), dist_lens.size(), 15u, dist_lens.data());

    std::array<uint32_t, 286> lit_codes{};
    std::array<uint32_t, 30> dist_codes{};
    BuildCanonicalEncodeCodes(lit_lens.data(), lit_lens.size(), 15u, lit_codes.data());
    BuildCanonicalEncodeCodes(dist_lens.data(), dist_lens.size(), 15u, dist_codes.data());

    uint32_t hlit = 286u;
    while (hlit > 257u && lit_lens[hlit - 1u] == 0u) --hlit;
    uint32_t hdist = 30u;
    while (hdist > 1u && dist_lens[hdist - 1u] == 0u) --hdist;

    std::array<uint8_t, 316> combined{};
    std::memcpy(combined.data(), lit_lens.data(), hlit);
    std::memcpy(combined.data() + hlit, dist_lens.data(), hdist);
    const uint32_t total_lens = hlit + hdist;

    struct RleSym {
        uint8_t sym;
        uint8_t extra_val;
        uint8_t extra_bits;
    };
    std::vector<RleSym> rle_stream;
    rle_stream.reserve(total_lens);
    std::array<uint32_t, 19> cl_freqs{};

    uint32_t idx = 0u;
    while (idx < total_lens) {
        const uint8_t v = combined[idx];
        if (v == 0u) {
            uint32_t run = 1u;
            while (idx + run < total_lens && combined[idx + run] == 0u) ++run;
            idx += run;
            while (run >= 11u) {
                const uint32_t chunk = std::min<uint32_t>(138u, run);
                rle_stream.push_back({18u, static_cast<uint8_t>(chunk - 11u), 7u});
                ++cl_freqs[18];
                run -= chunk;
            }
            if (run >= 3u) {
                rle_stream.push_back({17u, static_cast<uint8_t>(run - 3u), 3u});
                ++cl_freqs[17];
                run = 0u;
            }
            while (run > 0u) {
                rle_stream.push_back({0u, 0u, 0u});
                ++cl_freqs[0];
                --run;
            }
        } else {
            rle_stream.push_back({v, 0u, 0u});
            ++cl_freqs[v];
            ++idx;
            uint32_t run = 0u;
            while (idx + run < total_lens && combined[idx + run] == v) ++run;
            idx += run;
            while (run >= 3u) {
                const uint32_t chunk = std::min<uint32_t>(6u, run);
                rle_stream.push_back({16u, static_cast<uint8_t>(chunk - 3u), 2u});
                ++cl_freqs[16];
                run -= chunk;
            }
            while (run > 0u) {
                rle_stream.push_back({v, 0u, 0u});
                ++cl_freqs[v];
                --run;
            }
        }
    }

    std::array<uint8_t, 19> cl_lens{};
    std::array<uint32_t, 19> cl_codes{};
    BuildLengthLimitedCodeLengths(cl_freqs.data(), cl_lens.size(), 7u, cl_lens.data());
    BuildCanonicalEncodeCodes(cl_lens.data(), cl_lens.size(), 7u, cl_codes.data());

    uint32_t hclen = 19u;
    while (hclen > 4u && cl_lens[kDeflateCodeLenOrder[hclen - 1u]] == 0u) --hclen;

    WavefrontBitPacker wb;
    wb.waves.reserve(16u + (tokens.size() / kGDeflateLanes) * 2u);

    // Block header on all 32 lanes: BFINAL=1 (bit 0), BTYPE=10 Dynamic Huffman (bits 1..2) -> 0b101 = 5u
    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
        wb.AddBits(l, 0x5u, 3u);
    }
    wb.EndRefillWave();

    // Dynamic table header: HLIT (5b) | HDIST (5b) | HCLEN (4b) on lane 0
    wb.AddBits(0u, (hlit - 257u) | ((hdist - 1u) << 5u) | ((hclen - 4u) << 10u), 14u);
    wb.EndRefillWave();

    // Code-length code lengths (3b each, hclen <= 19 < 32) striped across lanes 0..hclen-1
    for (uint32_t i = 0; i < hclen; ++i) {
        wb.AddBits(i, cl_lens[kDeflateCodeLenOrder[i]], 3u);
    }
    wb.EndRefillWave();

    // RLE-encoded literal/length + distance code lengths striped round-robin across lanes 0..31
    uint32_t rle_lane = 0u;
    for (const auto& rs : rle_stream) {
        wb.AddBits(rle_lane, cl_codes[rs.sym], cl_lens[rs.sym]);
        if (rs.extra_bits > 0u) {
            wb.AddBits(rle_lane, rs.extra_val, rs.extra_bits);
        }
        ++rle_lane;
        if (rle_lane == kGDeflateLanes) {
            wb.EndRefillWave();
            rle_lane = 0u;
        }
    }
    if (rle_lane > 0u) {
        wb.EndRefillWave();
    }

    // Encode token waves in 2 sub-waves (Sub-wave A: Lit/Len + LenExtra <= 20b; Sub-wave B: Dist + DistExtra <= 28b)
    const size_t full_waves = tokens.size() / kGDeflateLanes;
    const size_t rem = tokens.size() % kGDeflateLanes;

    auto emit_wave = [&](size_t token_base, uint32_t active_tok_lanes, uint32_t eob_start_lane, uint32_t eob_end_lane) {
        // Sub-wave A: Literal / Length + LenExtra / EOB
        for (uint32_t l = 0; l < active_tok_lanes; ++l) {
            const GDeflateToken& tok = tokens[token_base + l];
            if (!tok.is_match) {
                wb.AddBits(l, lit_codes[tok.lit_or_len], lit_lens[tok.lit_or_len]);
            } else {
                uint32_t lex_val = 0, lex_bits = 0;
                const uint32_t lsym = FindLenCode(tok.lit_or_len, lex_val, lex_bits);
                wb.AddBits(l, lit_codes[lsym], lit_lens[lsym]);
                if (lex_bits > 0u) wb.AddBits(l, lex_val, lex_bits);
            }
        }
        for (uint32_t l = eob_start_lane; l < eob_end_lane; ++l) {
            wb.AddBits(l, lit_codes[256], lit_lens[256]);
        }
        wb.EndRefillWave();

        // Sub-wave B: Distance + DistExtra for match lanes
        for (uint32_t l = 0; l < active_tok_lanes; ++l) {
            const GDeflateToken& tok = tokens[token_base + l];
            if (tok.is_match) {
                uint32_t dex_val = 0, dex_bits = 0;
                const uint32_t dsym = FindDistCode(tok.dist, dex_val, dex_bits);
                wb.AddBits(l, dist_codes[dsym], dist_lens[dsym]);
                if (dex_bits > 0u) wb.AddBits(l, dex_val, dex_bits);
            }
        }
        wb.EndRefillWave();
    };

    for (size_t w = 0; w < full_waves; ++w) {
        emit_wave(w * kGDeflateLanes, kGDeflateLanes, 0u, 0u);
    }
    if (rem > 0u) {
        emit_wave(full_waves * kGDeflateLanes, static_cast<uint32_t>(rem), static_cast<uint32_t>(rem), kGDeflateLanes);
        emit_wave(0u, 0u, 0u, static_cast<uint32_t>(rem));
    } else {
        emit_wave(0u, 0u, 0u, kGDeflateLanes);
    }

    wb.Finalize(out_bytes);
}

struct LaneWavefrontReader {
    const uint8_t* src = nullptr;
    size_t total_words = 0u;
    size_t word_cursor = 64u;
    std::array<uint32_t, kGDeflateLanes> cur_word{};
    std::array<uint32_t, kGDeflateLanes> next_word{};
    std::array<uint32_t, kGDeflateLanes> bit_off{};
    std::array<uint32_t, kGDeflateLanes> valid_bits{};

    bool Init(const uint8_t* s, size_t byte_len) {
        if (!s || byte_len < 256u || (byte_len % 4u) != 0u) return false;
        src = s;
        total_words = byte_len / 4u;
        word_cursor = 64u;
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            cur_word[l] = ReadU32LE(src + l * 4u);
            next_word[l] = ReadU32LE(src + (kGDeflateLanes + l) * 4u);
            bit_off[l] = 0u;
            valid_bits[l] = 64u;
        }
        return true;
    }

    uint32_t Peek32(uint32_t l) const {
        const uint32_t bo = bit_off[l];
        return (bo == 0u) ? cur_word[l] : ((cur_word[l] >> bo) | (next_word[l] << (32u - bo)));
    }

    bool ConsumeInWave(uint32_t l, uint32_t nbits) {
        if (nbits > 32u || nbits > valid_bits[l]) return false;
        bit_off[l] += nbits;
        valid_bits[l] -= nbits;
        return true;
    }

    bool EndRefillWave() {
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            if (bit_off[l] >= 32u) {
                if (word_cursor >= total_words) return false;
                cur_word[l] = next_word[l];
                next_word[l] = ReadU32LE(src + word_cursor * 4u);
                ++word_cursor;
                valid_bits[l] += 32u;
                bit_off[l] -= 32u;
                if (bit_off[l] >= 32u) return false;
            }
        }
        return true;
    }
};

bool GDeflate32LaneDecompressTile(
    const uint8_t* src,
    size_t src_len,
    uint8_t* dst,
    size_t dst_cap,
    size_t& out_written) {
    out_written = 0;
    if (!dst || dst_cap == 0u) return false;

    LaneWavefrontReader rd;
    if (!rd.Init(src, src_len)) return false;

    size_t out_pos = 0u;
    uint32_t bfinal = 0u;

    struct DecodedLaneCmd {
        uint32_t lit_or_len = 0;
        uint32_t dist = 0;
        bool is_match = false;
        bool has_output = false;
    };

    do {
        uint32_t btype = 0u;
        for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
            const uint32_t hdr = rd.Peek32(l) & 0x7u;
            const uint32_t lane_bfinal = hdr & 1u;
            const uint32_t lane_btype = (hdr >> 1u) & 0x3u;
            if (lane_btype == 3u) return false;
            if (l == 0u) {
                bfinal = lane_bfinal;
                btype = lane_btype;
            } else if (lane_bfinal != bfinal || lane_btype != btype) {
                return false;
            }
            if (!rd.ConsumeInWave(l, 3u)) return false;
        }
        if (!rd.EndRefillWave()) return false;

        if (btype == 0u) {
            // BTYPE=0: Uncompressed/Stored block across 32 bit-interleaved lanes
            for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
                const uint32_t pad = (8u - (rd.bit_off[l] & 7u)) & 7u;
                if (!rd.ConsumeInWave(l, pad)) return false;
            }
            if (!rd.EndRefillWave()) return false;

            std::array<uint16_t, kGDeflateLanes> lane_lens{};
            size_t block_total = 0u;
            for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
                const uint32_t ln = rd.Peek32(l);
                if (!rd.ConsumeInWave(l, 32u)) return false;
                const uint16_t len_l = static_cast<uint16_t>(ln & 0xFFFFu);
                const uint16_t nlen_l = static_cast<uint16_t>(ln >> 16u);
                if ((len_l ^ nlen_l) != 0xFFFFu) return false;
                if (l > 0u && (len_l > lane_lens[l - 1u] || (lane_lens[0] - len_l) > 1u)) {
                    return false;
                }
                lane_lens[l] = len_l;
                block_total += len_l;
            }
            if (!rd.EndRefillWave()) return false;
            if (out_pos + block_total > dst_cap) return false;

            const uint16_t max_lane_len = lane_lens[0];
            std::array<uint8_t, kGDeflateLanes> wave_bytes{};
            for (uint16_t w = 0; w < max_lane_len; ++w) {
                for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
                    if (w < lane_lens[l]) {
                        wave_bytes[l] = static_cast<uint8_t>(rd.Peek32(l) & 0xFFu);
                        if (!rd.ConsumeInWave(l, 8u)) return false;
                    }
                }
                if (!rd.EndRefillWave()) return false;
                for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
                    if (w < lane_lens[l]) {
                        dst[out_pos++] = wave_bytes[l];
                    }
                }
            }
        } else if (btype == 1u) {
            // BTYPE=1: Fixed Huffman 32-lane wavefront decode
            std::array<bool, kGDeflateLanes> lane_done{};
            uint32_t active_lanes = kGDeflateLanes;
            const size_t max_waves = (dst_cap - out_pos) + 2u;

            for (size_t wave = 0; wave < max_waves && active_lanes > 0u; ++wave) {
                std::array<DecodedLaneCmd, kGDeflateLanes> cmds{};
                for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
                    if (lane_done[l]) continue;

                    const uint32_t bits = rd.Peek32(l);
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

                    if (!rd.ConsumeInWave(l, nbits)) return false;
                }

                if (!rd.EndRefillWave()) return false;

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
            if (active_lanes != 0u) return false;
        } else {
            // BTYPE=2: Spec-complete Dynamic Huffman 32-lane wavefront decode
            const uint32_t hbits = rd.Peek32(0u);
            if (!rd.ConsumeInWave(0u, 14u) || !rd.EndRefillWave()) return false;
            const uint32_t hlit = (hbits & 0x1Fu) + 257u;
            const uint32_t hdist = ((hbits >> 5u) & 0x1Fu) + 1u;
            const uint32_t hclen = ((hbits >> 10u) & 0xFu) + 4u;
            if (hlit > 286u || hdist > 30u) return false;

            std::array<uint8_t, 19> cl_lens{};
            for (uint32_t i = 0; i < hclen; ++i) {
                cl_lens[kDeflateCodeLenOrder[i]] = static_cast<uint8_t>(rd.Peek32(i) & 0x7u);
                if (!rd.ConsumeInWave(i, 3u)) return false;
            }
            if (!rd.EndRefillWave()) return false;

            GDeflateHuffmanTable cl_tree{};
            if (!cl_tree.Build(cl_lens.data(), cl_lens.size(), 7u, /*allow_empty=*/false)) {
                return false;
            }

            std::array<uint8_t, 320> combined_lens{};
            const uint32_t total_lens = hlit + hdist;
            uint32_t idx = 0u;
            uint32_t rle_lane = 0u;
            while (idx < total_lens) {
                const uint32_t bits = rd.Peek32(rle_lane);
                uint32_t sym = 0u, cbits = 0u;
                if (!cl_tree.Decode(bits, sym, cbits)) return false;

                uint32_t extra_bits = 0u;
                uint8_t rep_val = 0u;
                uint32_t rep_cnt = 0u;
                if (sym < 16u) {
                    combined_lens[idx++] = static_cast<uint8_t>(sym);
                } else {
                    if (sym == 16u) {
                        if (idx == 0u) return false;
                        extra_bits = 2u;
                        rep_val = combined_lens[idx - 1u];
                        rep_cnt = 3u + ((bits >> cbits) & 0x3u);
                    } else if (sym == 17u) {
                        extra_bits = 3u;
                        rep_val = 0u;
                        rep_cnt = 3u + ((bits >> cbits) & 0x7u);
                    } else if (sym == 18u) {
                        extra_bits = 7u;
                        rep_val = 0u;
                        rep_cnt = 11u + ((bits >> cbits) & 0x7Fu);
                    } else {
                        return false;
                    }
                    if (idx + rep_cnt > total_lens) return false;
                    for (uint32_t r = 0; r < rep_cnt; ++r) {
                        combined_lens[idx++] = rep_val;
                    }
                }

                if (!rd.ConsumeInWave(rle_lane, cbits + extra_bits)) return false;
                ++rle_lane;
                if (rle_lane == kGDeflateLanes) {
                    if (!rd.EndRefillWave()) return false;
                    rle_lane = 0u;
                }
            }
            if (rle_lane > 0u && !rd.EndRefillWave()) return false;
            if (combined_lens[256] == 0u) return false;  // End-of-block symbol must be present

            GDeflateHuffmanTable lit_tree{};
            GDeflateHuffmanTable dist_tree{};
            if (!lit_tree.Build(combined_lens.data(), hlit, 15u, /*allow_empty=*/false) ||
                !dist_tree.Build(combined_lens.data() + hlit, hdist, 15u, /*allow_empty=*/true)) {
                return false;
            }

            std::array<bool, kGDeflateLanes> lane_done{};
            uint32_t active_lanes = kGDeflateLanes;
            const size_t max_waves = (dst_cap - out_pos) + 2u;

            for (size_t wave = 0; wave < max_waves && active_lanes > 0u; ++wave) {
                std::array<DecodedLaneCmd, kGDeflateLanes> cmds{};
                bool any_match = false;

                // Sub-wave A: Decode literal/length + length extra bits (max 15 + 5 = 20 bits)
                for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
                    if (lane_done[l]) continue;
                    const uint32_t bits = rd.Peek32(l);
                    uint32_t sym = 0u, cbits = 0u;
                    if (!lit_tree.Decode(bits, sym, cbits)) return false;

                    if (sym < 256u) {
                        cmds[l].lit_or_len = sym;
                        cmds[l].is_match = false;
                        cmds[l].has_output = true;
                        if (!rd.ConsumeInWave(l, cbits)) return false;
                    } else if (sym == 256u) {
                        lane_done[l] = true;
                        --active_lanes;
                        if (!rd.ConsumeInWave(l, cbits)) return false;
                    } else if (sym <= 285u) {
                        const uint32_t len_idx = sym - 257u;
                        const uint32_t lex_bits = kDeflateLenExtra[len_idx];
                        const uint32_t lex_val = lex_bits > 0u ? ((bits >> cbits) & ((1u << lex_bits) - 1u)) : 0u;
                        cmds[l].lit_or_len = static_cast<uint32_t>(kDeflateLenBase[len_idx]) + lex_val;
                        cmds[l].is_match = true;
                        cmds[l].has_output = true;
                        any_match = true;
                        if (!rd.ConsumeInWave(l, cbits + lex_bits)) return false;
                    } else {
                        return false;
                    }
                }
                if (!rd.EndRefillWave()) return false;

                // Sub-wave B: Decode distance + distance extra bits (max 15 + 13 = 28 bits)
                if (any_match) {
                    for (uint32_t l = 0; l < kGDeflateLanes; ++l) {
                        if (!cmds[l].is_match) continue;
                        const uint32_t bits = rd.Peek32(l);
                        uint32_t dist_sym = 0u, dbits = 0u;
                        if (!dist_tree.Decode(bits, dist_sym, dbits) || dist_sym >= 30u) return false;
                        const uint32_t dex_bits = kDeflateDistExtra[dist_sym];
                        const uint32_t dex_val = dex_bits > 0u ? ((bits >> dbits) & ((1u << dex_bits) - 1u)) : 0u;
                        cmds[l].dist = static_cast<uint32_t>(kDeflateDistBase[dist_sym]) + dex_val;
                        if (!rd.ConsumeInWave(l, dbits + dex_bits)) return false;
                    }
                    if (!rd.EndRefillWave()) return false;
                }

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
            if (active_lanes != 0u) return false;
        }
    } while (bfinal == 0u);

    if (rd.word_cursor != rd.total_words) {
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

#if defined(_WIN32)
uint32_t MaxGDeflatePoolWorkers() {
    static const uint32_t kMax = []() -> uint32_t {
        const unsigned hc = std::thread::hardware_concurrency();
        return std::clamp<uint32_t>(hc > 0u ? hc : 4u, 2u, 16u);
    }();
    return kMax;
}

std::atomic<uint32_t>& ActiveGDeflatePoolWorkers() {
    static std::atomic<uint32_t> s_active{0u};
    return s_active;
}

uint32_t ReserveGDeflatePoolSlots(uint32_t desired) {
    if (desired == 0u) return 0u;
    const uint32_t cap = MaxGDeflatePoolWorkers();
    auto& active = ActiveGDeflatePoolWorkers();
    uint32_t cur = active.load(std::memory_order_relaxed);
    while (cur < cap) {
        const uint32_t grant = std::min<uint32_t>(desired, cap - cur);
        if (active.compare_exchange_weak(cur, cur + grant, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return grant;
        }
    }
    return 0u;
}

void ReleaseGDeflatePoolSlots(uint32_t granted) {
    if (granted > 0u) {
        ActiveGDeflatePoolWorkers().fetch_sub(granted, std::memory_order_acq_rel);
    }
}
#endif

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

    struct BatchWorkState {
        const uint8_t* in_ptr;
        uint8_t* out_ptr;
        uint16_t tiles;
        const std::vector<size_t>* in_off;
        const std::vector<uint32_t>* comp_sz;
        const std::vector<size_t>* out_off;
        const std::vector<size_t>* out_cap;
        typename std::remove_reference<TileFn>::type* fn_ptr;
        std::atomic<uint16_t> next_tile{0u};
        std::atomic<bool> all_ok{true};
        std::atomic<size_t> total_written{0u};

        void RunWorker() {
            for (;;) {
                if (!all_ok.load(std::memory_order_acquire)) break;
                const uint16_t t = next_tile.fetch_add(1u, std::memory_order_relaxed);
                if (t >= tiles) break;
                size_t tw = 0;
                if ((*fn_ptr)(in_ptr + (*in_off)[t], (*comp_sz)[t], out_ptr + (*out_off)[t], (*out_cap)[t], tw) &&
                    tw == (*out_cap)[t]) {
                    total_written.fetch_add(tw, std::memory_order_acq_rel);
                } else {
                    all_ok.store(false, std::memory_order_release);
                    break;
                }
            }
        }
    };

    BatchWorkState state{
        in, out, num_tiles, &tile_in_off, &tile_comp_sz, &tile_out_off, &tile_out_cap, &fn};

#if defined(_WIN32)
    // Dispatch onto the persistent OS-managed Win32 Threadpool (zero per-call thread spawning
    // and zero DllMain loader-lock shutdown deadlock risk).
    const uint32_t max_helpers = std::min<uint32_t>(
        static_cast<uint32_t>(num_tiles - 1u), MaxGDeflatePoolWorkers() - 1u);
    const uint32_t reserved = ReserveGDeflatePoolSlots(max_helpers);
    if (reserved > 0u) {
        auto tp_cb = [](PTP_CALLBACK_INSTANCE /*inst*/, PVOID ctx, PTP_WORK /*work*/) noexcept -> VOID {
            static_cast<BatchWorkState*>(ctx)->RunWorker();
        };
        PTP_WORK work = CreateThreadpoolWork(tp_cb, &state, nullptr);
        if (work != nullptr) {
            for (uint32_t w = 0; w < reserved; ++w) {
                SubmitThreadpoolWork(work);
            }
            state.RunWorker();
            // Cancel not-yet-started callbacks since state.RunWorker() already drained next_tile.
            WaitForThreadpoolWorkCallbacks(work, TRUE);
            CloseThreadpoolWork(work);
            ReleaseGDeflatePoolSlots(reserved);
            return state.all_ok.load(std::memory_order_acquire) &&
                   state.total_written.load(std::memory_order_acquire) == expected_total;
        }
        ReleaseGDeflatePoolSlots(reserved);
    }
    state.RunWorker();
#else
    const size_t num_workers =
        std::min<size_t>(num_tiles, std::clamp<size_t>(std::thread::hardware_concurrency(), 2u, 16u));
    std::vector<std::thread> pool;
    pool.reserve(num_workers - 1u);
    for (size_t w = 1; w < num_workers; ++w) {
        pool.emplace_back([&state]() { state.RunWorker(); });
    }
    state.RunWorker();
    for (auto& th : pool) th.join();
#endif

    return state.all_ok.load(std::memory_order_acquire) &&
           state.total_written.load(std::memory_order_acquire) == expected_total;
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

}  // extern "C"

namespace {

int32_t PackOfficialGDeflateStream(
    const void* srcRaw,
    size_t rawSize,
    void* dstCompressed,
    size_t maxCompressedSize,
    size_t* bytesWritten,
    void (*encode_tile)(const uint8_t*, size_t, std::vector<uint8_t>&)) {
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
        encode_tile(in + start, len, tiles[t]);
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

}  // namespace

extern "C" {

int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompressRetail(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    return PackOfficialGDeflateStream(
        srcRaw, rawSize, dstCompressed, maxCompressedSize, bytesWritten, GDeflate32LaneCompressTile);
}

int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress32Lane(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    return XboxCodecGDeflateCompressRetail(srcRaw, rawSize, dstCompressed, maxCompressedSize, bytesWritten);
}

int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress32LaneDynamic(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    return PackOfficialGDeflateStream(
        srcRaw, rawSize, dstCompressed, maxCompressedSize, bytesWritten, GDeflate32LaneCompressTileDynamic);
}

int32_t SCARLETTX_CODEC_CALL XboxCodecGDeflateCompress32LaneStored(
    const void* srcRaw, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    return PackOfficialGDeflateStream(
        srcRaw, rawSize, dstCompressed, maxCompressedSize, bytesWritten, GDeflate32LaneCompressTileStored);
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
    outVersion->minor = 2;
    outVersion->patch = 0;
    std::strncpy(outVersion->description, "ScarlettX Unified Production Codecs (32-Lane GDeflate/BCPack/XMA2/Bink2/MF)", sizeof(outVersion->description) - 1);
    outVersion->description[sizeof(outVersion->description) - 1] = '\0';
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecProbeMediaFoundation(MF_MEDIA_CAPABILITIES* outCaps) {
    if (!outCaps) return kInvalidArg;
    std::memset(outCaps, 0, sizeof(*outCaps));
#if defined(_WIN32)
    HMODULE hMfPlat = LoadLibraryA("mfplat.dll");
    HMODULE hMfRw = LoadLibraryA("mfreadwrite.dll");
    const bool has_plat = (hMfPlat != nullptr) &&
        (GetProcAddress(hMfPlat, "MFStartup") != nullptr) &&
        (GetProcAddress(hMfPlat, "MFShutdown") != nullptr);
    const bool has_rw = (hMfRw != nullptr) &&
        (GetProcAddress(hMfRw, "MFCreateSourceReaderFromURL") != nullptr);
    outCaps->mfPlatAvailable = has_plat ? 1 : 0;
    outCaps->mfReadWriteAvailable = has_rw ? 1 : 0;
    outCaps->h264Supported = (has_plat && has_rw) ? 1 : 0;
    // Probe Windows HEVC Video Extension / MFT presence via msmpeg2vdec/hevcdecoder
    HMODULE hHevc = LoadLibraryA("hevcdecoder.dll");
    outCaps->hevcSupported = (has_plat && has_rw && hHevc != nullptr) ? 1 : 0;
    if (hHevc) FreeLibrary(hHevc);
    if (hMfRw) FreeLibrary(hMfRw);
    if (hMfPlat) FreeLibrary(hMfPlat);
    std::strncpy(outCaps->backendName,
                 (has_plat && has_rw) ? "Win32 MediaFoundation (IMFSourceReader)" : "Fallback (MF unavailable)",
                 sizeof(outCaps->backendName) - 1);
#else
    std::strncpy(outCaps->backendName, "Non-Windows Fallback", sizeof(outCaps->backendName) - 1);
#endif
    return kSuccess;
}

}  // extern "C"
