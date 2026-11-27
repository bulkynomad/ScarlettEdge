// Real-title proxy path: where CopyTextureRegionX sits in the command-list
// table. A title uploads its textures with it (CopyTextureRegion's arguments
// plus copy flags), and the slot depends on the SDK the title was built
// with: 83, or 85 from D3D12XboxCreateDevice version 0x20004 on, where two
// methods were added at slots 59 and 60. In that later table slot 83 is a
// different method and must not copy.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

constexpr int kRelease = 2;
constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommandAllocator = 9;
constexpr int kDevCreateCommandList = 12;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kDevCreateFence = 36;
constexpr int kResMap = 8;
constexpr int kResUnmap = 9;
constexpr int kListClose = 9;
constexpr int kListCopyTextureRegion = 16;
constexpr int kListResourceBarrier = 26;
constexpr int kQueueExecuteCommandLists = 10;
constexpr int kFenceGetCompletedValue = 8;

constexpr uint32_t kSize = 8;
constexpr uint32_t kPitch = 256;  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
constexpr intptr_t kNotImplemented = static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));

struct Layout {
    uint32_t version;
    int copy_texture_region_x;  // command list
    int other_layout_slot;      // where the other SDK has it
    int queue_signal;
    int flush_pipeline_x;       // command list slot for FlushPipelineX
    int other_flush_slot;       // where the other SDK has it
};

void* CreateResource(void* device, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc,
                     D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    void* res = nullptr;
    SX_REQUIRE(Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &desc, static_cast<uintptr_t>(state),
                    static_cast<void*>(nullptr), static_cast<void*>(nullptr), &res) == 0 &&
               res != nullptr);
    return res;
}

