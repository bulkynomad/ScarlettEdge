// ScarlettX — Xbox texture memory layout (swizzled texel addressing).
#include "scarlettx/gpu/xg_layout.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace scarlettx::gpu::xg {
namespace {

constexpr uint32_t kMode256BS = 1;
constexpr uint32_t kMode4KbZ = 4;
constexpr uint32_t kMode4KbS = XG_SWIZZLE_MODE_4KB_S;
constexpr uint32_t kMode4KbD = XG_SWIZZLE_MODE_4KB_D;
constexpr uint32_t kMode4KbR = 7;
constexpr uint32_t kMode64KbZ = 8;
constexpr uint32_t kMode64KbS = 9;
constexpr uint32_t kMode64KbD = 10;
constexpr uint32_t kMode64KbR = 11;
constexpr uint32_t kMode64KbZT = 16;
constexpr uint32_t kMode64KbST = 17;
constexpr uint32_t kMode64KbDT = 18;
constexpr uint32_t kMode64KbRT = 19;
constexpr uint32_t kMode4KbZX = 20;
constexpr uint32_t kMode4KbSX = 21;
constexpr uint32_t kMode4KbDX = 22;
constexpr uint32_t kMode4KbRX = 23;
constexpr uint32_t kMode64KbZX = 24;
constexpr uint32_t kMode64KbSX = 25;
constexpr uint32_t kMode64KbDX = 26;
constexpr uint32_t kMode64KbRX = 27;
constexpr uint32_t kBlockBytes = 4096;
constexpr uint32_t kBlockBytes64K = 65536;
constexpr uint32_t kMaxMips = 15;

bool Is4KbAddressableMode(uint32_t mode) {
    return mode == kMode4KbZ || mode == kMode4KbS || mode == kMode4KbD || mode == kMode4KbR ||
           mode == kMode4KbZX || mode == kMode4KbSX || mode == kMode4KbDX || mode == kMode4KbRX;
}

bool Is64KbAddressableMode(uint32_t mode) {
    return mode == kMode64KbZ || mode == kMode64KbS || mode == kMode64KbD || mode == kMode64KbR ||
           mode == kMode64KbZT || mode == kMode64KbST || mode == kMode64KbDT || mode == kMode64KbRT ||
           mode == kMode64KbZX || mode == kMode64KbSX || mode == kMode64KbDX || mode == kMode64KbRX;
}

bool Is3dZInterleavedMode(uint32_t mode) {
    return mode == kMode4KbD || mode == kMode4KbDX ||
           mode == kMode64KbD || mode == kMode64KbDT || mode == kMode64KbDX;
}

// Source of each offset bit inside a 4 KB block: 'x'/'y' and the coordinate bit.
struct BitSource {
    char axis;
    uint8_t bit;
};
struct BlockEquation {
    uint32_t bytes_per_element;
    BitSource bits[12];
};

constexpr BitSource N{0, 0};
#define SX_X(n) BitSource{'x', n}
#define SX_Y(n) BitSource{'y', n}
constexpr BlockEquation kEquations[] = {
    {1, {SX_X(0), SX_X(1), SX_X(2), SX_X(3), SX_Y(0), SX_Y(1), SX_Y(2), SX_Y(3), SX_Y(4), SX_X(4), SX_Y(5), SX_X(5)}},
    {2, {N, SX_X(0), SX_X(1), SX_X(2), SX_Y(0), SX_Y(1), SX_Y(2), SX_X(3), SX_Y(3), SX_X(4), SX_Y(4), SX_X(5)}},
    {4, {N, N, SX_X(0), SX_X(1), SX_Y(0), SX_Y(1), SX_Y(2), SX_X(2), SX_Y(3), SX_X(3), SX_Y(4), SX_X(4)}},
    {8, {N, N, N, SX_X(0), SX_Y(0), SX_Y(1), SX_X(1), SX_X(2), SX_Y(2), SX_X(3), SX_Y(3), SX_X(4)}},
    {16, {N, N, N, N, SX_Y(0), SX_Y(1), SX_X(0), SX_X(1), SX_Y(2), SX_X(2), SX_Y(3), SX_X(3)}},
};
#undef SX_X
#undef SX_Y

// Offsets of the mips packed into the 4 KB tail block, in 256-byte units.
constexpr uint32_t kTailOffset256[8] = {8, 6, 5, 4, 3, 2, 1, 0};

// Offsets and byte sizes of mips packed into an ADDR2 64 KB macro-tile tail block.
constexpr uint32_t kTail64Offset256[10] = {128, 96, 8, 6, 5, 4, 3, 2, 1, 0};
constexpr uint32_t kTail64RegionBytes[10] = {
    0x8000u, 0x2000u, 0x800u, 0x200u, 0x100u, 0x100u, 0x100u, 0x100u, 0x100u, 0x100u};

const BlockEquation* FindEquation(uint32_t bytes_per_element) {
    for (const auto& eq : kEquations) {
        if (eq.bytes_per_element == bytes_per_element) return &eq;
    }
    return nullptr;
}

// Number of coordinate bits of `axis` among offset bits below `limit`.
uint32_t AxisBits(const BlockEquation& eq, char axis, uint32_t limit) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < limit && i < 12; ++i) {
        if (eq.bits[i].axis == axis) ++n;
    }
    return n;
}

