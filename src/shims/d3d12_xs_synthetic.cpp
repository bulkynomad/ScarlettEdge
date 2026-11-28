#include "d3d12_xs_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/rt_bvh.h"
#include "scarlettx/gpu/shader_ir.h"

namespace {

using scarlettx::d3d12_xs_internal::AlignUp;
using scarlettx::d3d12_xs_internal::g_gpu_profiler;
using scarlettx::d3d12_xs_internal::g_live_command_lists;
using scarlettx::d3d12_xs_internal::g_proxy_pso_cache_hits;
using scarlettx::d3d12_xs_internal::g_proxy_pso_cache_misses;
using scarlettx::d3d12_xs_internal::g_submit_count;
using scarlettx::d3d12_xs_internal::PreferWarpFromEnv;
using scarlettx::d3d12_xs_internal::TryGetDeviceProxyRemovedReason;
#if defined(_WIN32)
using scarlettx::d3d12_xs_internal::CallSystemD3D12SerializeRootSignature;
using scarlettx::d3d12_xs_internal::EnqueueDelayedEventSignal;
#endif

uint32_t IntervalToHz(uint32_t interval) {
    switch (interval) {
        case D3D12XBOX_FRAME_INTERVAL_120_HZ: return 120;
        case D3D12XBOX_FRAME_INTERVAL_60_HZ:  return 60;
        case D3D12XBOX_FRAME_INTERVAL_30_HZ:  return 30;
        case D3D12XBOX_FRAME_INTERVAL_24_HZ:  return 24;
        default: return 60;
    }
}

struct AccelerationStructureEntry {
    uint32_t type = D3D12XBOX_AS_TYPE_BOTTOM_LEVEL;
    uint32_t flags = 0;
    std::vector<scarlettx::RtTriangle> triangles;
    scarlettx::BvhAccelerationStructure bvh;
};

struct RootSignatureEntry {
    D3D12XBOX_ROOT_SIGNATURE_DESC desc{};
    int32_t constants_param_idx = -1;
    int32_t uav0_param_idx = -1;
#if defined(_WIN32)
    ID3D12RootSignature* native = nullptr;
    ~RootSignatureEntry() {
        if (native) native->Release();
    }
#endif
};

struct DescriptorHeapEntry {
    D3D12XBOX_DESCRIPTOR_HEAP_DESC desc{};
#if defined(_WIN32)
    ID3D12DescriptorHeap* native = nullptr;
    ~DescriptorHeapEntry() {
        if (native) native->Release();
    }
#endif
};

struct PipelineStateEntry {
    bool is_compute = false;
    bool is_raytracing = false;
    uint64_t root_sig_handle = 0;
    uint32_t rtv_format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    uint64_t pso_key = 0;
    uint32_t max_payload_bytes = 0;
    uint32_t max_attribute_bytes = 0;
    uint32_t max_recursion_depth = 0;
#if defined(_WIN32)
    ID3D12PipelineState* native = nullptr;
    ID3D12StateObject* rt_state_object = nullptr;
    ~PipelineStateEntry() {
        if (rt_state_object) rt_state_object->Release();
        if (native) native->Release();
    }
#endif
};

struct ReservedResourceEntry {
#if defined(_WIN32)
    ID3D12Resource* resource = nullptr;
    ID3D12Heap* tile_heap = nullptr;
    ~ReservedResourceEntry() {
        if (resource) resource->Release();
        if (tile_heap) tile_heap->Release();
    }
#endif
    uint64_t byte_size = 0;
    bool is_true_reserved = false;
};

struct WorkGraphEntry {
#if defined(_WIN32)
    ID3D12RootSignature* root_sig = nullptr;
    ID3D12PipelineState* producer_pso = nullptr;
    ID3D12PipelineState* consumer_pso = nullptr;
    ID3D12CommandSignature* cmd_sig = nullptr;
    ~WorkGraphEntry() {
        if (cmd_sig) cmd_sig->Release();
        if (consumer_pso) consumer_pso->Release();
        if (producer_pso) producer_pso->Release();
        if (root_sig) root_sig->Release();
    }
#endif
    uint32_t max_records = 64;
    uint32_t record_stride = 16;
};

struct D3D12XsDeviceState {
    std::unique_ptr<scarlettx::D3D12Backend> backend;
    uint64_t next_frame_token = 1;
    uint64_t last_issued_token = 0;
    uint64_t last_presented_token = 0;
    uint64_t presented_frame_count = 0;
    uint32_t frame_interval = D3D12XBOX_FRAME_INTERVAL_60_HZ;
    uint32_t frame_interval_flags = 0;
    bool suspended = false;
    std::chrono::steady_clock::time_point last_present_time = std::chrono::steady_clock::now();
    uint64_t last_frame_duration_us = 16667;
    uint64_t next_as_handle = 0xA5000001ull;
    std::unordered_map<uint64_t, std::unique_ptr<AccelerationStructureEntry>> accel_structs;

    uint64_t next_root_sig_handle = 0x21000001ull;
    uint64_t next_heap_handle = 0x22000001ull;
    uint64_t next_pso_handle = 0x23000001ull;
    uint64_t next_reserved_handle = 0x24000001ull;
    uint64_t next_work_graph_handle = 0x25000001ull;
    std::unordered_map<uint64_t, std::unique_ptr<RootSignatureEntry>> root_signatures;
    std::unordered_map<uint64_t, std::unique_ptr<DescriptorHeapEntry>> descriptor_heaps;
    std::unordered_map<uint64_t, std::unique_ptr<PipelineStateEntry>> psos;
    std::unordered_map<uint64_t, std::unique_ptr<ReservedResourceEntry>> reserved_resources;
    std::unordered_map<uint64_t, std::unique_ptr<WorkGraphEntry>> work_graphs;
    uint64_t pso_cache_hits = 0;
    uint64_t pso_cache_misses = 0;
#if defined(_WIN32)
    std::unordered_map<uint64_t, ID3D12PipelineState*> pso_cache;
    std::unordered_map<uint64_t, ID3D12StateObject*> rt_pso_cache;
    ~D3D12XsDeviceState() {
        for (auto& kv : rt_pso_cache) {
            if (kv.second) kv.second->Release();
        }
        rt_pso_cache.clear();
        for (auto& kv : pso_cache) {
            if (kv.second) kv.second->Release();
        }
        pso_cache.clear();
        work_graphs.clear();
        reserved_resources.clear();
        psos.clear();
        descriptor_heaps.clear();
        root_signatures.clear();
    }
#endif
};

std::mutex g_mutex;
uint64_t g_next_handle = 0x12000001ull;
std::unordered_map<uint64_t, std::unique_ptr<D3D12XsDeviceState>> g_devices;

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// ScarlettX synthetic handle-based device, frame pipeline & DXR test helpers
// ---------------------------------------------------------------------------
int32_t D3D12XboxCreateSyntheticDevice(const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params,
                                       uint64_t* out_device_handle) {
    if (!out_device_handle) {
        return -1;
    }
    const bool prefer_warp = PreferWarpFromEnv(params ? (params->PreferWarp != 0) : true);
    const bool debug_layer = params ? (params->ProcessDebugFlags != 0) : false;

    auto state = std::make_unique<D3D12XsDeviceState>();
    state->backend = std::make_unique<scarlettx::D3D12Backend>();
    auto st = state->backend->Initialize(prefer_warp, debug_layer);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t handle = g_next_handle++;
    g_devices[handle] = std::move(state);
    *out_device_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyDevice(uint64_t device_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    g_devices.erase(it);
    return 0;
}

int32_t D3D12XboxGetNativeDevice(uint64_t device_handle, void** out_id3d12_device) {
    if (!out_id3d12_device) {
        return -1;
    }
    *out_id3d12_device = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    void* dev = it->second->backend->NativeDevice();
    if (!dev) {
        return -3;
    }
#if defined(_WIN32)
    static_cast<ID3D12Device*>(dev)->AddRef();
#endif
    *out_id3d12_device = dev;
    return 0;
}

int32_t D3D12XboxQueryCapabilities(uint64_t device_handle, D3D12XBOX_CAPABILITIES* out_caps) {
    if (!out_caps) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& info = it->second->backend->AdapterInfo();
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->RaytracingTier = info.raytracing_tier;
    out_caps->IsWarpAdapter = info.is_warp ? 1u : 0u;
    out_caps->MaxRenderWidth = 3840;
    out_caps->MaxRenderHeight = 2160;
    std::strncpy(out_caps->AdapterName, info.description.c_str(), sizeof(out_caps->AdapterName) - 1);
    return 0;
}

int32_t D3D12XboxQueryRaytracingCapabilities(uint64_t device_handle,
                                             D3D12XBOX_RAYTRACING_CAPABILITIES* out_rt_caps) {
    if (!out_rt_caps) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& info = it->second->backend->AdapterInfo();
    std::memset(out_rt_caps, 0, sizeof(*out_rt_caps));
    out_rt_caps->HardwareRaytracingTier = info.raytracing_tier;
    out_rt_caps->ActiveBackend = (info.raytracing_tier >= 10)
        ? D3D12XBOX_RT_BACKEND_HARDWARE_DXR
        : D3D12XBOX_RT_BACKEND_SOFTWARE_BVH;
    out_rt_caps->SupportsInlineRaytracing = (info.raytracing_tier >= 11) ? 1u : 0u;
    out_rt_caps->MaxRecursionDepth = 31;
    std::strncpy(out_rt_caps->AdapterName, info.description.c_str(), sizeof(out_rt_caps->AdapterName) - 1);
    return 0;
}

int32_t D3D12XboxSetFrameIntervalX(uint64_t device_handle, uint32_t interval, uint32_t flags) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->frame_interval = interval;
    it->second->frame_interval_flags = flags;
    return 0;
}

int32_t D3D12XboxScheduleFrameEventX(uint64_t device_handle,
                                     uint32_t frame_event,
                                     uint64_t offset_us,
                                     void* event_handle) {
    (void)frame_event;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
#if defined(_WIN32)
    if (event_handle) {
        EnqueueDelayedEventSignal(static_cast<HANDLE>(event_handle), offset_us);
    }
#else
    (void)event_handle;
    (void)offset_us;
#endif
    return 0;
}

int32_t D3D12XboxWaitFrameEventX(uint64_t device_handle, uint64_t* out_frame_token) {
    return D3D12XboxWaitFrameEventXEx(device_handle, D3D12XBOX_FRAME_EVENT_ORIGIN, 0, out_frame_token);
}

int32_t D3D12XboxWaitFrameEventXEx(uint64_t device_handle,
                                   uint32_t frame_event,
                                   uint32_t timeout_ms,
                                   D3D12XBOX_FRAME_PIPELINE_TOKEN* out_frame_token) {
    (void)frame_event;
    (void)timeout_ms;
    if (!out_frame_token) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const uint64_t tok = it->second->next_frame_token++;
    it->second->last_issued_token = tok;
    *out_frame_token = tok;
    return 0;
}

int32_t D3D12XboxRenderTestTriangle(uint64_t device_handle,
                                    uint32_t width,
                                    uint32_t height,
                                    const char* optional_ppm_path,
                                    uint64_t* out_pixel_hash) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    scarlettx::FrameBufferRgba8 fb;
    auto st = it->second->backend->RenderTestTriangle(width, height, fb);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (optional_ppm_path && *optional_ppm_path) {
        fb.WritePpm(optional_ppm_path);
    }
    if (out_pixel_hash) {
        *out_pixel_hash = fb.ComputeFnv1aHash();
    }
    return 0;
}

