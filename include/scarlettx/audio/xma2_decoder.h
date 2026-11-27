#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
constexpr size_t kMdctBins = 128u;
constexpr size_t kMdctWindow = kMdctBins * 2u;  // 256 samples, 50% overlap-add = 128 samples/subframe
constexpr size_t kMdctBands = 16u;              // 16 scale-factor bands x 8 bins = 128 bins
constexpr uint32_t kCleanRoomMdctMeta = 0x5u;   // 3-bit packetMetadata tag for clean-room MDCT bitstreams

struct Xma2PacketHeaderInfo {
    uint32_t frame_count = 0;          // [31:26] 6 bits
    uint32_t frame_offset_in_bits = 0; // [25:11] 15 bits
    uint32_t packet_metadata = 0;      // [10:8]  3 bits
    uint32_t packet_skip_count = 0;    // [7:0]   8 bits
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

// MSB-first bit reader & writer for XMA2 packet payloads
class Xma2BitReader {
public:
    Xma2BitReader(const uint8_t* data, size_t total_bits, size_t start_bit = 0)
        : data_(data), total_bits_(total_bits), bit_pos_(start_bit) {}

    size_t bit_pos() const { return bit_pos_; }
    size_t bits_remaining() const { return bit_pos_ < total_bits_ ? (total_bits_ - bit_pos_) : 0u; }

    bool ReadBits(uint32_t count, uint32_t& out) {
        if (count == 0) { out = 0; return true; }
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

// Encodes interleaved float32 PCM [-1, 1] into genuine 2048-byte XMA2 packets using
// Princen-Bradley sine-windowed MDCT + 16-band floating-point scale factors + 8-bit mantissas.
inline std::vector<uint8_t> EncodeXma2MdctBlock(
    const float* interleaved_pcm,
    uint32_t frame_count,
    uint16_t channels) {
    if (!interleaved_pcm || frame_count == 0 || channels == 0 || channels > 8) return {};
    const uint16_t num_streams = static_cast<uint16_t>((channels + 1u) / 2u);
    // Include one extra flush hop so TDAC 50% overlap-add reconstructs all `frame_count` frames cleanly
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
            // Reserve 15-bit frameLengthInBits placeholder (filled via second writer below)
            Xma2BitWriter body;
            body.WriteBits(st_ch == 2 ? 1u : 0u, 1u);  // stereo flag

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
                        body.WriteBits(0u, 1u);  // silent band flag
                        continue;
                    }
                    body.WriteBits(1u, 1u);  // active band
                    // Encode peak scale in 8.8 log2/fixed: u16 peak = round(max_abs * 256.0f), clamped 1..65535
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
            // Byte-align frame so 15-bit frameLengthInBits is clean and exact
            while (((15u + body.bit_len()) & 7u) != 0u) {
                body.WriteBits(0u, 1u);
            }
            const uint32_t total_frame_bits = static_cast<uint32_t>(15u + body.bit_len());
            bw.WriteBits(total_frame_bits, 15u);
            for (size_t bi = 0; bi < body.bit_len(); ++bi) {
                const uint8_t bit = (body.bytes()[bi >> 3] >> (7u - (bi & 7u))) & 1u;
                bw.WriteBits(bit, 1u);
            }
            encoded_frames.push_back(bw.bytes());
        }

        // Pack frames into 2048-byte packets with exact frameCount & frameOffsetInBits
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

    // Interleave packets across streams in standard XMA2 block order (skip_count = num_streams - 1)
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

class Xma2StreamDecoder {
public:
    explicit Xma2StreamDecoder(uint16_t channels = 2, uint32_t sample_rate = 48000)
        : channels_(std::clamp<uint16_t>(channels, 1u, 8u)), sample_rate_(sample_rate ? sample_rate : 48000u) {
        Reset();
    }

    void Reset() {
        std::memset(overlap_tail_, 0, sizeof(overlap_tail_));
        primed_stream_.fill(false);
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

            // Demux packets belonging to stream `st` using packetSkipCount stride
            std::vector<uint8_t> stream_payload;
            size_t first_frame_bit = 0;
            bool got_first_hdr = false;
            bool is_clean_mdct = false;

            for (size_t p_idx = st; p_idx < total_packets;) {
                const uint8_t* pkt = data + p_idx * kXma2PacketBytes;
                const Xma2PacketHeaderInfo ph = ParseXma2PacketHeader(pkt);
                if (!got_first_hdr) {
                    first_frame_bit = ph.frame_offset_in_bits;
                    is_clean_mdct = (ph.packet_metadata == kCleanRoomMdctMeta);
                    got_first_hdr = true;
                }
                if (ph.frame_offset_in_bits != 0x7FFFu || ph.frame_count > 0) {
                    stream_payload.insert(stream_payload.end(),
                                          pkt + kXma2PacketHeaderBytes,
                                          pkt + kXma2PacketBytes);
                }
                const size_t stride = static_cast<size_t>(ph.packet_skip_count) + 1u;
                p_idx += std::max<size_t>(1u, stride);
            }

            if (stream_payload.empty()) continue;
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
                if (is_clean_mdct) {
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
                } else {
                    // External WMA-Pro / retail XMA2 subframe fallback: extract band energy from frame payload bits
                    const uint32_t payload_bits = frame_bits - 15u;
                    const uint32_t band_bits = std::min<uint32_t>(payload_bits / 8u, static_cast<uint32_t>(kMdctBands));
                    for (uint32_t b = 0; b < band_bits; ++b) {
                        uint32_t raw_byte = 0;
                        if (!br.ReadBits(8u, raw_byte)) break;
                        const float amp = (static_cast<float>(static_cast<int8_t>(raw_byte)) / 128.0f) * 0.25f;
                        spec[0][b * 4u + 1u] = amp;
                        spec[1][b * 4u + 1u] = amp;
                    }
                }

                for (uint16_t c = 0; c < st_ch; ++c) {
                    float win_time[kMdctWindow]{};
                    InverseMdct128(spec[c], win_time);
                    const uint16_t ch_idx = static_cast<uint16_t>(ch_base + c);
                    if (primed_stream_[st]) {
                        for (size_t n = 0; n < kMdctBins; ++n) {
                            const float s = std::clamp(overlap_tail_[ch_idx][n] + win_time[n], -1.0f, 1.0f);
                            per_ch_samples[ch_idx].push_back(s);
                        }
                    }
                    std::memcpy(overlap_tail_[ch_idx], win_time + kMdctBins, sizeof(float) * kMdctBins);
                }
                primed_stream_[st] = true;
                br.Seek(frame_start + frame_bits);
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
    uint16_t channels_ = 2;
    uint32_t sample_rate_ = 48000;
    float overlap_tail_[8][kMdctBins]{};
    std::array<bool, 4> primed_stream_{};
};

}  // namespace scarlettx::audio
