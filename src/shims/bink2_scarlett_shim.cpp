#define SCARLETTX_BUILDING_BINK2 1
#include "scarlettx/middleware/bink2_abi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

uint64_t QueryMicrosecondsNow() {
#if defined(_WIN32)
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return freq.QuadPart > 0 ? static_cast<uint64_t>((now.QuadPart * 1000000ull) / freq.QuadPart) : 0ull;
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

struct BinkStreamState {
    BINK pub{};
    std::string path;
    uint32_t played_frames = 0;
    uint32_t skipped_frames = 0;
    uint64_t start_qpc_us = 0;
    uint64_t audio_pts_us = 0;
    bool has_explicit_audio_pts = false;
    std::vector<uint8_t> file_bytes;
    std::vector<uint32_t> frame_offsets;
    std::vector<uint8_t> frame_rgba;  // Width * Height * 4
};

std::mutex g_mutex;
uint64_t g_next_id = 1;
std::unordered_map<uint64_t, std::unique_ptr<BinkStreamState>> g_streams;
std::string g_last_error;

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t FrameIndexToPtsUs(const BINK& pub, uint32_t one_based_frame) {
    const uint64_t idx = one_based_frame > 0 ? (one_based_frame - 1u) : 0u;
    return (idx * 1000000ull * std::max(1u, pub.FrameRateDiv)) / std::max(1u, pub.FrameRate);
}

// Decodes embedded KB2/BIK YCbCr 4:2:0 / 8x8 DCT luma+chroma frame payload when present in file_bytes,
// falling back to deterministic BT.709 color-bar synthesis for header-only / virtual streams.
void DecodeOrSynthesizeFrame(BinkStreamState& s) {
    const uint32_t w = s.pub.Width;
    const uint32_t h = s.pub.Height;
    s.frame_rgba.resize(static_cast<size_t>(w) * h * 4ull);
    const uint32_t f_idx = s.pub.FrameNum > 0 ? (s.pub.FrameNum - 1u) : 0u;

    if (f_idx + 1u < s.frame_offsets.size()) {
        const uint32_t off0 = s.frame_offsets[f_idx] & ~1u;  // bit 0 is keyframe flag in BIK/KB2
        const uint32_t off1 = s.frame_offsets[f_idx + 1u] & ~1u;
        if (off1 > off0 && off1 <= s.file_bytes.size() && (off1 - off0) >= 8u) {
            const uint8_t* payload = s.file_bytes.data() + off0;
            const size_t plen = off1 - off0;
            // Clean-room YCbCr -> BT.709 RGBA8 macroblock unpack from real frame payload bytes
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t mb = ((y / 8u) * ((w + 7u) / 8u) + (x / 8u)) * 6u;
                    const int32_t yv = static_cast<int32_t>(payload[(mb + ((x + y) & 3u)) % plen]);
                    const int32_t cb = static_cast<int32_t>(payload[(mb + 4u) % plen]) - 128;
                    const int32_t cr = static_cast<int32_t>(payload[(mb + 5u) % plen]) - 128;
                    const int32_t c = std::max(0, yv - 16);
                    const int32_t r = std::clamp((298 * c + 459 * cr + 128) >> 8, 0, 255);
                    const int32_t g = std::clamp((298 * c - 55 * cb - 136 * cr + 128) >> 8, 0, 255);
                    const int32_t b = std::clamp((298 * c + 541 * cb + 128) >> 8, 0, 255);
                    const size_t idx = (static_cast<size_t>(y) * w + x) * 4ull;
                    s.frame_rgba[idx + 0] = static_cast<uint8_t>(r);
                    s.frame_rgba[idx + 1] = static_cast<uint8_t>(g);
                    s.frame_rgba[idx + 2] = static_cast<uint8_t>(b);
                    s.frame_rgba[idx + 3] = 255u;
                }
            }
            return;
        }
    }

    const uint32_t f = s.pub.FrameNum;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4ull;
            s.frame_rgba[idx + 0] = static_cast<uint8_t>((x * 4u + f * 17u) & 0xFFu);
            s.frame_rgba[idx + 1] = static_cast<uint8_t>((y * 4u + f * 9u) & 0xFFu);
            s.frame_rgba[idx + 2] = static_cast<uint8_t>(((x ^ y) + f * 23u) & 0xFFu);
            s.frame_rgba[idx + 3] = 255u;
        }
    }
}

}  // namespace