int32_t D3D12XboxPresentX(uint64_t device_handle, const D3D12XBOX_PRESENT_PLANE_PARAMETERS* plane) {
    if (!plane) {
        return -1;
    }
    D3D12XBOX_PRESENT_PARAMETERS params{};
    params.Token = plane->Token;
    params.PlaneCount = 1;
    params.Flags = D3D12XBOX_PRESENT_FLAG_NONE;
    params.Planes = plane;
    params.CommandQueue = nullptr;
    return D3D12XboxPresentXParams(device_handle, &params);
}

int32_t D3D12XboxPresentXParams(uint64_t device_handle, const D3D12XBOX_PRESENT_PARAMETERS* params) {
    if (!params) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    if (dev.suspended) {
        return -4;
    }

#if defined(_WIN32)
    if (params->CommandQueue) {
        auto* queue = static_cast<ID3D12CommandQueue*>(params->CommandQueue);
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (native_dev) {
            ID3D12Fence* fence = nullptr;
            if (SUCCEEDED(native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                                  reinterpret_cast<void**>(&fence))) && fence) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev) {
                    if (SUCCEEDED(queue->Signal(fence, 1))) {
                        if (fence->GetCompletedValue() < 1) {
                            fence->SetEventOnCompletion(1, ev);
                            WaitForSingleObject(ev, 5000);
                        }
                    }
                    CloseHandle(ev);
                }
                fence->Release();
            }
        }
    }
#endif

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - dev.last_present_time).count());
    dev.last_frame_duration_us = std::max<uint64_t>(1, elapsed_us);
    dev.last_present_time = now;
    dev.last_presented_token = params->Token;
    dev.presented_frame_count++;
    g_gpu_profiler.RecordDurationUs(dev.last_frame_duration_us);
    return 0;
}

int32_t D3D12XboxGetFrameStatisticsX(uint64_t device_handle,
                                     D3D12XBOX_FRAME_STATISTICS* out_stats) {
    if (!out_stats) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& dev = *it->second;
    out_stats->PresentedFrameCount = dev.presented_frame_count;
    out_stats->LastPresentedToken = dev.last_presented_token;
    out_stats->LastIssuedToken = dev.last_issued_token;
    out_stats->IntervalHz = IntervalToHz(dev.frame_interval);
    out_stats->Suspended = dev.suspended ? 1u : 0u;
    out_stats->LastFrameDurationUs = dev.last_frame_duration_us;
    return 0;
}

int32_t D3D12XboxSuspendX(uint64_t device_handle, uint32_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->suspended = true;
    return 0;
}

int32_t D3D12XboxResumeX(uint64_t device_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->suspended = false;
    return 0;
}

int32_t D3D12XboxGetResourceAllocationInfo(uint64_t device_handle,
                                           uint64_t width,
                                           uint32_t height,
                                           uint32_t dxgi_format,
                                           uint64_t* out_size_bytes,
                                           uint64_t* out_alignment) {
    if (!out_size_bytes || !out_alignment || width == 0 || height == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
#if defined(_WIN32)
    auto* dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (dev) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = static_cast<DXGI_FORMAT>(dxgi_format);
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &desc);
        *out_size_bytes = info.SizeInBytes;
        *out_alignment = info.Alignment;
        return 0;
    }
#endif
    (void)dxgi_format;
    *out_alignment = 65536;
    *out_size_bytes = AlignUp(width * static_cast<uint64_t>(height) * 4, 65536);
    return 0;
}

int32_t D3D12XboxGetRaytracingAccelerationStructurePrebuildInfo(
    uint64_t device_handle,
    const D3D12XBOX_RT_AS_INPUTS* inputs,
    D3D12XBOX_RT_AS_PREBUILD_INFO* out_info) {
    if (!inputs || !out_info) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }

#if defined(_WIN32)
    const auto& adapter = it->second->backend->AdapterInfo();
    auto* dev5 = static_cast<ID3D12Device5*>(it->second->backend->NativeDevice5());
    if (dev5 && adapter.raytracing_tier >= 10) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS dxr_inputs{};
        dxr_inputs.Type = (inputs->Type == D3D12XBOX_AS_TYPE_TOP_LEVEL)
            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL
            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        dxr_inputs.Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(inputs->Flags);
        dxr_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        if (inputs->Type == D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
            geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geom.Triangles.VertexCount = inputs->NumDescs * 3;
            dxr_inputs.NumDescs = 1;
            dxr_inputs.pGeometryDescs = &geom;
        } else {
            dxr_inputs.NumDescs = inputs->NumDescs;
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        dev5->GetRaytracingAccelerationStructurePrebuildInfo(&dxr_inputs, &info);
        out_info->ResultDataMaxSizeInBytes = info.ResultDataMaxSizeInBytes;
        out_info->ScratchDataSizeInBytes = info.ScratchDataSizeInBytes;
        out_info->UpdateScratchDataSizeInBytes = info.UpdateScratchDataSizeInBytes;
        if ((inputs->Flags & D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE) &&
            out_info->UpdateScratchDataSizeInBytes == 0) {
            out_info->UpdateScratchDataSizeInBytes = 256;
        }
        return 0;
    }
