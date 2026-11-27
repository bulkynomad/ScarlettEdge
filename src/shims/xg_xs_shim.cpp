#define SCARLETTX_BUILDING_XG_XS 1
#include "scarlettx/gpu/xg_xs_abi.h"
#include "scarlettx/gpu/xg_layout.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool TraceEnabled() {
    const char* v = std::getenv("SCARLETTX_TRACE");
    return v && v[0] != '\0' && v[0] != '0';
}

uint64_t AlignUp(uint64_t value, uint64_t align) {
    if (align <= 1) return value;
    return (value + align - 1) & ~(align - 1);
}

void ComputeLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& out) {
    scarlettx::gpu::xg::ComputeTexture2DLayout(desc, &out);
}

// Vtable of the texture computer. It is reference counted but is not an
// IUnknown: there is no QueryInterface, so the layout query is slot 2. The
// title calls slot 2 with a layout out-pointer right after creating the object.
struct IXGTexture2DComputerVtbl {
    uint32_t (SCARLETTX_XG_CALL* AddRef)(void* self);
    uint32_t (SCARLETTX_XG_CALL* Release)(void* self);
    int32_t (SCARLETTX_XG_CALL* GetResourceLayout)(void* self, XG_RESOURCE_LAYOUT* layout);
    uint64_t (SCARLETTX_XG_CALL* GetResourceSizeBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetResourceBaseAlignmentBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetMipLevelOffsetBytes)(void* self, uint32_t plane, uint32_t miplevel);
    uint64_t (SCARLETTX_XG_CALL* GetTexelElementOffsetBytes)(
        void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t zOrSlice, uint32_t sample);
    int32_t (SCARLETTX_XG_CALL* CopyIntoSubresource)(
        void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t srcDepthPitch);
    int32_t (SCARLETTX_XG_CALL* CopyFromSubresource)(
        void* self, void* dst, uint32_t dstRowPitch, uint32_t dstDepthPitch, const void* src, uint32_t plane, uint32_t subresource);
};

struct XGTexture2DComputerImpl {
    const IXGTexture2DComputerVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XG_TEXTURE2D_DESC desc{};
    XG_RESOURCE_LAYOUT layout{};
};

uint32_t SCARLETTX_XG_CALL XgAddRef(void* self) {
    return static_cast<XGTexture2DComputerImpl*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_XG_CALL XgRelease(void* self) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    const uint32_t remaining = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete obj;
    }
    return remaining;
}

int32_t SCARLETTX_XG_CALL XgGetResourceLayout(void* self, XG_RESOURCE_LAYOUT* layout) {
    if (!self || !layout) return static_cast<int32_t>(0x80070057u);
    *layout = static_cast<XGTexture2DComputerImpl*>(self)->layout;
    return 0;
}

uint64_t SCARLETTX_XG_CALL XgGetResourceSizeBytes(void* self) {
    return self ? static_cast<XGTexture2DComputerImpl*>(self)->layout.SizeBytes : 0;
}

uint64_t SCARLETTX_XG_CALL XgGetResourceBaseAlignmentBytes(void* self) {
    return self ? static_cast<XGTexture2DComputerImpl*>(self)->layout.BaseAlignmentBytes : 65536;
}

uint64_t SCARLETTX_XG_CALL XgGetMipLevelOffsetBytes(void* self, uint32_t plane, uint32_t miplevel) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    const auto& pl = obj->layout.Plane[plane];
    return pl.BaseOffsetBytes + pl.MipLayout[miplevel].OffsetBytes;
}

uint64_t SCARLETTX_XG_CALL XgGetTexelElementOffsetBytes(
    void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t zOrSlice, uint32_t sample) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    uint64_t offset = 0;
    if (plane == 0 && scarlettx::gpu::xg::TexelElementOffsetSample(
            obj->layout, miplevel, zOrSlice, static_cast<uint32_t>(x), y, sample, &offset)) {
        return offset;
    }
    // Modes without single-plane texel addressing: rows inside the mip, slices one mip chain (or 2D slice for 3D) apart.
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[miplevel];
    const uint32_t samples = std::max<uint32_t>(1u, ml.SampleCount);
    const uint64_t sample_stride = ml.Slice2DSizeBytes / samples;
    const uint64_t slice_stride = (obj->layout.Dimension == 4u) ? ml.Slice2DSizeBytes : pl.Reserved10;
    return pl.BaseOffsetBytes + ml.OffsetBytes + static_cast<uint64_t>(zOrSlice) * slice_stride +
           static_cast<uint64_t>(sample % samples) * sample_stride +
           static_cast<uint64_t>(y) * ml.PitchBytes + x * pl.BytesPerElement;
}