uint32_t AxisOffset(const BlockEquation& eq, char axis, uint32_t coord) {
    uint32_t off = 0;
    for (uint32_t i = 0; i < 12; ++i) {
        if (eq.bits[i].axis == axis && ((coord >> eq.bits[i].bit) & 1u)) off |= 1u << i;
    }
    return off;
}

// Coordinate whose in-block offset is `offset`.
uint32_t AxisOrigin(const BlockEquation& eq, char axis, uint32_t offset) {
    uint32_t coord = 0;
    for (uint32_t i = 0; i < 12; ++i) {
        if (eq.bits[i].axis == axis && ((offset >> i) & 1u)) coord |= 1u << eq.bits[i].bit;
    }
    return coord;
}

uint32_t Log2(uint32_t v) {
    uint32_t n = 0;
    while ((1u << n) < v) ++n;
    return n;
}

uint32_t MaxMipLevelsForDim(uint32_t max_dim) {
    uint32_t levels = 1u;
    while ((max_dim >>= 1u) != 0u) ++levels;
    return std::min<uint32_t>(levels, kMaxMips);
}

uint32_t AlignUp32(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint64_t AlignUp64(uint64_t v, uint64_t a) { return a <= 1 ? v : ((v + a - 1) / a * a); }

uint32_t TailRegionBytes(uint32_t slot) { return slot == 0 ? 0x800u : (slot == 1 ? 0x200u : 0x100u); }

void Get3dZBlockDims(uint32_t bpe, bool is_64k, uint32_t* bw, uint32_t* bh, uint32_t* bd) {
    switch (bpe) {
        case 1:  *bw = 16; *bh = 16; *bd = 16; break;
        case 2:  *bw = 16; *bh = 16; *bd = 8;  break;
        case 8:  *bw = 8;  *bh = 8;  *bd = 8;  break;
        case 16: *bw = 8;  *bh = 8;  *bd = 4;  break;
        default: *bw = 16; *bh = 8;  *bd = 8;  break;  // 4 BPE -> 16x8x8 = 1024 elems = 4 KB
    }
    if (is_64k) {
        *bw *= 4u;
        *bh *= 2u;
        *bd *= 2u;
    }
}

uint64_t Morton3dElementByteOffset(uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t bpe, uint32_t bw, uint32_t bh, uint32_t bd,
                                   uint32_t pitch_x_blocks, uint32_t pitch_y_blocks,
                                   uint32_t block_bytes) {
    const uint32_t bx = x / bw;
    const uint32_t by = y / bh;
    const uint32_t bz = z / bd;
    const uint32_t ix = x % bw;
    const uint32_t iy = y % bh;
    const uint32_t iz = z % bd;
    const uint32_t nx = Log2(bw);
    const uint32_t ny = Log2(bh);
    const uint32_t nz = Log2(bd);
    const uint32_t max_b = std::max({nx, ny, nz});
    uint32_t in_elem = 0;
    uint32_t out_bit = 0;
    for (uint32_t b = 0; b < max_b; ++b) {
        if (b < nx) { in_elem |= ((ix >> b) & 1u) << out_bit; ++out_bit; }
        if (b < ny) { in_elem |= ((iy >> b) & 1u) << out_bit; ++out_bit; }
        if (b < nz) { in_elem |= ((iz >> b) & 1u) << out_bit; ++out_bit; }
    }
    const uint64_t block_idx =
        (static_cast<uint64_t>(bz) * pitch_y_blocks + by) * pitch_x_blocks + bx;
    return block_idx * block_bytes + static_cast<uint64_t>(in_elem) * bpe;
}

uint32_t BlockBytesForMode(uint32_t mode) {
    if (mode == 0) return 256;
    if (mode <= 3) return 256;
    if (mode <= 7 || (mode >= 20 && mode <= 23)) return 4096;
    return 65536;
}

struct MipView {
    const XG_MIPLEVEL_LAYOUT* ml = nullptr;
    const BlockEquation* eq = nullptr;
    uint32_t block_bytes = kBlockBytes;
    uint32_t micro_w = 0, micro_h = 0;  // 4KB micro-tile width/height
    uint32_t block_w = 0, block_h = 0, block_d = 1;  // macro-tile width/height/depth
    uint32_t origin_x = 0, origin_y = 0;  // position inside the tail block
    uint32_t pitch_blocks = 0;
    uint32_t pitch_y_blocks = 0;
    uint32_t slice_z = 0;
    uint32_t bpe = 4;
    uint64_t base = 0;
    bool is_64k = false;
    bool is_3d_z = false;
};

bool ViewMip(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice, MipView* view) {
    if (!IsAddressable(layout) || mip >= layout.MipLevels || mip >= kMaxMips) return false;
    const XG_PLANE_LAYOUT& plane = layout.Plane[0];
    const XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[mip];
    if (slice >= std::max<uint32_t>(1, ml.DepthOrArraySize)) return false;
    const BlockEquation* eq = FindEquation(plane.BytesPerElement);
    view->ml = &ml;
    view->eq = eq;
    view->bpe = plane.BytesPerElement;
    view->slice_z = slice;
    const bool is_3d = (layout.Dimension == 4u);
    if (is_3d && Is3dZInterleavedMode(ml.SwizzleMode)) {
        view->is_3d_z = true;
        view->is_64k = Is64KbAddressableMode(ml.SwizzleMode);
        view->block_bytes = view->is_64k ? kBlockBytes64K : kBlockBytes;
        Get3dZBlockDims(plane.BytesPerElement, view->is_64k, &view->block_w, &view->block_h, &view->block_d);
        view->pitch_blocks = std::max<uint32_t>(1u, ml.PaddedWidthElements / view->block_w);
        view->pitch_y_blocks = std::max<uint32_t>(1u, ml.PaddedHeightElements / view->block_h);
        view->base = ml.OffsetBytes;
        return true;
    }

    view->micro_w = 1u << AxisBits(*eq, 'x', 12);
    view->micro_h = 1u << AxisBits(*eq, 'y', 12);
    view->is_64k = Is64KbAddressableMode(ml.SwizzleMode) && !ml.Reserved64;
    view->block_bytes = view->is_64k ? kBlockBytes64K : kBlockBytes;
    view->block_w = view->is_64k ? (view->micro_w * 4u) : view->micro_w;
    view->block_h = view->is_64k ? (view->micro_h * 4u) : view->micro_h;
    view->base = is_3d ? (static_cast<uint64_t>(slice) * ml.Slice2DSizeBytes)
                       : (static_cast<uint64_t>(slice) * plane.Reserved10);
    if (ml.Reserved64) {
        const uint32_t tail_mask = (plane.BaseAlignmentBytes >= kBlockBytes64K)
                                       ? (kBlockBytes64K - 1u)
                                       : (kBlockBytes - 1u);
        const uint32_t in_block = static_cast<uint32_t>(ml.OffsetBytes) & tail_mask;
        if (in_block >= kBlockBytes) {
            view->is_64k = true;
            view->block_bytes = kBlockBytes64K;
            view->block_w = view->micro_w * 4u;
            view->block_h = view->micro_h * 4u;
            const uint32_t sub_x = ((in_block >> 13u) & 1u) | (((in_block >> 15u) & 1u) << 1u);
            const uint32_t sub_y = ((in_block >> 12u) & 1u) | (((in_block >> 14u) & 1u) << 1u);
            view->origin_x = sub_x * view->micro_w + AxisOrigin(*eq, 'x', in_block & (kBlockBytes - 1u));
            view->origin_y = sub_y * view->micro_h + AxisOrigin(*eq, 'y', in_block & (kBlockBytes - 1u));
        } else {
            view->origin_x = AxisOrigin(*eq, 'x', in_block);
            view->origin_y = AxisOrigin(*eq, 'y', in_block);
        }
        view->pitch_blocks = 1;
        view->base += ml.OffsetBytes - in_block;
    } else {
        view->pitch_blocks = std::max<uint32_t>(1, ml.PaddedWidthElements / view->block_w);
        view->base += ml.OffsetBytes;
    }
    return true;
}

uint64_t XOffset(const MipView& v, uint32_t x) {
    x += v.origin_x;
    if (!v.is_64k) {
        return static_cast<uint64_t>(x / v.block_w) * kBlockBytes + AxisOffset(*v.eq, 'x', x % v.block_w);
    }
    const uint32_t macro_x = x / v.block_w;
    const uint32_t in_macro_x = x % v.block_w;
    const uint32_t sub_x = in_macro_x / v.micro_w;  // 0..3 -> bits 13, 15
    const uint64_t sub_off = (static_cast<uint64_t>(sub_x & 1u) << 13) |
                             (static_cast<uint64_t>((sub_x >> 1) & 1u) << 15);
    return static_cast<uint64_t>(macro_x) * kBlockBytes64K + sub_off +
           AxisOffset(*v.eq, 'x', in_macro_x % v.micro_w);
}
uint64_t YOffset(const MipView& v, uint32_t y) {
    y += v.origin_y;
    if (!v.is_64k) {
        return static_cast<uint64_t>(y / v.block_h) * v.pitch_blocks * kBlockBytes +
               AxisOffset(*v.eq, 'y', y % v.block_h);
    }
    const uint32_t macro_y = y / v.block_h;
    const uint32_t in_macro_y = y % v.block_h;
    const uint32_t sub_y = in_macro_y / v.micro_h;  // 0..3 -> bits 12, 14
    const uint64_t sub_off = (static_cast<uint64_t>(sub_y & 1u) << 12) |
                             (static_cast<uint64_t>((sub_y >> 1) & 1u) << 14);
    return static_cast<uint64_t>(macro_y) * v.pitch_blocks * kBlockBytes64K + sub_off +
           AxisOffset(*v.eq, 'y', in_macro_y % v.micro_h);
}

template <typename Fn>
bool ForEachElementSample(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                          uint32_t sample, Fn&& fn) {
    MipView v;
    if (!ViewMip(layout, mip, slice, &v)) return false;
    const uint32_t samples = std::max<uint32_t>(1u, v.ml->SampleCount);
    if (sample >= samples) return false;
    const uint32_t w = v.ml->WidthElements;
    const uint32_t h = v.ml->HeightElements;
    if (v.is_3d_z) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint64_t off = v.base + Morton3dElementByteOffset(
                    x, y, v.slice_z, v.bpe, v.block_w, v.block_h, v.block_d,
                    v.pitch_blocks, v.pitch_y_blocks, v.block_bytes);
                fn(x, y, off);
            }
        }
        return true;
    }
    const uint64_t sample_stride = v.ml->Slice2DSizeBytes / samples;
    const uint64_t sample_base = v.base + static_cast<uint64_t>(sample) * sample_stride;
    std::vector<uint64_t> xoff(w);
    for (uint32_t x = 0; x < w; ++x) xoff[x] = XOffset(v, x);
    for (uint32_t y = 0; y < h; ++y) {
        const uint64_t row = sample_base + YOffset(v, y);
        for (uint32_t x = 0; x < w; ++x) fn(x, y, row + xoff[x]);
    }
    return true;
}