#endif

    const uint64_t n = std::max<uint64_t>(1, inputs->NumDescs);
    const uint64_t node_bytes = (2 * n) * 64;
    const uint64_t prim_bytes = n * sizeof(SCARLETT_BVH_TRIANGLE);
    out_info->ResultDataMaxSizeInBytes = AlignUp(node_bytes + prim_bytes + 256, 256);
    out_info->ScratchDataSizeInBytes = AlignUp(n * 128 + 256, 256);
    out_info->UpdateScratchDataSizeInBytes =
        (inputs->Flags & D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE)
            ? AlignUp(n * 64 + 256, 256)
            : 0;
    return 0;
}

int32_t D3D12XboxBuildRaytracingAccelerationStructure(
    uint64_t device_handle,
    const D3D12XBOX_RT_AS_BUILD_DESC* desc,
    uint64_t* out_as_handle) {
    if (!desc || !out_as_handle || desc->Inputs.NumDescs == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<AccelerationStructureEntry>();
    entry->type = desc->Inputs.Type;
    entry->flags = desc->Inputs.Flags;

    if (desc->Inputs.Type == D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
        if (!desc->Triangles) {
            return -1;
        }
        entry->triangles.reserve(desc->Inputs.NumDescs);
        for (uint32_t i = 0; i < desc->Inputs.NumDescs; ++i) {
            const auto& src = desc->Triangles[i];
            scarlettx::RtTriangle tri{};
            tri.v0 = {src.v0[0], src.v0[1], src.v0[2]};
            tri.v1 = {src.v1[0], src.v1[1], src.v1[2]};
            tri.v2 = {src.v2[0], src.v2[1], src.v2[2]};
            tri.color = {src.color[0], src.color[1], src.color[2]};
            tri.reflectivity = src.reflectivity;
            entry->triangles.push_back(tri);
        }
        auto st = entry->bvh.Build(entry->triangles);
        if (!st.ok()) {
            return static_cast<int32_t>(st.code);
        }
    } else if (desc->Inputs.Type == D3D12XBOX_AS_TYPE_TOP_LEVEL) {
        if (!desc->BlasHandles && !desc->Instances) {
            return -1;
        }
        std::vector<scarlettx::RtTriangle> combined;
        for (uint32_t i = 0; i < desc->Inputs.NumDescs; ++i) {
            const uint64_t blas_h = desc->Instances ? desc->Instances[i].BlasHandle : desc->BlasHandles[i];
            auto bit = dev.accel_structs.find(blas_h);
            if (bit == dev.accel_structs.end() ||
                bit->second->type != D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
                return -2;
            }
            if (desc->Instances) {
                const auto& m = desc->Instances[i].Transform;
                bool is_zero = true;
                for (int r = 0; r < 3 && is_zero; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        if (m[r][c] != 0.0f) {
                            is_zero = false;
                            break;
                        }
                    }
                }
                if (is_zero) {
                    combined.insert(combined.end(),
                                    bit->second->triangles.begin(),
                                    bit->second->triangles.end());
                } else {
                    auto xform = [&](const scarlettx::Vec3& p) -> scarlettx::Vec3 {
                        return {
                            m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
                            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
                            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3],
                        };
                    };
                    for (const auto& st_tri : bit->second->triangles) {
                        scarlettx::RtTriangle wt = st_tri;
                        wt.v0 = xform(st_tri.v0);
                        wt.v1 = xform(st_tri.v1);
                        wt.v2 = xform(st_tri.v2);
                        combined.push_back(wt);
                    }
                }
            } else {
                combined.insert(combined.end(),
                                bit->second->triangles.begin(),
                                bit->second->triangles.end());
            }
        }
        entry->triangles = combined;
        auto st = entry->bvh.Build(entry->triangles);
        if (!st.ok()) {
            return static_cast<int32_t>(st.code);
        }
    } else {
        return -1;
    }

    const uint64_t handle = dev.next_as_handle++;
    dev.accel_structs[handle] = std::move(entry);
    *out_as_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyRaytracingAccelerationStructure(uint64_t device_handle, uint64_t as_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->accel_structs.erase(as_handle) > 0 ? 0 : -2;
}

int32_t D3D12XboxInspectShader(
    const D3D12XBOX_SHADER_BYTECODE* shader,
    D3D12XBOX_SHADER_INSPECTION* out_info) {
    if (!shader || !shader->Bytecode || shader->BytecodeLength == 0 || !out_info) {
        return -1;
    }
    std::memset(out_info, 0, sizeof(*out_info));
    const auto* bytes = static_cast<const uint8_t*>(shader->Bytecode);
    std::vector<uint8_t> buf(bytes, bytes + shader->BytecodeLength);
    scarlettx::ShaderModuleInfo info{};
    auto st = scarlettx::InspectShaderContainer(buf, info);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    out_info->ContainerKind = static_cast<uint32_t>(info.kind);
    out_info->Stage = static_cast<uint32_t>(info.stage);
    out_info->ShaderModelMajor = info.shader_model_major;
    out_info->ShaderModelMinor = info.shader_model_minor;
    out_info->ChunkCount = info.chunk_count;
    out_info->InputParamCount = info.input_param_count;
    out_info->OutputParamCount = info.output_param_count;
    out_info->HasRaytracingExports = info.has_raytracing_exports ? 1u : 0u;
    out_info->HasEmbeddedRootSignature = info.has_embedded_root_signature ? 1u : 0u;
    out_info->HasXboxPrivateChunks = info.has_xbox_private_chunks ? 1u : 0u;
    out_info->ContentHash = info.content_hash;
    return 0;
}

int32_t D3D12XboxCreateRootSignature(
    uint64_t device_handle,
    const D3D12XBOX_ROOT_SIGNATURE_DESC* desc,
    uint64_t* out_root_sig_handle) {
    if (!desc || !out_root_sig_handle) {
        return -1;
    }
    *out_root_sig_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<RootSignatureEntry>();
    entry->desc = *desc;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev) {
        return -3;
    }
    std::vector<D3D12_ROOT_PARAMETER> params;
    if (desc->NumRootConstants32Bit > 0) {
        entry->constants_param_idx = static_cast<int32_t>(params.size());
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants.ShaderRegister = 0;
        p.Constants.RegisterSpace = 0;
        p.Constants.Num32BitValues = desc->NumRootConstants32Bit;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumCbvs; ++i) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = (desc->NumRootConstants32Bit > 0 ? 1u : 0u) + i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumSrvs; ++i) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p.Descriptor.ShaderRegister = i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumUavs; ++i) {
        if (i == 0) {
            entry->uav0_param_idx = static_cast<int32_t>(params.size());
        }
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p.Descriptor.ShaderRegister = i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = static_cast<UINT>(params.size());
    rs_desc.pParameters = params.empty() ? nullptr : params.data();
    rs_desc.NumStaticSamplers = 0;
    rs_desc.pStaticSamplers = nullptr;
    rs_desc.Flags = desc->Flags
        ? static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(desc->Flags)
        : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    HRESULT hr = CallSystemD3D12SerializeRootSignature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
    if (err_blob) err_blob->Release();
    if (FAILED(hr) || !sig_blob) {
        if (sig_blob) sig_blob->Release();
        return static_cast<int32_t>(hr);
    }

    hr = native_dev->CreateRootSignature(
        0,
        sig_blob->GetBufferPointer(),
        sig_blob->GetBufferSize(),
        __uuidof(ID3D12RootSignature),
        reinterpret_cast<void**>(&entry->native));
    sig_blob->Release();
    if (FAILED(hr) || !entry->native) {
        return static_cast<int32_t>(hr);
    }
#endif

    const uint64_t handle = dev.next_root_sig_handle++;
    dev.root_signatures[handle] = std::move(entry);
    *out_root_sig_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyRootSignature(uint64_t device_handle, uint64_t root_sig_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->root_signatures.erase(root_sig_handle) > 0 ? 0 : -2;
}

int32_t D3D12XboxCreateDescriptorHeap(
    uint64_t device_handle,
    const D3D12XBOX_DESCRIPTOR_HEAP_DESC* desc,
    uint64_t* out_heap_handle) {
    if (!desc || desc->NumDescriptors == 0 || !out_heap_handle) {
        return -1;
    }
    *out_heap_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<DescriptorHeapEntry>();
    entry->desc = *desc;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev) {
        return -3;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hdesc{};
    hdesc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(desc->Type);
    hdesc.NumDescriptors = desc->NumDescriptors;
    hdesc.Flags = (desc->ShaderVisible &&
                   (desc->Type == D3D12XBOX_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                    desc->Type == D3D12XBOX_DESCRIPTOR_HEAP_TYPE_SAMPLER))
                      ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                      : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hdesc.NodeMask = 0;
    HRESULT hr = native_dev->CreateDescriptorHeap(
        &hdesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&entry->native));
    if (FAILED(hr) || !entry->native) {
        return static_cast<int32_t>(hr);
    }
#endif

    const uint64_t handle = dev.next_heap_handle++;
    dev.descriptor_heaps[handle] = std::move(entry);
    *out_heap_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyDescriptorHeap(uint64_t device_handle, uint64_t heap_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->descriptor_heaps.erase(heap_handle) > 0 ? 0 : -2;
}

uint32_t D3D12XboxGetDescriptorHandleIncrementSize(uint64_t device_handle, uint32_t heap_type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return 0;
    }
#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (native_dev) {
        return native_dev->GetDescriptorHandleIncrementSize(
            static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heap_type));
    }
