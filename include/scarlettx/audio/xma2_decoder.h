#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace scarlettx::audio {

#pragma pack(push, 1)
// Exact 52-byte Xbox Series X|S / GDK XMA2WAVEFORMATEX (wFormatTag = 0x0166, cbSize = 34).
struct Xma2WaveFormatEx {
    uint16_t wFormatTag;          // 0x0166
    uint16_t nChannels;           // 1..8
    uint32_t nSamplesPerSec;      // e.g. 48000
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;      // 16
    uint16_t cbSize;              // 34
    uint16_t NumStreams;          // ceil(nChannels / 2)
    uint32_t ChannelMask;
    uint32_t SamplesEncoded;
    uint32_t BytesPerBlock;       // Typically 2048..65536 (multiple of 2048)
    uint32_t PlayBegin;
    uint32_t PlayLength;
    uint32_t LoopBegin;
    uint32_t LoopLength;
    uint8_t LoopCount;
    uint8_t EncoderVersion;       // 3 or 4 (XMA2)
    uint16_t BlockCount;
};
#pragma pack(pop)

static_assert(sizeof(Xma2WaveFormatEx) == 52, "XMA2WAVEFORMATEX must be exactly 52 bytes");

constexpr size_t kXma2PacketBytes = 2048u;
constexpr size_t kXma2PacketHeaderBytes = 4u;
constexpr size_t kXma2PacketPayloadBytes = kXma2PacketBytes - kXma2PacketHeaderBytes;
constexpr size_t kXma2PacketPayloadBits = kXma2PacketPayloadBytes * 8u;  // 16352 bits

// Legacy 128-bin clean-room MDCT constants (packet_metadata == 5)
constexpr size_t kMdctBins = 128u;
constexpr size_t kMdctWindow = kMdctBins * 2u;
constexpr size_t kMdctBands = 16u;
constexpr uint32_t kCleanRoomMdctMeta = 0x5u;

// Retail WMA-Pro / XMA2 (0x0166) 512-sample frame & 1024-sample TDAC window constants
constexpr size_t kXma2FrameSamples = 512u;
constexpr size_t kXma2MaxWindow = kXma2FrameSamples * 2u;  // 1024 samples
constexpr size_t kXma2BarkBands = 25u;                     // 25 Bark critical bands @ 48 kHz
constexpr uint32_t kRetailWmaProMeta = 0x1u;
constexpr uint32_t kWmaProFrameSyncTag = 0x5u;             // 3-bit structured WMA-Pro subframe sync

// 25 psychoacoustic Bark critical scale-factor band boundaries across 512 MDCT bins @ 48 kHz
static constexpr uint16_t kBarkBandEdges512[kXma2BarkBands + 1u] = {
    0,  4,  8,  12, 16, 20,  24,  28,  32,  36,  42,  48,  56,
    64, 74, 86, 100, 116, 136, 160, 190, 226, 270, 326, 400, 512};

struct Xma2PacketHeaderInfo {
    uint32_t frame_count = 0;          // [31:26] 6 bits
    uint32_t frame_offset_in_bits = 0; // [25:11] 15 bits (relative to end of 32-bit packet header)
    uint32_t packet_metadata = 0;      // [10:8]  3 bits
    uint32_t packet_skip_count = 0;    // [7:0]   8 bits (0xFF = end-of-block sentinel)
};

inline Xma2PacketHeaderInfo ParseXma2PacketHeader(const uint8_t* pkt) {
    Xma2PacketHeaderInfo h{};
    if (!pkt) return h;
    const uint32_t be = (static_cast<uint32_t>(pkt[0]) << 24) |
                        (static_cast<uint32_t>(pkt[1]) << 16) |
                        (static_cast<uint32_t>(pkt[2]) << 8) |
                        static_cast<uint32_t>(pkt[3]);
    h.frame_count = (be >> 26) & 0x3Fu;
    h.frame_offset_in_bits = (be >> 11) & 0x7FFFu;
    h.packet_metadata = (be >> 8) & 0x07u;
    h.packet_skip_count = be & 0xFFu;
    return h;
}

inline void WriteXma2PacketHeader(uint8_t* pkt, const Xma2PacketHeaderInfo& h) {
    const uint32_t be = ((h.frame_count & 0x3Fu) << 26) |
                        ((h.frame_offset_in_bits & 0x7FFFu) << 11) |
                        ((h.packet_metadata & 0x07u) << 8) |
                        (h.packet_skip_count & 0xFFu);
    pkt[0] = static_cast<uint8_t>((be >> 24) & 0xFFu);
    pkt[1] = static_cast<uint8_t>((be >> 16) & 0xFFu);
    pkt[2] = static_cast<uint8_t>((be >> 8) & 0xFFu);
    pkt[3] = static_cast<uint8_t>(be & 0xFFu);
}

// MSB-first cross-packet bitreservoir reader & writer for XMA2 packet payloads
class Xma2BitReader {
public:
    Xma2BitReader(const uint8_t* data, size_t total_bits, size_t start_bit = 0)
        : data_(data), total_bits_(total_bits), bit_pos_(std::min(start_bit, total_bits)) {}

    size_t bit_pos() const { return bit_pos_; }
    size_t total_bits() const { return total_bits_; }
    size_t bits_remaining() const { return bit_pos_ < total_bits_ ? (total_bits_ - bit_pos_) : 0u; }

    bool ReadBits(uint32_t count, uint32_t& out) {
        if (count == 0) {
            out = 0;
            return true;
        }
        if (count > 32 || bit_pos_ + count > total_bits_) return false;
        uint32_t v = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t bp = bit_pos_++;
            const uint8_t bit = (data_[bp >> 3] >> (7u - (bp & 7u))) & 1u;
            v = (v << 1) | bit;
        }
        out = v;
        return true;
    }

    bool PeekBits(uint32_t count, uint32_t& out) const {
        if (count == 0) {
            out = 0;
            return true;
        }
        if (count > 32 || bit_pos_ + count > total_bits_) return false;
        uint32_t v = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t bp = bit_pos_ + i;
            const uint8_t bit = (data_[bp >> 3] >> (7u - (bp & 7u))) & 1u;
            v = (v << 1) | bit;
        }
        out = v;
        return true;
    }

    void Seek(size_t pos) { bit_pos_ = std::min(pos, total_bits_); }