int32_t SCARLETTX_XG_CALL XgCopyIntoSubresource(
    void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t srcDepthPitch) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    const uint32_t mips = std::max<uint32_t>(1, obj->layout.MipLevels);

    // 3D Volume texture (Dimension == 4): subresource == mip, copy all Depth >> mip Z-slices.
    if (obj->layout.Dimension == 4u) {
        if (subresource >= mips) return static_cast<int32_t>(0x80070057u);
        const uint32_t mip = subresource;
        const auto& pl = obj->layout.Plane[plane];
        const auto& ml = pl.MipLayout[mip];
        const uint32_t slice_depth = std::max<uint32_t>(1u, ml.DepthOrArraySize);
        const uint32_t eff_depth_pitch = (srcDepthPitch != 0) ? srcDepthPitch : (srcRowPitch * ml.HeightElements);
        const auto* src_bytes = static_cast<const uint8_t*>(src);
        if (plane == 0 && scarlettx::gpu::xg::IsAddressable(obj->layout)) {
            for (uint32_t z = 0; z < slice_depth; ++z) {
                if (!scarlettx::gpu::xg::CopyIntoSubresource(
                        obj->layout, dst, mip, z,
                        src_bytes + static_cast<uint64_t>(z) * eff_depth_pitch, srcRowPitch)) {
                    return static_cast<int32_t>(0x80070057u);
                }
            }
            return 0;
        }
        auto* dst_mip = static_cast<uint8_t*>(dst) + pl.BaseOffsetBytes + ml.OffsetBytes;
        const uint32_t copy_bytes = std::min(srcRowPitch, ml.PitchBytes);
        for (uint32_t z = 0; z < slice_depth; ++z) {
            auto* dst_slice = dst_mip + static_cast<uint64_t>(z) * ml.Slice2DSizeBytes;
            const auto* src_slice = src_bytes + static_cast<uint64_t>(z) * eff_depth_pitch;
            for (uint32_t row = 0; row < ml.HeightElements; ++row) {
                std::memcpy(dst_slice + static_cast<uint64_t>(row) * ml.PitchBytes,
                            src_slice + static_cast<uint64_t>(row) * srcRowPitch,
                            copy_bytes);
            }
        }
        return 0;
    }

    if (plane == 0 && scarlettx::gpu::xg::IsAddressable(obj->layout)) {
        return scarlettx::gpu::xg::CopyIntoSubresource(
                   obj->layout, dst, subresource % mips, subresource / mips, src, srcRowPitch)
                   ? 0
                   : static_cast<int32_t>(0x80070057u);
    }
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[mip];
    auto* dst_base = static_cast<uint8_t*>(dst) + pl.BaseOffsetBytes + ml.OffsetBytes +
                     static_cast<uint64_t>(slice) * pl.Reserved10;
    const auto* src_base = static_cast<const uint8_t*>(src);
    const uint32_t copy_bytes = std::min(srcRowPitch, ml.PitchBytes);
    for (uint32_t row = 0; row < ml.HeightElements; ++row) {
        std::memcpy(dst_base + static_cast<uint64_t>(row) * ml.PitchBytes,
                    src_base + static_cast<uint64_t>(row) * srcRowPitch,
                    copy_bytes);
    }
    return 0;
}

int32_t SCARLETTX_XG_CALL XgCopyFromSubresource(
    void* self, void* dst, uint32_t dstRowPitch, uint32_t dstDepthPitch, const void* src, uint32_t plane, uint32_t subresource) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    const uint32_t mips = std::max<uint32_t>(1, obj->layout.MipLevels);

    // 3D Volume texture (Dimension == 4): subresource == mip, copy all Depth >> mip Z-slices.
    if (obj->layout.Dimension == 4u) {
        if (subresource >= mips) return static_cast<int32_t>(0x80070057u);
        const uint32_t mip = subresource;
        const auto& pl = obj->layout.Plane[plane];
        const auto& ml = pl.MipLayout[mip];
        const uint32_t slice_depth = std::max<uint32_t>(1u, ml.DepthOrArraySize);
        const uint32_t eff_depth_pitch = (dstDepthPitch != 0) ? dstDepthPitch : (dstRowPitch * ml.HeightElements);
        auto* dst_bytes = static_cast<uint8_t*>(dst);
        if (plane == 0 && scarlettx::gpu::xg::IsAddressable(obj->layout)) {
            for (uint32_t z = 0; z < slice_depth; ++z) {
                if (!scarlettx::gpu::xg::CopyFromSubresource(
                        obj->layout, src, mip, z,
                        dst_bytes + static_cast<uint64_t>(z) * eff_depth_pitch, dstRowPitch)) {
                    return static_cast<int32_t>(0x80070057u);
                }
            }
            return 0;
        }
        const auto* src_mip = static_cast<const uint8_t*>(src) + pl.BaseOffsetBytes + ml.OffsetBytes;
        const uint32_t copy_bytes = std::min(dstRowPitch, ml.PitchBytes);
        for (uint32_t z = 0; z < slice_depth; ++z) {
            const auto* src_slice = src_mip + static_cast<uint64_t>(z) * ml.Slice2DSizeBytes;
            auto* dst_slice = dst_bytes + static_cast<uint64_t>(z) * eff_depth_pitch;
            for (uint32_t row = 0; row < ml.HeightElements; ++row) {
                std::memcpy(dst_slice + static_cast<uint64_t>(row) * dstRowPitch,
                            src_slice + static_cast<uint64_t>(row) * ml.PitchBytes,
                            copy_bytes);
            }
        }
        return 0;
    }

    if (plane == 0 && scarlettx::gpu::xg::IsAddressable(obj->layout)) {
        return scarlettx::gpu::xg::CopyFromSubresource(
                   obj->layout, src, subresource % mips, subresource / mips, dst, dstRowPitch)
                   ? 0
                   : static_cast<int32_t>(0x80070057u);
    }
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[mip];
    const auto* src_base = static_cast<const uint8_t*>(src) + pl.BaseOffsetBytes + ml.OffsetBytes +
                           static_cast<uint64_t>(slice) * pl.Reserved10;
    auto* dst_base = static_cast<uint8_t*>(dst);
    const uint32_t copy_bytes = std::min(dstRowPitch, ml.PitchBytes);
    for (uint32_t row = 0; row < ml.HeightElements; ++row) {
        std::memcpy(dst_base + static_cast<uint64_t>(row) * dstRowPitch,
                    src_base + static_cast<uint64_t>(row) * ml.PitchBytes,
                    copy_bytes);
    }
    return 0;
}