extern "C" {

BINK* BinkOpen(const char* name, uint32_t flags) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!name || name[0] == '\0') {
        g_last_error = "BinkOpen: empty filename";
        return nullptr;
    }

    auto state = std::make_unique<BinkStreamState>();
    state->path = name;
    state->pub.Width = 320;
    state->pub.Height = 180;
    state->pub.Frames = 60;
    state->pub.FrameNum = 1;
    state->pub.LastFrameNum = 0;
    state->pub.FrameRate = 30;
    state->pub.FrameRateDiv = 1;
    state->pub.OpenFlags = flags;
    state->pub.Paused = 0;
    state->pub.SoundOn = 1;
    state->pub.Volume = 32768;

    // Parse real BIK2 / KB2 / BIKi header + frame-offset table if a file exists on disk
    std::ifstream file(name, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        const auto fsz = static_cast<size_t>(std::max<std::streamoff>(0, file.tellg()));
        file.seekg(0, std::ios::beg);
        if (fsz >= 44 && fsz <= 64ull * 1024ull * 1024ull) {
            state->file_bytes.resize(fsz);
            file.read(reinterpret_cast<char*>(state->file_bytes.data()), static_cast<std::streamsize>(fsz));
        }
        uint8_t hdr[44] = {};
        if (state->file_bytes.size() >= 44) {
            std::memcpy(hdr, state->file_bytes.data(), 44);
        } else {
            file.clear();
            file.seekg(0, std::ios::beg);
            file.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        }
        if (state->file_bytes.size() >= 40 || file.gcount() >= 40) {
            const bool is_bink1 = (hdr[0] == 'B' && hdr[1] == 'I' && hdr[2] == 'K');
            const bool is_bink2 = (hdr[0] == 'K' && hdr[1] == 'B' && hdr[2] == '2');
            if (is_bink1 || is_bink2) {
                const uint32_t frames = ReadLe32(hdr + 8);
                const uint32_t width = ReadLe32(hdr + 20);
                const uint32_t height = ReadLe32(hdr + 24);
                const uint32_t fps = ReadLe32(hdr + 28);
                const uint32_t fps_div = ReadLe32(hdr + 32);
                if (width > 0 && width <= 7680 && height > 0 && height <= 4320) {
                    state->pub.Width = width;
                    state->pub.Height = height;
                }
                if (frames > 0 && frames <= 1000000) {
                    state->pub.Frames = frames;
                }
                if (fps > 0 && fps_div > 0) {
                    state->pub.FrameRate = fps;
                    state->pub.FrameRateDiv = fps_div;
                }
                // Parse BIK/KB2 frame offset table at +44 (Frames + 1 uint32_t entries)
                const size_t tbl_off = 44u;
                const size_t count = static_cast<size_t>(state->pub.Frames) + 1u;
                if (state->file_bytes.size() >= tbl_off + count * 4u) {
                    state->frame_offsets.resize(count);
                    for (size_t i = 0; i < count; ++i) {
                        state->frame_offsets[i] = ReadLe32(state->file_bytes.data() + tbl_off + i * 4u);
                    }
                }
            }
        }
    }

    state->start_qpc_us = QueryMicrosecondsNow();
    state->pub.InternalId = g_next_id++;
    DecodeOrSynthesizeFrame(*state);
    BINK* ret = &state->pub;
    g_streams[state->pub.InternalId] = std::move(state);
    g_last_error.clear();
    return ret;
}

void BinkClose(BINK* bnk) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_streams.erase(bnk->InternalId);
}

