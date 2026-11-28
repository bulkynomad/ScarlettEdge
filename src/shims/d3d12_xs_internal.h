#pragma once

// Internal shared declarations between d3d12_xs_shim.cpp (Retail Xbox COM Proxy +
// Retail ABI entry points) and d3d12_xs_synthetic.cpp (Synthetic uint64_t-handle
// test-harness exports).

#include "scarlettx/gpu/d3d12_xs_abi.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d12.h>
#endif

namespace scarlettx::d3d12_xs_internal {

inline uint64_t AlignUp(uint64_t v, uint64_t align) {
    return (v + align - 1ull) & ~(align - 1ull);
}

bool PreferWarpFromEnv(bool fallback);

#if defined(_WIN32)
HMODULE GetSystemD3D12Module();
HRESULT CallSystemD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppv);
HRESULT CallSystemD3D12GetDebugInterface(REFIID riid, void** ppv);
HRESULT CallSystemD3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* desc,
    D3D_ROOT_SIGNATURE_VERSION ver,
    ID3DBlob** blob,
    ID3DBlob** err);
HRESULT CallSystemD3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
    ID3DBlob** blob,
    ID3DBlob** err);
HRESULT CallSystemD3D12CreateRootSignatureDeserializer(
    LPCVOID src,
    SIZE_T size,
    REFIID riid,
    void** ppv);
HRESULT CallSystemD3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID src,
    SIZE_T size,
    REFIID riid,
    void** ppv);
HRESULT CallSystemD3D12EnableExperimentalFeatures(
    UINT numFeatures,
    const IID* pIIDs,
    void* pConfigurationStructs,
    UINT* pConfigurationStructSizes);
void EnqueueDelayedEventSignal(HANDLE hEvent, uint64_t offset_us);
#endif

struct GpuStabilityProfiler {
    static constexpr size_t kBreadcrumbCap = 64;
    static constexpr size_t kFrameHistCap = 256;

    mutable std::mutex mu;
    std::array<D3D12XBOX_GPU_BREADCRUMB_ENTRY, kBreadcrumbCap> breadcrumbs{};
    uint64_t total_breadcrumbs = 0;
    std::array<uint64_t, kFrameHistCap> frame_times_us{};
    uint64_t total_frames = 0;
    int64_t last_qpc_tick = 0;

    void WriteBreadcrumb(uint64_t cmd_list_id, uint64_t marker_value, const char* tag);
    void RecordDurationUs(uint64_t us);
    void RecordQpcTick();
    void FillReport(D3D12XBOX_GPU_STABILITY_REPORT& rep) const;
};

extern std::atomic<uint32_t> g_process_debug_flags;
extern std::atomic<uint32_t> g_live_command_lists;
extern std::atomic<uint64_t> g_submit_count;
extern std::atomic<uint64_t> g_proxy_pso_cache_hits;
extern std::atomic<uint64_t> g_proxy_pso_cache_misses;
extern GpuStabilityProfiler g_gpu_profiler;

bool TryGetDeviceProxyRemovedReason(uint64_t candidate_ptr, int32_t* out_reason);

}  // namespace scarlettx::d3d12_xs_internal
