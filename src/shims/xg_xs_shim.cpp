#define SCARLETTX_BUILDING_XG_XS 1
#include "scarlettx/gpu/xg_xs_abi.h"
#include "scarlettx/gpu/xg_layout.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
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
    return obj->layout.Plane[plane].MipLayout[miplevel].OffsetBytes;
}

uint64_t SCARLETTX_XG_CALL XgGetTexelElementOffsetBytes(
    void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t zOrSlice, uint32_t /*sample*/) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    uint64_t offset = 0;
    if (scarlettx::gpu::xg::TexelElementOffset(obj->layout, miplevel, zOrSlice, static_cast<uint32_t>(x), y, &offset)) {
        return offset;
    }
    // Modes without texel addressing: rows inside the mip, slices one mip chain apart.
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[miplevel];
    return ml.OffsetBytes + static_cast<uint64_t>(zOrSlice) * pl.Reserved10 +
           static_cast<uint64_t>(y) * ml.PitchBytes + x * pl.BytesPerElement;
}

int32_t SCARLETTX_XG_CALL XgCopyIntoSubresource(
    void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t /*srcDepthPitch*/) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    if (scarlettx::gpu::xg::IsAddressable(obj->layout)) {
        const uint32_t levels = std::max<uint32_t>(1, obj->layout.MipLevels);
        return scarlettx::gpu::xg::CopyIntoSubresource(obj->layout, dst, subresource % levels, subresource / levels, src, srcRowPitch)
                   ? 0
                   : static_cast<int32_t>(0x80070057u);
    }
    const uint32_t mips = std::max<uint32_t>(1, obj->layout.MipLevels);
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& ml = obj->layout.Plane[plane].MipLayout[mip];
    auto* dst_base = static_cast<uint8_t*>(dst) + ml.OffsetBytes +
                     static_cast<uint64_t>(slice) * obj->layout.Plane[plane].Reserved10;
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
    void* self, void* dst, uint32_t dstRowPitch, uint32_t /*dstDepthPitch*/, const void* src, uint32_t plane, uint32_t subresource) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    if (scarlettx::gpu::xg::IsAddressable(obj->layout)) {
        const uint32_t levels = std::max<uint32_t>(1, obj->layout.MipLevels);
        return scarlettx::gpu::xg::CopyFromSubresource(obj->layout, src, subresource % levels, subresource / levels, dst, dstRowPitch)
                   ? 0
                   : static_cast<int32_t>(0x80070057u);
    }
    const uint32_t mips = std::max<uint32_t>(1, obj->layout.MipLevels);
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& ml = obj->layout.Plane[plane].MipLayout[mip];
    const auto* src_base = static_cast<const uint8_t*>(src) + ml.OffsetBytes +
                           static_cast<uint64_t>(slice) * obj->layout.Plane[plane].Reserved10;
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
    // Matches what the console library answers for these bind flags.
    if (bindFlags & 0x40u) return XG_SWIZZLE_MODE_64KB_Z_X;  // depth stencil
    if (bindFlags & 0x20u) return XG_SWIZZLE_MODE_64KB_R_X;  // render target
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

}  // extern "C"