#endif
    (void)heap_type;
    return 32;
}

int32_t D3D12XboxCreateGraphicsPipelineState(
    uint64_t device_handle,
    const D3D12XBOX_GRAPHICS_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->VS.Bytecode || desc->VS.BytecodeLength == 0 ||
        !desc->PS.Bytecode || desc->PS.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto rs_it = dev.root_signatures.find(desc->RootSignatureHandle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

    std::vector<uint8_t> vs_in(
        static_cast<const uint8_t*>(desc->VS.Bytecode),
        static_cast<const uint8_t*>(desc->VS.Bytecode) + desc->VS.BytecodeLength);
    std::vector<uint8_t> ps_in(
        static_cast<const uint8_t*>(desc->PS.Bytecode),
        static_cast<const uint8_t*>(desc->PS.Bytecode) + desc->PS.BytecodeLength);

    scarlettx::ShaderTranslationResult vs_tr{};
    scarlettx::ShaderTranslationResult ps_tr{};
    if (auto st = scarlettx::TranslateXboxShaderToPc(vs_in, vs_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (auto st = scarlettx::TranslateXboxShaderToPc(ps_in, ps_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    const uint32_t rtv_fmt = desc->RtvFormat ? desc->RtvFormat : 28u;
    const uint64_t pso_key = vs_tr.translated_hash ^ (ps_tr.translated_hash * 1315423911ull) ^
                             (desc->RootSignatureHandle << 1) ^ static_cast<uint64_t>(rtv_fmt);

    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_compute = false;
    entry->root_sig_handle = desc->RootSignatureHandle;
    entry->rtv_format = rtv_fmt;
    entry->pso_key = pso_key;

#if defined(_WIN32)
    auto cache_it = dev.pso_cache.find(pso_key);
    if (cache_it != dev.pso_cache.end() && cache_it->second) {
        ++dev.pso_cache_hits;
        entry->native = cache_it->second;
        entry->native->AddRef();
    } else {
        ++dev.pso_cache_misses;
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (!native_dev || !rs_it->second->native) {
            return -3;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = rs_it->second->native;
        pso_desc.VS = {vs_tr.pc_bytecode.data(), vs_tr.pc_bytecode.size()};
        pso_desc.PS = {ps_tr.pc_bytecode.data(), ps_tr.pc_bytecode.size()};
        pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
        pso_desc.BlendState.IndependentBlendEnable = FALSE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = static_cast<DXGI_FORMAT>(rtv_fmt);
        pso_desc.SampleDesc.Count = desc->SampleCount ? desc->SampleCount : 1u;

        HRESULT hr = native_dev->CreateGraphicsPipelineState(
            &pso_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&entry->native));
        if (FAILED(hr) || !entry->native) {
            return static_cast<int32_t>(hr);
        }
        entry->native->AddRef();
        dev.pso_cache[pso_key] = entry->native;
    }
#endif

    const uint64_t handle = dev.next_pso_handle++;
    dev.psos[handle] = std::move(entry);
    *out_pso_handle = handle;
    return 0;
}

int32_t D3D12XboxCreateComputePipelineState(
    uint64_t device_handle,
    const D3D12XBOX_COMPUTE_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->CS.Bytecode || desc->CS.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto rs_it = dev.root_signatures.find(desc->RootSignatureHandle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

    std::vector<uint8_t> cs_in(
        static_cast<const uint8_t*>(desc->CS.Bytecode),
        static_cast<const uint8_t*>(desc->CS.Bytecode) + desc->CS.BytecodeLength);
    scarlettx::ShaderTranslationResult cs_tr{};
    if (auto st = scarlettx::TranslateXboxShaderToPc(cs_in, cs_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    const uint64_t pso_key = cs_tr.translated_hash ^ (desc->RootSignatureHandle << 3) ^ 0xC0DE0001ull;
    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_compute = true;
    entry->root_sig_handle = desc->RootSignatureHandle;
    entry->pso_key = pso_key;

#if defined(_WIN32)
    auto cache_it = dev.pso_cache.find(pso_key);
    if (cache_it != dev.pso_cache.end() && cache_it->second) {
        ++dev.pso_cache_hits;
        entry->native = cache_it->second;
        entry->native->AddRef();
    } else {
        ++dev.pso_cache_misses;
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (!native_dev || !rs_it->second->native) {
            return -3;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = rs_it->second->native;
        pso_desc.CS = {cs_tr.pc_bytecode.data(), cs_tr.pc_bytecode.size()};
        HRESULT hr = native_dev->CreateComputePipelineState(
            &pso_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&entry->native));
        if (FAILED(hr) || !entry->native) {
            return static_cast<int32_t>(hr);
        }
        entry->native->AddRef();
        dev.pso_cache[pso_key] = entry->native;
    }
#endif

    const uint64_t handle = dev.next_pso_handle++;
    dev.psos[handle] = std::move(entry);
    *out_pso_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyPipelineState(uint64_t device_handle, uint64_t pso_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->psos.erase(pso_handle) > 0 ? 0 : -2;
}

int32_t D3D12XboxDispatchCompute(
    uint64_t device_handle,
    uint64_t pso_handle,
    uint32_t group_x,
    uint32_t group_y,
    uint32_t group_z,
    const uint32_t* root_constants,
    uint32_t root_constant_count,
    void* out_uav_data,
    uint32_t uav_byte_size) {
    if (group_x == 0 || group_y == 0 || group_z == 0 || !out_uav_data || uav_byte_size == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto pso_it = dev.psos.find(pso_handle);
    if (pso_it == dev.psos.end() || !pso_it->second->is_compute) {
        return -2;
    }
    auto rs_it = dev.root_signatures.find(pso_it->second->root_sig_handle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev || !pso_it->second->native || !rs_it->second->native) {
        return -3;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* uav_buf = nullptr;
    ID3D12Resource* rb_buf = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb_buf) rb_buf->Release();
        if (uav_buf) uav_buf->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
    };

    const uint64_t aligned_bytes = AlignUp(uav_byte_size, 256);

    HRESULT hr = native_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso_it->second->native, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC uav_desc{};
    uav_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uav_desc.Width = aligned_bytes;
    uav_desc.Height = 1;
    uav_desc.DepthOrArraySize = 1;
    uav_desc.MipLevels = 1;
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.SampleDesc.Count = 1;
    uav_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uav_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = native_dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&uav_buf));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc = uav_desc;
    rb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = native_dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb_buf));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    cmd->SetComputeRootSignature(rs_it->second->native);
    if (rs_it->second->constants_param_idx >= 0 && root_constants && root_constant_count > 0) {
        const uint32_t n = std::min(root_constant_count, rs_it->second->desc.NumRootConstants32Bit);
        cmd->SetComputeRoot32BitConstants(
            static_cast<UINT>(rs_it->second->constants_param_idx), n, root_constants, 0);
    }
    if (rs_it->second->uav0_param_idx >= 0) {
        cmd->SetComputeRootUnorderedAccessView(
            static_cast<UINT>(rs_it->second->uav0_param_idx), uav_buf->GetGPUVirtualAddress());
    }
    cmd->Dispatch(group_x, group_y, group_z);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = uav_buf;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
    cmd->CopyBufferRegion(rb_buf, 0, uav_buf, 0, uav_byte_size);

    hr = cmd->Close();
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    g_submit_count.fetch_add(1, std::memory_order_relaxed);

    hr = native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { cleanup(); return -4; }
    hr = queue->Signal(fence, 1);
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }

    void* mapped = nullptr;
    D3D12_RANGE range{0, uav_byte_size};
    hr = rb_buf->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped) { cleanup(); return static_cast<int32_t>(hr); }
    std::memcpy(out_uav_data, mapped, uav_byte_size);
    D3D12_RANGE empty_range{0, 0};
    rb_buf->Unmap(0, &empty_range);

    cleanup();
    return 0;
#else
    (void)root_constants;
    (void)root_constant_count;
    return -3;
#endif
}

int32_t D3D12XboxRenderWithPipelineState(
    uint64_t device_handle,
    uint64_t pso_handle,
    uint32_t width,
    uint32_t height,
    uint32_t vertex_count,
    const char* optional_ppm_path,
    uint64_t* out_pixel_hash) {
    if (width == 0 || height == 0 || vertex_count == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto pso_it = dev.psos.find(pso_handle);
    if (pso_it == dev.psos.end() || pso_it->second->is_compute) {
        return -2;
    }
    auto rs_it = dev.root_signatures.find(pso_it->second->root_sig_handle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev || !pso_it->second->native || !rs_it->second->native) {
        return -3;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* rt = nullptr;
    ID3D12Resource* rb = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb) rb->Release();
        if (rt) rt->Release();
        if (rtv_heap) rtv_heap->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
    };

    HRESULT hr = native_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso_it->second->native, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    hr = native_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtv_heap));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = width;
    rt_desc.Height = height;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = static_cast<DXGI_FORMAT>(pso_it->second->rtv_format);
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = rt_desc.Format;
    cv.Color[0] = 0.05f;
    cv.Color[1] = 0.07f;
    cv.Color[2] = 0.11f;
    cv.Color[3] = 1.0f;

    hr = native_dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rt));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    native_dev->CreateRenderTargetView(rt, nullptr, rtv_handle);

    const uint64_t row_pitch = AlignUp(static_cast<uint64_t>(width) * 4ull, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t rb_size = row_pitch * height;
    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = rb_size;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.Format = DXGI_FORMAT_UNKNOWN;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = native_dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    cmd->SetGraphicsRootSignature(rs_it->second->native);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv_handle, cv.Color, 0, nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(vertex_count, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = rt_desc.Format;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(row_pitch);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    hr = cmd->Close();
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    g_submit_count.fetch_add(1, std::memory_order_relaxed);

    hr = native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { cleanup(); return -4; }
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }

    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(rb_size)};
    hr = rb->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped) { cleanup(); return static_cast<int32_t>(hr); }

    scarlettx::FrameBufferRgba8 frame{};
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<size_t>(width) * height * 4ull);
    const auto* src_bytes = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * width * 4ull,
                    src_bytes + static_cast<size_t>(y) * row_pitch,
                    static_cast<size_t>(width) * 4ull);
    }
    D3D12_RANGE empty_range{0, 0};
    rb->Unmap(0, &empty_range);

    if (out_pixel_hash) {
        *out_pixel_hash = frame.ComputeFnv1aHash();
    }
    if (optional_ppm_path && optional_ppm_path[0] != '\0') {
        auto st = frame.WritePpm(optional_ppm_path);
        if (!st.ok()) {
            cleanup();
            return static_cast<int32_t>(st.code);
        }
    }
    cleanup();
    return 0;