const IXGTexture2DComputerVtbl kVtbl = {
    &XgAddRef,
    &XgRelease,
    &XgGetResourceLayout,
    &XgGetResourceSizeBytes,
    &XgGetResourceBaseAlignmentBytes,
    &XgGetMipLevelOffsetBytes,
    &XgGetTexelElementOffsetBytes,
    &XgCopyIntoSubresource,
    &XgCopyFromSubresource,
};

}  // namespace

extern "C" {

uint32_t SCARLETTX_XG_CALL XGComputeOptimalSwizzleMode(
    uint32_t resourceDimension,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t depthOrArraySize,
    uint32_t sampleCount,
    uint32_t bindFlags,
    uint32_t reserved) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][xg_xs] XGComputeOptimalSwizzleMode(dim=" << resourceDimension
                  << ", fmt=" << format << ", " << width << "x" << height
                  << "x" << depthOrArraySize << ", samples=" << sampleCount
                  << ", bind=0x" << std::hex << bindFlags << std::dec << ")\n";
    }
    (void)reserved;  // 0 at every call seen
    // Matches what the console library answers for these bind flags and MSAA sample counts.
    if (bindFlags & 0x40u) return XG_SWIZZLE_MODE_64KB_Z_X;  // depth stencil (single or MSAA)
    if (bindFlags & 0x20u) return XG_SWIZZLE_MODE_64KB_R_X;  // render target (single or MSAA)
    if (sampleCount > 1u) return XG_SWIZZLE_MODE_64KB_D_X;   // MSAA non-RT/DS surface
    return XG_SWIZZLE_MODE_4KB_S;
}

int32_t SCARLETTX_XG_CALL XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* desc,
    void** computer) {
    if (!desc || !computer) {
        return static_cast<int32_t>(0x80070057u);  // E_INVALIDARG
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][xg_xs] XGCreateTexture2DComputer(" << desc->Width << "x"
                  << desc->Height << ", mips=" << desc->MipLevels << ", fmt=" << desc->Format
                  << ", array=" << desc->ArraySize << ")\n";
    }
    auto* obj = new XGTexture2DComputerImpl();
    obj->lpVtbl = &kVtbl;
    obj->desc = *desc;
    ComputeLayout(obj->desc, obj->layout);
    *computer = obj;
    return 0;  // S_OK
}

int32_t SCARLETTX_XG_CALL XGCreateTexture3DComputer(
    const XG_TEXTURE3D_DESC* desc,
    void** computer) {
    if (!desc || !computer) {
        return static_cast<int32_t>(0x80070057u);  // E_INVALIDARG
    }
    auto* obj = new XGTexture2DComputerImpl();
    obj->lpVtbl = &kVtbl;
    scarlettx::gpu::xg::ComputeTexture3DLayout(*desc, &obj->layout);
    *computer = obj;
    return 0;  // S_OK
}