template <typename Fn>
bool ForEachElement(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice, Fn&& fn) {
    return ForEachElementSample(layout, mip, slice, 0u, std::forward<Fn>(fn));
}

}  // namespace

uint32_t FormatBytesPerElement(uint32_t dxgi_format) {
    // Bytes per pixel, or per 4x4 block for block-compressed formats.
    switch (dxgi_format) {
        case 1: case 2: case 3: case 4:
            return 16;  // R32G32B32A32
        case 5: case 6: case 7: case 8:
            return 12;  // R32G32B32
        case 9: case 10: case 11: case 12: case 13: case 14:
        case 15: case 16: case 17: case 18:
        case 19: case 20: case 21: case 22:
            return 8;  // R16G16B16A16, R32G32, R32G8X24
        case 48: case 49: case 50: case 51: case 52:
        case 53: case 54: case 55: case 56: case 57: case 58: case 59:
        case 85: case 86: case 115:
            return 2;  // R8G8, R16, B5G6R5, B5G5R5A1, B4G4R4A4
        case 60: case 61: case 62: case 63: case 64: case 65:
        case 103:  // NV12 Plane 0 (Luma Y8)
            return 1;  // R8, A8
        case 104:  // P010 Plane 0 (Luma Y16)
            return 2;
        case 70: case 71: case 72:  // BC1
        case 79: case 80: case 81:  // BC4
            return 8;
        case 73: case 74: case 75:  // BC2
        case 76: case 77: case 78:  // BC3
        case 82: case 83: case 84:  // BC5
        case 94: case 95: case 96:  // BC6H
        case 97: case 98: case 99:  // BC7
            return 16;
        default:
            return 4;  // 32-bit formats (R8G8B8A8, B8G8R8A8, R10G10B10A2, R32, D24S8, ...)
    }
}