private:
    const uint8_t* data_ = nullptr;
    size_t total_bits_ = 0;
    size_t bit_pos_ = 0;
};

class Xma2BitWriter {
public:
    void WriteBits(uint32_t val, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t shift = count - 1u - i;
            const uint8_t bit = static_cast<uint8_t>((val >> shift) & 1u);
            if ((bit_len_ & 7u) == 0u) bytes_.push_back(0u);
            bytes_.back() |= static_cast<uint8_t>(bit << (7u - (bit_len_ & 7u)));
            ++bit_len_;
        }
    }

    void AppendWriter(const Xma2BitWriter& other) {
        const auto& ob = other.bytes();
        const size_t obits = other.bit_len();
        for (size_t bi = 0; bi < obits; ++bi) {
            const uint8_t bit = (ob[bi >> 3] >> (7u - (bi & 7u))) & 1u;
            WriteBits(bit, 1u);
        }
    }

    size_t bit_len() const { return bit_len_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    size_t bit_len_ = 0;
};

struct MdctTables128 {
    float win[kMdctWindow]{};
    float cos_tbl[kMdctWindow][kMdctBins]{};

    MdctTables128() {
        constexpr double kPi = 3.14159265358979323846;
        const double inv2n = kPi / static_cast<double>(kMdctWindow);
        const double inv_n = kPi / static_cast<double>(kMdctBins);
        const double half_n = static_cast<double>(kMdctBins) * 0.5;
        for (size_t n = 0; n < kMdctWindow; ++n) {
            win[n] = static_cast<float>(std::sin(inv2n * (static_cast<double>(n) + 0.5)));
            for (size_t k = 0; k < kMdctBins; ++k) {
                cos_tbl[n][k] = static_cast<float>(
                    std::cos(inv_n * (static_cast<double>(n) + 0.5 + half_n) * (static_cast<double>(k) + 0.5)));
            }
        }
    }
};

inline const MdctTables128& GetMdctTables() {
    static const MdctTables128 tbl;
    return tbl;
}

inline void ForwardMdct128(const float time_in[kMdctWindow], float spec_out[kMdctBins]) {
    const auto& t = GetMdctTables();
    for (size_t k = 0; k < kMdctBins; ++k) {
        float sum = 0.0f;
        for (size_t n = 0; n < kMdctWindow; ++n) {
            sum += time_in[n] * t.win[n] * t.cos_tbl[n][k];
        }
        spec_out[k] = sum;
    }
}

inline void InverseMdct128(const float spec_in[kMdctBins], float win_time_out[kMdctWindow]) {
    const auto& t = GetMdctTables();
    constexpr float kScale = 2.0f / static_cast<float>(kMdctBins);
    for (size_t n = 0; n < kMdctWindow; ++n) {
        float sum = 0.0f;
        for (size_t k = 0; k < kMdctBins; ++k) {
            sum += spec_in[k] * t.cos_tbl[n][k];
        }
        win_time_out[n] = sum * kScale * t.win[n];
    }
}

// Fast O(M log M) radix-2 FFT + Type-IV DCT Princen-Bradley MDCT/IMDCT for WMA-Pro/XMA2
// subframe sizes M in {64, 128, 256, 512} (TDAC windows 128, 256, 512, 1024).
struct WmaProMdctTables {
    float win128[128]{};
    float win256[256]{};
    float win512[512]{};
    float win1024[1024]{};

    WmaProMdctTables() {
        InitSineWindow(win128, 128);
        InitSineWindow(win256, 256);
        InitSineWindow(win512, 512);
        InitSineWindow(win1024, 1024);
    }

    static void InitSineWindow(float* w, size_t len) {
        constexpr double kPi = 3.14159265358979323846;
        const double scale = kPi / static_cast<double>(len);
        for (size_t n = 0; n < len; ++n) {
            w[n] = static_cast<float>(std::sin(scale * (static_cast<double>(n) + 0.5)));
        }
    }

    const float* WindowForBins(size_t bins) const {
        switch (bins) {
            case 64u:  return win128;
            case 128u: return win256;
            case 256u: return win512;
            default:   return win1024;
        }
    }
};

inline const WmaProMdctTables& GetWmaProMdctTables() {
    static const WmaProMdctTables tbl;
    return tbl;
}

inline void FftRadix2(double* re, double* im, size_t n) {
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    constexpr double kPi = 3.14159265358979323846;
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const double wlen_re = std::cos(ang);
        const double wlen_im = std::sin(ang);
        const size_t half = len >> 1;
        for (size_t i = 0; i < n; i += len) {
            double w_re = 1.0;
            double w_im = 0.0;
            for (size_t j = 0; j < half; ++j) {
                const double u_re = re[i + j];
                const double u_im = im[i + j];
                const double v_re = re[i + j + half] * w_re - im[i + j + half] * w_im;
                const double v_im = re[i + j + half] * w_im + im[i + j + half] * w_re;
                re[i + j] = u_re + v_re;
                im[i + j] = u_im + v_im;
                re[i + j + half] = u_re - v_re;
                im[i + j + half] = u_im - v_im;
                const double nw_re = w_re * wlen_re - w_im * wlen_im;
                w_im = w_re * wlen_im + w_im * wlen_re;
                w_re = nw_re;
            }
        }
    }
}

inline void Dct4Var(const float* u, float* out_x, size_t m) {
    constexpr double kPi = 3.14159265358979323846;
    const size_t n2 = m >> 1;
    double z_re[256]{};
    double z_im[256]{};
    const double pi_over_m = kPi / static_cast<double>(m);
    for (size_t p = 0; p < n2; ++p) {
        const double a = u[2 * p];
        const double b = u[m - 1 - 2 * p];
        const double phi = -pi_over_m * static_cast<double>(p);
        const double c = std::cos(phi);
        const double s = std::sin(phi);
        z_re[p] = a * c - b * s;
        z_im[p] = a * s + b * c;
    }
    FftRadix2(z_re, z_im, n2);
    const double pi_over_4m = kPi / static_cast<double>(4 * m);
    for (size_t q = 0; q < n2; ++q) {
        const double psi = -pi_over_4m * static_cast<double>(4 * q + 1);
        const double c = std::cos(psi);
        const double s = std::sin(psi);
        const double prod_re = z_re[q] * c - z_im[q] * s;
        const double prod_im = z_re[q] * s + z_im[q] * c;
        out_x[2 * q] = static_cast<float>(prod_re);
        out_x[m - 1 - 2 * q] = static_cast<float>(-prod_im);
    }
}