int32_t SCARLETTX_XG_CALL XGCompressDccSurface(
    const uint8_t* srcRgba8,
    uint32_t width,
    uint32_t height,
    uint32_t srcRowPitch,
    uint8_t* outMetaKeys,
    uint32_t metaCapacity,
    uint8_t* outPayload,
    uint64_t payloadCapacity,
    uint64_t* outPayloadBytes,
    XG_DCC_STATS* outStats) {
    constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
    constexpr int32_t kInsufficientBuf = static_cast<int32_t>(0x8007007Au);
    if (!srcRgba8 || width == 0 || height == 0 || !outPayloadBytes) {
        return kInvalidArg;
    }
    const uint32_t pitch = (srcRowPitch == 0) ? (width * 4u) : srcRowPitch;
    if (pitch < width * 4u) return kInvalidArg;

    const uint32_t tiles_x = (width + 7u) / 8u;
    const uint32_t tiles_y = (height + 7u) / 8u;
    const uint32_t total_tiles = tiles_x * tiles_y;
    const bool size_query_only = (outMetaKeys == nullptr && outPayload == nullptr);
    if (!size_query_only && (!outMetaKeys || metaCapacity < total_tiles)) {
        return kInvalidArg;
    }

    XG_DCC_STATS stats{};
    stats.TileCount = total_tiles;
    uint64_t payload_cursor = 0;

    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            uint8_t tile[64][4];
            uint8_t min_c[4] = {255, 255, 255, 255};
            uint8_t max_c[4] = {0, 0, 0, 0};

            for (uint32_t py = 0; py < 8u; ++py) {
                const uint32_t sy = std::min(ty * 8u + py, height - 1u);
                const uint8_t* row = srcRgba8 + static_cast<size_t>(sy) * pitch;
                for (uint32_t px = 0; px < 8u; ++px) {
                    const uint32_t sx = std::min(tx * 8u + px, width - 1u);
                    const uint8_t* p = row + static_cast<size_t>(sx) * 4u;
                    const uint32_t idx = py * 8u + px;
                    for (uint32_t c = 0; c < 4u; ++c) {
                        tile[idx][c] = p[c];
                        min_c[c] = std::min(min_c[c], p[c]);
                        max_c[c] = std::max(max_c[c], p[c]);
                    }
                }
            }

            const uint32_t tile_index = ty * tiles_x + tx;
            uint8_t key = XG_DCC_KEY_UNCOMPRESSED;
            uint64_t tile_bytes = 0;

            const bool uniform = (min_c[0] == max_c[0]) && (min_c[1] == max_c[1]) &&
                                 (min_c[2] == max_c[2]) && (min_c[3] == max_c[3]);
            if (uniform) {
                if (min_c[0] == 0 && min_c[1] == 0 && min_c[2] == 0 && min_c[3] == 0) {
                    key = XG_DCC_KEY_CLEAR_0000;
                    ++stats.FastClearTiles;
                } else if (min_c[0] == 0 && min_c[1] == 0 && min_c[2] == 0 && min_c[3] == 255) {
                    key = XG_DCC_KEY_CLEAR_0001;
                    ++stats.FastClearTiles;
                } else if (min_c[0] == 255 && min_c[1] == 255 && min_c[2] == 255 && min_c[3] == 0) {
                    key = XG_DCC_KEY_CLEAR_1110;
                    ++stats.FastClearTiles;
                } else if (min_c[0] == 255 && min_c[1] == 255 && min_c[2] == 255 && min_c[3] == 255) {
                    key = XG_DCC_KEY_CLEAR_1111;
                    ++stats.FastClearTiles;
                } else {
                    key = XG_DCC_KEY_SOLID_COLOR;
                    tile_bytes = 4u;
                    ++stats.SolidColorTiles;
                }
            } else if ((max_c[0] - min_c[0] <= 15u) && (max_c[1] - min_c[1] <= 15u) &&
                       (max_c[2] - min_c[2] <= 15u) && (max_c[3] - min_c[3] <= 15u)) {
                key = XG_DCC_KEY_DELTA_4BIT;
                tile_bytes = 132u;  // 4B base RGBA8 + 128B 4-bit unsigned delta stream
                ++stats.DeltaTiles;
            } else {
                key = XG_DCC_KEY_UNCOMPRESSED;
                tile_bytes = 256u;
                ++stats.UncompressedTiles;
            }

            if (!size_query_only) {
                outMetaKeys[tile_index] = key;
                if (tile_bytes > 0) {
                    if (!outPayload || payload_cursor + tile_bytes > payloadCapacity) {
                        *outPayloadBytes = payload_cursor + tile_bytes;
                        return kInsufficientBuf;
                    }
                    uint8_t* dst = outPayload + payload_cursor;
                    if (key == XG_DCC_KEY_SOLID_COLOR) {
                        std::memcpy(dst, min_c, 4u);
                    } else if (key == XG_DCC_KEY_DELTA_4BIT) {
                        std::memcpy(dst, min_c, 4u);
                        for (uint32_t idx = 0; idx < 64u; ++idx) {
                            const uint8_t d0 = static_cast<uint8_t>(tile[idx][0] - min_c[0]);
                            const uint8_t d1 = static_cast<uint8_t>(tile[idx][1] - min_c[1]);
                            const uint8_t d2 = static_cast<uint8_t>(tile[idx][2] - min_c[2]);
                            const uint8_t d3 = static_cast<uint8_t>(tile[idx][3] - min_c[3]);
                            dst[4u + idx * 2u + 0u] = static_cast<uint8_t>(d0 | (d1 << 4u));
                            dst[4u + idx * 2u + 1u] = static_cast<uint8_t>(d2 | (d3 << 4u));
                        }
                    } else {
                        std::memcpy(dst, tile, 256u);
                    }
                }
            }
            payload_cursor += tile_bytes;
        }
    }

    stats.CompressedPayloadBytes = payload_cursor;
    *outPayloadBytes = payload_cursor;
    if (outStats) *outStats = stats;
    return 0;
}