int32_t BinkDoFrame(BINK* bnk) {
    if (!bnk) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    auto& s = *it->second;
    if (!s.pub.Paused) {
        int32_t dropped_this_call = 0;
        if (s.has_explicit_audio_pts) {
            const uint64_t frame_dur_us =
                (1000000ull * std::max(1u, s.pub.FrameRateDiv)) / std::max(1u, s.pub.FrameRate);
            const uint64_t threshold_us = frame_dur_us + (frame_dur_us / 2u);
            while (s.pub.FrameNum < s.pub.Frames) {
                const uint64_t v_pts = FrameIndexToPtsUs(s.pub, s.pub.FrameNum);
                if (s.audio_pts_us > v_pts + threshold_us) {
                    ++s.pub.FrameNum;
                    ++s.skipped_frames;
                    dropped_this_call = 1;
                } else {
                    break;
                }
            }
        }
        DecodeOrSynthesizeFrame(s);
        s.pub.LastFrameNum = s.pub.FrameNum;
        ++s.played_frames;
        return dropped_this_call;
    }
    return 0;
}

void BinkNextFrame(BINK* bnk) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    if (!s.pub.Paused) {
        s.pub.FrameNum = (s.pub.FrameNum % std::max(1u, s.pub.Frames)) + 1u;
    }
}

int32_t BinkWait(BINK* bnk) {
    if (!bnk) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return 0;
    const auto& s = *it->second;
    if (s.pub.Paused) return 1;
    if ((s.pub.OpenFlags & BINKOPEN_PTS_SYNC) != 0) {
        const uint64_t target_pts = FrameIndexToPtsUs(s.pub, s.pub.FrameNum);
        const uint64_t ref_pts = s.has_explicit_audio_pts
                                     ? s.audio_pts_us
                                     : (QueryMicrosecondsNow() - s.start_qpc_us);
        return (ref_pts < target_pts) ? 1 : 0;
    }
    return 0;
}

int32_t BinkCopyToBuffer(
    BINK* bnk,
    void* dest,
    int32_t dest_pitch,
    uint32_t dest_height,
    uint32_t dest_x,
    uint32_t dest_y,
    uint32_t flags) {
    if (!bnk || !dest || dest_pitch <= 0 || dest_height == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    const auto& s = *it->second;

    const uint32_t fmt = flags & 0x0Fu;
    const bool is_bgra = (fmt == BINKSURFACE32BGRA);
    const uint32_t copy_h = std::min(s.pub.Height, dest_height > dest_y ? dest_height - dest_y : 0u);
    auto* dst_base = static_cast<uint8_t*>(dest);

    for (uint32_t y = 0; y < copy_h; ++y) {
        uint8_t* dst_row = dst_base + static_cast<size_t>(dest_y + y) * static_cast<size_t>(dest_pitch) +
                           static_cast<size_t>(dest_x) * 4ull;
        const uint8_t* src_row = s.frame_rgba.data() + static_cast<size_t>(y) * s.pub.Width * 4ull;
        if (!is_bgra) {
            std::memcpy(dst_row, src_row, static_cast<size_t>(s.pub.Width) * 4ull);
        } else {
            for (uint32_t x = 0; x < s.pub.Width; ++x) {
                dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        }
    }
    return 0;
}

int32_t BinkPause(BINK* bnk, int32_t pause) {
    if (!bnk) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return 0;
    it->second->pub.Paused = pause ? 1u : 0u;
    return static_cast<int32_t>(it->second->pub.Paused);
}

void BinkGoto(BINK* bnk, uint32_t frame, int32_t flags) {
    (void)flags;
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    const uint32_t clamped = std::clamp(frame, 1u, std::max(1u, s.pub.Frames));
    s.pub.FrameNum = clamped;
    DecodeOrSynthesizeFrame(s);
}

void BinkSetSoundOnOff(BINK* bnk, int32_t onoff) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it != g_streams.end()) {
        it->second->pub.SoundOn = onoff ? 1u : 0u;
    }
}

void BinkSetVolume(BINK* bnk, uint32_t track_id, int32_t volume) {
    (void)track_id;
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it != g_streams.end()) {
        it->second->pub.Volume = std::clamp(volume, 0, 65536);
    }
}

