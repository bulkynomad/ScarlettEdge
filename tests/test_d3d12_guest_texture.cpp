// Real-title proxy path: a texture filled through the address returned by
// ID3D12Resource::GetGPUVirtualAddress. A title copies cooked texel data in
// console layout straight to that address; the data must reach the host
// texture, and later writes must be picked up again. Everything goes through
// the Xbox-facing COM proxies, as a guest would call them.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/kernel/kernelx_abi.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace xg = scarlettx::gpu::xg;

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

// Vtable slots used below.
constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommandAllocator = 9;
constexpr int kDevCreateCommandList = 12;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kDevCreateHeap = 28;
constexpr int kDevCreatePlacedResource = 29;
constexpr int kDevCreateFence = 36;
constexpr int kResMap = 8;
constexpr int kResUnmap = 9;
constexpr int kResGetGPUVirtualAddress = 11;
constexpr int kListClose = 9;
constexpr int kListReset = 10;
constexpr int kListCopyTextureRegion = 16;
constexpr int kListResourceBarrier = 26;
constexpr int kQueueExecuteCommandLists = 10;
constexpr int kQueueSignal = 14;
constexpr int kFenceGetCompletedValue = 8;

constexpr uint32_t kWidth = 100;
constexpr uint32_t kHeight = 60;
constexpr uint32_t kMips = 7;  // 100x60 ... 1x1, the last four share the mip tail

struct Mip {
    uint32_t width, height, pitch;
    uint64_t offset;
    std::vector<uint8_t> pixels;  // width * 4 bytes per row
};

void* CreateResource(void* device, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc,
                     D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    void* res = nullptr;
    const intptr_t hr = Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &desc,
                             static_cast<uintptr_t>(state), static_cast<void*>(nullptr),
                             static_cast<void*>(nullptr), &res);
    SX_REQUIRE(hr == 0 && res != nullptr);
    return res;
}

void Fill(std::vector<Mip>& mips, uint8_t seed) {
    for (size_t m = 0; m < mips.size(); ++m) {
        for (size_t i = 0; i < mips[m].pixels.size(); ++i) {
            mips[m].pixels[i] = static_cast<uint8_t>(i * 13 + m * 29 + seed);
        }
    }
}

struct Context {
    void* device = nullptr;
    void* queue = nullptr;
    void* allocator = nullptr;
    void* list = nullptr;
    void* fence = nullptr;
    void* texture = nullptr;
    void* readback = nullptr;
    uint64_t fence_value = 0;
};

// Copies every mip of the texture into the readback buffer and compares.
void ReadBackAndCompare(Context& c, const std::vector<Mip>& mips, bool first_pass) {
    if (!first_pass) SX_REQUIRE(Call(c.list, kListReset, c.allocator, static_cast<void*>(nullptr)) == 0);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = static_cast<ID3D12Resource*>(c.texture);  // proxy pointer, as a guest passes it
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Call(c.list, kListResourceBarrier, uintptr_t{1}, &barrier);
    for (size_t m = 0; m < mips.size(); ++m) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = static_cast<ID3D12Resource*>(c.readback);
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = mips[m].offset;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = mips[m].width;
        dst.PlacedFootprint.Footprint.Height = mips[m].height;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = mips[m].pitch;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = static_cast<ID3D12Resource*>(c.texture);
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = static_cast<UINT>(m);
        Call(c.list, kListCopyTextureRegion, &dst, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, &src,
             static_cast<void*>(nullptr));
    }
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    Call(c.list, kListResourceBarrier, uintptr_t{1}, &barrier);
    SX_REQUIRE(Call(c.list, kListClose) == 0);

    void* lists[] = {c.list};
    Call(c.queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
    const uint64_t value = ++c.fence_value;
    SX_REQUIRE(Call(c.queue, kQueueSignal, c.fence, static_cast<uintptr_t>(value)) == 0);
    for (int i = 0; i < 5000 && static_cast<uint64_t>(Call(c.fence, kFenceGetCompletedValue)) < value; ++i) {
        Sleep(1);
    }
    SX_REQUIRE(static_cast<uint64_t>(Call(c.fence, kFenceGetCompletedValue)) >= value);

    uint8_t* data = nullptr;
    SX_REQUIRE(Call(c.readback, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &data) == 0 && data);
    for (size_t m = 0; m < mips.size(); ++m) {
        for (uint32_t y = 0; y < mips[m].height; ++y) {
            SX_REQUIRE(std::memcmp(data + mips[m].offset + static_cast<size_t>(y) * mips[m].pitch,
                                   mips[m].pixels.data() + static_cast<size_t>(y) * mips[m].width * 4,
                                   mips[m].width * 4) == 0);
        }
    }
    Call(c.readback, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));
}

}  // namespace