#else
    (void)optional_ppm_path;
    (void)out_pixel_hash;
    return -3;
#endif
}

int32_t D3D12XboxCreateRaytracingPipelineState(
    uint64_t device_handle,
    const D3D12XBOX_RAYTRACING_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->DxilLibrary.Bytecode ||
        desc->DxilLibrary.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;

    const auto* bytes = static_cast<const uint8_t*>(desc->DxilLibrary.Bytecode);
    std::vector<uint8_t> raw(bytes, bytes + desc->DxilLibrary.BytecodeLength);
    scarlettx::ShaderTranslationResult translated{};
    auto st = scarlettx::TranslateXboxShaderToPc(raw, translated);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (translated.source_info.kind != scarlettx::ShaderContainerKind::Dxil) {
        return static_cast<int32_t>(scarlettx::StatusCode::ShaderError);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    if (desc->GlobalRootSignatureHandle != 0 &&
        dev.root_signatures.find(desc->GlobalRootSignatureHandle) == dev.root_signatures.end()) {
        return -2;
    }

    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_raytracing = true;
    entry->root_sig_handle = desc->GlobalRootSignatureHandle;
    entry->pso_key = translated.translated_hash ^
                     (static_cast<uint64_t>(desc->MaxPayloadSizeInBytes) << 32) ^
                     desc->MaxTraceRecursionDepth;
    entry->max_payload_bytes = desc->MaxPayloadSizeInBytes;
    entry->max_attribute_bytes = desc->MaxAttributeSizeInBytes;
    entry->max_recursion_depth = desc->MaxTraceRecursionDepth;
#if defined(_WIN32)
    auto rt_cache_it = dev.rt_pso_cache.find(entry->pso_key);
    if (rt_cache_it != dev.rt_pso_cache.end() && rt_cache_it->second) {
        ++dev.pso_cache_hits;
        entry->rt_state_object = rt_cache_it->second;
        entry->rt_state_object->AddRef();
    } else {
        ++dev.pso_cache_misses;
        auto* dev5 = static_cast<ID3D12Device5*>(dev.backend->NativeDevice5());
        if (dev5 && dev.backend->AdapterInfo().raytracing_tier >= 10 &&
            translated.pc_bytecode.size() >= 32u) {
            D3D12_DXIL_LIBRARY_DESC lib_desc{};
            lib_desc.DXILLibrary.pShaderBytecode = translated.pc_bytecode.data();
            lib_desc.DXILLibrary.BytecodeLength = translated.pc_bytecode.size();

            D3D12_RAYTRACING_SHADER_CONFIG shader_cfg{};
            shader_cfg.MaxPayloadSizeInBytes = std::max<UINT>(4u, desc->MaxPayloadSizeInBytes);
            shader_cfg.MaxAttributeSizeInBytes =
                desc->MaxAttributeSizeInBytes ? desc->MaxAttributeSizeInBytes : 8u;

            D3D12_RAYTRACING_PIPELINE_CONFIG pipe_cfg{};
            const UINT req_rec = desc->MaxTraceRecursionDepth ? desc->MaxTraceRecursionDepth : 1u;
            pipe_cfg.MaxTraceRecursionDepth = std::min<UINT>(req_rec, 31u);

            D3D12_GLOBAL_ROOT_SIGNATURE global_rs{};
            std::vector<D3D12_STATE_SUBOBJECT> subobjs;
            subobjs.reserve(4);
            subobjs.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib_desc});
            subobjs.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_cfg});
            subobjs.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipe_cfg});
            if (desc->GlobalRootSignatureHandle != 0) {
                auto rs_it = dev.root_signatures.find(desc->GlobalRootSignatureHandle);
                if (rs_it != dev.root_signatures.end() && rs_it->second->native) {
                    global_rs.pGlobalRootSignature = rs_it->second->native;
                    subobjs.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_rs});
                }
            }

            D3D12_STATE_OBJECT_DESC so_desc{};
            so_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
            so_desc.NumSubobjects = static_cast<UINT>(subobjs.size());
            so_desc.pSubobjects = subobjs.data();

            ID3D12StateObject* created_so = nullptr;
            if (SUCCEEDED(dev5->CreateStateObject(&so_desc, IID_PPV_ARGS(&created_so))) && created_so) {
                entry->rt_state_object = created_so;
                entry->rt_state_object->AddRef();
                dev.rt_pso_cache[entry->pso_key] = entry->rt_state_object;
            }
        }
    }
#else
    ++dev.pso_cache_misses;
#endif
    const uint64_t h = dev.next_pso_handle++;
    dev.psos.emplace(h, std::move(entry));
    *out_pso_handle = h;
    return 0;
}

int32_t D3D12XboxGetPipelineCacheStats(
    uint64_t device_handle,
    D3D12XBOX_PIPELINE_CACHE_STATS* out_stats) {
    if (!out_stats) {
        return -1;
    }
    std::memset(out_stats, 0, sizeof(*out_stats));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& dev = *it->second;
    out_stats->CacheHits = dev.pso_cache_hits;
    out_stats->CacheMisses = dev.pso_cache_misses;
    out_stats->LiveRootSignatures = static_cast<uint32_t>(dev.root_signatures.size());
    out_stats->LiveDescriptorHeaps = static_cast<uint32_t>(dev.descriptor_heaps.size());
    for (const auto& kv : dev.psos) {
        if (kv.second->is_raytracing) {
            ++out_stats->LiveRaytracingPsos;
        } else if (kv.second->is_compute) {
            ++out_stats->LiveComputePsos;
        } else {
            ++out_stats->LiveGraphicsPsos;
        }
    }
    return 0;
}