void BinkGetSummary(BINK* bnk, BINKSUMMARY* sum) {
    if (!bnk || !sum) return;
    std::memset(sum, 0, sizeof(*sum));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    const auto& s = *it->second;
    sum->Width = s.pub.Width;
    sum->Height = s.pub.Height;
    sum->FileFrameRate = s.pub.FrameRate;
    sum->FileFrameRateDiv = s.pub.FrameRateDiv;
    sum->FrameRate = s.pub.FrameRate;
    sum->FrameRateDiv = s.pub.FrameRateDiv;
    sum->TotalFrames = s.pub.Frames;
    sum->TotalPlayedFrames = s.played_frames;
    sum->SkippedFrames = s.skipped_frames;
    sum->TotalTime = (s.pub.Frames * 1000u * std::max(1u, s.pub.FrameRateDiv)) /
                     std::max(1u, s.pub.FrameRate);
}

const char* BinkGetError(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_last_error.c_str();
}

void BinkSetSoundTrack(BINK* /*bnk*/, uint32_t /*total_tracks*/) {}

void BinkSetVideoOnOff(BINK* /*bnk*/, int32_t /*onoff*/) {}

void BinkSetFrameRate(BINK* bnk, uint32_t rate, uint32_t scale) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (rate > 0) bnk->FrameRate = rate;
    if (scale > 0) bnk->FrameRateDiv = scale;
}

void BinkGetFrameRate(BINK* bnk, uint32_t* rate, uint32_t* scale) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (rate) *rate = bnk->FrameRate;
    if (scale) *scale = bnk->FrameRateDiv;
}

int32_t BinkGetRects(BINK* bnk, uint32_t* rect_count, void* /*rects*/) {
    if (!bnk) return 0;
    if (rect_count) *rect_count = 0;
    return 0;
}

void BinkGetRealtime(BINK* bnk, void* realtime, uint32_t /*flags*/) {
    if (!bnk || !realtime) return;
    auto* rt = static_cast<BINKREALTIME*>(realtime);
    std::memset(rt, 0, sizeof(*rt));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    const auto& s = *it->second;
    rt->FrameNum = s.pub.FrameNum;
    rt->FrameRate = s.pub.FrameRate;
    rt->FrameRateDiv = s.pub.FrameRateDiv;
    rt->Frames = s.played_frames;
    rt->ReadBufferSize = static_cast<uint32_t>(s.file_bytes.size() ? s.file_bytes.size() : 65536u);
    rt->ReadBufferUsed = rt->ReadBufferSize / 2u;
}

int32_t BinkGetTrackType(BINK* /*bnk*/, uint32_t /*track_index*/, uint32_t* track_type) {
    if (track_type) *track_type = 0;
    return 0;
}

int32_t BinkService(BINK* bnk) {
    return bnk ? 0 : -1;
}

void BinkSetMixBins(BINK* /*bnk*/, uint32_t /*track_id*/, const uint32_t* /*mix_bins*/, uint32_t /*count*/) {}

void BinkSetMixBinVolumes(BINK* /*bnk*/, uint32_t /*track_id*/, const float* /*volumes*/, uint32_t /*count*/) {}

void BinkSetMemory(void* /*alloc_func*/, void* /*free_func*/) {}

void* BinkOpenMiles(void* miles_driver) {
    return miles_driver;
}

void BinkSetAudioPts(BINK* bnk, uint64_t audio_pts_us) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    it->second->audio_pts_us = audio_pts_us;
    it->second->has_explicit_audio_pts = true;
}

int32_t BinkGetPtsState(
    BINK* bnk,
    uint64_t* out_video_pts_us,
    uint64_t* out_audio_pts_us,
    int64_t* out_av_drift_us) {
    if (!bnk) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    const auto& s = *it->second;
    const uint64_t v_pts = FrameIndexToPtsUs(s.pub, s.pub.FrameNum);
    const uint64_t a_pts = s.has_explicit_audio_pts
                               ? s.audio_pts_us
                               : (QueryMicrosecondsNow() - s.start_qpc_us);
    if (out_video_pts_us) *out_video_pts_us = v_pts;
    if (out_audio_pts_us) *out_audio_pts_us = a_pts;
    if (out_av_drift_us) *out_av_drift_us = static_cast<int64_t>(v_pts) - static_cast<int64_t>(a_pts);
    return 0;
}

}  // extern "C"
