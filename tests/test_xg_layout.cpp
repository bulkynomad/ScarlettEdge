// Xbox texture memory layout: struct ABI, swizzled addressing and the xg_xs
// texture computer. Expected numbers are the console library's answers for
// the same inputs.
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/gpu/xg_xs_abi.h"
#include "sx_check.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace xg = scarlettx::gpu::xg;

namespace {

XG_TEXTURE2D_DESC Desc(uint32_t w, uint32_t h, uint32_t mips, uint32_t format, uint32_t array = 1) {
    XG_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = mips;
    d.ArraySize = array;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = 0x8;
    d.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    return d;
}

uint64_t Offset(const XG_RESOURCE_LAYOUT& l, uint32_t mip, uint32_t slice, uint32_t x, uint32_t y) {
    uint64_t off = ~0ull;
    SX_REQUIRE(xg::TexelElementOffset(l, mip, slice, x, y, &off));
    return off;
}

constexpr uint32_t kRgba8 = 28;
constexpr uint32_t kBc1 = 71;
constexpr uint32_t kBc3 = 77;
constexpr uint32_t kR8 = 61;

void TestSingleMip() {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(Desc(700, 284, 1, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0xC6000);
    SX_REQUIRE(l->BaseAlignmentBytes == 0x1000);
    SX_REQUIRE(l->MipLevels == 1 && l->Planes == 1);
    const XG_PLANE_LAYOUT& p = l->Plane[0];
    SX_REQUIRE(p.Usage == 1 && p.SizeBytes == 0xC6000 && p.Reserved10 == 0xC6000);
    SX_REQUIRE(p.BytesPerElement == 4 && p.Reserved2C == 1 && p.Reserved30 == 0);
    const XG_MIPLEVEL_LAYOUT& m = p.MipLayout[0];
    SX_REQUIRE(m.SizeBytes == 0xC6000 && m.OffsetBytes == 0 && m.Slice2DSizeBytes == 0xC6000);
    SX_REQUIRE(m.PitchPixels == 704 && m.PitchBytes == 2816 && m.AlignmentBytes == 0x1000);
    SX_REQUIRE(m.PaddedWidthElements == 704 && m.PaddedHeightElements == 288);
    SX_REQUIRE(m.WidthElements == 700 && m.HeightElements == 284);
    SX_REQUIRE(m.ReservedWidth == 700 && m.ReservedHeight == 284);
    SX_REQUIRE(m.SwizzleMode == XG_SWIZZLE_MODE_4KB_S && m.Reserved60 == 1 && m.Reserved64 == 0);

    // 32x32 texel blocks of 4 KB, stored row by row (22 blocks per row).
    SX_REQUIRE(Offset(*l, 0, 0, 0, 0) == 0);
    SX_REQUIRE(Offset(*l, 0, 0, 1, 0) == 0x4);
    SX_REQUIRE(Offset(*l, 0, 0, 2, 0) == 0x8);
    SX_REQUIRE(Offset(*l, 0, 0, 4, 0) == 0x80);
    SX_REQUIRE(Offset(*l, 0, 0, 8, 0) == 0x200);
    SX_REQUIRE(Offset(*l, 0, 0, 16, 0) == 0x800);
    SX_REQUIRE(Offset(*l, 0, 0, 32, 0) == 0x1000);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 1) == 0x10);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 4) == 0x40);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 8) == 0x100);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 16) == 0x400);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 32) == 0x16000);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 256) == 0xB0000);

    // A single mip is never packed into a tail, however small.
    xg::ComputeTexture2DLayout(Desc(16, 16, 1, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x1000 && l->Plane[0].Reserved2C == 1 && l->Plane[0].Reserved30 == 0);
}