int32_t D3D12XboxWriteBreadcrumb(
    uint64_t device_handle,
    uint64_t cmd_list_id,
    uint64_t marker_value,
    const char* tag) {
    (void)device_handle;
    g_gpu_profiler.WriteBreadcrumb(cmd_list_id, marker_value, tag);
    return 0;
}

int32_t D3D12XboxRecordFrameTimestamp(
    uint64_t device_handle,
    uint64_t frame_duration_us) {
    (void)device_handle;
    g_gpu_profiler.RecordDurationUs(frame_duration_us);
    return 0;
}

int32_t D3D12XboxGetGpuStabilityReport(
    uint64_t device_handle,
    D3D12XBOX_GPU_STABILITY_REPORT* out_report) {
    if (!out_report) {
        return -1;
    }
    std::memset(out_report, 0, sizeof(*out_report));
    out_report->LiveCommandLists = g_live_command_lists.load(std::memory_order_relaxed);
    out_report->CompletedSubmits = g_submit_count.load(std::memory_order_relaxed);
    out_report->PsoCacheHits = g_proxy_pso_cache_hits.load(std::memory_order_relaxed);
    out_report->PsoCacheMisses = g_proxy_pso_cache_misses.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(device_handle);
        if (it != g_devices.end()) {
            out_report->PsoCacheHits += it->second->pso_cache_hits;
            out_report->PsoCacheMisses += it->second->pso_cache_misses;
#if defined(_WIN32)
            if (auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice())) {
                out_report->DeviceRemovedReason = static_cast<int32_t>(native_dev->GetDeviceRemovedReason());
            }
#endif
        } else {
            int32_t proxy_reason = 0;
            if (TryGetDeviceProxyRemovedReason(device_handle, &proxy_reason)) {
                out_report->DeviceRemovedReason = proxy_reason;
            }
        }
    }

    g_gpu_profiler.FillReport(*out_report);
    return 0;
}

int32_t D3D12XboxQueryVrsCapabilities(
    uint64_t device_handle,
    D3D12XBOX_VRS_CAPABILITIES* out_caps) {
    if (!out_caps) return -1;
    std::memset(out_caps, 0, sizeof(*out_caps));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (!native_dev) return -3;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 opts6{};
    if (SUCCEEDED(native_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opts6, sizeof(opts6)))) {
        out_caps->VariableShadingRateTier = static_cast<uint32_t>(opts6.VariableShadingRateTier);
        out_caps->AdditionalShadingRatesSupported = opts6.AdditionalShadingRatesSupported ? 1u : 0u;
        out_caps->PerPrimitiveShadingRateSupported =
            opts6.PerPrimitiveShadingRateSupportedWithViewportIndexing ? 1u : 0u;
        out_caps->ShadingRateImageTileSize = opts6.ShadingRateImageTileSize > 0 ? opts6.ShadingRateImageTileSize : 16u;
    } else {
        out_caps->ShadingRateImageTileSize = 16u;
    }
    return 0;
#else
    return -3;
#endif
}

int32_t D3D12XboxSetShadingRate(
    uint64_t device_handle,
    uint32_t base_shading_rate,
    const uint32_t* combiners_pair) {
    if (base_shading_rate > 0xAu) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (!native_dev) return -3;

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 opts6{};
    const bool has_hw_vrs =
        SUCCEEDED(native_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opts6, sizeof(opts6))) &&
        opts6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
    if (!has_hw_vrs) {
        return 0;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12GraphicsCommandList5* cl5 = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (cl5) cl5->Release();
        if (cl) cl->Release();
        if (alloc) alloc->Release();
        if (q) q->Release();
    };

    if (FAILED(native_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
        cleanup();
        return -3;
    }

    if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl5))) && cl5) {
        auto rate = static_cast<D3D12_SHADING_RATE>(base_shading_rate);
        if (!opts6.AdditionalShadingRatesSupported && rate > D3D12_SHADING_RATE_2X2) {
            rate = D3D12_SHADING_RATE_2X2;
        }
        D3D12_SHADING_RATE_COMBINER combs[2] = {
            D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
            D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
        };
        if (combiners_pair) {
            combs[0] = static_cast<D3D12_SHADING_RATE_COMBINER>(combiners_pair[0] % 5u);
            combs[1] = static_cast<D3D12_SHADING_RATE_COMBINER>(combiners_pair[1] % 5u);
        }
        cl5->RSSetShadingRate(rate, combs);
    }
    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    if (SUCCEEDED(native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        q->Signal(fence, 1ull);
        if (ev && fence->GetCompletedValue() < 1ull) {
            fence->SetEventOnCompletion(1ull, ev);
            WaitForSingleObject(ev, 5000);
        }
    }
    cleanup();
    return 0;
#else
    (void)combiners_pair;
    return -3;
#endif
}

int32_t D3D12XboxCreateReservedResource(
    uint64_t device_handle,
    uint64_t byte_size_aligned_64kb,
    uint64_t* out_reserved_handle) {
    if (!out_reserved_handle || byte_size_aligned_64kb == 0) return -1;
    *out_reserved_handle = 0;
    const uint64_t aligned_bytes = (byte_size_aligned_64kb + 65535ull) & ~65535ull;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (!native_dev) return -3;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = aligned_bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto entry = std::make_unique<ReservedResourceEntry>();
    entry->byte_size = aligned_bytes;

    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if (SUCCEEDED(native_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))) &&
        opts.TiledResourcesTier != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
        SUCCEEDED(native_dev->CreateReservedResource(&rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&entry->resource))) &&
        entry->resource) {
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = aligned_bytes;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        if (SUCCEEDED(native_dev->CreateHeap(&hd, IID_PPV_ARGS(&entry->tile_heap))) && entry->tile_heap) {
            entry->is_true_reserved = true;
        } else {
            entry->resource->Release();
            entry->resource = nullptr;
        }
    }

    if (!entry->resource) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(native_dev->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&entry->resource)))) {
            return -3;
        }
    }

    const uint64_t h = it->second->next_reserved_handle++;
    it->second->reserved_resources.emplace(h, std::move(entry));
    *out_reserved_handle = h;
    return 0;
#else
    return -3;
#endif
}

int32_t D3D12XboxUpdateTileMappings(
    uint64_t device_handle,
    uint64_t reserved_handle,
    uint32_t start_tile_index,
    uint32_t num_tiles_64kb,
    const void* optional_upload_data,
    uint64_t upload_byte_size,
    void* optional_readback_data,
    uint64_t readback_byte_size) {
    if (num_tiles_64kb == 0) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
    auto rit = it->second->reserved_resources.find(reserved_handle);
    if (rit == it->second->reserved_resources.end() || !rit->second->resource) return -2;

    const uint64_t total_tiles = rit->second->byte_size / 65536ull;
    if (static_cast<uint64_t>(start_tile_index) + num_tiles_64kb > total_tiles) return -1;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (!native_dev) return -3;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Resource* up_buf = nullptr;
    ID3D12Resource* rb_buf = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb_buf) rb_buf->Release();
        if (up_buf) up_buf->Release();
        if (cl) cl->Release();
        if (alloc) alloc->Release();
        if (q) q->Release();
    };

    if (FAILED(native_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) {
        cleanup();
        return -3;
    }

    if (rit->second->is_true_reserved && rit->second->tile_heap) {
        D3D12_TILED_RESOURCE_COORDINATE coord{start_tile_index, 0u, 0u, 0u};
        D3D12_TILE_REGION_SIZE region{};
        region.NumTiles = num_tiles_64kb;
        region.UseBox = FALSE;
        const D3D12_TILE_RANGE_FLAGS range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        const UINT heap_offset = start_tile_index;
        const UINT range_cnt = num_tiles_64kb;
        q->UpdateTileMappings(rit->second->resource, 1u, &coord, &region,
                              rit->second->tile_heap, 1u, &range_flag, &heap_offset, &range_cnt,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    }

    const uint64_t byte_offset = static_cast<uint64_t>(start_tile_index) * 65536ull;
    const uint64_t max_region_bytes = static_cast<uint64_t>(num_tiles_64kb) * 65536ull;

    if ((optional_upload_data && upload_byte_size > 0) || (optional_readback_data && readback_byte_size > 0)) {
        if (upload_byte_size > max_region_bytes || readback_byte_size > max_region_bytes) {
            cleanup();
            return -1;
        }
        if (FAILED(native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
            cleanup();
            return -3;
        }

        if (optional_upload_data && upload_byte_size > 0) {
            D3D12_HEAP_PROPERTIES up_hp{};
            up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = upload_byte_size;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(native_dev->CreateCommittedResource(&up_hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up_buf)))) {
                cleanup();
                return -3;
            }
            void* mapped = nullptr;
            if (SUCCEEDED(up_buf->Map(0, nullptr, &mapped)) && mapped) {
                std::memcpy(mapped, optional_upload_data, static_cast<size_t>(upload_byte_size));
                up_buf->Unmap(0, nullptr);
            }
            cl->CopyBufferRegion(rit->second->resource, byte_offset, up_buf, 0ull, upload_byte_size);
        }

        if (optional_readback_data && readback_byte_size > 0) {
            if (optional_upload_data && upload_byte_size > 0) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = rit->second->resource;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                cl->ResourceBarrier(1, &b);
            }
            D3D12_HEAP_PROPERTIES rb_hp{};
            rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = readback_byte_size;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(native_dev->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb_buf)))) {
                cleanup();
                return -3;
            }
            cl->CopyBufferRegion(rb_buf, 0ull, rit->second->resource, byte_offset, readback_byte_size);
            D3D12_RESOURCE_BARRIER back{};
            back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            back.Transition.pResource = rit->second->resource;
            back.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            back.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            cl->ResourceBarrier(1, &back);
        }

        cl->Close();
        ID3D12CommandList* lists[] = {cl};
        q->ExecuteCommandLists(1, lists);
    }

    if (FAILED(native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        cleanup();
        return -3;
    }
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->Signal(fence, 1ull);
    if (ev && fence->GetCompletedValue() < 1ull) {
        fence->SetEventOnCompletion(1ull, ev);
        WaitForSingleObject(ev, 5000);
    }

    if (optional_readback_data && readback_byte_size > 0 && rb_buf) {
        void* mapped = nullptr;
        if (SUCCEEDED(rb_buf->Map(0, nullptr, &mapped)) && mapped) {
            std::memcpy(optional_readback_data, mapped, static_cast<size_t>(readback_byte_size));
            rb_buf->Unmap(0, nullptr);
        }
    }

    cleanup();
    return 0;
#else
    (void)optional_upload_data; (void)upload_byte_size;
    (void)optional_readback_data; (void)readback_byte_size;
    return -3;
#endif
}