int32_t SCARLETTX_XG_CALL XGDecompressDccSurface(
    const uint8_t* metaKeys,
    uint32_t metaSize,
    const uint8_t* payload,
    uint64_t payloadSize,
    uint8_t* dstRgba8,
    uint32_t width,
    uint32_t height,
    uint32_t dstRowPitch) {
    constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
    if (!metaKeys || !dstRgba8 || width == 0 || height == 0) return kInvalidArg;
    const uint32_t pitch = (dstRowPitch == 0) ? (width * 4u) : dstRowPitch;
    if (pitch < width * 4u) return kInvalidArg;

    const uint32_t tiles_x = (width + 7u) / 8u;
    const uint32_t tiles_y = (height + 7u) / 8u;
    const uint32_t total_tiles = tiles_x * tiles_y;
    if (metaSize < total_tiles) return kInvalidArg;

    uint64_t payload_cursor = 0;
    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            const uint8_t key = metaKeys[ty * tiles_x + tx];
            uint8_t tile[64][4];

            if (key == XG_DCC_KEY_CLEAR_0000) {
                std::memset(tile, 0, sizeof(tile));
            } else if (key == XG_DCC_KEY_CLEAR_0001) {
                for (uint32_t i = 0; i < 64u; ++i) {
                    tile[i][0] = 0;
                    tile[i][1] = 0;
                    tile[i][2] = 0;
                    tile[i][3] = 255;
                }
            } else if (key == XG_DCC_KEY_CLEAR_1110) {
                for (uint32_t i = 0; i < 64u; ++i) {
                    tile[i][0] = 255;
                    tile[i][1] = 255;
                    tile[i][2] = 255;
                    tile[i][3] = 0;
                }
            } else if (key == XG_DCC_KEY_CLEAR_1111) {
                std::memset(tile, 255, sizeof(tile));
            } else if (key == XG_DCC_KEY_SOLID_COLOR) {
                if (!payload || payload_cursor + 4u > payloadSize) return kInvalidArg;
                const uint8_t* c = payload + payload_cursor;
                payload_cursor += 4u;
                for (uint32_t i = 0; i < 64u; ++i) {
                    std::memcpy(tile[i], c, 4u);
                }
            } else if (key == XG_DCC_KEY_SECTOR_1_64B) {
                if (!payload || payload_cursor + 64u > payloadSize) return kInvalidArg;
                const uint8_t* src = payload + payload_cursor;
                payload_cursor += 64u;
                // 4B base + 60B? Wait: 64 pixels * 4 channels * 2 bits = 512 bits = 64B (or solid/2-bit with 8-byte header in 64B sector):
                // Layout in 64B sector: base RGBA8 (bytes 0..3), 60 pixels 2-bit deltas (bytes 4..63) + last 4 pixels clamped to base
                const uint8_t base[4] = {src[0], src[1], src[2], src[3]};
                for (uint32_t i = 0; i < 60u; ++i) {
                    const uint8_t pk = src[4u + i];
                    tile[i][0] = static_cast<uint8_t>(base[0] + (pk & 0x03u));
                    tile[i][1] = static_cast<uint8_t>(base[1] + ((pk >> 2u) & 0x03u));
                    tile[i][2] = static_cast<uint8_t>(base[2] + ((pk >> 4u) & 0x03u));
                    tile[i][3] = static_cast<uint8_t>(base[3] + ((pk >> 6u) & 0x03u));
                }
                for (uint32_t i = 60u; i < 64u; ++i) std::memcpy(tile[i], base, 4u);
            } else if (key == XG_DCC_KEY_DELTA_4BIT || key == XG_DCC_KEY_SECTOR_3_64B) {
                const uint64_t step = (key == XG_DCC_KEY_SECTOR_3_64B) ? 192u : 132u;
                if (!payload || payload_cursor + step > payloadSize) return kInvalidArg;
                const uint8_t* src = payload + payload_cursor;
                payload_cursor += step;
                const uint8_t base[4] = {src[0], src[1], src[2], src[3]};
                for (uint32_t i = 0; i < 64u; ++i) {
                    const uint8_t b0 = src[4u + i * 2u + 0u];
                    const uint8_t b1 = src[4u + i * 2u + 1u];
                    tile[i][0] = static_cast<uint8_t>(base[0] + (b0 & 0x0Fu));
                    tile[i][1] = static_cast<uint8_t>(base[1] + ((b0 >> 4u) & 0x0Fu));
                    tile[i][2] = static_cast<uint8_t>(base[2] + (b1 & 0x0Fu));
                    tile[i][3] = static_cast<uint8_t>(base[3] + ((b1 >> 4u) & 0x0Fu));
                }
            } else if (key == XG_DCC_KEY_UNCOMPRESSED) {
                if (!payload || payload_cursor + 256u > payloadSize) return kInvalidArg;
                std::memcpy(tile, payload + payload_cursor, 256u);
                payload_cursor += 256u;
            } else {
                return kInvalidArg;
            }

            for (uint32_t py = 0; py < 8u; ++py) {
                const uint32_t dy = ty * 8u + py;
                if (dy >= height) break;
                uint8_t* row = dstRgba8 + static_cast<size_t>(dy) * pitch;
                for (uint32_t px = 0; px < 8u; ++px) {
                    const uint32_t dx = tx * 8u + px;
                    if (dx >= width) break;
                    std::memcpy(row + static_cast<size_t>(dx) * 4u, tile[py * 8u + px], 4u);
                }
            }
        }
    }
    return 0;
}