void TestMipChain() {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kRgba8), l.get());
    const XG_PLANE_LAYOUT& p = l->Plane[0];
    SX_REQUIRE(l->SizeBytes == 0x56000);
    SX_REQUIRE(p.Reserved2C == 4 && p.Reserved30 == 5);
    // Smallest mip first: the tail block, then mips 3, 2, 1, 0.
    const uint64_t offsets[9] = {0x16000, 0x6000, 0x2000, 0x1000, 0x800, 0x600, 0x500, 0x400, 0x300};
    const uint64_t sizes[9] = {0x40000, 0x10000, 0x4000, 0x1000, 0x800, 0x200, 0x100, 0x100, 0x100};
    for (uint32_t m = 0; m < 9; ++m) {
        SX_REQUIRE(p.MipLayout[m].OffsetBytes == offsets[m]);
        SX_REQUIRE(p.MipLayout[m].SizeBytes == sizes[m]);
        SX_REQUIRE(p.MipLayout[m].Reserved64 == (m >= 4 ? 1u : 0u));
        SX_REQUIRE(p.MipLayout[m].SwizzleMode == (m >= 4 ? 1u : 5u));
    }
    SX_REQUIRE(p.MipLayout[4].PaddedWidthElements == 16 && p.MipLayout[4].PaddedHeightElements == 32);
    SX_REQUIRE(p.MipLayout[5].PaddedWidthElements == 8 && p.MipLayout[5].PaddedHeightElements == 16);
    SX_REQUIRE(p.MipLayout[6].PaddedWidthElements == 8 && p.MipLayout[6].PaddedHeightElements == 8);
    SX_REQUIRE(Offset(*l, 4, 0, 0, 0) == 0x800);
    SX_REQUIRE(Offset(*l, 4, 0, 15, 15) == 0xBFC);
    SX_REQUIRE(Offset(*l, 5, 0, 0, 1) == 0x610);
    SX_REQUIRE(Offset(*l, 5, 0, 7, 7) == 0x6FC);
    SX_REQUIRE(Offset(*l, 6, 0, 3, 3) == 0x53C);

    // Array slices repeat the whole chain.
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kRgba8, 2), l.get());
    SX_REQUIRE(l->SizeBytes == 0xAC000 && l->Plane[0].Reserved10 == 0x56000);
    SX_REQUIRE(Offset(*l, 0, 1, 0, 0) == 0x6C000);
    SX_REQUIRE(Offset(*l, 8, 1, 0, 0) == 0x56300);

    // Other element sizes.
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kBc1), l.get());
    SX_REQUIRE(l->SizeBytes == 0xB000 && l->Plane[0].Reserved2C == 2 && l->Plane[0].Reserved30 == 7);
    SX_REQUIRE(l->Plane[0].MipLayout[0].OffsetBytes == 0x3000 && l->Plane[0].MipLayout[0].PitchPixels == 256);
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kBc3), l.get());
    SX_REQUIRE(l->SizeBytes == 0x16000 && l->Plane[0].Reserved2C == 3);
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kR8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x16000 && l->Plane[0].Reserved2C == 3);

    // Odd sizes: the tail test uses the power-of-two size, padding rounds up.
    xg::ComputeTexture2DLayout(Desc(33, 17, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x4000 && l->Plane[0].Reserved2C == 2 && l->Plane[0].Reserved30 == 1);
    xg::ComputeTexture2DLayout(Desc(16, 32, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x1000 && l->Plane[0].Reserved2C == 0 && l->Plane[0].Reserved30 == 3);
    xg::ComputeTexture2DLayout(Desc(32, 16, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x2000 && l->Plane[0].Reserved2C == 1 && l->Plane[0].Reserved30 == 2);
}

void RoundTrip(const XG_TEXTURE2D_DESC& d) {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(d, l.get());
    SX_REQUIRE(xg::IsAddressable(*l));
    const uint32_t bpe = l->Plane[0].BytesPerElement;
    std::vector<uint8_t> memory(static_cast<size_t>(l->SizeBytes), 0);
    std::vector<uint32_t> owner(memory.size(), 0);  // which element wrote each byte
    uint32_t id = 0;
    std::vector<std::vector<uint8_t>> sources;
    for (uint32_t slice = 0; slice < d.ArraySize; ++slice) {
        for (uint32_t mip = 0; mip < l->MipLevels; ++mip) {
            const XG_MIPLEVEL_LAYOUT& m = l->Plane[0].MipLayout[mip];
            const uint32_t pitch = m.WidthElements * bpe + 12;  // caller-chosen row pitch
            std::vector<uint8_t> src(static_cast<size_t>(pitch) * m.HeightElements);
            for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 7 + mip * 31 + slice * 101 + 1);
            SX_REQUIRE(xg::CopyIntoSubresource(*l, memory.data(), mip, slice, src.data(), pitch));
            // No two elements of any subresource may share memory.
            for (uint32_t y = 0; y < m.HeightElements; ++y) {
                for (uint32_t x = 0; x < m.WidthElements; ++x) {
                    const uint64_t off = Offset(*l, mip, slice, x, y);
                    SX_REQUIRE(off + bpe <= memory.size());
                    ++id;
                    for (uint32_t b = 0; b < bpe; ++b) {
                        SX_REQUIRE(owner[off + b] == 0);
                        owner[off + b] = id;
                    }
                }
            }
            sources.push_back(std::move(src));
        }
    }
    size_t index = 0;
    for (uint32_t slice = 0; slice < d.ArraySize; ++slice) {
        for (uint32_t mip = 0; mip < l->MipLevels; ++mip, ++index) {
            const XG_MIPLEVEL_LAYOUT& m = l->Plane[0].MipLayout[mip];
            const uint32_t pitch = m.WidthElements * bpe + 12;
            std::vector<uint8_t> out(sources[index].size(), 0);
            SX_REQUIRE(xg::CopyFromSubresource(*l, memory.data(), mip, slice, out.data(), pitch));
            for (uint32_t y = 0; y < m.HeightElements; ++y) {
                SX_REQUIRE(std::memcmp(out.data() + static_cast<size_t>(y) * pitch,
                                       sources[index].data() + static_cast<size_t>(y) * pitch,
                                       m.WidthElements * bpe) == 0);
            }
        }
    }
}

void TestRoundTrip() {
    RoundTrip(Desc(700, 284, 1, kRgba8));
    RoundTrip(Desc(100, 60, 7, kRgba8));
    RoundTrip(Desc(256, 256, 9, kBc1));
    RoundTrip(Desc(64, 64, 7, kBc3, 3));
    RoundTrip(Desc(40, 24, 6, kR8, 2));
    RoundTrip(Desc(128, 64, 8, 49));  // R8G8
    RoundTrip(Desc(48, 80, 7, 10));   // R16G16B16A16_FLOAT

    // 64KB_S (mode 9), 64KB_Z_X depth (mode 24), 64KB_R_X (mode 25) & 6-slice CubeMap addressing
    auto d64s = Desc(256, 256, 1, kRgba8);
    d64s.SwizzleMode = 9;
    RoundTrip(d64s);
    auto d64zx = Desc(256, 256, 1, 40);  // D32_FLOAT depth
    d64zx.SwizzleMode = 24;
    RoundTrip(d64zx);
    auto d64rx = Desc(512, 256, 1, kBc3);
    d64rx.SwizzleMode = 25;
    RoundTrip(d64rx);
    auto cube = Desc(128, 128, 1, kRgba8, 6);  // 6-face CubeMap array
    cube.SwizzleMode = 9;
    RoundTrip(cube);

    // 64KB_S & 64KB_R_X (mode 27) full mip-chains with RDNA2 64 KB packed mip-tails!
    auto d64s_mipped = Desc(512, 512, 10, kRgba8);
    d64s_mipped.SwizzleMode = 9;
    RoundTrip(d64s_mipped);
    {
        XG_RESOURCE_LAYOUT ml64{};
        xg::ComputeTexture2DLayout(d64s_mipped, &ml64);
        // Mips 5..9 (16x16 down to 1x1) fit inside half a 32x32 micro-tile and share one 64 KB tail block!
        SX_REQUIRE(ml64.Plane[0].Reserved2C == 5 && ml64.Plane[0].Reserved30 == 5);
        SX_REQUIRE(ml64.Plane[0].MipLayout[5].Reserved64 == 1);
    }
    auto d64rx_mipped = Desc(256, 256, 9, kRgba8);
    d64rx_mipped.SwizzleMode = 27;  // XG_SWIZZLE_MODE_64KB_R_X
    RoundTrip(d64rx_mipped);

    // 3D Volume Texture (64x64x8, 4 mips, 4KB_S & 64KB_S) zero-overlap round-trip via XGCreateTexture3DComputer
    {
        XG_TEXTURE3D_DESC d3{};
        d3.Width = 64;
        d3.Height = 64;
        d3.Depth = 8;
        d3.MipLevels = 4;
        d3.Format = kRgba8;
        d3.BindFlags = 0x8;
        d3.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
        void* comp3d = nullptr;
        SX_REQUIRE(XGCreateTexture3DComputer(&d3, &comp3d) == 0 && comp3d != nullptr);
        XG_RESOURCE_LAYOUT l3{};
        using LayoutFn3 = int32_t (SCARLETTX_XG_CALL*)(void*, XG_RESOURCE_LAYOUT*);
        using RefFn3 = uint32_t (SCARLETTX_XG_CALL*)(void*);
        SX_REQUIRE((*reinterpret_cast<void***>(comp3d))[2] != nullptr);
        SX_REQUIRE(reinterpret_cast<LayoutFn3>((*reinterpret_cast<void***>(comp3d))[2])(comp3d, &l3) == 0);
        SX_REQUIRE(l3.Dimension == 4u && l3.MipLevels == 4u);
        SX_REQUIRE(l3.Plane[0].MipLayout[0].DepthOrArraySize == 8u);
        SX_REQUIRE(l3.Plane[0].MipLayout[1].DepthOrArraySize == 4u);
        SX_REQUIRE(l3.Plane[0].MipLayout[2].DepthOrArraySize == 2u);
        SX_REQUIRE(l3.Plane[0].MipLayout[3].DepthOrArraySize == 1u);

        std::vector<uint8_t> vol_mem(static_cast<size_t>(l3.SizeBytes), 0);
        std::vector<uint8_t> slice_in(64 * 64 * 4), slice_out(64 * 64 * 4);
        for (uint32_t mip = 0; mip < 4u; ++mip) {
            const uint32_t w = 64u >> mip, h = 64u >> mip, dep = 8u >> mip;
            for (uint32_t z = 0; z < dep; ++z) {
                for (size_t i = 0; i < w * h * 4u; ++i) {
                    slice_in[i] = static_cast<uint8_t>((i * 11u + z * 53u + mip * 97u) & 0xFFu);
                }
                SX_REQUIRE(xg::CopyIntoSubresource(l3, vol_mem.data(), mip, z, slice_in.data(), w * 4u));
            }
        }
        for (uint32_t mip = 0; mip < 4u; ++mip) {
            const uint32_t w = 64u >> mip, h = 64u >> mip, dep = 8u >> mip;
            for (uint32_t z = 0; z < dep; ++z) {
                SX_REQUIRE(xg::CopyFromSubresource(l3, vol_mem.data(), mip, z, slice_out.data(), w * 4u));
                for (size_t i = 0; i < w * h * 4u; ++i) {
                    const uint8_t exp = static_cast<uint8_t>((i * 11u + z * 53u + mip * 97u) & 0xFFu);
                    SX_REQUIRE(slice_out[i] == exp);
                }
            }
        }
        SX_REQUIRE(reinterpret_cast<RefFn3>((*reinterpret_cast<void***>(comp3d))[1])(comp3d) == 0);
    }
}

// The texture computer as a title uses it: through its vtable.
template <typename Fn>
Fn Slot(void* object, int index) {
    void** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Fn>(vtable[index]);
}

void TestComputer() {
    // Sampled textures, render targets and depth buffers.
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kRgba8, 700, 284, 1, 1, 0x8, 0) == 5);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kBc3, 1024, 1024, 1, 1, 0x0, 0) == 5);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kRgba8, 1920, 1080, 1, 1, 0x28, 0) == 27);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, 40, 1920, 1080, 1, 1, 0x40, 0) == 24);

    // The description exactly as the title lays it out in memory.
    uint32_t raw[16] = {};
    raw[0] = 256;   // +0x00 width
    raw[1] = 256;   // +0x04 height
    raw[2] = 9;     // +0x08 mip levels
    raw[3] = 1;     // +0x0C array size
    raw[4] = kRgba8;  // +0x10 format
    raw[5] = 1;     // +0x14 sample count
    raw[8] = 0x8;   // +0x20 bind flags
    raw[11] = 5;    // +0x2C swizzle mode
    void* computer = nullptr;
    SX_REQUIRE(XGCreateTexture2DComputer(reinterpret_cast<const XG_TEXTURE2D_DESC*>(raw), &computer) == 0);
    SX_REQUIRE(computer != nullptr);

    using RefFn = uint32_t (SCARLETTX_XG_CALL*)(void*);
    using LayoutFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*);
    using SizeFn = uint64_t (SCARLETTX_XG_CALL*)(void*);
    using MipFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t);
    using TexelFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t);

    // Slot 2 is the layout query; the object must survive it.
    std::vector<uint8_t> out(0x2000, 0xCD);
    SX_REQUIRE(Slot<LayoutFn>(computer, 2)(computer, out.data()) == 0);
    auto rd32 = [&](size_t off) { uint32_t v; std::memcpy(&v, out.data() + off, 4); return v; };
    auto rd64 = [&](size_t off) { uint64_t v; std::memcpy(&v, out.data() + off, 8); return v; };
    SX_REQUIRE(rd64(0x00) == 0x56000);        // resource size
    SX_REQUIRE(rd64(0x08) == 0x1000);         // base alignment
    SX_REQUIRE(rd32(0x10) == 9 && rd32(0x14) == 1);
    SX_REQUIRE(rd32(0x40) == 4);              // bytes per element
    SX_REQUIRE(rd32(0x44) == 4);              // first mip of the tail
    SX_REQUIRE(rd64(0x50) == 0x40000);        // mip 0 size
    SX_REQUIRE(rd64(0x58) == 0x16000);        // mip 0 offset
    SX_REQUIRE(rd32(0x50 + 0x38) == 256 && rd32(0x50 + 0x3C) == 256);
    SX_REQUIRE(rd32(0x50 + 0x64) == 0);
    SX_REQUIRE(rd64(0x50 + 0x70 + 0x08) == 0x6000);        // mip 1 offset
    SX_REQUIRE(rd32(0x50 + 4 * 0x70 + 0x64) == 1);         // mip 4 is in the tail
    SX_REQUIRE(rd64(0x50 + 4 * 0x70 + 0x08) == 0x800);

    SX_REQUIRE(Slot<SizeFn>(computer, 3)(computer) == 0x56000);
    SX_REQUIRE(Slot<SizeFn>(computer, 4)(computer) == 0x1000);
    SX_REQUIRE(Slot<MipFn>(computer, 5)(computer, 0, 1) == 0x6000);
    SX_REQUIRE(Slot<TexelFn>(computer, 6)(computer, 0, 0, 1, 1, 0, 0) == 0x16014);
    SX_REQUIRE(Slot<TexelFn>(computer, 6)(computer, 0, 4, 15, 15, 0, 0) == 0xBFC);

    SX_REQUIRE(Slot<RefFn>(computer, 0)(computer) == 2);  // AddRef
    SX_REQUIRE(Slot<RefFn>(computer, 1)(computer) == 1);  // Release
    SX_REQUIRE(Slot<RefFn>(computer, 1)(computer) == 0);

    // A mode without texel addressing: every (mip, slice) still gets its own
    // memory inside the resource.
    raw[0] = 64;
    raw[1] = 64;
    raw[2] = 2;
    raw[3] = 2;
    raw[11] = 0;
    void* linear = nullptr;
    SX_REQUIRE(XGCreateTexture2DComputer(reinterpret_cast<const XG_TEXTURE2D_DESC*>(raw), &linear) == 0);
    const uint64_t linear_size = Slot<SizeFn>(linear, 3)(linear);
    struct Range {
        uint64_t first, last;
    } ranges[4];
    int n = 0;
    for (uint32_t slice = 0; slice < 2; ++slice) {
        for (uint32_t mip = 0; mip < 2; ++mip) {
            const uint32_t dim = 64u >> mip;
            ranges[n].first = Slot<TexelFn>(linear, 6)(linear, 0, mip, 0, 0, slice, 0);
            ranges[n].last = Slot<TexelFn>(linear, 6)(linear, 0, mip, dim - 1, dim - 1, slice, 0) + 4;
            SX_REQUIRE(ranges[n].first < ranges[n].last && ranges[n].last <= linear_size);
            ++n;
        }
    }
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            SX_REQUIRE(ranges[a].last <= ranges[b].first || ranges[b].last <= ranges[a].first);
        }
    }
    SX_REQUIRE(Slot<RefFn>(linear, 1)(linear) == 0);
}

