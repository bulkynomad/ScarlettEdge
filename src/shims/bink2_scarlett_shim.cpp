#define SCARLETTX_BUILDING_BINK2 1
#include "scarlettx/middleware/bink2_abi.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct BinkStreamState {
    BINK pub{};
    std::string path;
    uint32_t played_frames = 0;
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

void SynthesizeFrame(BinkStreamState& s) {
    const uint32_t w = s.pub.Width;
    const uint32_t h = s.pub.Height;
    s.frame_rgba.resize(static_cast<size_t>(w) * h * 4ull);
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

    // Parse real BIK2 / KB2 / BIKi header if a file exists on disk
    std::ifstream file(name, std::ios::binary);
    if (file.is_open()) {
        uint8_t hdr[44] = {};
        file.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        if (file.gcount() >= 40) {
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
            }
        }
    }

    state->pub.InternalId = g_next_id++;
    SynthesizeFrame(*state);
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
        SynthesizeFrame(s);
        s.pub.LastFrameNum = s.pub.FrameNum;
        ++s.played_frames;
    }
    return 0;  // 0 = frame decoded (no skip)
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
    return bnk->Paused ? 1 : 0;  // 0 = ready for next frame immediately
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
    SynthesizeFrame(s);
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

void BinkGetRealtime(BINK* /*bnk*/, void* /*realtime*/, uint32_t /*flags*/) {}

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

}  // extern "C"