bool IsBlockCompressed(uint32_t dxgi_format) {
    return (dxgi_format >= 70 && dxgi_format <= 84) || (dxgi_format >= 94 && dxgi_format <= 99);
}

bool IsAddressable(const XG_RESOURCE_LAYOUT& layout) {
    if (layout.Planes != 1 || layout.MipLevels == 0 || layout.MipLevels > kMaxMips) return false;
    const XG_PLANE_LAYOUT& plane = layout.Plane[0];
    if (!FindEquation(plane.BytesPerElement)) return false;
    const uint32_t mode = plane.MipLayout[0].SwizzleMode;
    return Is4KbAddressableMode(mode) || Is64KbAddressableMode(mode) ||
           (plane.MipLayout[0].Reserved64 && mode == kMode256BS);
}

bool TexelElementOffset(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                        uint32_t x, uint32_t y, uint64_t* offset) {
    return TexelElementOffsetSample(layout, mip, slice, x, y, 0u, offset);
}

bool TexelElementOffsetSample(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                              uint32_t x, uint32_t y, uint32_t sample, uint64_t* offset) {
    MipView v;
    if (!offset || !ViewMip(layout, mip, slice, &v)) return false;
    const uint32_t samples = std::max<uint32_t>(1u, v.ml->SampleCount);
    if (sample >= samples) return false;
    if (v.is_3d_z) {
        *offset = v.base + Morton3dElementByteOffset(
            x, y, v.slice_z, v.bpe, v.block_w, v.block_h, v.block_d,
            v.pitch_blocks, v.pitch_y_blocks, v.block_bytes);
        return true;
    }
    const uint64_t sample_stride = v.ml->Slice2DSizeBytes / samples;
    *offset = v.base + static_cast<uint64_t>(sample) * sample_stride + XOffset(v, x) + YOffset(v, y);
    return true;
}