int32_t SCARLETTX_XG_CALL XGCompressDccInPlace(
    uint8_t* inoutSurfaceRgba8,
    uint32_t width,
    uint32_t height,
    uint32_t rowPitch,
    uint8_t* outMetaKeys,
    uint32_t metaCapacity,
    uint8_t* optionalTiledSectors256B,
    uint64_t tiledCapacityBytes,
    XG_DCC_STATS* outStats) {
    constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
    constexpr int32_t kInsufficientBuf = static_cast<int32_t>(0x8007007Au);
    if (!inoutSurfaceRgba8 || width == 0 || height == 0 || !outMetaKeys) return kInvalidArg;
    const uint32_t pitch = (rowPitch == 0) ? (width * 4u) : rowPitch;
    if (pitch < width * 4u) return kInvalidArg;

    const uint32_t tiles_x = (width + 7u) / 8u;
    const uint32_t tiles_y = (height + 7u) / 8u;
    const uint32_t total_tiles = tiles_x * tiles_y;
    if (metaCapacity < total_tiles) return kInvalidArg;
    if (optionalTiledSectors256B && tiledCapacityBytes < static_cast<uint64_t>(total_tiles) * 256ull) {
        return kInsufficientBuf;
    }

    XG_DCC_STATS stats{};
    stats.TileCount = total_tiles;
    uint64_t active_sector_bytes = 0;

    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            uint8_t tile[64][4];
            uint8_t min_c[4] = {255, 255, 255, 255};
            uint8_t max_c[4] = {0, 0, 0, 0};

            for (uint32_t py = 0; py < 8u; ++py) {
                const uint32_t sy = std::min(ty * 8u + py, height - 1u);
                const uint8_t* row = inoutSurfaceRgba8 + static_cast<size_t>(sy) * pitch;
                for (uint32_t px = 0; px < 8u; ++px) {
                    const uint32_t sx = std::min(tx * 8u + px, width - 1u);
                    const uint8_t* p = row + static_cast<size_t>(sx) * 4u;
                    const uint32_t idx = py * 8u + px;
                    for (uint32_t c = 0; c < 4u; ++c) {
                        tile[idx][c] = p[c];
                        min_c[c] = std::min(min_c[c], p[c]);
                        max_c[c] = std::max(max_c[c], p[c]);
                    }
                }
            }

            const uint32_t tile_idx = ty * tiles_x + tx;
            uint8_t sector_block[256] = {};
            uint8_t key = XG_DCC_KEY_UNCOMPRESSED;
            uint64_t sector_bytes = 256u;

            const bool uniform = (min_c[0] == max_c[0]) && (min_c[1] == max_c[1]) &&
                                 (min_c[2] == max_c[2]) && (min_c[3] == max_c[3]);
            const uint8_t span_r = static_cast<uint8_t>(max_c[0] - min_c[0]);
            const uint8_t span_g = static_cast<uint8_t>(max_c[1] - min_c[1]);
            const uint8_t span_b = static_cast<uint8_t>(max_c[2] - min_c[2]);
            const uint8_t span_a = static_cast<uint8_t>(max_c[3] - min_c[3]);

            if (uniform) {
                if (min_c[0] == 0 && min_c[1] == 0 && min_c[2] == 0 && min_c[3] == 0) {
                    key = XG_DCC_KEY_CLEAR_0000;
                    sector_bytes = 0u;
                    ++stats.FastClearTiles;
                } else if (min_c[0] == 0 && min_c[1] == 0 && min_c[2] == 0 && min_c[3] == 255) {
                    key = XG_DCC_KEY_CLEAR_0001;
                    sector_bytes = 0u;
                    ++stats.FastClearTiles;
                } else if (min_c[0] == 255 && min_c[1] == 255 && min_c[2] == 255 && min_c[3] == 0) {
                    key = XG_DCC_KEY_CLEAR_1110;
                    sector_bytes = 0u;
                    ++stats.FastClearTiles;
                } else if (min_c[0] == 255 && min_c[1] == 255 && min_c[2] == 255 && min_c[3] == 255) {
                    key = XG_DCC_KEY_CLEAR_1111;
                    sector_bytes = 0u;
                    ++stats.FastClearTiles;
                } else {
                    key = XG_DCC_KEY_SOLID_COLOR;  // 1x64B sector (0..63)
                    sector_bytes = 64u;
                    std::memcpy(sector_block, min_c, 4u);
                    ++stats.SolidColorTiles;
                }
            } else if (span_r <= 7u && span_g <= 7u && span_b <= 7u && span_a <= 3u) {
                // 2x64B sectors (128B): 4B base + 64 * 16-bit (3+3+3+2 bits) deltas = 132B? Pack 64 * 11 bits -> 88B + 4B = 92B <= 128B!
                key = XG_DCC_KEY_SECTOR_2_64B;
                sector_bytes = 128u;
                std::memcpy(sector_block, min_c, 4u);
                uint32_t bit_pos = 32u;
                for (uint32_t i = 0; i < 64u; ++i) {
                    const uint32_t packed =
                        static_cast<uint32_t>(tile[i][0] - min_c[0]) |
                        (static_cast<uint32_t>(tile[i][1] - min_c[1]) << 3u) |
                        (static_cast<uint32_t>(tile[i][2] - min_c[2]) << 6u) |
                        (static_cast<uint32_t>(tile[i][3] - min_c[3]) << 9u);
                    for (uint32_t b = 0; b < 11u; ++b, ++bit_pos) {
                        if ((packed >> b) & 1u) {
                            sector_block[bit_pos >> 3u] |= static_cast<uint8_t>(1u << (bit_pos & 7u));
                        }
                    }
                }
                ++stats.DeltaTiles;
            } else if (span_r <= 15u && span_g <= 15u && span_b <= 15u && span_a <= 15u) {
                // 3x64B sectors (192B): 4B base + 128B 4-bit deltas = 132B <= 192B!
                key = XG_DCC_KEY_SECTOR_3_64B;
                sector_bytes = 192u;
                std::memcpy(sector_block, min_c, 4u);
                for (uint32_t i = 0; i < 64u; ++i) {
                    const uint8_t d0 = static_cast<uint8_t>(tile[i][0] - min_c[0]);
                    const uint8_t d1 = static_cast<uint8_t>(tile[i][1] - min_c[1]);
                    const uint8_t d2 = static_cast<uint8_t>(tile[i][2] - min_c[2]);
                    const uint8_t d3 = static_cast<uint8_t>(tile[i][3] - min_c[3]);
                    sector_block[4u + i * 2u + 0u] = static_cast<uint8_t>(d0 | (d1 << 4u));
                    sector_block[4u + i * 2u + 1u] = static_cast<uint8_t>(d2 | (d3 << 4u));
                }
                ++stats.DeltaTiles;
            } else {
                key = XG_DCC_KEY_UNCOMPRESSED;
                sector_bytes = 256u;
                std::memcpy(sector_block, tile, 256u);
                ++stats.UncompressedTiles;
            }

            outMetaKeys[tile_idx] = key;
            active_sector_bytes += sector_bytes;
            if (optionalTiledSectors256B) {
                std::memcpy(optionalTiledSectors256B + static_cast<size_t>(tile_idx) * 256u, sector_block, 256u);
            } else if ((width % 8u) == 0 && (height % 8u) == 0) {
                // Full 8x8 tile in-place writeback into the 8x8 surface region (4 x 64B sectors)
                for (uint32_t py = 0; py < 8u; ++py) {
                    uint8_t* row = inoutSurfaceRgba8 + static_cast<size_t>(ty * 8u + py) * pitch +
                                   static_cast<size_t>(tx * 8u) * 4u;
                    std::memcpy(row, sector_block + py * 32u, 32u);
                }
            }
        }
    }

    stats.CompressedPayloadBytes = active_sector_bytes;
    if (outStats) *outStats = stats;
    return 0;
}

