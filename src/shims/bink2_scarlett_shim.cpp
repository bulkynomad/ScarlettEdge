#define SCARLETTX_BUILDING_BINK2 1
#include "scarlettx/middleware/bink2_abi.h"
#include "scarlettx/loader/package_vfs.h"

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

#if defined(_WIN32)
size_t QueryReadableCommittedBytes(const void* base_ptr, size_t max_needed) {
    if (!base_ptr || max_needed == 0) return 0;
    const auto* cur = static_cast<const uint8_t*>(base_ptr);
    size_t total = 0;
    while (total < max_needed) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) break;
        const DWORD prot = mbi.Protect & 0xFFu;
        const bool readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                               prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                               prot == PAGE_EXECUTE_WRITECOPY);
        if (!readable) break;
        const auto* reg_end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (reg_end <= cur) break;
        total += static_cast<size_t>(reg_end - cur);
        cur = reg_end;
    }
    return total;
}
#endif

struct BinkStreamState {
    BINK pub{};
    std::string path;
    uint32_t played_frames = 0;
    uint32_t skipped_frames = 0;
    uint64_t start_qpc_us = 0;
    uint64_t audio_pts_us = 0;
    bool has_explicit_audio_pts = false;
    bool has_registered_fb = false;
    BINKFRAMEBUFFERS registered_fb{};
    BINKFRAMEBUFFERS* registered_fb_ptr = nullptr;
    std::vector<uint8_t> file_bytes;
    std::vector<uint32_t> frame_offsets;
    std::vector<uint8_t> frame_rgba;  // Width * Height * 4
};

std::mutex g_mutex;
uint64_t g_next_id = 1;
std::unordered_map<uint64_t, std::unique_ptr<BinkStreamState>> g_streams;
std::string g_last_error;
thread_local char t_last_error[256] = {};

void SetBinkErrorLocked(const char* msg) {
    g_last_error = msg ? msg : "";
    std::memset(t_last_error, 0, sizeof(t_last_error));
    if (msg) {
        std::strncpy(t_last_error, msg, sizeof(t_last_error) - 1);
    }
}

void ClearBinkErrorLocked() {
    g_last_error.clear();
    t_last_error[0] = '\0';
}

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

void ParseBinkContainerHeader(BinkStreamState& state) {
    if (state.file_bytes.size() < 40u) return;
    const uint8_t* hdr = state.file_bytes.data();
    const bool is_bink1 = (hdr[0] == 'B' && hdr[1] == 'I' && hdr[2] == 'K');
    const bool is_bink2 = (hdr[0] == 'K' && hdr[1] == 'B' && hdr[2] == '2');
    if (!is_bink1 && !is_bink2) return;

    const uint32_t frames = ReadLe32(hdr + 8);
    const uint32_t width = ReadLe32(hdr + 20);
    const uint32_t height = ReadLe32(hdr + 24);
    const uint32_t fps = ReadLe32(hdr + 28);
    const uint32_t fps_div = ReadLe32(hdr + 32);
    if (width > 0 && width <= 7680 && height > 0 && height <= 4320) {
        state.pub.Width = width;
        state.pub.Height = height;
    }
    if (frames > 0 && frames <= 1000000) {
        state.pub.Frames = frames;
    }
    if (fps > 0 && fps_div > 0) {
        state.pub.FrameRate = fps;
        state.pub.FrameRateDiv = fps_div;
    }
    const size_t tbl_off = 44u;
    const size_t count = static_cast<size_t>(state.pub.Frames) + 1u;
    if (state.file_bytes.size() >= tbl_off + count * 4u) {
        state.frame_offsets.resize(count);
        for (size_t i = 0; i < count; ++i) {
            state.frame_offsets[i] = ReadLe32(state.file_bytes.data() + tbl_off + i * 4u);
        }
    }
}