void TestMsaaAddressing() {
    // 2x, 4x, and 8x MSAA optimal swizzle modes and disjoint sample-plane swizzled addressing.
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kRgba8, 1920, 1080, 1, 4, 0x20, 0) == XG_SWIZZLE_MODE_64KB_R_X);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, 40, 1920, 1080, 1, 8, 0x40, 0) == XG_SWIZZLE_MODE_64KB_Z_X);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kRgba8, 1280, 720, 1, 2, 0x08, 0) == XG_SWIZZLE_MODE_64KB_D_X);

    const uint32_t sample_counts[3] = {2u, 4u, 8u};
    for (uint32_t samples : sample_counts) {
        XG_TEXTURE2D_DESC d = Desc(130, 70, 1, kRgba8, 2);
        d.SampleDesc.Count = samples;
        d.BindFlags = 0x20;
        d.SwizzleMode = XGComputeOptimalSwizzleMode(3, d.Format, d.Width, d.Height, d.ArraySize, samples, d.BindFlags, 0);

        void* comp = nullptr;
        SX_REQUIRE(XGCreateTexture2DComputer(&d, &comp) == 0 && comp != nullptr);

        XG_RESOURCE_LAYOUT layout{};
        using LayoutFn = int32_t (SCARLETTX_XG_CALL*)(void*, XG_RESOURCE_LAYOUT*);
        using TexelFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t);
        using RefFn = uint32_t (SCARLETTX_XG_CALL*)(void*);
        SX_REQUIRE(Slot<LayoutFn>(comp, 2)(comp, &layout) == 0);
        SX_REQUIRE(layout.MipLevels == 1u);
        SX_REQUIRE(layout.Plane[0].MipLayout[0].SampleCount == samples);
        SX_REQUIRE(xg::IsAddressable(layout));

        std::vector<uint8_t> mem(static_cast<size_t>(layout.SizeBytes), 0);
        std::vector<uint32_t> owner(mem.size(), 0);
        uint32_t tag = 0;

        for (uint32_t slice = 0; slice < d.ArraySize; ++slice) {
            for (uint32_t s = 0; s < samples; ++s) {
                std::vector<uint8_t> src(d.Width * d.Height * 4u);
                for (size_t i = 0; i < src.size(); ++i) {
                    src[i] = static_cast<uint8_t>((i * 13u + s * 41u + slice * 89u + samples) & 0xFFu);
                }
                SX_REQUIRE(xg::CopyIntoMsaaSample(layout, mem.data(), 0, slice, s, src.data(), d.Width * 4u));

                for (uint32_t y = 0; y < d.Height; ++y) {
                    for (uint32_t x = 0; x < d.Width; ++x) {
                        uint64_t off = ~0ull;
                        SX_REQUIRE(xg::TexelElementOffsetSample(layout, 0, slice, x, y, s, &off));
                        const uint64_t vtbl_off = Slot<TexelFn>(comp, 6)(comp, 0, 0, x, y, slice, s);
                        SX_REQUIRE(off == vtbl_off);
                        SX_REQUIRE(off + 4u <= mem.size());
                        ++tag;
                        for (uint32_t b = 0; b < 4u; ++b) {
                            SX_REQUIRE(owner[off + b] == 0u);
                            owner[off + b] = tag;
                        }
                    }
                }
            }
        }

        // Verify out-of-range sample index is rejected cleanly.
        uint64_t bad_off = 0;
        SX_REQUIRE(!xg::TexelElementOffsetSample(layout, 0, 0, 0, 0, samples, &bad_off));

        // Read back every MSAA sample plane and verify bit-exact data integrity.
        for (uint32_t slice = 0; slice < d.ArraySize; ++slice) {
            for (uint32_t s = 0; s < samples; ++s) {
                std::vector<uint8_t> dst(d.Width * d.Height * 4u, 0);
                SX_REQUIRE(xg::CopyFromMsaaSample(layout, mem.data(), 0, slice, s, dst.data(), d.Width * 4u));
                for (size_t i = 0; i < dst.size(); ++i) {
                    const uint8_t exp = static_cast<uint8_t>((i * 13u + s * 41u + slice * 89u + samples) & 0xFFu);
                    SX_REQUIRE(dst[i] == exp);
                }
            }
        }

        SX_REQUIRE(Slot<RefFn>(comp, 1)(comp) == 0);
    }
}