bool SubresourceRange(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                      uint64_t* offset, uint64_t* size) {
    if (!offset || !size || layout.Planes != 1 || mip >= layout.MipLevels || mip >= kMaxMips) return false;
    const XG_PLANE_LAYOUT& plane = layout.Plane[0];
    const XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[mip];
    if (slice >= std::max<uint32_t>(1, ml.DepthOrArraySize)) return false;
    const bool is_3d = (layout.Dimension == 4u);
    if (is_3d && Is3dZInterleavedMode(ml.SwizzleMode)) {
        uint32_t bw = 1, bh = 1, bd = 1;
        const bool is_64k = Is64KbAddressableMode(ml.SwizzleMode);
        const uint32_t blk_bytes = is_64k ? kBlockBytes64K : kBlockBytes;
        Get3dZBlockDims(plane.BytesPerElement, is_64k, &bw, &bh, &bd);
        const uint64_t px = std::max<uint32_t>(1u, ml.PaddedWidthElements / bw);
        const uint64_t py = std::max<uint32_t>(1u, ml.PaddedHeightElements / bh);
        const uint64_t slab_bytes = px * py * blk_bytes;
        *offset = ml.OffsetBytes + static_cast<uint64_t>(slice / bd) * slab_bytes;
        *size = slab_bytes;
        return true;
    }
    const uint64_t base = is_3d ? (static_cast<uint64_t>(slice) * ml.Slice2DSizeBytes)
                                : (static_cast<uint64_t>(slice) * plane.Reserved10);
    if (ml.Reserved64) {
        const uint64_t tail_blk = (plane.BaseAlignmentBytes >= kBlockBytes64K) ? kBlockBytes64K : kBlockBytes;
        *offset = base + (ml.OffsetBytes & ~(tail_blk - 1));
        *size = tail_blk;
    } else {
        *offset = base + ml.OffsetBytes;
        *size = is_3d ? ml.Slice2DSizeBytes : ml.SizeBytes;
    }
    return true;
}

bool CopyIntoMsaaSample(const XG_RESOURCE_LAYOUT& layout, void* resource_base, uint32_t mip,
                        uint32_t slice, uint32_t sample, const void* src, uint32_t src_row_pitch) {
    if (!resource_base || !src) return false;
    const uint32_t bpe = layout.Plane[0].BytesPerElement;
    auto* dst_bytes = static_cast<uint8_t*>(resource_base);
    const auto* src_bytes = static_cast<const uint8_t*>(src);
    return ForEachElementSample(layout, mip, slice, sample, [&](uint32_t x, uint32_t y, uint64_t off) {
        std::memcpy(dst_bytes + off, src_bytes + static_cast<uint64_t>(y) * src_row_pitch + x * bpe, bpe);
    });
}

bool CopyIntoSubresource(const XG_RESOURCE_LAYOUT& layout, void* resource_base, uint32_t mip,
                         uint32_t slice, const void* src, uint32_t src_row_pitch) {
    return CopyIntoMsaaSample(layout, resource_base, mip, slice, 0u, src, src_row_pitch);
}

bool CopyFromMsaaSample(const XG_RESOURCE_LAYOUT& layout, const void* resource_base, uint32_t mip,
                        uint32_t slice, uint32_t sample, void* dst, uint32_t dst_row_pitch) {
    if (!resource_base || !dst) return false;
    const uint32_t bpe = layout.Plane[0].BytesPerElement;
    const auto* src_bytes = static_cast<const uint8_t*>(resource_base);
    auto* dst_bytes = static_cast<uint8_t*>(dst);
    return ForEachElementSample(layout, mip, slice, sample, [&](uint32_t x, uint32_t y, uint64_t off) {
        std::memcpy(dst_bytes + static_cast<uint64_t>(y) * dst_row_pitch + x * bpe, src_bytes + off, bpe);
    });
}

bool CopyFromSubresource(const XG_RESOURCE_LAYOUT& layout, const void* resource_base, uint32_t mip,
                         uint32_t slice, void* dst, uint32_t dst_row_pitch) {
    return CopyFromMsaaSample(layout, resource_base, mip, slice, 0u, dst, dst_row_pitch);
}