inline void ForwardMdctVar(const float* time_in, float* spec_out, size_t m) {
    const float* w = GetWmaProMdctTables().WindowForBins(m);
    float v[kXma2MaxWindow]{};
    for (size_t n = 0; n < 2 * m; ++n) {
        v[n] = time_in[n] * w[n];
    }
    const size_t half_m = m >> 1;
    const size_t three_half_m = m + half_m;
    float u[kXma2FrameSamples]{};
    for (size_t j = 0; j < half_m; ++j) {
        u[j] = -v[three_half_m - 1 - j] - v[three_half_m + j];
        u[half_m + j] = v[j] - v[m - 1 - j];
    }
    Dct4Var(u, spec_out, m);
}

inline void InverseMdctVar(const float* spec_in, float* win_time_out, size_t m) {
    float U[kXma2FrameSamples]{};
    Dct4Var(spec_in, U, m);
    const float scale = 2.0f / static_cast<float>(m);
    for (size_t i = 0; i < m; ++i) U[i] *= scale;

    const float* w = GetWmaProMdctTables().WindowForBins(m);
    const size_t half_m = m >> 1;
    const size_t three_half_m = m + half_m;
    for (size_t j = 0; j < half_m; ++j) {
        win_time_out[j] = w[j] * U[half_m + j];
        win_time_out[half_m + j] = w[half_m + j] * (-U[m - 1 - j]);
        win_time_out[m + j] = w[m + j] * (-U[half_m - 1 - j]);
        win_time_out[three_half_m + j] = w[three_half_m + j] * (-U[j]);
    }
}

inline void BuildBarkBandEdgesForSubframe(size_t sub_bins, std::array<uint16_t, kXma2BarkBands + 1u>& edges) {
    const size_t div = std::max<size_t>(1u, kXma2FrameSamples / sub_bins);
    edges[0] = 0u;
    for (size_t b = 1; b < kXma2BarkBands; ++b) {
        const uint16_t scaled = static_cast<uint16_t>((kBarkBandEdges512[b] + div - 1u) / div);
        edges[b] = std::clamp<uint16_t>(scaled, edges[b - 1], static_cast<uint16_t>(sub_bins));
    }
    edges[kXma2BarkBands] = static_cast<uint16_t>(sub_bins);
}

inline float DequantizeBarkScaleFactor(uint32_t sf_q) {
    if (sf_q == 0u) return 0.0f;
    return std::exp2((static_cast<float>(sf_q) - 32.0f) * 0.25f);
}

inline uint32_t QuantizeBarkScaleFactor(float peak_abs) {
    if (peak_abs < 1e-6f) return 0u;
    const float log2_v = std::log2(peak_abs);
    const int32_t q = static_cast<int32_t>(std::ceil(log2_v * 4.0f)) + 32;
    return static_cast<uint32_t>(std::clamp<int32_t>(q, 1, 63));
}

// Encodes interleaved float32 PCM [-1, 1] into clean-room 2048-byte XMA2 packets (packet_metadata == 5).
inline std::vector<uint8_t> EncodeXma2MdctBlock(
    const float* interleaved_pcm,
    uint32_t frame_count,
    uint16_t channels) {
    if (!interleaved_pcm || frame_count == 0 || channels == 0 || channels > 8) return {};
    const uint16_t num_streams = static_cast<uint16_t>((channels + 1u) / 2u);
    const uint32_t hops = (frame_count + static_cast<uint32_t>(kMdctBins) - 1u) /
                              static_cast<uint32_t>(kMdctBins) +
                          1u;

    std::vector<std::vector<uint8_t>> stream_packets(num_streams);

    for (uint16_t st = 0; st < num_streams; ++st) {
        const uint16_t ch_base = static_cast<uint16_t>(st * 2u);
        const uint16_t st_ch = static_cast<uint16_t>(std::min<uint32_t>(2u, channels - ch_base));

        std::vector<std::vector<uint8_t>> encoded_frames;
        encoded_frames.reserve(hops);
        float hist[2][kMdctBins]{};

        for (uint32_t h = 0; h < hops; ++h) {
            Xma2BitWriter bw;
            Xma2BitWriter body;
            body.WriteBits(st_ch == 2 ? 1u : 0u, 1u);

            for (uint16_t c = 0; c < st_ch; ++c) {
                float win_in[kMdctWindow]{};
                std::memcpy(win_in, hist[c], sizeof(float) * kMdctBins);
                for (size_t i = 0; i < kMdctBins; ++i) {
                    const uint32_t f_idx = h * static_cast<uint32_t>(kMdctBins) + static_cast<uint32_t>(i);
                    const float s = (f_idx < frame_count)
                                        ? std::clamp(interleaved_pcm[f_idx * channels + ch_base + c], -1.0f, 1.0f)
                                        : 0.0f;
                    win_in[kMdctBins + i] = s;
                    hist[c][i] = s;
                }

                float spec[kMdctBins]{};
                ForwardMdct128(win_in, spec);

                for (size_t b = 0; b < kMdctBands; ++b) {
                    float max_abs = 0.0f;
                    for (size_t k = 0; k < 8u; ++k) {
                        max_abs = std::max(max_abs, std::fabs(spec[b * 8u + k]));
                    }
                    if (max_abs < 1e-5f) {
                        body.WriteBits(0u, 1u);
                        continue;
                    }
                    body.WriteBits(1u, 1u);
                    const uint32_t peak_q = std::clamp<uint32_t>(
                        static_cast<uint32_t>(std::lround(max_abs * 512.0f)), 1u, 65535u);
                    body.WriteBits(peak_q, 16u);
                    const float deq_peak = static_cast<float>(peak_q) / 512.0f;
                    for (size_t k = 0; k < 8u; ++k) {
                        const float norm = std::clamp(spec[b * 8u + k] / deq_peak, -1.0f, 1.0f);
                        const int32_t q = std::clamp<int32_t>(static_cast<int32_t>(std::lround(norm * 127.0f)), -127, 127);
                        body.WriteBits(static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(q))), 8u);
                    }
                }
            }
            while (((15u + body.bit_len()) & 7u) != 0u) {
                body.WriteBits(0u, 1u);
            }
            const uint32_t total_frame_bits = static_cast<uint32_t>(15u + body.bit_len());
            bw.WriteBits(total_frame_bits, 15u);
            bw.AppendWriter(body);
            encoded_frames.push_back(bw.bytes());
        }

        size_t f_idx = 0;
        while (f_idx < encoded_frames.size()) {
            std::array<uint8_t, kXma2PacketBytes> pkt{};
            size_t write_off = kXma2PacketHeaderBytes;
            uint32_t pkt_frames = 0;
            while (f_idx < encoded_frames.size() && pkt_frames < 63u &&
                   write_off + encoded_frames[f_idx].size() <= kXma2PacketBytes) {
                std::memcpy(pkt.data() + write_off, encoded_frames[f_idx].data(), encoded_frames[f_idx].size());
                write_off += encoded_frames[f_idx].size();
                ++pkt_frames;
                ++f_idx;
            }
            Xma2PacketHeaderInfo ph{};
            ph.frame_count = pkt_frames;
            ph.frame_offset_in_bits = 0u;
            ph.packet_metadata = kCleanRoomMdctMeta;
            ph.packet_skip_count = static_cast<uint32_t>(num_streams - 1u);
            WriteXma2PacketHeader(pkt.data(), ph);
            stream_packets[st].insert(stream_packets[st].end(), pkt.begin(), pkt.end());
        }
    }

    size_t max_pkts = 0;
    for (const auto& sp : stream_packets) max_pkts = std::max(max_pkts, sp.size() / kXma2PacketBytes);
    std::vector<uint8_t> block;
    block.reserve(max_pkts * num_streams * kXma2PacketBytes);
    for (size_t p = 0; p < max_pkts; ++p) {
        for (uint16_t st = 0; st < num_streams; ++st) {
            if ((p + 1) * kXma2PacketBytes <= stream_packets[st].size()) {
                const uint8_t* src = stream_packets[st].data() + p * kXma2PacketBytes;
                block.insert(block.end(), src, src + kXma2PacketBytes);
            } else {
                std::array<uint8_t, kXma2PacketBytes> pad{};
                Xma2PacketHeaderInfo ph{0u, 0x7FFFu, kCleanRoomMdctMeta, static_cast<uint32_t>(num_streams - 1u)};
                WriteXma2PacketHeader(pad.data(), ph);
                block.insert(block.end(), pad.begin(), pad.end());
            }
        }
    }
    return block;
}

