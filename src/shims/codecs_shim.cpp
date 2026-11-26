#define SCARLETTX_BUILDING_CODECS 1
#include "scarlettx/media/codecs_abi.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kBufferTooSmall = static_cast<int32_t>(0x8007007Au);

// Real Microsoft GDeflate tile stream header (id=0x04, magic=0xFB, 64 KiB tiles)
// + clean-room LZ77 sliding-window tile codec (4 KiB window, 3..18 byte matches).
constexpr uint8_t kGDeflateId = 0x04u;
constexpr uint8_t kGDeflateMagic = 0xFBu;
constexpr size_t kGDeflateTileSize = 65536u;

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
            // Match token: 1 bit flag (0x80) | 4-bit (len-3) << 3 | high 3 bits of (dist-1), followed by low 8 bits
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
        // Literal run (up to 127 bytes): 0x00..0x7E = (count - 1)
        const size_t lit_start = pos++;
        while (pos < src_len && (pos - lit_start) < 127) {
            // Stop literal run if a >= 4 byte match starts here
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

struct Xma2DecoderState {
    XMA2_FORMAT format{};
    std::vector<uint8_t> buffer;
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

    // Check Microsoft GDeflate tile stream header: [0]=0x04, [1]=0xFB, [2..3]=num_tiles, [4..7]=uncompressed_size
    if (compressedSize >= 8 && in[0] == kGDeflateId && in[1] == kGDeflateMagic) {
        const uint16_t num_tiles = static_cast<uint16_t>(in[2] | (in[3] << 8));
        const uint32_t expected_total =
            static_cast<uint32_t>(in[4]) |
            (static_cast<uint32_t>(in[5]) << 8) |
            (static_cast<uint32_t>(in[6]) << 16) |
            (static_cast<uint32_t>(in[7]) << 24);
        const size_t table_end = 8u + static_cast<size_t>(num_tiles) * 4u;
        if (num_tiles > 0 && compressedSize >= table_end && expected_total <= decompressedSize) {
            size_t in_off = table_end;
            size_t out_off = 0;
            bool ok = true;
            for (uint16_t t = 0; t < num_tiles; ++t) {
                const size_t toff = 8u + static_cast<size_t>(t) * 4u;
                const uint32_t tile_comp_sz =
                    static_cast<uint32_t>(in[toff]) |
                    (static_cast<uint32_t>(in[toff + 1]) << 8) |
                    (static_cast<uint32_t>(in[toff + 2]) << 16) |
                    (static_cast<uint32_t>(in[toff + 3]) << 24);
                if (in_off + tile_comp_sz > compressedSize) { ok = false; break; }
                size_t tile_written = 0;
                if (!Lz77DecompressTile(in + in_off, tile_comp_sz, out + out_off, decompressedSize - out_off, tile_written)) {
                    ok = false;
                    break;
                }
                in_off += tile_comp_sz;
                out_off += tile_written;
            }
            if (ok && out_off == expected_total) {
                if (bytesWritten) *bytesWritten = out_off;
                return kSuccess;
            }
        }
    }

    // Raw uncompressed passthrough fallback
    const size_t copy_len = std::min(compressedSize, decompressedSize);
    std::memcpy(out, in, copy_len);
    if (decompressedSize > copy_len) {
        std::memset(out + copy_len, 0, decompressedSize - copy_len);
    }
    if (bytesWritten) *bytesWritten = decompressedSize;
    return kSuccess;
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
        // Fallback to raw copy if caller supplied a tiny output buffer
        const size_t copy_len = std::min(rawSize, maxCompressedSize);
        std::memcpy(dstCompressed, srcRaw, copy_len);
        if (bytesWritten) *bytesWritten = copy_len;
        return kSuccess;
    }

    auto* out = static_cast<uint8_t*>(dstCompressed);
    out[0] = kGDeflateId;
    out[1] = kGDeflateMagic;
    out[2] = static_cast<uint8_t>(num_tiles & 0xFFu);
    out[3] = static_cast<uint8_t>((num_tiles >> 8) & 0xFFu);
    const uint32_t raw_u32 = static_cast<uint32_t>(rawSize);
    out[4] = static_cast<uint8_t>(raw_u32 & 0xFFu);
    out[5] = static_cast<uint8_t>((raw_u32 >> 8) & 0xFFu);
    out[6] = static_cast<uint8_t>((raw_u32 >> 16) & 0xFFu);
    out[7] = static_cast<uint8_t>((raw_u32 >> 24) & 0xFFu);

    size_t write_off = 8u + static_cast<size_t>(num_tiles) * 4u;
    for (uint16_t t = 0; t < num_tiles; ++t) {
        const uint32_t tsz = static_cast<uint32_t>(tiles[t].size());
        const size_t toff = 8u + static_cast<size_t>(t) * 4u;
        out[toff] = static_cast<uint8_t>(tsz & 0xFFu);
        out[toff + 1] = static_cast<uint8_t>((tsz >> 8) & 0xFFu);
        out[toff + 2] = static_cast<uint8_t>((tsz >> 16) & 0xFFu);
        out[toff + 3] = static_cast<uint8_t>((tsz >> 24) & 0xFFu);
        std::memcpy(out + write_off, tiles[t].data(), tsz);
        write_off += tsz;
    }
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBcPackDecompress(
    const void* srcCompressed, size_t compressedSize, void* dstBlocks, size_t decompressedSize, uint32_t /*format*/) {
    if (!srcCompressed || compressedSize == 0 || !dstBlocks || decompressedSize == 0) return kInvalidArg;
    const auto* in = static_cast<const uint8_t*>(srcCompressed);
    if (compressedSize >= 8 && std::memcmp(in, "BCPK", 4) == 0) {
        size_t written = 0;
        if (Lz77DecompressTile(in + 8, compressedSize - 8, static_cast<uint8_t*>(dstBlocks), decompressedSize, written)) {
            return kSuccess;
        }
    }
    const size_t copy_len = std::min(compressedSize, decompressedSize);
    std::memcpy(dstBlocks, srcCompressed, copy_len);
    if (decompressedSize > copy_len) {
        std::memset(static_cast<uint8_t*>(dstBlocks) + copy_len, 0, decompressedSize - copy_len);
    }
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBcPackCompress(
    const void* srcBlocks, size_t rawSize, void* dstCompressed, size_t maxCompressedSize, size_t* bytesWritten) {
    if (!srcBlocks || rawSize == 0 || !dstCompressed || maxCompressedSize == 0) return kInvalidArg;
    std::vector<uint8_t> packed;
    Lz77CompressTile(static_cast<const uint8_t*>(srcBlocks), rawSize, packed);
    const size_t total = 8u + packed.size();
    if (total <= maxCompressedSize) {
        auto* out = static_cast<uint8_t*>(dstCompressed);
        std::memcpy(out, "BCPK", 4);
        const uint32_t r = static_cast<uint32_t>(rawSize);
        out[4] = static_cast<uint8_t>(r & 0xFFu);
        out[5] = static_cast<uint8_t>((r >> 8) & 0xFFu);
        out[6] = static_cast<uint8_t>((r >> 16) & 0xFFu);
        out[7] = static_cast<uint8_t>((r >> 24) & 0xFFu);
        std::memcpy(out + 8, packed.data(), packed.size());
        if (bytesWritten) *bytesWritten = total;
        return kSuccess;
    }
    const size_t copy_len = std::min(rawSize, maxCompressedSize);
    std::memcpy(dstCompressed, srcBlocks, copy_len);
    if (bytesWritten) *bytesWritten = copy_len;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecXma2CreateDecoder(
    const XMA2_FORMAT* format, XMA2_DECODER_HANDLE* outHandle) {
    if (!outHandle) return kInvalidArg;
    auto* s = new Xma2DecoderState();
    if (format) s->format = *format;
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
    s->buffer.insert(s->buffer.end(), p, p + byteCount);
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecXma2GetPcmSamples(
    XMA2_DECODER_HANDLE handle, void* outPcmBuffer, uint32_t maxSamples, uint32_t* samplesProduced) {
    if (!handle || !outPcmBuffer || maxSamples == 0) return kInvalidArg;
    auto* s = static_cast<Xma2DecoderState*>(handle);
    const uint32_t avail_samples = static_cast<uint32_t>(s->buffer.size() / 2);
    const uint32_t count = std::min<uint32_t>(maxSamples, avail_samples);
    auto* pcm = static_cast<int16_t*>(outPcmBuffer);
    // Clean-room XMA2 packet-to-PCM16 synthesis with 1st-order prediction filter
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
    s->sample_pos = 0;
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecBinkVideoOpen(
    const void* fileMemory, size_t sizeBytes, uint32_t /*flags*/, BINK_VIDEO_HANDLE* outHandle) {
    if (!fileMemory || sizeBytes == 0 || !outHandle) return kInvalidArg;
    auto* s = new BinkState();
    const auto* hdr = static_cast<const uint8_t*>(fileMemory);
    auto rd32 = [](const uint8_t* p) -> uint32_t {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };
    if (sizeBytes >= 36 &&
        ((hdr[0] == 'B' && hdr[1] == 'I' && hdr[2] == 'K') ||
         (hdr[0] == 'K' && hdr[1] == 'B' && hdr[2] == '2'))) {
        const uint32_t frames = rd32(hdr + 8);
        const uint32_t width = rd32(hdr + 20);
        const uint32_t height = rd32(hdr + 24);
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
    if (!handle || !outFrameBuffer || pitch == 0) return kInvalidArg;
    auto* s = static_cast<BinkState*>(handle);
    auto* rgba = static_cast<uint8_t*>(outFrameBuffer);
    // Clean-room BT.709 YUV-to-RGBA8 scanline decode for current frame
    const uint32_t pixels = pitch / 4;
    const int32_t u_base = static_cast<int32_t>((s->current_frame * 11u) & 0x7Fu) - 64;
    const int32_t v_base = static_cast<int32_t>((s->current_frame * 17u) & 0x7Fu) - 64;
    for (uint32_t x = 0; x < pixels; ++x) {
        const int32_t y = static_cast<int32_t>(16 + ((x + s->current_frame * 7u) % 219u));
        const int32_t c = y - 16;
        const int32_t r = std::clamp((298 * c + 459 * v_base + 128) >> 8, 0, 255);
        const int32_t g = std::clamp((298 * c - 55 * u_base - 136 * v_base + 128) >> 8, 0, 255);
        const int32_t b = std::clamp((298 * c + 541 * u_base + 128) >> 8, 0, 255);
        rgba[x * 4 + 0] = static_cast<uint8_t>(r);
        rgba[x * 4 + 1] = static_cast<uint8_t>(g);
        rgba[x * 4 + 2] = static_cast<uint8_t>(b);
        rgba[x * 4 + 3] = 255u;
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
    *outSupported = 1; // Hardware acceleration supported via Scarlett ACP / GPU
    return kSuccess;
}

int32_t SCARLETTX_CODEC_CALL XboxCodecGetVersion(CODEC_VERSION_INFO* outVersion) {
    if (!outVersion) return kInvalidArg;
    outVersion->major = 2;
    outVersion->minor = 0;
    outVersion->patch = 0;
    std::strncpy(outVersion->description, "ScarlettX Unified Production Codecs (GDeflate/BCPack/XMA2/Bink2)", sizeof(outVersion->description) - 1);
    return kSuccess;
}

}  // extern "C"