void ComputeSingle2DPlaneLayout(uint32_t width,
                                uint32_t height,
                                uint32_t array_size,
                                uint32_t samples,
                                uint32_t mips,
                                uint32_t bpe,
                                bool bc,
                                uint32_t swizzle_mode,
                                uint32_t usage,
                                XG_PLANE_LAYOUT& plane) {
    const BlockEquation* eq = FindEquation(bpe);
    const bool is_64k = Is64KbAddressableMode(swizzle_mode) && eq != nullptr;
    const bool swizzled = Is4KbAddressableMode(swizzle_mode) && eq != nullptr;
    // RDNA2 64KB_* modes use the 64KB macro-tile mip-tail threshold (block_w / 2 x block_h),
    // while legacy XG_SWIZZLE_MODE_64KB_S (9) preserves micro_w / 2 x micro_h compatibility.
    const bool use_macro_64k_tail = is_64k && (swizzle_mode != kMode64KbS);

    const uint32_t block_bytes = swizzled ? kBlockBytes : (is_64k ? kBlockBytes64K : BlockBytesForMode(swizzle_mode));
    uint32_t block_w = 0, block_h = 0;
    if (swizzled) {
        block_w = 1u << AxisBits(*eq, 'x', 12);
        block_h = 1u << AxisBits(*eq, 'y', 12);
    } else if (is_64k) {
        block_w = (1u << AxisBits(*eq, 'x', 12)) * 4u;
        block_h = (1u << AxisBits(*eq, 'y', 12)) * 4u;
    } else {
        const uint32_t elems_log2 = Log2(std::max<uint32_t>(1, block_bytes / std::max<uint32_t>(1, bpe)));
        block_w = 1u << ((elems_log2 + 1) / 2);
        block_h = 1u << (elems_log2 / 2);
    }

    plane.Usage = usage;
    plane.BaseAlignmentBytes = block_bytes;
    plane.BytesPerElement = bpe;

    uint32_t we[kMaxMips], he[kMaxMips];
    uint32_t first_tail = mips;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max<uint32_t>(1, width >> m);
        const uint32_t mh = std::max<uint32_t>(1, height >> m);
        we[m] = bc ? std::max<uint32_t>(1, (mw + 3) / 4) : mw;
        he[m] = bc ? std::max<uint32_t>(1, (mh + 3) / 4) : mh;
        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[m];
        ml.WidthElements = we[m];
        ml.HeightElements = he[m];
        ml.ReservedWidth = mw;
        ml.ReservedHeight = mh;
        ml.DepthOrArraySize = array_size;
        ml.PaddedDepthOrArraySize = array_size;
        ml.SampleCount = samples;
        ml.Reserved60 = 1;

        const uint32_t micro_w = eq ? (1u << AxisBits(*eq, 'x', 12)) : block_w;
        const uint32_t micro_h = eq ? (1u << AxisBits(*eq, 'y', 12)) : block_h;
        const uint32_t tail_thresh_w = use_macro_64k_tail ? (block_w / 2u) : (micro_w / 2u);
        const uint32_t tail_thresh_h = use_macro_64k_tail ? block_h : micro_h;
        const uint32_t tw = std::max<uint32_t>(1, (1u << Log2(we[0])) >> m);
        const uint32_t th = std::max<uint32_t>(1, (1u << Log2(he[0])) >> m);
        if ((swizzled || is_64k) && samples == 1u && mips > 1 && first_tail == mips &&
            tw <= tail_thresh_w && th <= tail_thresh_h) {
            first_tail = m;
        }
    }

    uint64_t offset = first_tail < mips ? block_bytes : 0;
    for (uint32_t i = first_tail; i-- > 0;) {
        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[i];
        ml.PaddedWidthElements = AlignUp32(std::max<uint32_t>(1, (we[0] + (1u << i) - 1) >> i), block_w);
        ml.PaddedHeightElements = AlignUp32(std::max<uint32_t>(1, (he[0] + (1u << i) - 1) >> i), block_h);
        ml.PitchPixels = ml.PaddedWidthElements * (bc ? 4u : 1u);
        ml.PitchBytes = ml.PaddedWidthElements * bpe;
        ml.AlignmentBytes = block_bytes;
        ml.SwizzleMode = swizzle_mode;
        const uint64_t sample_plane_bytes =
            static_cast<uint64_t>(ml.PaddedWidthElements) * ml.PaddedHeightElements * bpe;
        ml.SizeBytes = sample_plane_bytes * samples;
        ml.Slice2DSizeBytes = ml.SizeBytes;
        ml.OffsetBytes = offset;
        offset += ml.SizeBytes;
    }
    for (uint32_t m = first_tail; m < mips; ++m) {
        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[m];
        if (use_macro_64k_tail) {
            const uint32_t slot = std::min<uint32_t>(m - first_tail, 9u);
            const uint32_t region = kTail64RegionBytes[slot];
            const uint32_t micro_w = 1u << AxisBits(*eq, 'x', 12);
            const uint32_t micro_h = 1u << AxisBits(*eq, 'y', 12);
            if (slot == 0u) {
                ml.PaddedWidthElements = micro_w * 2u;
                ml.PaddedHeightElements = micro_h * 4u;
                ml.AlignmentBytes = 4096u;
                ml.SwizzleMode = swizzle_mode;
            } else if (slot == 1u) {
                ml.PaddedWidthElements = micro_w;
                ml.PaddedHeightElements = micro_h * 2u;
                ml.AlignmentBytes = 4096u;
                ml.SwizzleMode = swizzle_mode;
            } else {
                const uint32_t region_bits = Log2(region);
                ml.PaddedWidthElements = 1u << AxisBits(*eq, 'x', region_bits);
                ml.PaddedHeightElements = 1u << AxisBits(*eq, 'y', region_bits);
                ml.AlignmentBytes = 256u;
                ml.SwizzleMode = kMode256BS;
            }
            ml.PitchPixels = ml.PaddedWidthElements * (bc ? 4u : 1u);
            ml.PitchBytes = ml.PaddedWidthElements * bpe;
            ml.SizeBytes = region;
            ml.Slice2DSizeBytes = region;
            ml.OffsetBytes = kTail64Offset256[slot] * 256u;
            ml.Reserved64 = 1;
        } else {
            const uint32_t slot = std::min<uint32_t>(m - first_tail, 7);
            const uint32_t region = TailRegionBytes(slot);
            const uint32_t region_bits = Log2(region);
            ml.PaddedWidthElements = 1u << AxisBits(*eq, 'x', region_bits);
            ml.PaddedHeightElements = 1u << AxisBits(*eq, 'y', region_bits);
            ml.PitchPixels = ml.PaddedWidthElements * (bc ? 4u : 1u);
            ml.PitchBytes = ml.PaddedWidthElements * bpe;
            ml.AlignmentBytes = 256;
            ml.SwizzleMode = kMode256BS;
            ml.SizeBytes = region;
            ml.Slice2DSizeBytes = region;
            ml.OffsetBytes = kTailOffset256[slot] * 256u;
            ml.Reserved64 = 1;
        }
    }

    plane.Reserved10 = offset;  // one array slice
    plane.Reserved2C = first_tail;
    plane.Reserved30 = mips - first_tail;
    plane.SizeBytes = offset * array_size;
}