void TestDccAndHtileCodecs() {
    // Non-multiple-of-8 surface (29x19 -> 4x3 = 12 tiles) exercising all 6 RDNA2 DCC sector key codes:
    //   tile (0,0): 0x00 fast-clear (0,0,0,0)
    //   tile (1,0): 0x20 fast-clear (0,0,0,255)
    //   tile (2,0): 0x80 fast-clear (255,255,255,255)
    //   tile (3,0): 0x40 solid arbitrary color (42, 128, 200, 255) on partial right-edge tile
    //   row 1 tiles: 0xC0 4-bit delta blocks (max-min <= 15 per channel)
    //   row 2 tiles: 0xFF high-frequency uncompressed blocks (max-min > 15)
    const uint32_t w = 29u, h = 19u;
    const uint32_t src_pitch = w * 4u + 16u;
    std::vector<uint8_t> src(static_cast<size_t>(src_pitch) * h, 0xAA);

    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = src.data() + static_cast<size_t>(y) * src_pitch;
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t tx = x / 8u;
            const uint32_t ty = y / 8u;
            uint8_t rgba[4] = {0, 0, 0, 0};
            if (ty == 0u) {
                if (tx == 0u) {
                    rgba[0] = 0; rgba[1] = 0; rgba[2] = 0; rgba[3] = 0;
                } else if (tx == 1u) {
                    rgba[0] = 0; rgba[1] = 0; rgba[2] = 0; rgba[3] = 255;
                } else if (tx == 2u) {
                    rgba[0] = 255; rgba[1] = 255; rgba[2] = 255; rgba[3] = 255;
                } else {
                    rgba[0] = 42; rgba[1] = 128; rgba[2] = 200; rgba[3] = 255;
                }
            } else if (ty == 1u) {
                const uint8_t dx = static_cast<uint8_t>(x & 7u);
                const uint8_t dy = static_cast<uint8_t>(y & 7u);
                rgba[0] = static_cast<uint8_t>(60u + dx + dy);       // delta <= 14
                rgba[1] = static_cast<uint8_t>(100u + dx * 2u);      // delta <= 14
                rgba[2] = static_cast<uint8_t>(180u + dy * 2u);      // delta <= 14
                rgba[3] = static_cast<uint8_t>(240u + (dx ^ dy));    // delta <= 7
            } else {
                rgba[0] = static_cast<uint8_t>((x * 73u + y * 19u) & 0xFFu);
                rgba[1] = static_cast<uint8_t>((x * 151u + y * 43u) & 0xFFu);
                rgba[2] = static_cast<uint8_t>((x * 29u + y * 211u) & 0xFFu);
                rgba[3] = static_cast<uint8_t>((x * 97u + y * 131u) & 0xFFu);
            }
            std::memcpy(row + static_cast<size_t>(x) * 4u, rgba, 4u);
        }
    }

    uint64_t needed_payload = 0;
    XG_DCC_STATS stats{};
    SX_REQUIRE(XGCompressDccSurface(src.data(), w, h, src_pitch, nullptr, 0, nullptr, 0, &needed_payload, &stats) == 0);
    SX_REQUIRE(stats.TileCount == 12u);
    SX_REQUIRE(stats.FastClearTiles == 3u);
    SX_REQUIRE(stats.SolidColorTiles == 1u);
    SX_REQUIRE(stats.DeltaTiles == 4u);
    SX_REQUIRE(stats.UncompressedTiles == 4u);
    SX_REQUIRE(needed_payload == 4u + 4u * 132u + 4u * 256u);
    SX_REQUIRE(needed_payload < 12u * 256u);

    std::vector<uint8_t> meta(stats.TileCount, 0);
    std::vector<uint8_t> payload(static_cast<size_t>(needed_payload), 0);
    uint64_t written_payload = 0;
    SX_REQUIRE(XGCompressDccSurface(src.data(), w, h, src_pitch, meta.data(), stats.TileCount,
                                    payload.data(), payload.size(), &written_payload, nullptr) == 0);
    SX_REQUIRE(written_payload == needed_payload);
    SX_REQUIRE(meta[0] == XG_DCC_KEY_CLEAR_0000);
    SX_REQUIRE(meta[1] == XG_DCC_KEY_CLEAR_0001);
    SX_REQUIRE(meta[2] == XG_DCC_KEY_CLEAR_1111);
    SX_REQUIRE(meta[3] == XG_DCC_KEY_SOLID_COLOR);
    SX_REQUIRE(meta[4] == XG_DCC_KEY_DELTA_4BIT);
    SX_REQUIRE(meta[8] == XG_DCC_KEY_UNCOMPRESSED);

    const uint32_t dst_pitch = w * 4u + 8u;
    std::vector<uint8_t> dst(static_cast<size_t>(dst_pitch) * h, 0x55);
    SX_REQUIRE(XGDecompressDccSurface(meta.data(), static_cast<uint32_t>(meta.size()),
                                      payload.data(), payload.size(), dst.data(), w, h, dst_pitch) == 0);
    for (uint32_t y = 0; y < h; ++y) {
        SX_REQUIRE(std::memcmp(src.data() + static_cast<size_t>(y) * src_pitch,
                               dst.data() + static_cast<size_t>(y) * dst_pitch,
                               w * 4u) == 0);
        // Verify row padding outside width was untouched!
        for (uint32_t pad = w * 4u; pad < dst_pitch; ++pad) {
            SX_REQUIRE(dst[static_cast<size_t>(y) * dst_pitch + pad] == 0x55);
        }
    }

    // Metadata-only fast-clear render target decompression (zero payload bytes).
    std::vector<uint8_t> fc_meta(4, XG_DCC_KEY_CLEAR_0001);
    std::vector<uint8_t> fc_dst(16u * 16u * 4u, 0);
    SX_REQUIRE(XGDecompressDccSurface(fc_meta.data(), 4u, nullptr, 0, fc_dst.data(), 16u, 16u, 64u) == 0);
    for (size_t p = 0; p < 16u * 16u; ++p) {
        SX_REQUIRE(fc_dst[p * 4u + 0u] == 0 && fc_dst[p * 4u + 1u] == 0 &&
                   fc_dst[p * 4u + 2u] == 0 && fc_dst[p * 4u + 3u] == 255);
    }

    // RDNA2 HTILE 8x8 hierarchical depth metadata: fast-clear 1.0f, constant 0.25f, and expanded depth plane.
    const uint32_t dw = 25u, dh = 17u;  // 4x3 = 12 HTILE words
    std::vector<float> depth_in(dw * dh, 1.0f);
    for (uint32_t y = 0; y < dh; ++y) {
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t tx = x / 8u, ty = y / 8u;
            if (ty == 0u && tx < 2u) {
                depth_in[y * dw + x] = 1.0f;  // fast-clear ZMax = 1.0
            } else if (ty == 0u && tx >= 2u) {
                depth_in[y * dw + x] = 0.0f;  // reversed-Z fast-clear ZMax = 0.0
            } else if (ty == 1u) {
                depth_in[y * dw + x] = static_cast<float>(4096) / 16383.0f;  // constant ZMin
            } else {
                depth_in[y * dw + x] = static_cast<float>(x + y * 3u) / 100.0f;  // expanded depth plane
            }
        }
    }

    std::vector<uint32_t> htile(12u, 0);
    std::vector<float> expanded(12u * 64u, 0.0f);
    uint32_t exp_written = 0;
    SX_REQUIRE(XGBuildHtileMetadata(depth_in.data(), dw, dh, dw, htile.data(), 12u,
                                    expanded.data(), static_cast<uint32_t>(expanded.size()), &exp_written) == 0);
    // Row 2 has 3 varying 8x1 tiles (expanded) and 1 single-pixel corner tile (24,16) compressed as CONST_ZMIN!
    SX_REQUIRE(exp_written == 3u * 64u);
    SX_REQUIRE((htile[0] & 0xFu) == XG_HTILE_ZMASK_CLEAR_ZMAX);
    SX_REQUIRE(((htile[0] >> 18u) & 0x3FFFu) == 0x3FFFu);
    SX_REQUIRE((htile[2] & 0xFu) == XG_HTILE_ZMASK_CLEAR_ZMAX);
    SX_REQUIRE(((htile[2] >> 18u) & 0x3FFFu) == 0u);
    SX_REQUIRE((htile[4] & 0xFu) == XG_HTILE_ZMASK_CONST_ZMIN);
    SX_REQUIRE((htile[8] & 0xFu) == XG_HTILE_ZMASK_EXPANDED);
    SX_REQUIRE((htile[11] & 0xFu) == XG_HTILE_ZMASK_CONST_ZMIN);

    std::vector<float> depth_out(dw * dh, -1.0f);
    SX_REQUIRE(XGDecompressHtileDepth(htile.data(), 12u, expanded.data(), exp_written,
                                      depth_out.data(), dw, dh, dw) == 0);
    for (size_t i = 0; i < depth_in.size(); ++i) {
        SX_REQUIRE(std::fabs(depth_out[i] - depth_in[i]) <= 1e-4f);
    }

    // Also verify metadata-only Hi-Z conservative depth decompression (expandedTiles == nullptr).
    std::vector<float> hiz_out(dw * dh, -1.0f);
    SX_REQUIRE(XGDecompressHtileDepth(htile.data(), 12u, nullptr, 0, hiz_out.data(), dw, dh, dw) == 0);
    for (size_t i = 0; i < hiz_out.size(); ++i) {
        SX_REQUIRE(hiz_out[i] >= 0.0f && hiz_out[i] <= 1.0f);
    }
}

}  // namespace

int main() {
    TestSingleMip();
    TestMipChain();
    TestRoundTrip();
    TestComputer();
    TestMsaaAddressing();
    TestDccAndHtileCodecs();
    std::printf("test_xg_layout: OK\n");
    return 0;
}