void Check(const Layout& layout) {
    void* device = nullptr;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = layout.version;
    params.PreferWarp = 1;
    const IID device_iid = __uuidof(ID3D12Device);
    SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &device_iid, &device) >= 0 && device != nullptr);

    void* queue = nullptr;
    void* allocator = nullptr;
    void* list = nullptr;
    void* fence = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(device, kDevCreateCommandQueue, &qdesc, static_cast<void*>(nullptr), &queue) == 0 && queue);
    SX_REQUIRE(Call(device, kDevCreateCommandAllocator, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                    static_cast<void*>(nullptr), &allocator) == 0 && allocator);
    SX_REQUIRE(Call(device, kDevCreateCommandList, uintptr_t{0}, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT}, allocator,
                    static_cast<void*>(nullptr), static_cast<void*>(nullptr), &list) == 0 && list);
    SX_REQUIRE(Call(device, kDevCreateFence, uintptr_t{0}, uintptr_t{0}, static_cast<void*>(nullptr), &fence) == 0 &&
               fence);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kSize;
    td.Height = kSize;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    void* texture = CreateResource(device, D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kPitch * kSize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* upload = CreateResource(device, D3D12_HEAP_TYPE_UPLOAD, bd, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* readback = CreateResource(device, D3D12_HEAP_TYPE_READBACK, bd, D3D12_RESOURCE_STATE_COPY_DEST);

    uint8_t* up = nullptr;
    SX_REQUIRE(Call(upload, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &up) == 0 && up);
    for (uint32_t i = 0; i < kPitch * kSize; ++i) up[i] = static_cast<uint8_t>(i * 7 + layout.version);
    Call(upload, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

    D3D12_TEXTURE_COPY_LOCATION in_texture = {};
    in_texture.pResource = static_cast<ID3D12Resource*>(texture);  // proxy pointer, as a guest passes it
    in_texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION in_buffer = {};
    in_buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    in_buffer.PlacedFootprint.Footprint.Format = td.Format;
    in_buffer.PlacedFootprint.Footprint.Width = kSize;
    in_buffer.PlacedFootprint.Footprint.Height = kSize;
    in_buffer.PlacedFootprint.Footprint.Depth = 1;
    in_buffer.PlacedFootprint.Footprint.RowPitch = kPitch;

    // The other SDK's slot is not this method here.
    in_buffer.pResource = static_cast<ID3D12Resource*>(upload);
    SX_REQUIRE(Call(list, layout.other_layout_slot, &in_texture, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, &in_buffer,
                    static_cast<void*>(nullptr), uintptr_t{0}) == kNotImplemented);

    // Slot 67 / 69 (FlushPipelineX) behaves according to SDK layout version.
    SX_REQUIRE(Call(list, layout.flush_pipeline_x, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}) == 0);
    SX_REQUIRE(Call(list, layout.other_flush_slot, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}) == kNotImplemented);

    // Verify extended D3D12.2 device, queue & command list COM slots (VRS, ReservedResource, UpdateTileMappings)
    SX_REQUIRE(Call(device, 40, uintptr_t{0}) == 0);  // SetStablePowerState
    LUID adapter_luid{};
    Call(device, 44, &adapter_luid);  // GetAdapterLuid
    SX_REQUIRE(adapter_luid.LowPart != 0 || adapter_luid.HighPart != 0);

    const int vrs_slot = (layout.version >= 0x20004u) ? 76 : 74;
    SX_REQUIRE(Call(list, vrs_slot, uintptr_t{0x5 /* 2X2 */}, static_cast<void*>(nullptr)) == 0);

    D3D12_RESOURCE_DESC rbd = bd;
    rbd.Width = 65536ull;
    void* reserved_buf = nullptr;
    SX_REQUIRE(Call(device, 30 /* CreateReservedResource */, &rbd, uintptr_t{D3D12_RESOURCE_STATE_COMMON},
                    static_cast<void*>(nullptr), static_cast<void*>(nullptr), &reserved_buf) == 0 &&
               reserved_buf != nullptr);
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 65536ull;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    const IID heap_iid = __uuidof(ID3D12Heap);
    void* tile_heap = nullptr;
    SX_REQUIRE(Call(device, 28 /* CreateHeap */, &hd, &heap_iid, &tile_heap) == 0 && tile_heap != nullptr);
    D3D12_TILED_RESOURCE_COORDINATE tcoord{0u, 0u, 0u, 0u};
    D3D12_TILE_REGION_SIZE tregion{1u, FALSE, 0u, 0u, 0u};
    const D3D12_TILE_RANGE_FLAGS trange_flag = D3D12_TILE_RANGE_FLAG_NONE;
    const UINT theap_off = 0u;
    const UINT tcount = 1u;
    SX_REQUIRE(Call(queue, 8 /* UpdateTileMappings */, reserved_buf, uintptr_t{1}, &tcoord, &tregion,
                    tile_heap, uintptr_t{1}, &trange_flag, &theap_off, &tcount, uintptr_t{0}) == 0);
    // Also verify NULL-unmapping safety path
    SX_REQUIRE(Call(queue, 8 /* UpdateTileMappings */, reserved_buf, uintptr_t{1}, &tcoord, &tregion,
                    static_cast<void*>(nullptr), uintptr_t{0}, static_cast<void*>(nullptr),
                    static_cast<void*>(nullptr), static_cast<void*>(nullptr), uintptr_t{0}) == 0);
    Call(reserved_buf, kRelease);
    Call(tile_heap, kRelease);

    // Upload with CopyTextureRegionX, flags 0, as the title does.
    Call(list, layout.copy_texture_region_x, &in_texture, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, &in_buffer,
         static_cast<void*>(nullptr), uintptr_t{0});

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = static_cast<ID3D12Resource*>(texture);
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Call(list, kListResourceBarrier, uintptr_t{1}, &barrier);
    in_buffer.pResource = static_cast<ID3D12Resource*>(readback);
    Call(list, kListCopyTextureRegion, &in_buffer, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, &in_texture,
         static_cast<void*>(nullptr));
    SX_REQUIRE(Call(list, kListClose) == 0);

    void* lists[] = {list};
    Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
    SX_REQUIRE(Call(queue, layout.queue_signal, fence, uintptr_t{1}) == 0);
    for (int i = 0; i < 5000 && Call(fence, kFenceGetCompletedValue) < 1; ++i) Sleep(1);
    SX_REQUIRE(Call(fence, kFenceGetCompletedValue) == 1);

    uint8_t* data = nullptr;
    SX_REQUIRE(Call(readback, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &data) == 0 && data);
    SX_REQUIRE(Call(upload, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &up) == 0 && up);
    for (uint32_t y = 0; y < kSize; ++y) {
        SX_REQUIRE(std::memcmp(data + y * kPitch, up + y * kPitch, kSize * 4) == 0);
    }
    Call(upload, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));
    Call(readback, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

    for (void* object : {readback, upload, texture, fence, list, allocator, queue, device}) Call(object, kRelease);
    std::printf("test_d3d12_list_layout: version 0x%X, CopyTextureRegionX at slot %d, FlushPipelineX at slot %d OK\n",
                layout.version, layout.copy_texture_region_x, layout.flush_pipeline_x);
}

}  // namespace

int main() {
    // Select the guest-facing proxies (also set by CTest for this test).
    _putenv_s("SCARLETTX_REAL_TITLE", "1");

    Check({0x20001, 83, 85, 14, 67, 69});
    Check({0x20004, 85, 83, 16, 69, 67});
    Check({0x20006, 85, 83, 16, 69, 67});

    std::printf("test_d3d12_list_layout: OK\n");
    return 0;
}