int32_t D3D12XboxDestroyReservedResource(
    uint64_t device_handle,
    uint64_t reserved_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
    return it->second->reserved_resources.erase(reserved_handle) ? 0 : -2;
}

int32_t D3D12XboxExecuteIndirectWithCount(
    uint64_t device_handle,
    uint64_t compute_pso_handle,
    uint32_t max_command_count,
    const uint32_t* dispatch_triplets_xyz,
    uint32_t gpu_count_buffer_value,
    void* inout_uav_data,
    uint32_t uav_byte_size) {
    if (max_command_count == 0 || !dispatch_triplets_xyz || !inout_uav_data || uav_byte_size == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
    auto& dev = *it->second;
    auto pso_it = dev.psos.find(compute_pso_handle);
    if (pso_it == dev.psos.end() || !pso_it->second->is_compute) return -2;
    auto rs_it = dev.root_signatures.find(pso_it->second->root_sig_handle);
    if (rs_it == dev.root_signatures.end()) return -2;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev || !pso_it->second->native || !rs_it->second->native) return -3;

    D3D12_INDIRECT_ARGUMENT_DESC arg_desc{};
    arg_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sig_desc{};
    sig_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sig_desc.NumArgumentDescs = 1;
    sig_desc.pArgumentDescs = &arg_desc;

    ID3D12CommandSignature* cmd_sig = nullptr;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Resource* up_args = nullptr;
    ID3D12Resource* up_cnt = nullptr;
    ID3D12Resource* up_uav = nullptr;
    ID3D12Resource* def_args = nullptr;
    ID3D12Resource* def_cnt = nullptr;
    ID3D12Resource* def_uav = nullptr;
    ID3D12Resource* rb_uav = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb_uav) rb_uav->Release();
        if (def_uav) def_uav->Release();
        if (def_cnt) def_cnt->Release();
        if (def_args) def_args->Release();
        if (up_uav) up_uav->Release();
        if (up_cnt) up_cnt->Release();
        if (up_args) up_args->Release();
        if (cl) cl->Release();
        if (alloc) alloc->Release();
        if (q) q->Release();
        if (cmd_sig) cmd_sig->Release();
    };

    if (FAILED(native_dev->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&cmd_sig))) || !cmd_sig) {
        cleanup();
        return -3;
    }

    auto make_buf = [&](D3D12_HEAP_TYPE htype, uint64_t bytes, D3D12_RESOURCE_FLAGS flags,
                        D3D12_RESOURCE_STATES state, ID3D12Resource** out) -> bool {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = htype;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = std::max<uint64_t>(16ull, (bytes + 15ull) & ~15ull);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = flags;
        return SUCCEEDED(native_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(out)));
    };

    const uint64_t args_bytes = static_cast<uint64_t>(max_command_count) * sizeof(D3D12_DISPATCH_ARGUMENTS);
    if (!make_buf(D3D12_HEAP_TYPE_UPLOAD, args_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &up_args) ||
        !make_buf(D3D12_HEAP_TYPE_UPLOAD, 16ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &up_cnt) ||
        !make_buf(D3D12_HEAP_TYPE_UPLOAD, uav_byte_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &up_uav) ||
        !make_buf(D3D12_HEAP_TYPE_DEFAULT, args_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, &def_args) ||
        !make_buf(D3D12_HEAP_TYPE_DEFAULT, 16ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, &def_cnt) ||
        !make_buf(D3D12_HEAP_TYPE_DEFAULT, uav_byte_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &def_uav) ||
        !make_buf(D3D12_HEAP_TYPE_READBACK, uav_byte_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &rb_uav)) {
        cleanup();
        return -3;
    }

    void* mapped = nullptr;
    if (SUCCEEDED(up_args->Map(0, nullptr, &mapped)) && mapped) {
        std::memcpy(mapped, dispatch_triplets_xyz, static_cast<size_t>(args_bytes));
        up_args->Unmap(0, nullptr);
    }
    mapped = nullptr;
    if (SUCCEEDED(up_cnt->Map(0, nullptr, &mapped)) && mapped) {
        std::memset(mapped, 0, 16u);
        std::memcpy(mapped, &gpu_count_buffer_value, sizeof(uint32_t));
        up_cnt->Unmap(0, nullptr);
    }
    mapped = nullptr;
    if (SUCCEEDED(up_uav->Map(0, nullptr, &mapped)) && mapped) {
        std::memcpy(mapped, inout_uav_data, uav_byte_size);
        up_uav->Unmap(0, nullptr);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(native_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso_it->second->native, IID_PPV_ARGS(&cl)))) {
        cleanup();
        return -3;
    }

    cl->CopyBufferRegion(def_args, 0, up_args, 0, args_bytes);
    cl->CopyBufferRegion(def_cnt, 0, up_cnt, 0, sizeof(uint32_t));
    cl->CopyBufferRegion(def_uav, 0, up_uav, 0, uav_byte_size);

    D3D12_RESOURCE_BARRIER pre[3]{};
    pre[0] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
              {def_args, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT}};
    pre[1] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
              {def_cnt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT}};
    pre[2] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
              {def_uav, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}};
    cl->ResourceBarrier(3, pre);

    cl->SetComputeRootSignature(rs_it->second->native);
    const auto& rs_desc = rs_it->second->desc;
    uint32_t uav_root_idx = 0;
    if (rs_desc.NumRootConstants32Bit > 0) ++uav_root_idx;
    uav_root_idx += rs_desc.NumCbvs + rs_desc.NumSrvs;
    if (rs_desc.NumUavs > 0) {
        cl->SetComputeRootUnorderedAccessView(uav_root_idx, def_uav->GetGPUVirtualAddress());
    }

    cl->ExecuteIndirect(cmd_sig, max_command_count, def_args, 0ull, def_cnt, 0ull);

    D3D12_RESOURCE_BARRIER post{};
    post.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post.Transition.pResource = def_uav;
    post.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    post.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &post);
    cl->CopyBufferRegion(rb_uav, 0, def_uav, 0, uav_byte_size);

    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    g_submit_count.fetch_add(1, std::memory_order_relaxed);

    if (FAILED(native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        cleanup();
        return -3;
    }
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->Signal(fence, 1ull);
    if (ev && fence->GetCompletedValue() < 1ull) {
        fence->SetEventOnCompletion(1ull, ev);
        WaitForSingleObject(ev, 5000);
    }

    mapped = nullptr;
    if (SUCCEEDED(rb_uav->Map(0, nullptr, &mapped)) && mapped) {
        std::memcpy(inout_uav_data, mapped, uav_byte_size);
        rb_uav->Unmap(0, nullptr);
    }
    cleanup();
    return 0;
#else
    return -3;
#endif
}

