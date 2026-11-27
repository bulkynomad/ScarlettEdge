// ScarlettX — Xbox texture memory layout (swizzled texel addressing).
//
// Titles can write cooked texel data directly into a texture's memory, so the
// layout reported by xg_xs and the addressing used to read that memory back
// must agree with the console's. Texel addressing is implemented for
// XG_SWIZZLE_MODE_4KB_S, the mode chosen for sampled textures.
#pragma once

#include <cstdint>

#include "scarlettx/gpu/xg_xs_abi.h"

namespace scarlettx::gpu::xg {

uint32_t FormatBytesPerElement(uint32_t dxgi_format);
bool IsBlockCompressed(uint32_t dxgi_format);

// Fills `out` for a 2D texture (or 2D array). Mode 4KB_S follows the console
// layout (block padding, mip tail, smallest mip first). Other modes get a
// block-padded layout without texel addressing.
void ComputeTexture2DLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT* out);
void ComputeTexture3DLayout(const XG_TEXTURE3D_DESC& desc, XG_RESOURCE_LAYOUT* out);

// True when texel addressing is available for this layout.
bool IsAddressable(const XG_RESOURCE_LAYOUT& layout);

// Byte offset of element (x, y) of `mip` in array slice `slice`. Coordinates
// are in elements (4x4 blocks for block-compressed formats).
bool TexelElementOffset(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                        uint32_t x, uint32_t y, uint64_t* offset);

// Byte offset of MSAA sample `sample` (0..SampleCount-1) at element (x, y) of
// `mip` in array slice `slice`. Sample planes are laid out contiguously within
// each slice so every (slice, sample, x, y) maps to a disjoint swizzled offset.
bool TexelElementOffsetSample(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                              uint32_t x, uint32_t y, uint32_t sample, uint64_t* offset);

// Memory range holding a subresource. Mips packed into the mip tail share
// one block, so their ranges are the same.
bool SubresourceRange(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                      uint64_t* offset, uint64_t* size);

// Linear rows -> resource memory (sample 0, or explicit MSAA sample plane).
bool CopyIntoSubresource(const XG_RESOURCE_LAYOUT& layout, void* resource_base, uint32_t mip,
                         uint32_t slice, const void* src, uint32_t src_row_pitch);
bool CopyIntoMsaaSample(const XG_RESOURCE_LAYOUT& layout, void* resource_base, uint32_t mip,
                        uint32_t slice, uint32_t sample, const void* src, uint32_t src_row_pitch);
// Resource memory -> linear rows (sample 0, or explicit MSAA sample plane).
bool CopyFromSubresource(const XG_RESOURCE_LAYOUT& layout, const void* resource_base, uint32_t mip,
                         uint32_t slice, void* dst, uint32_t dst_row_pitch);
bool CopyFromMsaaSample(const XG_RESOURCE_LAYOUT& layout, const void* resource_base, uint32_t mip,
                        uint32_t slice, uint32_t sample, void* dst, uint32_t dst_row_pitch);

}  // namespace scarlettx::gpu::xg