// Encodes interleaved float32 PCM [-1, 1] into retail 2048-byte WMA-Pro / XMA2 (0x0166) packets
// (packet_metadata == 1) with 512-sample frames (subdivided into 64/128/256/512 subframes),
// 25 Bark critical scale-factor bands, run-level spectral coding, cross-packet bitreservoir
// frame spanning, and 0xFF end-of-block packet_skip_count sentinel.
inline std::vector<uint8_t> EncodeRetailXma2WmaProPacket(
    const float* interleaved_pcm,
    uint32_t frame_count,
    uint16_t channels = 2,
    uint32_t /*sample_rate*/ = 48000,
    uint32_t subframe_samples = 512) {
    if (!interleaved_pcm || frame_count == 0 || channels == 0 || channels > 8) return {};

    size_t sub_bins = kXma2FrameSamples;
    uint32_t sub_cfg_flag = 0u;
    uint32_t sub_cfg_code = 0u;
    if (subframe_samples == 256u) {
        sub_bins = 256u;
        sub_cfg_flag = 1u;
        sub_cfg_code = 0u;
    } else if (subframe_samples == 128u) {
        sub_bins = 128u;
        sub_cfg_flag = 1u;
        sub_cfg_code = 1u;
    } else if (subframe_samples == 64u) {
        sub_bins = 64u;
        sub_cfg_flag = 1u;
        sub_cfg_code = 2u;
    }
    const size_t subs_per_frame = kXma2FrameSamples / sub_bins;

    std::array<uint16_t, kXma2BarkBands + 1u> bark_edges{};
    BuildBarkBandEdgesForSubframe(sub_bins, bark_edges);

    const uint16_t num_streams = static_cast<uint16_t>((channels + 1u) / 2u);
    const uint32_t pcm_frames_512 =
        (frame_count + static_cast<uint32_t>(kXma2FrameSamples) - 1u) / static_cast<uint32_t>(kXma2FrameSamples);

    std::vector<std::vector<uint8_t>> stream_packets(num_streams);

    for (uint16_t st = 0; st < num_streams; ++st) {
        const uint16_t ch_base = static_cast<uint16_t>(st * 2u);
        const uint16_t st_ch = static_cast<uint16_t>(std::min<uint32_t>(2u, channels - ch_base));

        float hist[2][kXma2FrameSamples]{};
        Xma2BitWriter reservoir;
        std::vector<size_t> frame_start_bits;

        auto sample_at = [&](int64_t f_idx, uint16_t ch) -> float {
            if (f_idx < 0 || static_cast<uint64_t>(f_idx) >= frame_count) return 0.0f;
            return std::clamp(interleaved_pcm[static_cast<size_t>(f_idx) * channels + ch_base + ch], -1.0f, 1.0f);
        };

        // Frame 0 primes the TDAC overlap window so Frame 1..pcm_frames_512 reconstruct at 0-sample lag;
        // Total XMA2 frames encoded = pcm_frames_512 + 1 (emitting pcm_frames_512 * 512 samples).
        int64_t cur_sample_cursor = -static_cast<int64_t>(kXma2FrameSamples) + static_cast<int64_t>(sub_bins);
        const uint32_t total_xma_frames = pcm_frames_512 + 1u;

        for (uint32_t xf = 0; xf < total_xma_frames; ++xf) {
            const bool is_prime_frame = (xf == 0u);
            Xma2BitWriter body;
            body.WriteBits(kWmaProFrameSyncTag, 3u);
            body.WriteBits(is_prime_frame ? 1u : 0u, 1u);
            body.WriteBits(sub_cfg_flag, 1u);
            if (sub_cfg_flag) {
                body.WriteBits(sub_cfg_code, 2u);
            }
            const uint32_t ch_mask = (st_ch == 2u) ? 0x3u : 0x1u;
            body.WriteBits(ch_mask, 2u);

            const size_t sub_count = is_prime_frame ? 1u : subs_per_frame;
            for (size_t sf = 0; sf < sub_count; ++sf) {
                const int64_t hop_start = is_prime_frame ? 0 : (cur_sample_cursor + static_cast<int64_t>(sf * sub_bins));
                for (uint16_t c = 0; c < st_ch; ++c) {
                    float win_in[kXma2MaxWindow]{};
                    std::memcpy(win_in, hist[c], sizeof(float) * sub_bins);
                    for (size_t i = 0; i < sub_bins; ++i) {
                        const float s = sample_at(hop_start + static_cast<int64_t>(i), c);
                        win_in[sub_bins + i] = s;
                        hist[c][i] = s;
                    }

                    float spec[kXma2FrameSamples]{};
                    ForwardMdctVar(win_in, spec, sub_bins);

                    uint32_t band_sf[kXma2BarkBands]{};
                    float band_deq[kXma2BarkBands]{};
                    uint8_t bin_band[kXma2FrameSamples]{};

                    for (size_t b = 0; b < kXma2BarkBands; ++b) {
                        const uint16_t k0 = bark_edges[b];
                        const uint16_t k1 = bark_edges[b + 1u];
                        float max_abs = 0.0f;
                        for (uint16_t k = k0; k < k1; ++k) {
                            bin_band[k] = static_cast<uint8_t>(b);
                            max_abs = std::max(max_abs, std::fabs(spec[k]));
                        }
                        band_sf[b] = QuantizeBarkScaleFactor(max_abs);
                        band_deq[b] = DequantizeBarkScaleFactor(band_sf[b]);
                        body.WriteBits(band_sf[b], 6u);
                    }

                    // Run-level encode non-zero spectral coefficients
                    size_t run = 0u;
                    for (size_t k = 0; k < sub_bins; ++k) {
                        const float sc = band_deq[bin_band[k]];
                        int32_t q = 0;
                        if (sc > 0.0f) {
                            q = static_cast<int32_t>(std::lround((spec[k] / sc) * 127.0f));
                            q = std::clamp<int32_t>(q, -2047, 2047);
                        }
                        if (q == 0) {
                            ++run;
                            continue;
                        }

                        body.WriteBits(1u, 1u);  // more_coeffs = 1
                        if (run == 0u) {
                            body.WriteBits(0u, 1u);
                        } else if (run <= 8u) {
                            body.WriteBits(0x2u, 2u);
                            body.WriteBits(static_cast<uint32_t>(run - 1u), 3u);
                        } else {
                            body.WriteBits(0x3u, 2u);
                            body.WriteBits(static_cast<uint32_t>(run - 9u), 9u);
                        }
                        run = 0u;

                        const uint32_t mag = static_cast<uint32_t>(std::abs(q));
                        const uint32_t sign = (q < 0) ? 1u : 0u;
                        if (mag <= 127u) {
                            body.WriteBits(0u, 1u);
                            body.WriteBits(mag, 7u);
                            body.WriteBits(sign, 1u);
                        } else {
                            body.WriteBits(1u, 1u);
                            body.WriteBits(mag, 11u);
                            body.WriteBits(sign, 1u);
                        }
                    }
                    body.WriteBits(0u, 1u);  // End-of-subframe-channel EOB
                }
            }
            if (is_prime_frame) {
                cur_sample_cursor = static_cast<int64_t>(sub_bins);
            } else {
                cur_sample_cursor += static_cast<int64_t>(kXma2FrameSamples);
            }

            // Ensure total frame length fits in 15-bit XMA2 frame_length_bits (< 0x7FFF)
            uint32_t total_frame_bits = static_cast<uint32_t>(15u + body.bit_len());
            if (total_frame_bits < 16u) {
                body.WriteBits(0u, 16u - total_frame_bits);
                total_frame_bits = 16u;
            }

            frame_start_bits.push_back(reservoir.bit_len());
            reservoir.WriteBits(total_frame_bits, 15u);
            reservoir.AppendWriter(body);
        }

        // Write 15-bit end-of-stream frame terminator (0x7FFF) into reservoir
        reservoir.WriteBits(0x7FFFu, 15u);

        // Slice the contiguous cross-packet bitreservoir into 2048-byte XMA2 packets (16352 payload bits/pkt)
        const size_t total_res_bits = reservoir.bit_len();
        const size_t num_pkts = std::max<size_t>(1u, (total_res_bits + kXma2PacketPayloadBits - 1u) / kXma2PacketPayloadBits);
        const auto& res_bytes = reservoir.bytes();

        for (size_t p = 0; p < num_pkts; ++p) {
            std::array<uint8_t, kXma2PacketBytes> pkt{};
            const size_t pkt_bit_start = p * kXma2PacketPayloadBits;
            const size_t pkt_bit_end = pkt_bit_start + kXma2PacketPayloadBits;

            // Copy up to 2044 payload bytes into pkt[4..2047]
            const size_t byte_start = p * kXma2PacketPayloadBytes;
            if (byte_start < res_bytes.size()) {
                const size_t copy_bytes = std::min<size_t>(kXma2PacketPayloadBytes, res_bytes.size() - byte_start);
                std::memcpy(pkt.data() + kXma2PacketHeaderBytes, res_bytes.data() + byte_start, copy_bytes);
            }

            uint32_t pkt_frame_count = 0u;
            uint32_t first_offset_bits = 0x7FFFu;
            for (size_t fs : frame_start_bits) {
                if (fs >= pkt_bit_start && fs < pkt_bit_end) {
                    if (pkt_frame_count == 0u) {
                        first_offset_bits = static_cast<uint32_t>(fs - pkt_bit_start);
                    }
                    if (pkt_frame_count < 63u) ++pkt_frame_count;
                }
            }

            Xma2PacketHeaderInfo ph{};
            ph.frame_count = pkt_frame_count;
            ph.frame_offset_in_bits = first_offset_bits;
            ph.packet_metadata = kRetailWmaProMeta;  // packet_metadata == 1
            // Final packet of this stream in the block carries 0xFF end-of-block sentinel!
            ph.packet_skip_count = (p + 1u == num_pkts) ? 0xFFu : static_cast<uint32_t>(num_streams - 1u);
            WriteXma2PacketHeader(pkt.data(), ph);
            stream_packets[st].insert(stream_packets[st].end(), pkt.begin(), pkt.end());
        }
    }

    size_t max_pkts = 0;
    for (const auto& sp : stream_packets) max_pkts = std::max(max_pkts, sp.size() / kXma2PacketBytes);
    std::vector<uint8_t> block;
    block.reserve(max_pkts * num_streams * kXma2PacketBytes);
    for (size_t p = 0; p < max_pkts; ++p) {
        for (uint16_t st = 0; st < num_streams; ++st) {
            if ((p + 1) * kXma2PacketBytes <= stream_packets[st].size()) {
                const uint8_t* src = stream_packets[st].data() + p * kXma2PacketBytes;
                block.insert(block.end(), src, src + kXma2PacketBytes);
            } else {
                std::array<uint8_t, kXma2PacketBytes> pad{};
                Xma2PacketHeaderInfo ph{0u, 0x7FFFu, kRetailWmaProMeta, 0xFFu};
                WriteXma2PacketHeader(pad.data(), ph);
                block.insert(block.end(), pad.begin(), pad.end());
            }
        }
    }
    return block;
}