void ComputeTexture2DLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT* out_ptr) {
    XG_RESOURCE_LAYOUT& out = *out_ptr;
    std::memset(&out, 0, sizeof(out));

    const uint32_t width = std::max<uint32_t>(1, desc.Width);
    const uint32_t height = std::max<uint32_t>(1, desc.Height);
    const uint32_t array_size = std::max<uint32_t>(1, desc.ArraySize);
    const uint32_t samples = (desc.SampleDesc.Count >= 8u) ? 8u
                           : (desc.SampleDesc.Count >= 4u) ? 4u
                           : (desc.SampleDesc.Count >= 2u) ? 2u
                                                           : 1u;
    const uint32_t max_valid_mips = MaxMipLevelsForDim(std::max(width, height));
    const uint32_t mips = (samples > 1u)
                              ? 1u
                              : std::clamp<uint32_t>(desc.MipLevels == 0 ? max_valid_mips : desc.MipLevels,
                                                     1u, max_valid_mips);

    out.Dimension = 3;  // D3D12_RESOURCE_DIMENSION_TEXTURE2D
    out.MipLevels = mips;

    // Multi-planar YUV (NV12=103, P010=104) & Depth+Stencil (D32_FLOAT_S8X24_UINT=20, D24_UNORM_S8_UINT=45)
    if (desc.Format == 103u || desc.Format == 104u || desc.Format == 20u || desc.Format == 45u) {
        const bool is_yuv = (desc.Format == 103u || desc.Format == 104u);
        const uint32_t p0_bpe = (desc.Format == 103u) ? 1u : ((desc.Format == 104u) ? 2u : 4u);
        const uint32_t p1_bpe = (desc.Format == 103u) ? 2u : ((desc.Format == 104u) ? 4u : 1u);
        const uint32_t p1_w = is_yuv ? std::max<uint32_t>(1u, (width + 1u) / 2u) : width;
        const uint32_t p1_h = is_yuv ? std::max<uint32_t>(1u, (height + 1u) / 2u) : height;
        const uint32_t plane_mips = is_yuv ? 1u : mips;
        out.MipLevels = plane_mips;
        out.Planes = 2;

        ComputeSingle2DPlaneLayout(width, height, array_size, samples, plane_mips,
                                   p0_bpe, false, desc.SwizzleMode, 1u, out.Plane[0]);
        ComputeSingle2DPlaneLayout(p1_w, p1_h, array_size, samples, plane_mips,
                                   p1_bpe, false, desc.SwizzleMode, is_yuv ? 1u : 2u, out.Plane[1]);
        out.Plane[0].BaseOffsetBytes = 0;
        const uint64_t p1_base = AlignUp64(out.Plane[0].SizeBytes, out.Plane[1].BaseAlignmentBytes);
        out.Plane[1].BaseOffsetBytes = p1_base;
        out.BaseAlignmentBytes = std::max(out.Plane[0].BaseAlignmentBytes, out.Plane[1].BaseAlignmentBytes);
        out.SizeBytes = AlignUp64(p1_base + out.Plane[1].SizeBytes, out.BaseAlignmentBytes);
        return;
    }

    const uint32_t bpe = FormatBytesPerElement(desc.Format);
    const bool bc = IsBlockCompressed(desc.Format);
    out.Planes = 1;
    ComputeSingle2DPlaneLayout(width, height, array_size, samples, mips,
                               bpe, bc, desc.SwizzleMode, 1u, out.Plane[0]);
    out.BaseAlignmentBytes = out.Plane[0].BaseAlignmentBytes;
    out.SizeBytes = out.Plane[0].SizeBytes;
}