int32_t SCARLETTX_XG_CALL XGDecompressDccInPlace(
    const uint8_t* metaKeys,
    uint32_t metaSize,
    const uint8_t* tiledSectors256B,
    uint64_t tiledCapacityBytes,
    uint8_t* inoutSurfaceRgba8,
    uint32_t width,
    uint32_t height,
    uint32_t rowPitch) {
    constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
    if (!metaKeys || !inoutSurfaceRgba8 || width == 0 || height == 0) return kInvalidArg;
    const uint32_t pitch = (rowPitch == 0) ? (width * 4u) : rowPitch;
    if (pitch < width * 4u) return kInvalidArg;

    const uint32_t tiles_x = (width + 7u) / 8u;
    const uint32_t tiles_y = (height + 7u) / 8u;
    const uint32_t total_tiles = tiles_x * tiles_y;
    if (metaSize < total_tiles) return kInvalidArg;
    if (tiledSectors256B && tiledCapacityBytes < static_cast<uint64_t>(total_tiles) * 256ull) return kInvalidArg;

    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            const uint32_t tile_idx = ty * tiles_x + tx;
            const uint8_t key = metaKeys[tile_idx];
            uint8_t sector_block[256] = {};
            if (tiledSectors256B) {
                std::memcpy(sector_block, tiledSectors256B + static_cast<size_t>(tile_idx) * 256u, 256u);
            } else if ((width % 8u) == 0 && (height % 8u) == 0) {
                for (uint32_t py = 0; py < 8u; ++py) {
                    const uint8_t* row = inoutSurfaceRgba8 + static_cast<size_t>(ty * 8u + py) * pitch +
                                         static_cast<size_t>(tx * 8u) * 4u;
                    std::memcpy(sector_block + py * 32u, row, 32u);
                }
            }

            uint8_t tile[64][4];
            if (key == XG_DCC_KEY_CLEAR_0000) {
                std::memset(tile, 0, sizeof(tile));
            } else if (key == XG_DCC_KEY_CLEAR_0001) {
                for (uint32_t i = 0; i < 64u; ++i) {
                    tile[i][0] = 0; tile[i][1] = 0; tile[i][2] = 0; tile[i][3] = 255;
                }
            } else if (key == XG_DCC_KEY_CLEAR_1110) {
                for (uint32_t i = 0; i < 64u; ++i) {
                    tile[i][0] = 255; tile[i][1] = 255; tile[i][2] = 255; tile[i][3] = 0;
                }
            } else if (key == XG_DCC_KEY_CLEAR_1111) {
                std::memset(tile, 255, sizeof(tile));
            } else if (key == XG_DCC_KEY_SOLID_COLOR) {
                for (uint32_t i = 0; i < 64u; ++i) {
                    std::memcpy(tile[i], sector_block, 4u);
                }
            } else if (key == XG_DCC_KEY_SECTOR_2_64B) {
                const uint8_t base[4] = {sector_block[0], sector_block[1], sector_block[2], sector_block[3]};
                uint32_t bit_pos = 32u;
                for (uint32_t i = 0; i < 64u; ++i) {
                    uint32_t packed = 0;
                    for (uint32_t b = 0; b < 11u; ++b, ++bit_pos) {
                        if ((sector_block[bit_pos >> 3u] >> (bit_pos & 7u)) & 1u) {
                            packed |= (1u << b);
                        }
                    }
                    tile[i][0] = static_cast<uint8_t>(base[0] + (packed & 0x07u));
                    tile[i][1] = static_cast<uint8_t>(base[1] + ((packed >> 3u) & 0x07u));
                    tile[i][2] = static_cast<uint8_t>(base[2] + ((packed >> 6u) & 0x07u));
                    tile[i][3] = static_cast<uint8_t>(base[3] + ((packed >> 9u) & 0x03u));
                }
            } else if (key == XG_DCC_KEY_SECTOR_3_64B || key == XG_DCC_KEY_DELTA_4BIT) {
                const uint8_t base[4] = {sector_block[0], sector_block[1], sector_block[2], sector_block[3]};
                for (uint32_t i = 0; i < 64u; ++i) {
                    const uint8_t b0 = sector_block[4u + i * 2u + 0u];
                    const uint8_t b1 = sector_block[4u + i * 2u + 1u];
                    tile[i][0] = static_cast<uint8_t>(base[0] + (b0 & 0x0Fu));
                    tile[i][1] = static_cast<uint8_t>(base[1] + ((b0 >> 4u) & 0x0Fu));
                    tile[i][2] = static_cast<uint8_t>(base[2] + (b1 & 0x0Fu));
                    tile[i][3] = static_cast<uint8_t>(base[3] + ((b1 >> 4u) & 0x0Fu));
                }
            } else if (key == XG_DCC_KEY_UNCOMPRESSED) {
                std::memcpy(tile, sector_block, 256u);
            } else {
                return kInvalidArg;
            }

            for (uint32_t py = 0; py < 8u; ++py) {
                const uint32_t dy = ty * 8u + py;
                if (dy >= height) break;
                uint8_t* row = inoutSurfaceRgba8 + static_cast<size_t>(dy) * pitch;
                for (uint32_t px = 0; px < 8u; ++px) {
                    const uint32_t dx = tx * 8u + px;
                    if (dx >= width) break;
                    std::memcpy(row + static_cast<size_t>(dx) * 4u, tile[py * 8u + px], 4u);
                }
            }
        }
    }
    return 0;
}