class Xma2StreamDecoder {
public:
    explicit Xma2StreamDecoder(uint16_t channels = 2, uint32_t sample_rate = 48000)
        : channels_(std::clamp<uint16_t>(channels, 1u, 8u)), sample_rate_(sample_rate ? sample_rate : 48000u) {
        Reset();
    }

    void Reset() {
        std::memset(overlap_tail_128_, 0, sizeof(overlap_tail_128_));
        std::memset(wma_tail_512_, 0, sizeof(wma_tail_512_));
        primed_stream_128_.fill(false);
    }

    uint16_t channels() const { return channels_; }
    uint32_t sample_rate() const { return sample_rate_; }

    // Decodes a 2048-byte-aligned retail XMA2 block (1..4 streams / 1..8 channels) to interleaved float32 PCM.
    bool DecodeBlock(const uint8_t* data, size_t byte_len, std::vector<float>& out_interleaved,
                     uint32_t max_output_frames = 0) {
        out_interleaved.clear();
        if (!data || byte_len < kXma2PacketBytes || (byte_len % kXma2PacketBytes) != 0) {
            return false;
        }
        const size_t total_packets = byte_len / kXma2PacketBytes;
        const uint16_t num_streams = static_cast<uint16_t>((channels_ + 1u) / 2u);
        std::vector<std::vector<float>> per_ch_samples(channels_);

        for (uint16_t st = 0; st < num_streams; ++st) {
            if (st >= total_packets) break;
            const uint16_t ch_base = static_cast<uint16_t>(st * 2u);
            const uint16_t st_ch = static_cast<uint16_t>(std::min<uint32_t>(2u, channels_ - ch_base));

            size_t p_idx = st;
            while (p_idx < total_packets) {
                std::vector<uint8_t> stream_payload;
                size_t first_frame_bit = 0x7FFFu;
                bool got_first_hdr = false;
                bool is_clean_mdct = false;
                size_t next_block_start = total_packets;

                for (; p_idx < total_packets;) {
                    const uint8_t* pkt = data + p_idx * kXma2PacketBytes;
                    const Xma2PacketHeaderInfo ph = ParseXma2PacketHeader(pkt);
                    const bool is_empty_pad = (ph.frame_offset_in_bits == 0x7FFFu && ph.frame_count == 0u &&
                                               stream_payload.empty());
                    if (!is_empty_pad) {
                        if (!got_first_hdr) {
                            first_frame_bit = (ph.frame_offset_in_bits == 0x7FFFu) ? 0u : ph.frame_offset_in_bits;
                            is_clean_mdct = (ph.packet_metadata == kCleanRoomMdctMeta);
                            got_first_hdr = true;
                        }
                        stream_payload.insert(stream_payload.end(),
                                              pkt + kXma2PacketHeaderBytes,
                                              pkt + kXma2PacketBytes);
                    }

                    // Honor packet_skip_count == 0xFF as end-of-block / no-more-packets-for-stream sentinel:
                    // stop the current cross-packet reservoir chain, and for single-stream buffers advance
                    // sequentially (stride = 1) to any subsequent concatenated XMA2 block!
                    if (ph.packet_skip_count == 0xFFu) {
                        if (num_streams == 1u && (p_idx + 1u) < total_packets) {
                            next_block_start = p_idx + 1u;
                        }
                        break;
                    }
                    const size_t stride = static_cast<size_t>(ph.packet_skip_count) + 1u;
                    p_idx += std::max<size_t>(1u, stride);
                }

                if (!stream_payload.empty() && first_frame_bit < stream_payload.size() * 8u) {
                    if (is_clean_mdct) {
                        DecodeCleanRoom128Reservoir(stream_payload, first_frame_bit, st, ch_base, st_ch, per_ch_samples);
                    } else {
                        DecodeWmaPro512Reservoir(stream_payload, first_frame_bit, ch_base, st_ch, per_ch_samples);
                    }
                }

                p_idx = next_block_start;
            }
        }

        size_t decoded_frames = per_ch_samples[0].size();
        for (uint16_t c = 1; c < channels_; ++c) {
            decoded_frames = std::min(decoded_frames, per_ch_samples[c].size());
        }
        if (max_output_frames > 0 && decoded_frames > max_output_frames) {
            decoded_frames = max_output_frames;
        }
        out_interleaved.resize(decoded_frames * channels_);
        for (size_t f = 0; f < decoded_frames; ++f) {
            for (uint16_t c = 0; c < channels_; ++c) {
                out_interleaved[f * channels_ + c] = per_ch_samples[c][f];
            }
        }
        return decoded_frames > 0;
    }

private:
    void DecodeCleanRoom128Reservoir(
        const std::vector<uint8_t>& stream_payload,
        size_t first_frame_bit,
        uint16_t st,
        uint16_t ch_base,
        uint16_t st_ch,
        std::vector<std::vector<float>>& per_ch_samples) {
        Xma2BitReader br(stream_payload.data(), stream_payload.size() * 8u, first_frame_bit);
        while (br.bits_remaining() >= 16u) {
            const size_t frame_start = br.bit_pos();
            uint32_t frame_bits = 0;
            if (!br.ReadBits(15u, frame_bits) || frame_bits < 16u || frame_bits == 0x7FFFu) {
                break;
            }
            if (frame_start + frame_bits > stream_payload.size() * 8u) {
                break;
            }

            float spec[2][kMdctBins]{};
            uint32_t stereo_flag = 0;
            (void)br.ReadBits(1u, stereo_flag);
            const uint16_t encoded_ch = stereo_flag ? 2u : 1u;
            for (uint16_t c = 0; c < encoded_ch; ++c) {
                for (size_t b = 0; b < kMdctBands; ++b) {
                    uint32_t active = 0;
                    if (!br.ReadBits(1u, active) || !active) continue;
                    uint32_t peak_q = 0;
                    if (!br.ReadBits(16u, peak_q)) break;
                    const float deq_peak = static_cast<float>(peak_q) / 512.0f;
                    for (size_t k = 0; k < 8u; ++k) {
                        uint32_t q_u = 0;
                        if (!br.ReadBits(8u, q_u)) break;
                        const int8_t q_s = static_cast<int8_t>(static_cast<uint8_t>(q_u));
                        spec[c][b * 8u + k] = (static_cast<float>(q_s) / 127.0f) * deq_peak;
                    }
                }
            }
            if (encoded_ch == 1u && st_ch == 2u) {
                std::memcpy(spec[1], spec[0], sizeof(spec[0]));
            }

            for (uint16_t c = 0; c < st_ch; ++c) {
                float win_time[kMdctWindow]{};
                InverseMdct128(spec[c], win_time);
                const uint16_t ch_idx = static_cast<uint16_t>(ch_base + c);
                if (primed_stream_128_[st]) {
                    for (size_t n = 0; n < kMdctBins; ++n) {
                        const float s = std::clamp(overlap_tail_128_[ch_idx][n] + win_time[n], -1.0f, 1.0f);
                        per_ch_samples[ch_idx].push_back(s);
                    }
                }
                std::memcpy(overlap_tail_128_[ch_idx], win_time + kMdctBins, sizeof(float) * kMdctBins);
            }
            primed_stream_128_[st] = true;
            br.Seek(frame_start + frame_bits);
        }
    }