void ComputeTexture3DLayout(const XG_TEXTURE3D_DESC& desc, XG_RESOURCE_LAYOUT* out_ptr) {
    XG_RESOURCE_LAYOUT& out = *out_ptr;
    std::memset(&out, 0, sizeof(out));

    const uint32_t width = std::max<uint32_t>(1, desc.Width);
    const uint32_t height = std::max<uint32_t>(1, desc.Height);
    const uint32_t depth = std::max<uint32_t>(1, desc.Depth);
    const uint32_t max_valid_mips = MaxMipLevelsForDim(std::max({width, height, depth}));
    const uint32_t mips =
        std::clamp<uint32_t>(desc.MipLevels == 0 ? max_valid_mips : desc.MipLevels, 1u, max_valid_mips);
    const uint32_t bpe = FormatBytesPerElement(desc.Format);
    const bool bc = IsBlockCompressed(desc.Format);
    const BlockEquation* eq = FindEquation(bpe);
    const bool is_64k = Is64KbAddressableMode(desc.SwizzleMode) && eq != nullptr;
    const bool swizzled = Is4KbAddressableMode(desc.SwizzleMode) && eq != nullptr;
    const bool is_3d_z = Is3dZInterleavedMode(desc.SwizzleMode) && eq != nullptr;

    const uint32_t block_bytes = swizzled ? kBlockBytes : (is_64k ? kBlockBytes64K : BlockBytesForMode(desc.SwizzleMode));
    uint32_t block_w = 0, block_h = 0, block_d = 1;
    if (is_3d_z) {
        Get3dZBlockDims(bpe, is_64k, &block_w, &block_h, &block_d);
    } else if (swizzled) {
        block_w = 1u << AxisBits(*eq, 'x', 12);
        block_h = 1u << AxisBits(*eq, 'y', 12);
    } else if (is_64k) {
        block_w = (1u << AxisBits(*eq, 'x', 12)) * 4u;
        block_h = (1u << AxisBits(*eq, 'y', 12)) * 4u;
    } else {
        const uint32_t elems_log2 = Log2(std::max<uint32_t>(1, block_bytes / std::max<uint32_t>(1, bpe)));
        block_w = 1u << ((elems_log2 + 1) / 2);
        block_h = 1u << (elems_log2 / 2);
    }

    out.Dimension = 4;  // D3D12_RESOURCE_DIMENSION_TEXTURE3D
    out.MipLevels = mips;
    out.Planes = 1;
    out.BaseAlignmentBytes = block_bytes;

    XG_PLANE_LAYOUT& plane = out.Plane[0];
    plane.Usage = 1;
    plane.BaseAlignmentBytes = block_bytes;
    plane.BytesPerElement = bpe;

    uint64_t offset = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max<uint32_t>(1, width >> m);
        const uint32_t mh = std::max<uint32_t>(1, height >> m);
        const uint32_t md = std::max<uint32_t>(1, depth >> m);
        const uint32_t we = bc ? std::max<uint32_t>(1, (mw + 3) / 4) : mw;
        const uint32_t he = bc ? std::max<uint32_t>(1, (mh + 3) / 4) : mh;

        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[m];
        ml.WidthElements = we;
        ml.HeightElements = he;
        ml.ReservedWidth = mw;
        ml.ReservedHeight = mh;
        ml.DepthOrArraySize = md;
        ml.PaddedDepthOrArraySize = is_3d_z ? AlignUp32(md, block_d) : md;
        ml.SampleCount = 1;
        ml.Reserved60 = 1;
        ml.PaddedWidthElements = AlignUp32(we, block_w);
        ml.PaddedHeightElements = AlignUp32(he, block_h);
        ml.PitchPixels = ml.PaddedWidthElements * (bc ? 4u : 1u);
        ml.PitchBytes = ml.PaddedWidthElements * bpe;
        ml.AlignmentBytes = block_bytes;
        ml.SwizzleMode = desc.SwizzleMode;
        ml.Slice2DSizeBytes = static_cast<uint64_t>(ml.PaddedWidthElements) * ml.PaddedHeightElements * bpe;
        ml.SizeBytes = ml.Slice2DSizeBytes * ml.PaddedDepthOrArraySize;
        ml.OffsetBytes = offset;
        offset += ml.SizeBytes;
    }

    plane.Reserved10 = offset;
    plane.Reserved2C = mips;
    plane.Reserved30 = 0;
    plane.SizeBytes = offset;
    out.SizeBytes = offset;
}

}  // namespace scarlettx::gpu::xg