int main() {
    // Select the guest-facing proxies (also set by CTest for this test).
    _putenv_s("SCARLETTX_REAL_TITLE", "1");

    Context c;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    const IID device_iid = __uuidof(ID3D12Device);
    SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &device_iid, &c.device) >= 0);
    SX_REQUIRE(c.device != nullptr);

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(c.device, kDevCreateCommandQueue, &qdesc, static_cast<void*>(nullptr), &c.queue) == 0);
    SX_REQUIRE(Call(c.device, kDevCreateCommandAllocator, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                    static_cast<void*>(nullptr), &c.allocator) == 0);
    SX_REQUIRE(Call(c.device, kDevCreateCommandList, uintptr_t{0}, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                    c.allocator, static_cast<void*>(nullptr), static_cast<void*>(nullptr), &c.list) == 0);
    SX_REQUIRE(Call(c.device, kDevCreateFence, uintptr_t{0}, uintptr_t{0}, static_cast<void*>(nullptr),
                    &c.fence) == 0);
    SX_REQUIRE(c.queue && c.allocator && c.list && c.fence);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kWidth;
    td.Height = kHeight;
    td.DepthOrArraySize = 1;
    td.MipLevels = kMips;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    c.texture = CreateResource(c.device, D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // The address is guest memory laid out like the console texture, and stable.
    auto* memory = reinterpret_cast<uint8_t*>(Call(c.texture, kResGetGPUVirtualAddress));
    SX_REQUIRE(memory != nullptr);
    SX_REQUIRE(reinterpret_cast<uint8_t*>(Call(c.texture, kResGetGPUVirtualAddress)) == memory);

    XG_TEXTURE2D_DESC xd{};
    xd.Width = kWidth;
    xd.Height = kHeight;
    xd.MipLevels = kMips;
    xd.ArraySize = 1;
    xd.Format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    xd.SampleDesc.Count = 1;
    xd.BindFlags = 0x8;
    xd.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    auto layout = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(xd, layout.get());

    std::vector<Mip> mips(kMips);
    uint64_t offset = 0;
    for (uint32_t m = 0; m < kMips; ++m) {
        mips[m].width = kWidth >> m ? kWidth >> m : 1;
        mips[m].height = kHeight >> m ? kHeight >> m : 1;
        mips[m].pitch = (mips[m].width * 4 + 255) & ~255u;
        mips[m].offset = offset;
        mips[m].pixels.resize(static_cast<size_t>(mips[m].width) * 4 * mips[m].height);
        offset += (static_cast<uint64_t>(mips[m].pitch) * mips[m].height + 511) & ~511ull;
    }
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = offset;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    c.readback = CreateResource(c.device, D3D12_HEAP_TYPE_READBACK, bd, D3D12_RESOURCE_STATE_COPY_DEST);

    // Write the cooked data the way a title does, then use the texture.
    for (int pass = 0; pass < 2; ++pass) {
        Fill(mips, static_cast<uint8_t>(pass == 0 ? 3 : 171));
        for (uint32_t m = 0; m < kMips; ++m) {
            SX_REQUIRE(xg::CopyIntoSubresource(*layout, memory, m, 0, mips[m].pixels.data(), mips[m].width * 4));
        }
        ReadBackAndCompare(c, mips, pass == 0);
    }

    // Mip 1 arrives through a command-list copy, then mip 0 through guest
    // memory: the upload of mip 0 must leave the copied mip alone.
    {
        D3D12_RESOURCE_DESC ud = bd;
        ud.Width = static_cast<uint64_t>(mips[1].pitch) * mips[1].height;
        void* upload = CreateResource(c.device, D3D12_HEAP_TYPE_UPLOAD, ud, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* up = nullptr;
        SX_REQUIRE(Call(upload, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &up) == 0 && up);
        for (size_t i = 0; i < mips[1].pixels.size(); ++i) mips[1].pixels[i] = static_cast<uint8_t>(i * 5 + 77);
        for (uint32_t y = 0; y < mips[1].height; ++y) {
            std::memcpy(up + static_cast<size_t>(y) * mips[1].pitch,
                        mips[1].pixels.data() + static_cast<size_t>(y) * mips[1].width * 4, mips[1].width * 4);
        }
        Call(upload, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

        SX_REQUIRE(Call(c.list, kListReset, c.allocator, static_cast<void*>(nullptr)) == 0);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = static_cast<ID3D12Resource*>(c.texture);
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        Call(c.list, kListResourceBarrier, uintptr_t{1}, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = static_cast<ID3D12Resource*>(c.texture);
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 1;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = static_cast<ID3D12Resource*>(upload);
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = mips[1].width;
        src.PlacedFootprint.Footprint.Height = mips[1].height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = mips[1].pitch;
        Call(c.list, kListCopyTextureRegion, &dst, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, &src,
             static_cast<void*>(nullptr));
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        Call(c.list, kListResourceBarrier, uintptr_t{1}, &barrier);
        SX_REQUIRE(Call(c.list, kListClose) == 0);
        void* lists[] = {c.list};
        Call(c.queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));

        for (size_t i = 0; i < mips[0].pixels.size(); ++i) mips[0].pixels[i] = static_cast<uint8_t>(i * 3 + 9);
        SX_REQUIRE(xg::CopyIntoSubresource(*layout, memory, 0, 0, mips[0].pixels.data(), mips[0].width * 4));
        ReadBackAndCompare(c, mips, false);
    }

    // A planar format has no single-plane console layout here: no guest
    // memory is handed out (when the adapter can create such a texture at all).
    D3D12_RESOURCE_DESC nv12 = td;
    nv12.Width = 64;
    nv12.Height = 64;
    nv12.MipLevels = 1;
    nv12.Format = DXGI_FORMAT_NV12;
    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    void* planar = nullptr;
    if (Call(c.device, kDevCreateCommittedResource, &default_heap, uintptr_t{0}, &nv12,
             static_cast<uintptr_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), static_cast<void*>(nullptr),
             static_cast<void*>(nullptr), &planar) == 0 &&
        planar) {
        SX_REQUIRE(Call(planar, kResGetGPUVirtualAddress) == 0);
    } else {
        std::printf("test_d3d12_guest_texture: NV12 texture not available, planar check skipped\n");
    }

    // Heaps: the title creates texture-only heaps and places its scene
    // textures in them. A placed texture behaves like a committed one.
    {
        D3D12_HEAP_DESC heap_desc = {};
        heap_desc.SizeInBytes = 0xFE0000;
        heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_desc.Alignment = 0x10000;
        heap_desc.Flags = static_cast<D3D12_HEAP_FLAGS>(0x84);  // the value the title passes
        const IID heap_iid = __uuidof(ID3D12Heap);
        const IID other_iid = __uuidof(ID3D12Fence);
        void* heap = nullptr;
        SX_REQUIRE(Call(c.device, kDevCreateHeap, &heap_desc, &other_iid, &heap) == static_cast<intptr_t>(E_NOINTERFACE));
        SX_REQUIRE(heap == nullptr);
        SX_REQUIRE(Call(c.device, kDevCreateHeap, &heap_desc, &heap_iid, &heap) == 0 && heap != nullptr);

        void* again = nullptr;
        SX_REQUIRE(Call(heap, 0, &other_iid, &again) == static_cast<intptr_t>(static_cast<int32_t>(0x80004002u)));
        SX_REQUIRE(again == nullptr);
        SX_REQUIRE(Call(heap, 0, &heap_iid, &again) == 0 && again == heap);
        SX_REQUIRE(Call(heap, 2) == 1);

        D3D12_RESOURCE_DESC placed_desc = td;
        placed_desc.Width = 256;
        placed_desc.Height = 128;
        placed_desc.MipLevels = 1;
        void* placed = nullptr;
        // Something that is not a heap from CreateHeap is refused.
        SX_REQUIRE(Call(c.device, kDevCreatePlacedResource, c.device, uintptr_t{0}, &placed_desc,
                        static_cast<uintptr_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                        static_cast<void*>(nullptr), static_cast<void*>(nullptr), &placed) ==
                   static_cast<intptr_t>(E_INVALIDARG));
        SX_REQUIRE(placed == nullptr);
        SX_REQUIRE(Call(c.device, kDevCreatePlacedResource, heap, uintptr_t{0x10000}, &placed_desc,
                        static_cast<uintptr_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                        static_cast<void*>(nullptr), static_cast<void*>(nullptr), &placed) == 0);
        SX_REQUIRE(placed != nullptr);
        // The placed texture hands out guest memory the title can write to.
        SX_REQUIRE(Call(placed, kResGetGPUVirtualAddress) != 0);
        // The heap may go away before the resources placed in it.
        SX_REQUIRE(Call(heap, 2) == 0);
        SX_REQUIRE(Call(placed, kResGetGPUVirtualAddress) != 0);
        SX_REQUIRE(Call(placed, 2) == 0);
    }

    // 4. CreatePlacedResourceX (slot 74) on KernelX MEM_GRAPHICS GPUVA + MapViewOfFile3 virtual alias:
    //    Write console-swizzled texels through the KernelX alias VA and verify automatic GPU upload!
    {
        constexpr int kDevCreateRootSignature = 16;
        constexpr int kDevCreatePipelineStateStream = 50;
        constexpr int kDevCreatePlacedResourceX = 74;

        void* kx_va = nullptr;
        uint64_t kx_gpu_va = 0;
        SX_REQUIRE(KernelXAllocateMemory(0x10000, KERNELX_PAGE_64KB, KERNELX_MEM_GPU_VISIBLE,
                                         &kx_va, &kx_gpu_va) == 0);
        SX_REQUIRE(kx_va != nullptr && kx_gpu_va != 0);

        void* kx_alias = nullptr;
        SX_REQUIRE(KernelXCreateMemoryAlias(kx_va, 0, 0x10000, &kx_alias) == 0);
        SX_REQUIRE(kx_alias != nullptr && kx_alias != kx_va);

        D3D12_RESOURCE_DESC px_desc = td;
        px_desc.Width = 32;
        px_desc.Height = 32;
        px_desc.MipLevels = 1;

        void* placed_x = nullptr;
        SX_REQUIRE(Call(c.device, kDevCreatePlacedResourceX, static_cast<uintptr_t>(kx_gpu_va), &px_desc,
                        static_cast<uintptr_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                        static_cast<void*>(nullptr), static_cast<void*>(nullptr), &placed_x) == 0);
        SX_REQUIRE(placed_x != nullptr);
        SX_REQUIRE(static_cast<uint64_t>(Call(placed_x, kResGetGPUVirtualAddress)) == kx_gpu_va);

        // Cook linear 32x32 RGBA8 pattern into console 4KB_S swizzled layout via the KernelX ALIAS view!
        XG_TEXTURE2D_DESC px_xd{};
        px_xd.Width = 32;
        px_xd.Height = 32;
        px_xd.MipLevels = 1;
        px_xd.ArraySize = 1;
        px_xd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        px_xd.SampleDesc.Count = 1;
        px_xd.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
        XG_RESOURCE_LAYOUT xlay{};
        xg::ComputeTexture2DLayout(px_xd, &xlay);

        std::vector<uint8_t> linear_src(32 * 32 * 4);
        for (size_t i = 0; i < linear_src.size(); ++i) {
            linear_src[i] = static_cast<uint8_t>((i * 37u + 0x5Au) & 0xFFu);
        }
        SX_REQUIRE(xg::CopyIntoSubresource(xlay, kx_alias, 0, 0, linear_src.data(), 32 * 4));

        // Copy from placed_x on the GPU command list to c.readback and verify every byte matches!
        SX_REQUIRE(Call(c.list, kListReset, c.allocator, static_cast<void*>(nullptr)) == 0);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = static_cast<ID3D12Resource*>(placed_x);
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        Call(c.list, kListResourceBarrier, uintptr_t{1}, &b);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = static_cast<ID3D12Resource*>(c.readback);
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = 32;
        dst.PlacedFootprint.Footprint.Height = 32;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = static_cast<ID3D12Resource*>(placed_x);
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        Call(c.list, kListCopyTextureRegion, &dst, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, &src,
             static_cast<void*>(nullptr));
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        Call(c.list, kListResourceBarrier, uintptr_t{1}, &b);
        SX_REQUIRE(Call(c.list, kListClose) == 0);

        void* lists[] = {c.list};
        Call(c.queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
        const uint64_t fval = ++c.fence_value;
        SX_REQUIRE(Call(c.queue, kQueueSignal, c.fence, static_cast<uintptr_t>(fval)) == 0);
        while (static_cast<uint64_t>(Call(c.fence, kFenceGetCompletedValue)) < fval) Sleep(1);

        void* rb_ptr = nullptr;
        SX_REQUIRE(Call(c.readback, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &rb_ptr) == 0);
        for (uint32_t row = 0; row < 32; ++row) {
            SX_REQUIRE(std::memcmp(static_cast<const uint8_t*>(rb_ptr) + row * 256,
                                   linear_src.data() + row * 128, 128) == 0);
        }
        Call(c.readback, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

        SX_REQUIRE(Call(placed_x, 2) == 0);
        SX_REQUIRE(KernelXFreeMemoryAlias(kx_alias) == 0);
        SX_REQUIRE(KernelXFreeMemory(kx_va) == 0);

        // 5. ID3D12Device2::CreatePipelineState (slot 50) stream PSO with Xbox RS proxy + XS2\0 shader!
        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        ID3DBlob* rs_blob = nullptr;
        SX_REQUIRE(D3D12XsSerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                 reinterpret_cast<void**>(&rs_blob), nullptr) >= 0);
        void* rs_proxy = nullptr;
        const IID rs_iid = __uuidof(ID3D12RootSignature);
        SX_REQUIRE(Call(c.device, kDevCreateRootSignature, uintptr_t{0}, rs_blob->GetBufferPointer(),
                        rs_blob->GetBufferSize(), &rs_iid, &rs_proxy) == 0 && rs_proxy != nullptr);
        rs_blob->Release();

        // Minimal XS2\0 compute shader (v_add_f32 v2, v0, v0; s_endpgm — no UAV/CBV needed for empty RS)
        const uint8_t xs2_stream_cs[] = {
            'X', 'S', '2', '\0',
            1, 0, 0, 0,
            3, 0, 0, 0,  // Compute stage
            8, 0, 0, 0,  // 8 bytes of RDNA2 ISA
            0x00, 0x00, 0x04, 0x06,  // v_add_f32 v2, v0, v0
            0x00, 0x00, 0x80, 0xBF   // s_endpgm
        };
        struct alignas(void*) TokRs { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; ID3D12RootSignature* rs; };
        struct alignas(void*) TokCs { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; D3D12_SHADER_BYTECODE cs; };
        struct StreamLayout {
            TokRs rs;
            TokCs cs;
        } stream_data{
            {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, static_cast<ID3D12RootSignature*>(rs_proxy)},
            {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, {xs2_stream_cs, sizeof(xs2_stream_cs)}},
        };
        D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream_data), &stream_data};
        void* stream_pso = nullptr;
        const IID pso_iid = __uuidof(ID3D12PipelineState);
        SX_REQUIRE(Call(c.device, kDevCreatePipelineStateStream, &stream_desc, &pso_iid, &stream_pso) == 0);
        SX_REQUIRE(stream_pso != nullptr);
        SX_REQUIRE(Call(stream_pso, 2) == 0);
        SX_REQUIRE(Call(rs_proxy, 2) == 0);
    }

    std::printf("test_d3d12_guest_texture: OK\n");
    return 0;
}