int32_t D3D12XboxCreateWorkGraph(
    uint64_t device_handle,
    const D3D12XBOX_WORK_GRAPH_DESC* desc,
    uint64_t* out_work_graph_handle) {
    if (!desc || !out_work_graph_handle ||
        !desc->ProducerCS.Bytecode || desc->ProducerCS.BytecodeLength == 0 ||
        !desc->ConsumerCS.Bytecode || desc->ConsumerCS.BytecodeLength == 0) {
        return -1;
    }
    *out_work_graph_handle = 0;

    std::vector<uint8_t> prod_raw(
        static_cast<const uint8_t*>(desc->ProducerCS.Bytecode),
        static_cast<const uint8_t*>(desc->ProducerCS.Bytecode) + desc->ProducerCS.BytecodeLength);
    std::vector<uint8_t> cons_raw(
        static_cast<const uint8_t*>(desc->ConsumerCS.Bytecode),
        static_cast<const uint8_t*>(desc->ConsumerCS.Bytecode) + desc->ConsumerCS.BytecodeLength);

    scarlettx::ShaderTranslationResult prod_tr{}, cons_tr{};
    if (!scarlettx::TranslateXboxShaderToPc(prod_raw, prod_tr).ok() ||
        !scarlettx::TranslateXboxShaderToPc(cons_raw, cons_tr).ok()) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (!native_dev) return -3;

    auto wg = std::make_unique<WorkGraphEntry>();
    wg->max_records = std::max<uint32_t>(1u, desc->MaxNodeRecords);
    wg->record_stride = std::max<uint32_t>(4u, desc->RecordStrideBytes);

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    for (uint32_t u = 0; u < 3u; ++u) {
        params[1u + u].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1u + u].Descriptor.ShaderRegister = u;
        params[1u + u].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* rs_err = nullptr;
    if (FAILED(CallSystemD3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rs_blob, &rs_err)) || !rs_blob) {
        if (rs_err) rs_err->Release();
        return -3;
    }
    if (rs_err) rs_err->Release();
    HRESULT hr = native_dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&wg->root_sig));
    rs_blob->Release();
    if (FAILED(hr) || !wg->root_sig) return -3;

    D3D12_COMPUTE_PIPELINE_STATE_DESC prod_desc{};
    prod_desc.pRootSignature = wg->root_sig;
    prod_desc.CS = {prod_tr.pc_bytecode.data(), prod_tr.pc_bytecode.size()};
    D3D12_COMPUTE_PIPELINE_STATE_DESC cons_desc{};
    cons_desc.pRootSignature = wg->root_sig;
    cons_desc.CS = {cons_tr.pc_bytecode.data(), cons_tr.pc_bytecode.size()};

    if (FAILED(native_dev->CreateComputePipelineState(&prod_desc, IID_PPV_ARGS(&wg->producer_pso))) ||
        FAILED(native_dev->CreateComputePipelineState(&cons_desc, IID_PPV_ARGS(&wg->consumer_pso)))) {
        return -3;
    }

    D3D12_INDIRECT_ARGUMENT_DESC arg_desc{};
    arg_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sig_desc{};
    sig_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sig_desc.NumArgumentDescs = 1;
    sig_desc.pArgumentDescs = &arg_desc;
    if (FAILED(native_dev->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&wg->cmd_sig)))) {
        return -3;
    }

    const uint64_t h = it->second->next_work_graph_handle++;
    it->second->work_graphs.emplace(h, std::move(wg));
    *out_work_graph_handle = h;
    return 0;
#else
    return -3;
#endif
}

int32_t D3D12XboxDispatchWorkGraph(
    uint64_t device_handle,
    uint64_t work_graph_handle,
    uint32_t producer_groups_x,
    const uint32_t* root_constants,
    uint32_t root_constant_count,
    void* out_uav_data,
    uint32_t uav_byte_size) {
    if (!out_uav_data || uav_byte_size == 0) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
    auto wgit = it->second->work_graphs.find(work_graph_handle);
    if (wgit == it->second->work_graphs.end()) return -2;
    auto& wg = *wgit->second;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (!native_dev) return -3;

    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Resource* arg_buf = nullptr;
    ID3D12Resource* rec_buf = nullptr;
    ID3D12Resource* out_buf = nullptr;
    ID3D12Resource* rb_buf = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb_buf) rb_buf->Release();
        if (out_buf) out_buf->Release();
        if (rec_buf) rec_buf->Release();
        if (arg_buf) arg_buf->Release();
        if (cl) cl->Release();
        if (alloc) alloc->Release();
        if (q) q->Release();
    };

    auto make_buf = [&](D3D12_HEAP_TYPE htype, uint64_t bytes, D3D12_RESOURCE_FLAGS flags,
                        D3D12_RESOURCE_STATES state, ID3D12Resource** out) -> bool {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = htype;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = std::max<uint64_t>(16ull, (bytes + 15ull) & ~15ull);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = flags;
        return SUCCEEDED(native_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(out)));
    };

    const uint64_t rec_bytes = static_cast<uint64_t>(wg.max_records) * wg.record_stride;
    if (!make_buf(D3D12_HEAP_TYPE_DEFAULT, 16ull, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &arg_buf) ||
        !make_buf(D3D12_HEAP_TYPE_DEFAULT, rec_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &rec_buf) ||
        !make_buf(D3D12_HEAP_TYPE_DEFAULT, uav_byte_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &out_buf) ||
        !make_buf(D3D12_HEAP_TYPE_READBACK, uav_byte_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &rb_buf)) {
        cleanup();
        return -3;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(native_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, wg.producer_pso, IID_PPV_ARGS(&cl)))) {
        cleanup();
        return -3;
    }

    D3D12_RESOURCE_BARRIER to_uav[3]{};
    to_uav[0] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
                 {arg_buf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}};
    to_uav[1] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
                 {rec_buf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}};
    to_uav[2] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
                 {out_buf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}};
    cl->ResourceBarrier(3, to_uav);

    uint32_t consts[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < std::min<uint32_t>(4u, root_constant_count) && root_constants; ++i) {
        consts[i] = root_constants[i];
    }

    cl->SetComputeRootSignature(wg.root_sig);
    cl->SetComputeRoot32BitConstants(0, 4, consts, 0);
    cl->SetComputeRootUnorderedAccessView(1, arg_buf->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(2, rec_buf->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(3, out_buf->GetGPUVirtualAddress());
    cl->Dispatch(std::max<uint32_t>(1u, producer_groups_x), 1u, 1u);

    D3D12_RESOURCE_BARRIER mid[2]{};
    mid[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    mid[0].Transition.pResource = arg_buf;
    mid[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mid[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mid[0].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    mid[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    mid[1].UAV.pResource = rec_buf;
    cl->ResourceBarrier(2, mid);

    cl->SetPipelineState(wg.consumer_pso);
    cl->ExecuteIndirect(wg.cmd_sig, 1u, arg_buf, 0ull, nullptr, 0ull);

    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = out_buf;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &to_copy);
    cl->CopyBufferRegion(rb_buf, 0ull, out_buf, 0ull, uav_byte_size);

    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    g_submit_count.fetch_add(1, std::memory_order_relaxed);

    if (FAILED(native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        cleanup();
        return -3;
    }
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->Signal(fence, 1ull);
    if (ev && fence->GetCompletedValue() < 1ull) {
        fence->SetEventOnCompletion(1ull, ev);
        WaitForSingleObject(ev, 5000);
    }

    void* mapped = nullptr;
    if (SUCCEEDED(rb_buf->Map(0, nullptr, &mapped)) && mapped) {
        std::memcpy(out_uav_data, mapped, uav_byte_size);
        rb_buf->Unmap(0, nullptr);
    }
    cleanup();
    return 0;
#else
    (void)producer_groups_x; (void)root_constants; (void)root_constant_count;
    return -3;
#endif
}

int32_t D3D12XboxDestroyWorkGraph(
    uint64_t device_handle,
    uint64_t work_graph_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) return -2;
    return it->second->work_graphs.erase(work_graph_handle) ? 0 : -2;
}

}  // extern "C"