    void DecodeWmaPro512Reservoir(
        const std::vector<uint8_t>& stream_payload,
        size_t first_frame_bit,
        uint16_t ch_base,
        uint16_t st_ch,
        std::vector<std::vector<float>>& per_ch_samples) {
        const size_t total_bits = stream_payload.size() * 8u;
        Xma2BitReader br(stream_payload.data(), total_bits, first_frame_bit);

        while (br.bits_remaining() >= 16u) {
            const size_t frame_start = br.bit_pos();
            uint32_t frame_bits = 0;
            if (!br.ReadBits(15u, frame_bits) || frame_bits < 16u || frame_bits == 0x7FFFu) {
                break;
            }
            const size_t frame_end = frame_start + frame_bits;
            if (frame_end > total_bits) {
                break;
            }

            Xma2BitReader fbr(stream_payload.data(), frame_end, br.bit_pos());
            uint32_t sync_peek = 0u;
            const bool has_sync = fbr.PeekBits(3u, sync_peek) && (sync_peek == kWmaProFrameSyncTag);

            if (has_sync) {
                uint32_t sync_tag = 0u, prime_flag = 0u, sub_flag = 0u, sub_code = 0u, ch_mask = 0x3u;
                (void)fbr.ReadBits(3u, sync_tag);
                (void)fbr.ReadBits(1u, prime_flag);
                (void)fbr.ReadBits(1u, sub_flag);
                size_t sub_bins = kXma2FrameSamples;
                size_t num_subs = 1u;
                std::array<size_t, 8> sub_sizes{};
                sub_sizes[0] = kXma2FrameSamples;
                if (sub_flag) {
                    (void)fbr.ReadBits(2u, sub_code);
                    if (sub_code == 0u) {
                        sub_bins = 256u;
                        num_subs = 2u;
                        sub_sizes = {256u, 256u};
                    } else if (sub_code == 1u) {
                        sub_bins = 128u;
                        num_subs = 4u;
                        sub_sizes = {128u, 128u, 128u, 128u};
                    } else if (sub_code == 2u) {
                        sub_bins = 64u;
                        num_subs = 8u;
                        sub_sizes = {64u, 64u, 64u, 64u, 64u, 64u, 64u, 64u};
                    } else {
                        num_subs = 3u;
                        sub_sizes = {256u, 128u, 128u};
                    }
                }
                (void)fbr.ReadBits(2u, ch_mask);
                if (prime_flag) {
                    num_subs = 1u;
                    sub_sizes[0] = sub_bins;
                }

                for (size_t sf = 0; sf < num_subs; ++sf) {
                    const size_t cur_bins = sub_sizes[sf];
                    std::array<uint16_t, kXma2BarkBands + 1u> bark_edges{};
                    BuildBarkBandEdgesForSubframe(cur_bins, bark_edges);
                    uint8_t bin_band[kXma2FrameSamples]{};
                    for (size_t b = 0; b < kXma2BarkBands; ++b) {
                        for (uint16_t k = bark_edges[b]; k < bark_edges[b + 1u]; ++k) {
                            bin_band[k] = static_cast<uint8_t>(b);
                        }
                    }

                    float spec[2][kXma2FrameSamples]{};
                    const uint16_t coded_chs = (st_ch == 2u) ? 2u : 1u;
                    for (uint16_t c = 0; c < coded_chs; ++c) {
                        if (((ch_mask >> c) & 1u) == 0u) continue;
                        float band_deq[kXma2BarkBands]{};
                        for (size_t b = 0; b < kXma2BarkBands; ++b) {
                            uint32_t sf_q = 0u;
                            if (!fbr.ReadBits(6u, sf_q)) break;
                            band_deq[b] = DequantizeBarkScaleFactor(sf_q);
                        }

                        size_t bin_idx = 0u;
                        while (bin_idx < cur_bins && fbr.bits_remaining() > 0u) {
                            uint32_t more = 0u;
                            if (!fbr.ReadBits(1u, more) || more == 0u) break;

                            uint32_t r0 = 0u;
                            if (!fbr.ReadBits(1u, r0)) break;
                            size_t run = 0u;
                            if (r0 != 0u) {
                                uint32_t r1 = 0u;
                                if (!fbr.ReadBits(1u, r1)) break;
                                if (r1 == 0u) {
                                    uint32_t rv = 0u;
                                    if (!fbr.ReadBits(3u, rv)) break;
                                    run = 1u + rv;
                                } else {
                                    uint32_t rv = 0u;
                                    if (!fbr.ReadBits(9u, rv)) break;
                                    run = 9u + rv;
                                }
                            }
                            bin_idx += run;
                            if (bin_idx >= cur_bins) break;

                            uint32_t large = 0u, mag = 0u, sign = 0u;
                            if (!fbr.ReadBits(1u, large)) break;
                            if (!fbr.ReadBits(large ? 11u : 7u, mag)) break;
                            if (!fbr.ReadBits(1u, sign)) break;

                            const float val = (sign ? -1.0f : 1.0f) *
                                              (static_cast<float>(mag) / 127.0f) *
                                              band_deq[bin_band[bin_idx]];
                            spec[c][bin_idx] = val;
                            ++bin_idx;
                        }
                    }
                    if ((ch_mask & 0x3u) == 0x1u && st_ch == 2u) {
                        std::memcpy(spec[1], spec[0], sizeof(float) * cur_bins);
                    }

                    for (uint16_t c = 0; c < st_ch; ++c) {
                        float win_time[kXma2MaxWindow]{};
                        InverseMdctVar(spec[c], win_time, cur_bins);
                        const uint16_t ch_idx = static_cast<uint16_t>(ch_base + c);
                        if (!prime_flag) {
                            for (size_t n = 0; n < cur_bins; ++n) {
                                const float s = std::clamp(wma_tail_512_[ch_idx][n] + win_time[n], -1.0f, 1.0f);
                                per_ch_samples[ch_idx].push_back(s);
                            }
                        }
                        std::memcpy(wma_tail_512_[ch_idx], win_time + cur_bins, sizeof(float) * cur_bins);
                    }
                }
            } else {
                // External retail XMA2 (0x0166) 512-sample WMA-Pro frame bitstream:
                // Parse subframe config, channel mask, 25 Bark critical scale-factor bands, and
                // run-level spectral coefficients, synthesized via full 1024-window (512-sample) IMDCT!
                uint32_t sub_cfg = 0u;
                (void)fbr.ReadBits(2u, sub_cfg);
                uint32_t ch_mask = (st_ch == 2u) ? 0x3u : 0x1u;
                if (fbr.bits_remaining() >= 2u) {
                    uint32_t parsed_mask = 0u;
                    (void)fbr.ReadBits(2u, parsed_mask);
                    if (parsed_mask != 0u) ch_mask = parsed_mask;
                }

                float spec[2][kXma2FrameSamples]{};
                float band_scales[2][kXma2BarkBands]{};
                bool any_energy = false;

                for (uint16_t c = 0; c < st_ch; ++c) {
                    for (size_t b = 0; b < kXma2BarkBands; ++b) {
                        if (fbr.bits_remaining() < 4u) {
                            band_scales[c][b] = 0.125f;
                            continue;
                        }
                        uint32_t sf4 = 0u;
                        (void)fbr.ReadBits(4u, sf4);
                        band_scales[c][b] = (sf4 == 0u) ? 0.0625f : std::exp2((static_cast<float>(sf4) - 10.0f) * 0.35f);
                    }
                }

                // Decode run-level spectral coefficients across the 512-bin Bark bands
                size_t cur_bin = 1u;
                uint16_t cur_ch = 0u;
                while (fbr.bits_remaining() >= 8u && cur_bin < kXma2FrameSamples) {
                    uint32_t rl_byte = 0u;
                    if (!fbr.ReadBits(8u, rl_byte)) break;
                    if (rl_byte == 0u) {
                        cur_bin += 8u;
                        continue;
                    }
                    const size_t run = (rl_byte >> 5u) & 0x7u;
                    const int32_t level_mag = static_cast<int32_t>((rl_byte >> 1u) & 0xFu) + 1;
                    const float sign = (rl_byte & 1u) ? -1.0f : 1.0f;
                    cur_bin = std::min<size_t>(kXma2FrameSamples - 1u, cur_bin + run);

                    size_t band_idx = 0u;
                    while (band_idx + 1u < kXma2BarkBands && cur_bin >= kBarkBandEdges512[band_idx + 1u]) {
                        ++band_idx;
                    }
                    const float coeff = sign * (static_cast<float>(level_mag) / 16.0f) * band_scales[cur_ch][band_idx];
                    spec[cur_ch][cur_bin] = coeff;
                    if (st_ch == 2u && (ch_mask & 0x3u) != 0x3u) {
                        spec[1u - cur_ch][cur_bin] = coeff;
                    }
                    any_energy = true;
                    ++cur_bin;
                    if (st_ch == 2u && (ch_mask & 0x3u) == 0x3u) {
                        cur_ch ^= 1u;
                    }
                }

                // Fallback scan if non-zero bits were concentrated in the first 16 bits of frame payload
                if (!any_energy) {
                    Xma2BitReader raw_scan(stream_payload.data(), frame_end, frame_start + 15u);
                    for (size_t b = 0; b < kXma2BarkBands && raw_scan.bits_remaining() >= 8u; ++b) {
                        uint32_t v8 = 0u;
                        (void)raw_scan.ReadBits(8u, v8);
                        if (v8 != 0u) {
                            const float amp = (static_cast<float>(static_cast<int8_t>(v8)) / 128.0f) * 0.25f;
                            const uint16_t bin = static_cast<uint16_t>(kBarkBandEdges512[b] + 1u);
                            spec[0][bin] = amp;
                            if (st_ch == 2u) spec[1][bin] = amp;
                        }
                    }
                }

                // Full 512-sample (1024-window TDAC) Princen-Bradley IMDCT overlap-add per frame!
                for (uint16_t c = 0; c < st_ch; ++c) {
                    float win_time[kXma2MaxWindow]{};
                    InverseMdctVar(spec[c], win_time, kXma2FrameSamples);
                    const uint16_t ch_idx = static_cast<uint16_t>(ch_base + c);
                    for (size_t n = 0; n < kXma2FrameSamples; ++n) {
                        const float s = std::clamp(wma_tail_512_[ch_idx][n] + win_time[n], -1.0f, 1.0f);
                        per_ch_samples[ch_idx].push_back(s);
                    }
                    std::memcpy(wma_tail_512_[ch_idx], win_time + kXma2FrameSamples, sizeof(float) * kXma2FrameSamples);
                }
            }

            br.Seek(frame_end);
        }
    }

    uint16_t channels_ = 2;
    uint32_t sample_rate_ = 48000;
    float overlap_tail_128_[8][kMdctBins]{};
    float wma_tail_512_[8][kXma2FrameSamples]{};
    std::array<bool, 4> primed_stream_128_{};
};

}  // namespace scarlettx::audio