void UpdateRegisteredYCrCbAPlanes(BinkStreamState& s) {
    if (!s.has_registered_fb || s.frame_rgba.empty()) return;
    const int32_t total = std::clamp<int32_t>(s.registered_fb.TotalFrames, 1, BINKMAXFRAMEBUFFERS);
    const uint32_t slot = s.played_frames % static_cast<uint32_t>(total);
    s.registered_fb.FrameNum = slot;
    if (s.registered_fb_ptr) {
        s.registered_fb_ptr->FrameNum = slot;
    }

    const BINKFRAMEPLANESET& ps = s.registered_fb.Frames[slot];
    const uint32_t w = s.pub.Width;
    const uint32_t h = s.pub.Height;

    if (ps.YPlane.Buffer && ps.YPlane.BufferPitch >= w) {
        auto* y_dst = static_cast<uint8_t*>(ps.YPlane.Buffer);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* px = s.frame_rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
                const int32_t r = px[0], g = px[1], b = px[2];
                const int32_t luma = std::clamp(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16, 16, 235);
                y_dst[static_cast<size_t>(y) * ps.YPlane.BufferPitch + x] = static_cast<uint8_t>(luma);
            }
        }
    }

    if (ps.APlane.Buffer && ps.APlane.BufferPitch >= w) {
        auto* a_dst = static_cast<uint8_t*>(ps.APlane.Buffer);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* px = s.frame_rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
                a_dst[static_cast<size_t>(y) * ps.APlane.BufferPitch + x] = px[3];
            }
        }
    }

    const uint32_t cw = (w + 1u) / 2u;
    const uint32_t ch = (h + 1u) / 2u;
    if ((ps.cRPlane.Buffer && ps.cRPlane.BufferPitch >= cw) ||
        (ps.cBPlane.Buffer && ps.cBPlane.BufferPitch >= cw)) {
        auto* cr_dst = static_cast<uint8_t*>(ps.cRPlane.Buffer);
        auto* cb_dst = static_cast<uint8_t*>(ps.cBPlane.Buffer);
        for (uint32_t cy = 0; cy < ch; ++cy) {
            const uint32_t sy = std::min<uint32_t>(cy * 2u, h - 1u);
            for (uint32_t cx = 0; cx < cw; ++cx) {
                const uint32_t sx = std::min<uint32_t>(cx * 2u, w - 1u);
                const uint8_t* px = s.frame_rgba.data() + (static_cast<size_t>(sy) * w + sx) * 4u;
                const int32_t r = px[0], g = px[1], b = px[2];
                if (cr_dst && ps.cRPlane.BufferPitch >= cw) {
                    const int32_t cr = std::clamp(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128, 16, 240);
                    cr_dst[static_cast<size_t>(cy) * ps.cRPlane.BufferPitch + cx] = static_cast<uint8_t>(cr);
                }
                if (cb_dst && ps.cBPlane.BufferPitch >= cw) {
                    const int32_t cb = std::clamp(((-26 * r - 86 * g + 112 * b + 128) >> 8) + 128, 16, 240);
                    cb_dst[static_cast<size_t>(cy) * ps.cBPlane.BufferPitch + cx] = static_cast<uint8_t>(cb);
                }
            }
        }
    }
}

void DecodeOrSynthesizeFrame(BinkStreamState& s) {
    const uint32_t w = s.pub.Width;
    const uint32_t h = s.pub.Height;
    s.frame_rgba.resize(static_cast<size_t>(w) * h * 4ull);
    const uint32_t f_idx = s.pub.FrameNum > 0 ? (s.pub.FrameNum - 1u) : 0u;

    bool decoded_from_payload = false;
    if (f_idx + 1u < s.frame_offsets.size()) {
        const uint32_t off0 = s.frame_offsets[f_idx] & ~1u;
        const uint32_t off1 = s.frame_offsets[f_idx + 1u] & ~1u;
        if (off1 > off0 && off1 <= s.file_bytes.size() && (off1 - off0) >= 8u) {
            const uint8_t* payload = s.file_bytes.data() + off0;
            const size_t plen = off1 - off0;
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
            decoded_from_payload = true;
        }
    }

    if (!decoded_from_payload) {
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

    UpdateRegisteredYCrCbAPlanes(s);
}

}  // namespace