int32_t SCARLETTX_XG_CALL XGBuildHtileMetadata(
    const float* srcDepth,
    uint32_t width,
    uint32_t height,
    uint32_t srcDepthStrideFloats,
    uint32_t* outHtileWords,
    uint32_t htileCapacityWords,
    float* outExpandedTiles,
    uint32_t expandedCapacityFloats,
    uint32_t* outExpandedFloatsWritten) {
    constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
    constexpr int32_t kInsufficientBuf = static_cast<int32_t>(0x8007007Au);
    if (!srcDepth || width == 0 || height == 0 || !outHtileWords) return kInvalidArg;
    const uint32_t stride = (srcDepthStrideFloats == 0) ? width : srcDepthStrideFloats;
    if (stride < width) return kInvalidArg;

    const uint32_t tiles_x = (width + 7u) / 8u;
    const uint32_t tiles_y = (height + 7u) / 8u;
    const uint32_t total_tiles = tiles_x * tiles_y;
    if (htileCapacityWords < total_tiles) return kInvalidArg;

    auto quantize14 = [](float z) -> uint32_t {
        const float clamped = std::clamp(z, 0.0f, 1.0f);
        return static_cast<uint32_t>(clamped * 16383.0f + 0.5f) & 0x3FFFu;
    };

    uint32_t expanded_floats = 0;
    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            float tile[64];
            float min_z = 1.0f;
            float max_z = 0.0f;
            bool first = true;

            for (uint32_t py = 0; py < 8u; ++py) {
                const uint32_t sy = std::min(ty * 8u + py, height - 1u);
                const float* row = srcDepth + static_cast<size_t>(sy) * stride;
                for (uint32_t px = 0; px < 8u; ++px) {
                    const uint32_t sx = std::min(tx * 8u + px, width - 1u);
                    const float z = std::clamp(row[sx], 0.0f, 1.0f);
                    tile[py * 8u + px] = z;
                    if (ty * 8u + py < height && tx * 8u + px < width) {
                        if (first) {
                            min_z = z;
                            max_z = z;
                            first = false;
                        } else {
                            min_z = std::min(min_z, z);
                            max_z = std::max(max_z, z);
                        }
                    }
                }
            }

            const uint32_t zmin14 = quantize14(min_z);
            const uint32_t zmax14 = quantize14(max_z);
            uint32_t zmask = XG_HTILE_ZMASK_EXPANDED;
            if (std::fabs(max_z - min_z) <= (0.5f / 16383.0f)) {
                if (zmax14 == 0u || zmax14 == 0x3FFFu) {
                    zmask = XG_HTILE_ZMASK_CLEAR_ZMAX;
                } else {
                    zmask = XG_HTILE_ZMASK_CONST_ZMIN;
                }
            } else if (outExpandedTiles != nullptr) {
                if (expanded_floats + 64u > expandedCapacityFloats) {
                    if (outExpandedFloatsWritten) *outExpandedFloatsWritten = expanded_floats + 64u;
                    return kInsufficientBuf;
                }
                std::memcpy(outExpandedTiles + expanded_floats, tile, 64u * sizeof(float));
                expanded_floats += 64u;
            } else {
                expanded_floats += 64u;
            }

            outHtileWords[ty * tiles_x + tx] = (zmax14 << 18u) | (zmin14 << 4u) | zmask;
        }
    }

    if (outExpandedFloatsWritten) *outExpandedFloatsWritten = expanded_floats;
    return 0;
}

int32_t SCARLETTX_XG_CALL XGDecompressHtileDepth(
    const uint32_t* htileWords,
    uint32_t htileWordCount,
    const float* expandedTiles,
    uint32_t expandedFloatCount,
    float* dstDepth,
    uint32_t width,
    uint32_t height,
    uint32_t dstDepthStrideFloats) {
    constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
    if (!htileWords || !dstDepth || width == 0 || height == 0) return kInvalidArg;
    const uint32_t stride = (dstDepthStrideFloats == 0) ? width : dstDepthStrideFloats;
    if (stride < width) return kInvalidArg;

    const uint32_t tiles_x = (width + 7u) / 8u;
    const uint32_t tiles_y = (height + 7u) / 8u;
    const uint32_t total_tiles = tiles_x * tiles_y;
    if (htileWordCount < total_tiles) return kInvalidArg;

    auto unquantize14 = [](uint32_t q) -> float {
        return static_cast<float>(q & 0x3FFFu) / 16383.0f;
    };

    uint32_t exp_cursor = 0;
    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            const uint32_t word = htileWords[ty * tiles_x + tx];
            const uint32_t zmax14 = (word >> 18u) & 0x3FFFu;
            const uint32_t zmin14 = (word >> 4u) & 0x3FFFu;
            const uint32_t zmask = word & 0xFu;

            float tile[64];
            if (zmask == XG_HTILE_ZMASK_CLEAR_ZMAX) {
                const float v = unquantize14(zmax14);
                std::fill_n(tile, 64, v);
            } else if (zmask == XG_HTILE_ZMASK_CONST_ZMIN) {
                const float v = unquantize14(zmin14);
                std::fill_n(tile, 64, v);
            } else if (zmask == XG_HTILE_ZMASK_EXPANDED) {
                if (expandedTiles != nullptr) {
                    if (exp_cursor + 64u > expandedFloatCount) return kInvalidArg;
                    std::memcpy(tile, expandedTiles + exp_cursor, 64u * sizeof(float));
                    exp_cursor += 64u;
                } else {
                    const float z0 = unquantize14(zmin14);
                    const float z1 = unquantize14(zmax14);
                    for (uint32_t i = 0; i < 64u; ++i) {
                        const float t = static_cast<float>(i) / 63.0f;
                        tile[i] = z0 + (z1 - z0) * t;
                    }
                }
            } else {
                return kInvalidArg;
            }

            for (uint32_t py = 0; py < 8u; ++py) {
                const uint32_t dy = ty * 8u + py;
                if (dy >= height) break;
                float* row = dstDepth + static_cast<size_t>(dy) * stride;
                for (uint32_t px = 0; px < 8u; ++px) {
                    const uint32_t dx = tx * 8u + px;
                    if (dx >= width) break;
                    row[dx] = tile[py * 8u + px];
                }
            }
        }
    }
    return 0;
}

}  // extern "C"
