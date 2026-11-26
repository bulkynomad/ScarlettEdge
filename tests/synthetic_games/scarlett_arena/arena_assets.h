#pragma once

#include <cstdint>

namespace scarlett_arena {

static constexpr uint32_t kArenaBundleMagic = 0x414E5241u;  // 'ARNA'
static constexpr uint32_t kArenaSaveMagic = 0x56415341u;    // 'ASAV'

struct ArenaAssetHeader {
    uint32_t magic;
    uint32_t triangle_count;
    uint32_t xma2_bytes;
    uint32_t uncompressed_size;
};

struct ArenaSaveState {
    uint32_t magic;
    uint32_t frames_rendered;
    uint32_t pulse_count;
    float player_x;
    float player_z;
    uint64_t first_frame_hash;
    uint64_t last_frame_hash;
    uint64_t audio_frames_mixed;
};

}  // namespace scarlett_arena