extern "C" {

BINK* BinkOpen(const char* name, uint32_t flags) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool from_memory = (flags & BINKFROMMEMORY) != 0u;
    if (!name || (!from_memory && name[0] == '\0')) {
        SetBinkErrorLocked("BinkOpen: invalid or empty source");
        return nullptr;
    }

    auto state = std::make_unique<BinkStreamState>();
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

    if (from_memory) {
        state->path = "<BINKFROMMEMORY>";
        const auto* mem = reinterpret_cast<const uint8_t*>(name);
        size_t readable = 64ull * 1024ull * 1024ull;
#if defined(_WIN32)
        readable = QueryReadableCommittedBytes(mem, 64ull * 1024ull * 1024ull);
#endif
        if (readable >= 40u) {
            const bool is_bink = (mem[0] == 'B' && mem[1] == 'I' && mem[2] == 'K') ||
                                 (mem[0] == 'K' && mem[1] == 'B' && mem[2] == '2');
            if (is_bink) {
                const uint32_t sz_minus_8 = ReadLe32(mem + 4);
                const uint32_t frames = ReadLe32(mem + 8);
                size_t total_sz = 44u;
                if (sz_minus_8 > 0u && static_cast<size_t>(sz_minus_8) + 8u <= 64ull * 1024ull * 1024ull) {
                    total_sz = static_cast<size_t>(sz_minus_8) + 8u;
                } else if (frames > 0u && frames <= 1000000u) {
                    const size_t tbl_end = 44u + (static_cast<size_t>(frames) + 1u) * 4u;
                    total_sz = tbl_end;
                    if (readable >= tbl_end) {
                        const uint32_t end_off = ReadLe32(mem + 44u + static_cast<size_t>(frames) * 4u) & ~1u;
                        total_sz = std::max<size_t>(tbl_end, end_off);
                    }
                }
                const size_t safe_copy = std::min<size_t>(total_sz, readable);
                state->file_bytes.assign(mem, mem + safe_copy);
                ParseBinkContainerHeader(*state);
            }
        }
    } else {
        state->path = name;
        std::ifstream file(name, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            const auto fsz = static_cast<size_t>(std::max<std::streamoff>(0, file.tellg()));
            file.seekg(0, std::ios::beg);
            if (fsz >= 40u && fsz <= 64ull * 1024ull * 1024ull) {
                state->file_bytes.resize(fsz);
                file.read(reinterpret_cast<char*>(state->file_bytes.data()), static_cast<std::streamsize>(fsz));
                ParseBinkContainerHeader(*state);
            }
        } else if (scarlettx::PackageVfs::Global().Exists(name)) {
            std::vector<uint8_t> vfs_bytes;
            if (scarlettx::PackageVfs::Global().ReadFileBytes(name, vfs_bytes).ok() && vfs_bytes.size() >= 40u) {
                state->file_bytes = std::move(vfs_bytes);
                ParseBinkContainerHeader(*state);
            }
        }
    }

    state->start_qpc_us = QueryMicrosecondsNow();
    state->pub.InternalId = g_next_id++;
    DecodeOrSynthesizeFrame(*state);
    BINK* ret = &state->pub;
    g_streams[state->pub.InternalId] = std::move(state);
    ClearBinkErrorLocked();
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
    if (!bnk || !dest || dest_pitch <= 0 || dest_height == 0 || dest_y >= dest_height) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    const auto& s = *it->second;

    const uint32_t fmt = flags & 0x0Fu;
    const bool is_24bpp = (fmt == BINKSURFACE24);
    const bool is_bgra = (fmt == BINKSURFACE32BGRA);
    const size_t bpp = is_24bpp ? 3u : 4u;
    const size_t pitch_bytes = static_cast<size_t>(dest_pitch);

    if (static_cast<size_t>(dest_x) * bpp >= pitch_bytes) {
        return -1;
    }
    const uint32_t max_cols = static_cast<uint32_t>((pitch_bytes - static_cast<size_t>(dest_x) * bpp) / bpp);
    const uint32_t copy_w = std::min<uint32_t>(s.pub.Width, max_cols);
    const uint32_t copy_h = std::min<uint32_t>(s.pub.Height, dest_height - dest_y);
    if (copy_w == 0u || copy_h == 0u ||
        static_cast<size_t>(dest_x) * bpp + static_cast<size_t>(copy_w) * bpp > pitch_bytes) {
        return -1;
    }

    auto* dst_base = static_cast<uint8_t*>(dest);
    for (uint32_t y = 0; y < copy_h; ++y) {
        uint8_t* dst_row =
            dst_base + static_cast<size_t>(dest_y + y) * pitch_bytes + static_cast<size_t>(dest_x) * bpp;
        const uint8_t* src_row = s.frame_rgba.data() + static_cast<size_t>(y) * s.pub.Width * 4ull;
        if (is_24bpp) {
            for (uint32_t x = 0; x < copy_w; ++x) {
                dst_row[x * 3 + 0] = src_row[x * 4 + 2];
                dst_row[x * 3 + 1] = src_row[x * 4 + 1];
                dst_row[x * 3 + 2] = src_row[x * 4 + 0];
            }
        } else if (!is_bgra) {
            std::memcpy(dst_row, src_row, static_cast<size_t>(copy_w) * 4ull);
        } else {
            for (uint32_t x = 0; x < copy_w; ++x) {
                dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        }
    }
    return 0;
}

void BinkGetFrameBuffersInfo(BINK* bnk, BINKFRAMEBUFFERS* fbset) {
    if (!fbset) return;
    std::memset(fbset, 0, sizeof(*fbset));
    uint32_t w = 320, h = 180, open_flags = 0;
    if (bnk) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_streams.find(bnk->InternalId);
        if (it != g_streams.end()) {
            w = it->second->pub.Width;
            h = it->second->pub.Height;
            open_flags = it->second->pub.OpenFlags;
        } else {
            w = bnk->Width ? bnk->Width : 320u;
            h = bnk->Height ? bnk->Height : 180u;
            open_flags = bnk->OpenFlags;
        }
    }
    fbset->TotalFrames = BINKMAXFRAMEBUFFERS;
    fbset->YABufferWidth = (w + 15u) & ~15u;
    fbset->YABufferHeight = (h + 15u) & ~15u;
    fbset->cRcBBufferWidth = fbset->YABufferWidth / 2u;
    fbset->cRcBBufferHeight = fbset->YABufferHeight / 2u;
    fbset->FrameNum = 0;
    const int32_t need_alpha = (open_flags & BINKALPHA) ? 1 : 0;
    for (int i = 0; i < BINKMAXFRAMEBUFFERS; ++i) {
        fbset->Frames[i].YPlane = {1, nullptr, fbset->YABufferWidth};
        fbset->Frames[i].cRPlane = {1, nullptr, fbset->cRcBBufferWidth};
        fbset->Frames[i].cBPlane = {1, nullptr, fbset->cRcBBufferWidth};
        fbset->Frames[i].APlane = {need_alpha, nullptr, fbset->YABufferWidth};
    }
}

void BinkRegisterFrameBuffers(BINK* bnk, BINKFRAMEBUFFERS* fbset) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    if (!fbset) {
        s.has_registered_fb = false;
        s.registered_fb_ptr = nullptr;
        std::memset(&s.registered_fb, 0, sizeof(s.registered_fb));
        return;
    }
    s.registered_fb = *fbset;
    s.registered_fb_ptr = fbset;
    s.has_registered_fb = true;
    UpdateRegisteredYCrCbAPlanes(s);
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
    if (t_last_error[0] != '\0') {
        return t_last_error;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::memset(t_last_error, 0, sizeof(t_last_error));
    if (!g_last_error.empty()) {
        std::strncpy(t_last_error, g_last_error.c_str(), sizeof(t_last_error) - 1);
    }
    return t_last_error;
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
