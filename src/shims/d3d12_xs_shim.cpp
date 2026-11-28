#define SCARLETTX_BUILDING_D3D12_XS 1
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "d3d12_xs_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "scarlettx/core/flight_recorder.h"
#include "scarlettx/gpu/shader_ir.h"
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/runtime/emulator.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define SCARLETTX_RETURN_ADDRESS() _ReturnAddress()
#else
#define SCARLETTX_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace scarlettx::d3d12_xs_internal {

std::atomic<uint32_t> g_process_debug_flags{0};
std::atomic<uint32_t> g_live_command_lists{0};
std::atomic<uint64_t> g_submit_count{0};
std::atomic<uint64_t> g_proxy_pso_cache_hits{0};
std::atomic<uint64_t> g_proxy_pso_cache_misses{0};
GpuStabilityProfiler g_gpu_profiler;

bool PreferWarpFromEnv(bool fallback) {
    const char* v = std::getenv("SCARLETTX_PREFER_WARP");
    if (!v || v[0] == '\0') {
        return fallback;
    }
    return v[0] != '0';
}

#if defined(_WIN32)
HMODULE GetSystemD3D12Module() {
    static HMODULE mod = []() -> HMODULE {
        wchar_t sys_dir[MAX_PATH] = {};
        if (GetSystemDirectoryW(sys_dir, MAX_PATH) == 0) return nullptr;
        std::wstring path = std::wstring(sys_dir) + L"\\d3d12.dll";
        return LoadLibraryW(path.c_str());
    }();
    return mod;
}

HRESULT CallSystemD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppv) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12CreateDevice"));
    return fn ? fn(adapter, fl, riid, ppv) : E_FAIL;
}

HRESULT CallSystemD3D12GetDebugInterface(REFIID riid, void** ppv) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12GetDebugInterface"));
    return fn ? fn(riid, ppv) : E_FAIL;
}

HRESULT CallSystemD3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* desc,
    D3D_ROOT_SIGNATURE_VERSION ver,
    ID3DBlob** blob,
    ID3DBlob** err) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12SerializeRootSignature"));
    return fn ? fn(desc, ver, blob, err) : E_FAIL;
}

HRESULT CallSystemD3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
    ID3DBlob** blob,
    ID3DBlob** err) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12SerializeVersionedRootSignature"));
    return fn ? fn(desc, blob, err) : E_FAIL;
}

HRESULT CallSystemD3D12CreateRootSignatureDeserializer(
    LPCVOID src,
    SIZE_T size,
    REFIID riid,
    void** ppv) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12CreateRootSignatureDeserializer"));
    return fn ? fn(src, size, riid, ppv) : E_FAIL;
}

HRESULT CallSystemD3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID src,
    SIZE_T size,
    REFIID riid,
    void** ppv) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12CreateVersionedRootSignatureDeserializer"));
    return fn ? fn(src, size, riid, ppv) : E_FAIL;
}

HRESULT CallSystemD3D12EnableExperimentalFeatures(
    UINT numFeatures,
    const IID* pIIDs,
    void* pConfigurationStructs,
    UINT* pConfigurationStructSizes) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(UINT, const IID*, void*, UINT*);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12EnableExperimentalFeatures"));
    return fn ? fn(numFeatures, pIIDs, pConfigurationStructs, pConfigurationStructSizes) : E_NOINTERFACE;
}
#endif

void GpuStabilityProfiler::WriteBreadcrumb(uint64_t cmd_list_id, uint64_t marker_value, const char* tag) {
    std::lock_guard<std::mutex> lock(mu);
    D3D12XBOX_GPU_BREADCRUMB_ENTRY entry{};
    entry.CommandListId = cmd_list_id;
    entry.MarkerValue = marker_value;
    if (tag && *tag) {
        std::strncpy(entry.Tag, tag, sizeof(entry.Tag) - 1);
        entry.Tag[sizeof(entry.Tag) - 1] = '\0';
    }
    breadcrumbs[static_cast<size_t>(total_breadcrumbs % kBreadcrumbCap)] = entry;
    ++total_breadcrumbs;
    scarlettx::FlightRecorder::Instance().RecordGpuBreadcrumb(cmd_list_id, marker_value, tag);
}

void GpuStabilityProfiler::RecordDurationUs(uint64_t us) {
    if (us == 0) us = 1;
    std::lock_guard<std::mutex> lock(mu);
    frame_times_us[static_cast<size_t>(total_frames % kFrameHistCap)] = us;
    ++total_frames;
}

void GpuStabilityProfiler::RecordQpcTick() {
#if defined(_WIN32)
    static const int64_t freq = []() -> int64_t {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1000000ll;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    std::lock_guard<std::mutex> lock(mu);
    if (last_qpc_tick > 0 && now.QuadPart >= last_qpc_tick) {
        const uint64_t us = std::max<uint64_t>(
            1ull, static_cast<uint64_t>((now.QuadPart - last_qpc_tick) * 1000000ll / freq));
        frame_times_us[static_cast<size_t>(total_frames % kFrameHistCap)] = us;
        ++total_frames;
    }
    last_qpc_tick = now.QuadPart;
#endif
}

void GpuStabilityProfiler::FillReport(D3D12XBOX_GPU_STABILITY_REPORT& rep) const {
    std::lock_guard<std::mutex> lock(mu);
    rep.BreadcrumbCount = static_cast<uint32_t>(std::min<uint64_t>(total_breadcrumbs, 0xFFFFFFFFull));
    if (total_breadcrumbs > 0) {
        const auto& last = breadcrumbs[static_cast<size_t>((total_breadcrumbs - 1ull) % kBreadcrumbCap)];
        rep.LastBreadcrumbValue = last.MarkerValue;
        std::memcpy(rep.LastBreadcrumbTag, last.Tag, sizeof(rep.LastBreadcrumbTag));
    }
    rep.RecordedFrameCount = total_frames;
    if (total_frames > 0) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(total_frames, kFrameHistCap));
        std::vector<uint64_t> window(n);
        for (size_t i = 0; i < n; ++i) {
            const uint64_t idx = (total_frames > kFrameHistCap) ? (total_frames - n + i) : i;
            window[i] = frame_times_us[static_cast<size_t>(idx % kFrameHistCap)];
        }
        std::sort(window.begin(), window.end());
        rep.MinFrameTimeUs = window.front();
        rep.MaxFrameTimeUs = window.back();
        uint64_t sum = 0;
        for (uint64_t v : window) sum += v;
        rep.AvgFrameTimeUs = sum / static_cast<uint64_t>(n);
        const size_t p99_idx = std::min<size_t>(n - 1u, (n * 99u) / 100u);
        rep.P99FrameTimeUs = window[p99_idx];
    }
}

}  // namespace scarlettx::d3d12_xs_internal

namespace {

using scarlettx::d3d12_xs_internal::AlignUp;
using scarlettx::d3d12_xs_internal::g_gpu_profiler;
using scarlettx::d3d12_xs_internal::g_live_command_lists;
using scarlettx::d3d12_xs_internal::g_process_debug_flags;
using scarlettx::d3d12_xs_internal::g_proxy_pso_cache_hits;
using scarlettx::d3d12_xs_internal::g_proxy_pso_cache_misses;
using scarlettx::d3d12_xs_internal::g_submit_count;
using scarlettx::d3d12_xs_internal::GpuStabilityProfiler;
using scarlettx::d3d12_xs_internal::PreferWarpFromEnv;
#if defined(_WIN32)
using scarlettx::d3d12_xs_internal::CallSystemD3D12CreateDevice;
using scarlettx::d3d12_xs_internal::CallSystemD3D12CreateRootSignatureDeserializer;
using scarlettx::d3d12_xs_internal::CallSystemD3D12CreateVersionedRootSignatureDeserializer;
using scarlettx::d3d12_xs_internal::CallSystemD3D12EnableExperimentalFeatures;
using scarlettx::d3d12_xs_internal::CallSystemD3D12GetDebugInterface;
using scarlettx::d3d12_xs_internal::CallSystemD3D12SerializeRootSignature;
using scarlettx::d3d12_xs_internal::CallSystemD3D12SerializeVersionedRootSignature;
using scarlettx::d3d12_xs_internal::EnqueueDelayedEventSignal;
using scarlettx::d3d12_xs_internal::GetSystemD3D12Module;
#endif

#if defined(_WIN32)
void MaybeRecordDeviceRemoved(ID3D12Device* pc_dev, HRESULT triggering_hr, const char* site) {
    if (!scarlettx::FlightRecorder::IsEnabled() || !pc_dev) return;
    const HRESULT reason = pc_dev->GetDeviceRemovedReason();
    if (reason != S_OK ||
        triggering_hr == DXGI_ERROR_DEVICE_REMOVED ||
        triggering_hr == DXGI_ERROR_DEVICE_HUNG ||
        triggering_hr == DXGI_ERROR_DEVICE_RESET) {
        std::vector<scarlettx::FlightGpuBreadcrumb> bcs;
        {
            std::lock_guard<std::mutex> lock(g_gpu_profiler.mu);
            const size_t n = static_cast<size_t>(
                std::min<uint64_t>(g_gpu_profiler.total_breadcrumbs, GpuStabilityProfiler::kBreadcrumbCap));
            bcs.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const uint64_t idx = (g_gpu_profiler.total_breadcrumbs > GpuStabilityProfiler::kBreadcrumbCap)
                    ? (g_gpu_profiler.total_breadcrumbs - n + i)
                    : i;
                const auto& src = g_gpu_profiler.breadcrumbs[static_cast<size_t>(idx % GpuStabilityProfiler::kBreadcrumbCap)];
                scarlettx::FlightGpuBreadcrumb dst{};
                dst.command_list_id = src.CommandListId;
                dst.marker_value = src.MarkerValue;
                std::memcpy(dst.tag, src.Tag, sizeof(dst.tag));
                bcs.push_back(dst);
            }
        }
        scarlettx::FlightRecorder::Instance().RecordGpuDeviceRemoved(
            pc_dev,
            static_cast<int32_t>(reason != S_OK ? reason : triggering_hr),
            site,
            bcs.data(),
            bcs.size());
    }
}
#endif

bool TraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

bool RealTitleMode() {
    const char* v = std::getenv("SCARLETTX_REAL_TITLE");
    return v && v[0] != '\0' && v[0] != '0';
}

// ---------------------------------------------------------------------------
// Xbox ID3D12Device / IDXGIDevice / IDXGIAdapter / ID3D12CommandQueue COM
// vtable proxies for real Scarlett binaries.
// Never hands back raw host PC COM pointers to guest code; translates only
// verified guest vtable slots and logs any unimplemented slot calls.
// ---------------------------------------------------------------------------
using ComVtableFn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, uintptr_t, uintptr_t, uintptr_t);

std::atomic<uint32_t> g_frame_interval_us{0};

struct ProxyGuid {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t d4[8];
};

bool IsReadableMemory(const void* ptr, size_t bytes) {
    if (!ptr || bytes == 0) return false;
#if defined(_WIN32)
    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000u || addr + bytes < addr) return false;
    uintptr_t cur = addr;
    const uintptr_t end = addr + bytes;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        const DWORD kReadableMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kReadableMask) == 0) return false;
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cur) return false;
        cur = region_end;
    }
    return true;
#else
    return true;
#endif
}

bool IsWritableMemory(void* ptr, size_t bytes) {
    if (!ptr || bytes == 0) return false;
#if defined(_WIN32)
    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000u || addr + bytes < addr) return false;
    uintptr_t cur = addr;
    const uintptr_t end = addr + bytes;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        const DWORD kWritableMask = PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kWritableMask) == 0) return false;
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cur) return false;
        cur = region_end;
    }
    return true;
#else
    return true;
#endif
}

bool ProxyGuidEquals(uintptr_t riid, const ProxyGuid& g) {
    return riid != 0 && IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid)) &&
           std::memcmp(reinterpret_cast<const void*>(riid), &g, sizeof(ProxyGuid)) == 0;
}

constexpr ProxyGuid kIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr ProxyGuid kIidIDXGIObject = {
    0xAEC22FB8u, 0x76F3u, 0x4639u, {0x9B, 0xE0, 0x28, 0xEB, 0x43, 0xA6, 0x7A, 0x2E}};

// Standard DXGI / D3D12 IIDs verified from target disassembly
constexpr ProxyGuid kIidIDXGIDevice = {
    0x54EC77FAu, 0x1377u, 0x44E6u, {0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C}};
constexpr ProxyGuid kIidIDXGIDevice1 = {
    0x77DB970Fu, 0x6276u, 0x48BAu, {0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C}};
constexpr ProxyGuid kIidIDXGIDevice2 = {
    0x05008617u, 0xFBFDu, 0x4051u, {0xA7, 0x90, 0x14, 0x48, 0x84, 0xB4, 0xF6, 0xA9}};
constexpr ProxyGuid kIidIDXGIDevice3 = {
    0x6007896Cu, 0x3244u, 0x4AFDu, {0xBF, 0x18, 0xA6, 0xD3, 0xBE, 0xDA, 0x50, 0x23}};
constexpr ProxyGuid kIidIDXGIDevice4 = {
    0x95B4F95Fu, 0xD8DAu, 0x4CA4u, {0x9E, 0xE6, 0x3B, 0x76, 0xD5, 0x96, 0x8A, 0x10}};
constexpr ProxyGuid kIidIDXGIAdapter = {
    0x2411E7E1u, 0x12ACu, 0x4CCFu, {0xBD, 0x14, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0}};
constexpr ProxyGuid kIidIDXGIAdapter1 = {
    0x29038F61u, 0x3839u, 0x4626u, {0x91, 0xFD, 0x08, 0x68, 0x79, 0x01, 0x1A, 0x05}};
constexpr ProxyGuid kIidIDXGIAdapter2 = {
    0x0AA1AE0Au, 0xFA0Eu, 0x4B84u, {0x86, 0x44, 0xE0, 0x5F, 0xF8, 0xE5, 0xAC, 0xB5}};
constexpr ProxyGuid kIidIDXGIAdapter3 = {
    0x645967A4u, 0x1392u, 0x4310u, {0xA7, 0x98, 0x80, 0x53, 0xCE, 0x3E, 0x93, 0xFD}};
constexpr ProxyGuid kIidIDXGIAdapter4 = {
    0x3C8D99D1u, 0x4FBFu, 0x4181u, {0xA8, 0x2C, 0xAF, 0x66, 0xBF, 0x7B, 0xD2, 0x4E}};
constexpr ProxyGuid kIidID3D12StateObject = {
    0x47016943u, 0xFCA8u, 0x4594u, {0x93, 0xEA, 0xAF, 0x25, 0x8B, 0x55, 0x34, 0x6D}};
constexpr ProxyGuid kIidID3D12StateObjectProperties = {
    0xDE5FA827u, 0x9BF9u, 0x4F26u, {0x89, 0xFF, 0xD7, 0xF5, 0x6F, 0xDE, 0x38, 0x60}};
constexpr ProxyGuid kIidID3D12Object = {
    0xC4FEC28Fu, 0x7966u, 0x4E95u, {0x9F, 0x94, 0xF4, 0x31, 0xCB, 0x56, 0xC3, 0xB8}};
constexpr ProxyGuid kIidID3D12DeviceChild = {
    0x905DB94Bu, 0xA00Cu, 0x4140u, {0x9D, 0xF5, 0x2B, 0x64, 0xCA, 0x9E, 0xA3, 0x57}};
constexpr ProxyGuid kIidID3D12DeviceChildXbox = {
    0x905DB94Bu, 0xA00Cu, 0x4140u, {0x9D, 0xFB, 0x2B, 0x63, 0xFA, 0x76, 0xAA, 0xE4}};
constexpr ProxyGuid kIidID3D12Pageable = {
    0x63EE58FBu, 0x1268u, 0x4835u, {0x86, 0xDA, 0xF0, 0x08, 0xCE, 0x62, 0xF0, 0xD6}};

bool IsD3D12BaseChildIid(uintptr_t riid) {
    return ProxyGuidEquals(riid, kIidIUnknown) ||
           ProxyGuidEquals(riid, kIidID3D12Object) ||
           ProxyGuidEquals(riid, kIidID3D12DeviceChild) ||
           ProxyGuidEquals(riid, kIidID3D12DeviceChildXbox);
}

bool IsD3D12PageableBaseIid(uintptr_t riid) {
    return IsD3D12BaseChildIid(riid) || ProxyGuidEquals(riid, kIidID3D12Pageable);
}

bool IsD3D12GraphicsCommandListIid(uintptr_t riid) {
    static constexpr ProxyGuid kCmdListIids[] = {
        {0x7116D91Cu, 0xE7E4u, 0x47CEu, {0xB8, 0xC6, 0xEC, 0x81, 0x68, 0xF4, 0x37, 0xE5}}, // ID3D12CommandList
        {0x5B160D0Fu, 0xAC1Bu, 0x4185u, {0x8B, 0xA8, 0xB3, 0xAE, 0x42, 0xA5, 0xA4, 0x55}}, // ID3D12GraphicsCommandList
        {0x553103FBu, 0x1FE7u, 0x4557u, {0xBB, 0x38, 0x94, 0x6D, 0x7D, 0x0E, 0x7C, 0xA7}}, // ID3D12GraphicsCommandList1
        {0x38C3E585u, 0xFF17u, 0x412Cu, {0x91, 0x50, 0x4F, 0xC6, 0xF9, 0xD7, 0x2A, 0x28}}, // ID3D12GraphicsCommandList2
        {0x6FDA83A7u, 0xB84Cu, 0x4E38u, {0x9A, 0xC8, 0xC7, 0xBD, 0x22, 0x01, 0x6B, 0x3D}}, // ID3D12GraphicsCommandList3
        {0x8754318Eu, 0xD3A9u, 0x4541u, {0x98, 0xCF, 0x64, 0x5B, 0x50, 0xDC, 0x48, 0x74}}, // ID3D12GraphicsCommandList4
        {0x55050859u, 0x4024u, 0x474Cu, {0x87, 0xF5, 0x64, 0x72, 0xEA, 0xEE, 0x44, 0xEA}}, // ID3D12GraphicsCommandList5
        {0xC3827890u, 0xE548u, 0x4CFAu, {0x96, 0xCF, 0x56, 0x89, 0xA9, 0x37, 0x0F, 0x80}}, // ID3D12GraphicsCommandList6
        {0xDD171223u, 0x8B61u, 0x4769u, {0x90, 0xE3, 0x16, 0x0C, 0xCD, 0x4E, 0x4F, 0x13}}, // ID3D12GraphicsCommandList7
        {0xEE936EF9u, 0x599Du, 0x4D28u, {0x93, 0x8E, 0x23, 0xC4, 0xAD, 0x05, 0xCE, 0x51}}, // ID3D12GraphicsCommandList8
        {0x34E01B1Fu, 0xB7F5u, 0x4C12u, {0x90, 0x71, 0x17, 0x78, 0x76, 0x3E, 0x93, 0x00}}, // ID3D12GraphicsCommandList9
        {0x70DA3C56u, 0x4C80u, 0x4C8Eu, {0xAD, 0xC6, 0xA6, 0x49, 0xA2, 0x6B, 0x85, 0x0E}}, // ID3D12GraphicsCommandList10
    };
    if (IsD3D12BaseChildIid(riid)) return true;
    for (const auto& g : kCmdListIids) {
        if (ProxyGuidEquals(riid, g)) return true;
    }
    return false;
}

bool IsD3D12CommandQueueIid(uintptr_t riid) {
    static constexpr ProxyGuid kQueueIids[] = {
        {0x0EC870A6u, 0x5D7Eu, 0x4C22u, {0x8C, 0xFC, 0x5B, 0xAA, 0xE0, 0x76, 0x16, 0xED}}, // ID3D12CommandQueue
        {0x3A2BB3BFu, 0xFA0Bu, 0x4C7Au, {0xA2, 0x8E, 0x7F, 0xA4, 0x19, 0xB0, 0x4D, 0x4F}}, // ID3D12CommandQueue1
    };
    if (IsD3D12PageableBaseIid(riid)) return true;
    for (const auto& g : kQueueIids) {
        if (ProxyGuidEquals(riid, g)) return true;
    }
    return false;
}

bool IsD3D12ResourceIid(uintptr_t riid) {
    static constexpr ProxyGuid kResIids[] = {
        {0x696442BEu, 0xA72Eu, 0x4059u, {0xBC, 0x79, 0x5B, 0x5C, 0x98, 0x04, 0x0F, 0xAD}}, // ID3D12Resource
        {0x9D5E227Au, 0x4430u, 0x4161u, {0x88, 0xB3, 0x3E, 0xCA, 0x6B, 0xB1, 0x6E, 0x19}}, // ID3D12Resource1
        {0xBE36EC3Bu, 0xEA85u, 0x4AEBu, {0xA4, 0x5A, 0xE9, 0xD7, 0x64, 0x04, 0xA4, 0x95}}, // ID3D12Resource2
    };
    if (IsD3D12PageableBaseIid(riid)) return true;
    for (const auto& g : kResIids) {
        if (ProxyGuidEquals(riid, g)) return true;
    }
    return false;
}

bool IsD3D12DescriptorHeapIid(uintptr_t riid) {
    static constexpr ProxyGuid kIidDescHeap = {
        0x8EFB471Du, 0x616Cu, 0x4F49u, {0x90, 0xF7, 0x12, 0x7B, 0xB7, 0x63, 0xFA, 0x51}};
    return IsD3D12PageableBaseIid(riid) || ProxyGuidEquals(riid, kIidDescHeap);
}

bool IsD3D12FenceIid(uintptr_t riid) {
    static constexpr ProxyGuid kFenceIids[] = {
        {0x0A753DCFu, 0xC4D8u, 0x4B91u, {0xAD, 0xF6, 0xBE, 0x5A, 0x60, 0xD9, 0x5A, 0x76}}, // ID3D12Fence
        {0x433685FEu, 0xE22Bu, 0x4CA0u, {0xA8, 0xDB, 0xB5, 0xB4, 0xF4, 0xDD, 0x0E, 0x4A}}, // ID3D12Fence1
    };
    if (IsD3D12PageableBaseIid(riid)) return true;
    for (const auto& g : kFenceIids) {
        if (ProxyGuidEquals(riid, g)) return true;
    }
    return false;
}

bool IsD3D12CommandAllocatorIid(uintptr_t riid) {
    static constexpr ProxyGuid kIidAlloc = {
        0x6102DEE4u, 0xAF59u, 0x4B09u, {0xB9, 0x99, 0xB4, 0x4D, 0x73, 0xF0, 0x9B, 0x24}};
    return IsD3D12PageableBaseIid(riid) || ProxyGuidEquals(riid, kIidAlloc);
}

bool IsD3D12SimpleObjIid(uintptr_t riid) {
    static constexpr ProxyGuid kSimpleIids[] = {
        {0xC54A6B66u, 0x72DFu, 0x4EE8u, {0x8B, 0xE5, 0xA9, 0x46, 0xA1, 0x42, 0x92, 0x14}}, // ID3D12RootSignature
        {0x765A30F3u, 0xF624u, 0x4C6Fu, {0xA8, 0x28, 0xAC, 0xE9, 0x48, 0x62, 0x24, 0x45}}, // ID3D12PipelineState
        {0x0D9658AEu, 0xED45u, 0x469Eu, {0xA6, 0x1D, 0x97, 0x0E, 0xC5, 0x83, 0xCA, 0xB4}}, // ID3D12QueryHeap
        {0xC36A797Cu, 0xEC80u, 0x4F0Au, {0x89, 0x85, 0xA7, 0xB2, 0x47, 0x50, 0x82, 0xD1}}, // ID3D12CommandSignature
        {0x47016943u, 0xFCA8u, 0x4594u, {0x93, 0xEA, 0xAF, 0x25, 0x8B, 0x55, 0x34, 0x6D}}, // ID3D12StateObject
    };
    if (IsD3D12PageableBaseIid(riid)) return true;
    for (const auto& g : kSimpleIids) {
        if (ProxyGuidEquals(riid, g)) return true;
    }
    return false;
}

bool IsDxgiDeviceIid(uintptr_t riid) {
    return ProxyGuidEquals(riid, kIidIDXGIDevice) ||
           ProxyGuidEquals(riid, kIidIDXGIDevice1) ||
           ProxyGuidEquals(riid, kIidIDXGIDevice2) ||
           ProxyGuidEquals(riid, kIidIDXGIDevice3) ||
           ProxyGuidEquals(riid, kIidIDXGIDevice4);
}

bool IsDxgiAdapterIid(uintptr_t riid) {
    return ProxyGuidEquals(riid, kIidIDXGIAdapter) ||
           ProxyGuidEquals(riid, kIidIDXGIAdapter1) ||
           ProxyGuidEquals(riid, kIidIDXGIAdapter2) ||
           ProxyGuidEquals(riid, kIidIDXGIAdapter3) ||
           ProxyGuidEquals(riid, kIidIDXGIAdapter4);
}

bool IsD3D12DeviceIid(uintptr_t riid) {
    static constexpr ProxyGuid kDeviceIids[] = {
        {0x189819F1u, 0x1DB6u, 0x4B57u, {0xBE, 0x54, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7}}, // ID3D12Device
        {0x77ACCE80u, 0x638Eu, 0x4E65u, {0x88, 0x95, 0xC1, 0xF2, 0x33, 0x86, 0x86, 0x3E}}, // ID3D12Device1
        {0x30BAA41Eu, 0xB15Bu, 0x475Cu, {0xA0, 0xBB, 0x1A, 0xF5, 0xC5, 0xB6, 0x43, 0x28}}, // ID3D12Device2
        {0x81DADC15u, 0x2BADu, 0x4392u, {0x93, 0xC5, 0x10, 0x13, 0x45, 0xC4, 0xAA, 0x98}}, // ID3D12Device3
        {0xE865DF17u, 0xA9EEu, 0x46F9u, {0xA4, 0x63, 0x30, 0x98, 0x31, 0x5A, 0xA2, 0xE5}}, // ID3D12Device4
        {0x8B4F173Bu, 0x2FEAu, 0x4B80u, {0x8F, 0x58, 0x43, 0x07, 0x19, 0x1A, 0xB9, 0x5D}}, // ID3D12Device5
        {0xC70B221Bu, 0x40E4u, 0x4A17u, {0x89, 0xAF, 0x02, 0x5A, 0x07, 0x27, 0xA6, 0xDC}}, // ID3D12Device6
        {0x5C014B53u, 0x68A1u, 0x4B9Bu, {0x8B, 0xD1, 0xDD, 0x60, 0x46, 0xB9, 0x35, 0x8B}}, // ID3D12Device7
        {0x9218E6BBu, 0xF944u, 0x4F7Eu, {0xA7, 0x5C, 0xB1, 0xB2, 0xC7, 0xB7, 0x01, 0xF3}}, // ID3D12Device8
        {0x4C80E962u, 0xF032u, 0x4F60u, {0xBC, 0x9E, 0xEB, 0xC2, 0xCF, 0xA1, 0xD8, 0x3C}}, // ID3D12Device9
        {0x517F8718u, 0xAA66u, 0x49F9u, {0xB0, 0x2B, 0xA7, 0xAB, 0x89, 0xC0, 0x60, 0x31}}, // ID3D12Device10
        {0x5405C344u, 0xD457u, 0x444Eu, {0xB4, 0xDD, 0x23, 0x66, 0xE4, 0x5A, 0xEE, 0x39}}, // ID3D12Device11
        {0x5AF5C532u, 0x4C91u, 0x4CD0u, {0xB3, 0x41, 0xF5, 0xA4, 0x05, 0xE6, 0x55, 0x3F}}, // ID3D12Device12
        {0x14EECFFCu, 0x4DF8u, 0x40F7u, {0xA1, 0x18, 0x5C, 0x81, 0xF0, 0x62, 0xC2, 0x9E}}, // ID3D12Device13
        {0x5FF3E912u, 0xFE16u, 0x4EF9u, {0x9B, 0x99, 0xB1, 0x59, 0x0E, 0x43, 0xC6, 0x5F}}, // ID3D12Device14
    };
    for (const auto& g : kDeviceIids) {
        if (ProxyGuidEquals(riid, g)) return true;
    }
    return false;
}

bool IsBlockedDiagnosticsOrComIid(uintptr_t riid) {
    static constexpr ProxyGuid kBlockedIids[] = {
        // COM marshaling / WinRT / weak references
        {0x00000003u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, // IMarshal
        {0x000001CFu, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, // IMarshal2
        {0xECC8691Bu, 0xC1DBu, 0x4DC0u, {0x85, 0x5E, 0x65, 0xF6, 0xC5, 0x51, 0xAF, 0x49}}, // INoMarshal
        {0x94EA2B94u, 0xE9CCu, 0x49E0u, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}}, // IAgileObject
        {0x00000040u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, // IFastRundown
        {0x00000018u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, // IStdMarshalInfo
        {0x00000019u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, // IExternalConnection
        {0x1C733A30u, 0x2A1Cu, 0x11CEu, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}}, // ICallFactory
        {0xAF86E2E0u, 0xB12Du, 0x4C6Au, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}}, // IInspectable
        {0x00000037u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, // IWeakReference
        {0x00000038u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, // IWeakReferenceSource
        // D3D12 InfoQueue / Debug / DRED / Diagnostics
        {0x0742A90Bu, 0xC387u, 0x483Fu, {0xB9, 0x46, 0x30, 0xA7, 0xE4, 0xE6, 0x14, 0x58}}, // ID3D12InfoQueue
        {0x2852DD88u, 0xB484u, 0x4C0Cu, {0xB6, 0xB1, 0x67, 0x16, 0x85, 0x00, 0xE6, 0x00}}, // ID3D12InfoQueue1
        {0x344488B7u, 0x6846u, 0x474Bu, {0xB9, 0x89, 0xF0, 0x27, 0x44, 0x82, 0x45, 0xE0}}, // ID3D12Debug
        {0xAFFAA4CAu, 0x63FEu, 0x4D8Eu, {0xB8, 0xAD, 0x15, 0x90, 0x00, 0xAF, 0x43, 0x04}}, // ID3D12Debug1
        {0x93A665C4u, 0xA3B2u, 0x4E5Du, {0xB6, 0x92, 0xA2, 0x6A, 0xE1, 0x4E, 0x33, 0x74}}, // ID3D12Debug2
        {0x5CF4E58Fu, 0xF671u, 0x4FF1u, {0xA5, 0x42, 0x36, 0x86, 0xE3, 0xD1, 0x53, 0xD1}}, // ID3D12Debug3
        {0x014B816Eu, 0x9EC5u, 0x472Fu, {0xA8, 0x45, 0xFF, 0xBE, 0x44, 0x1C, 0xE1, 0x3A}}, // ID3D12Debug4
        {0x548D6B12u, 0x09FAu, 0x40E0u, {0x90, 0x69, 0x5D, 0xCD, 0x58, 0x9A, 0x52, 0xC9}}, // ID3D12Debug5
        {0x82A816D6u, 0x5D01u, 0x4157u, {0x97, 0xD0, 0x49, 0x75, 0x46, 0x3F, 0xD1, 0xED}}, // ID3D12Debug6
        {0x3FEBD6DDu, 0x4973u, 0x4787u, {0x81, 0x94, 0xE4, 0x5F, 0x9E, 0x28, 0x92, 0x3E}}, // ID3D12DebugDevice
        {0xA9B71770u, 0xD099u, 0x4A65u, {0xA6, 0x98, 0x3D, 0xEE, 0x10, 0x02, 0x0F, 0x88}}, // ID3D12DebugDevice1
        {0x60ECCBC1u, 0x378Du, 0x4DF1u, {0x89, 0x4C, 0xF8, 0xAC, 0x5C, 0xE4, 0xD7, 0xDD}}, // ID3D12DebugDevice2
        {0x09E0BF36u, 0x54ACu, 0x484Fu, {0x88, 0x47, 0x4B, 0xAE, 0xEA, 0xB6, 0x05, 0x3A}}, // ID3D12DebugCommandQueue
        {0x16BE35A2u, 0xBFD6u, 0x49F2u, {0xBC, 0xAE, 0xEA, 0xAE, 0x4A, 0xFF, 0x86, 0x2D}}, // ID3D12DebugCommandQueue1
        {0x09E0BF36u, 0x54ACu, 0x484Fu, {0x88, 0x47, 0x4B, 0xAE, 0xEA, 0xB6, 0x05, 0x3F}}, // ID3D12DebugCommandList
        {0x102CA951u, 0x311Bu, 0x4B01u, {0xB1, 0x1F, 0xEC, 0xB8, 0x3E, 0x06, 0x1B, 0x37}}, // ID3D12DebugCommandList1
        {0xAEB575CFu, 0x4E06u, 0x48BEu, {0xBA, 0x3B, 0xC4, 0x50, 0xFC, 0x96, 0x65, 0x2E}}, // ID3D12DebugCommandList2
        {0x197D5E15u, 0x4D37u, 0x4D34u, {0xAF, 0x78, 0x72, 0x4C, 0xD7, 0x0F, 0xDB, 0x1F}}, // ID3D12DebugCommandList3
        {0x0ADF7D52u, 0x929Cu, 0x4E61u, {0xAD, 0xDB, 0xFF, 0xED, 0x30, 0xDE, 0x66, 0xEF}}, // ID3D12SharingContract
        {0x86CA3B85u, 0x49ADu, 0x4B6Eu, {0xAE, 0xD5, 0xED, 0xDB, 0x18, 0x54, 0x0F, 0x41}}, // ID3D12ManualWriteTrackingResource
        {0x82BC481Cu, 0x6B9Bu, 0x4030u, {0xAE, 0xDB, 0x7E, 0xE3, 0xD1, 0xDF, 0x1E, 0x63}}, // ID3D12DeviceRemovedExtendedDataSettings
        {0xDBD5AE51u, 0x3317u, 0x4F0Au, {0xAD, 0xF9, 0x1D, 0x7C, 0xED, 0xCA, 0xAE, 0x0B}}, // ID3D12DeviceRemovedExtendedDataSettings1
        {0x61552388u, 0x01ABu, 0x4008u, {0xA4, 0x36, 0x83, 0xDB, 0x18, 0x95, 0x66, 0xEA}}, // ID3D12DeviceRemovedExtendedDataSettings2
        {0x98931D33u, 0x5AE8u, 0x4791u, {0xAA, 0x3C, 0x1A, 0x73, 0xA2, 0x93, 0x4E, 0x71}}, // ID3D12DeviceRemovedExtendedData
        {0x9727A022u, 0xCF1Du, 0x4DDAu, {0x9E, 0xBA, 0xEF, 0xFA, 0x65, 0x3F, 0xC5, 0x06}}, // ID3D12DeviceRemovedExtendedData1
        {0x67FC5816u, 0xE4CAu, 0x4915u, {0xBF, 0x18, 0x42, 0x54, 0x12, 0x72, 0xDA, 0x54}}, // ID3D12DeviceRemovedExtendedData2
        {0x119E7452u, 0xDE9Eu, 0x40FEu, {0x88, 0x06, 0x88, 0xF9, 0x0C, 0x12, 0xB4, 0x41}}, // IDXGIDebug
        {0xC5A05F0Cu, 0x16F2u, 0x4ADFu, {0x9F, 0x4D, 0xA8, 0xC4, 0xD5, 0x8A, 0xC5, 0x50}}, // IDXGIDebug1
        {0xD67441C7u, 0x672Au, 0x476Fu, {0x9E, 0x82, 0xCD, 0x55, 0xB4, 0x49, 0x49, 0xCE}}, // IDXGIInfoQueue
    };
    for (const auto& g : kBlockedIids) {
        if (ProxyGuidEquals(riid, g)) return true;
    }
    return false;
}

// A title gets its output from IDXGIAdapter::EnumOutputs only to hand it to
// SetFrameIntervalX, so the output is an identity with a reference count.
struct XboxDXGIOutputProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

intptr_t SCARLETTX_GDK_CALLBACK OutputProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid)) ||
        IsBlockedDiagnosticsOrComIid(riid) || IsD3D12DeviceIid(riid) || IsDxgiDeviceIid(riid) ||
        IsDxgiAdapterIid(riid)) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    static_cast<XboxDXGIOutputProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK OutputProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxDXGIOutputProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK OutputProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxDXGIOutputProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete obj;
    return static_cast<intptr_t>(rem);
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK OutputProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIOutput::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeOutputProxyVtable(std::index_sequence<Is...>) {
    return {
        &OutputProxyQueryInterface,
        &OutputProxyAddRef,
        &OutputProxyRelease,
        &OutputProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

const auto kXboxDXGIOutputProxyVtbl = MakeOutputProxyVtable(std::make_index_sequence<21>{});

struct XboxDXGIAdapterProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    IDXGIAdapter1* pc_adapter = nullptr;
#endif
};

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxDXGIAdapterProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (TraceEnabled()) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIAdapter::QueryInterface("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ")\n";
    }
    if (!ProxyGuidEquals(riid, kIidIUnknown) &&
        !ProxyGuidEquals(riid, kIidIDXGIObject) &&
        !IsDxgiAdapterIid(riid)) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxDXGIAdapterProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxDXGIAdapterProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_adapter) obj->pc_adapter->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyGetDesc(void* self, uintptr_t pDesc, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxDXGIAdapterProxy*>(self);
    if (!pDesc) return static_cast<intptr_t>(E_INVALIDARG);
    if (obj->pc_adapter) {
        return static_cast<intptr_t>(obj->pc_adapter->GetDesc(reinterpret_cast<DXGI_ADAPTER_DESC*>(pDesc)));
    }
#endif
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyGetDesc1(void* self, uintptr_t pDesc, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxDXGIAdapterProxy*>(self);
    if (!pDesc) return static_cast<intptr_t>(E_INVALIDARG);
    if (obj->pc_adapter) {
        return static_cast<intptr_t>(obj->pc_adapter->GetDesc1(reinterpret_cast<DXGI_ADAPTER_DESC1*>(pDesc)));
    }
#endif
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

// IDXGIAdapter::EnumOutputs (+0x38), observed at 0x1404f5290 in a title: one output.
intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyEnumOutputs(void* /*self*/, uintptr_t index, uintptr_t ppOutput, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppOutput);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (static_cast<uint32_t>(index) != 0) return static_cast<intptr_t>(static_cast<int32_t>(0x887A0002u));
    auto* output = new XboxDXGIOutputProxy();
    output->lpVtbl = kXboxDXGIOutputProxyVtbl.data();
    *out = output;
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIAdapter::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeAdapterProxyVtable(std::index_sequence<Is...>) {
    return {
        &AdapterProxyQueryInterface,
        &AdapterProxyAddRef,
        &AdapterProxyRelease,
        &AdapterProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<ComVtableFn, 24> BuildAdapterProxyVtable() {
    auto vtbl = MakeAdapterProxyVtable(std::make_index_sequence<21>{});
    vtbl[7] = &AdapterProxyEnumOutputs;
    vtbl[8] = &AdapterProxyGetDesc;
    vtbl[10] = &AdapterProxyGetDesc1;
    return vtbl;
}

const auto kXboxDXGIAdapterProxyVtbl = BuildAdapterProxyVtable();

struct XboxD3D12DescriptorHeapProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12DescriptorHeap* pc_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC orig_desc{};
    SIZE_T cpu_start = 0;
    UINT64 gpu_start = 0;
    UINT descriptor_stride = 0;
    UINT num_descriptors = 0;
#endif
};

extern const std::array<ComVtableFn, 16> kXboxD3D12DescriptorHeapProxyVtbl;

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12DescriptorHeapProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (!IsD3D12DescriptorHeapIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12DescriptorHeap", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12DescriptorHeapProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12DescriptorHeapProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_heap) obj->pc_heap->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DescriptorHeapProxy*>(self);
    if (obj->pc_heap && name_ptr) {
        return static_cast<intptr_t>(obj->pc_heap->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyGetDesc(void* arg0, uintptr_t arg1, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    XboxD3D12DescriptorHeapProxy* obj = nullptr;
    uintptr_t sret_ptr = 0;
    if (IsReadableMemory(arg0, sizeof(void*)) &&
        *reinterpret_cast<const void* const*>(arg0) == kXboxD3D12DescriptorHeapProxyVtbl.data()) {
        obj = static_cast<XboxD3D12DescriptorHeapProxy*>(arg0);
        sret_ptr = arg1;
    } else if (IsReadableMemory(reinterpret_cast<const void*>(arg1), sizeof(void*)) &&
               *reinterpret_cast<const void* const*>(arg1) == kXboxD3D12DescriptorHeapProxyVtbl.data()) {
        obj = reinterpret_cast<XboxD3D12DescriptorHeapProxy*>(arg1);
        sret_ptr = reinterpret_cast<uintptr_t>(arg0);
    }
    if (obj && IsWritableMemory(reinterpret_cast<void*>(sret_ptr), sizeof(D3D12_DESCRIPTOR_HEAP_DESC))) {
        *reinterpret_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(sret_ptr) = obj->orig_desc;
    }
    return static_cast<intptr_t>(sret_ptr);
#else
    (void)arg0;
    return static_cast<intptr_t>(arg1);
#endif
}

bool IsLikelyDescriptorHandleSretPtr(uintptr_t sret_ptr, const void* self_ptr) {
#if defined(_WIN32)
    if (sret_ptr == 0 || (sret_ptr & 7u) != 0 || reinterpret_cast<const void*>(sret_ptr) == self_ptr) {
        return false;
    }
    if (!IsWritableMemory(reinterpret_cast<void*>(sret_ptr), sizeof(uint64_t))) {
        return false;
    }
    const void* maybe_vtbl = *reinterpret_cast<const void* const*>(sret_ptr);
    if (maybe_vtbl == kXboxD3D12DescriptorHeapProxyVtbl.data()) {
        return false;
    }
    return true;
#else
    (void)sret_ptr;
    (void)self_ptr;
    return false;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyGetCPUStart(void* arg0, uintptr_t arg1, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    XboxD3D12DescriptorHeapProxy* obj = nullptr;
    uintptr_t sret_ptr = 0;
    if (IsReadableMemory(arg0, sizeof(void*)) &&
        *reinterpret_cast<const void* const*>(arg0) == kXboxD3D12DescriptorHeapProxyVtbl.data()) {
        obj = static_cast<XboxD3D12DescriptorHeapProxy*>(arg0);
        sret_ptr = arg1;
    } else if (IsReadableMemory(reinterpret_cast<const void*>(arg1), sizeof(void*)) &&
               *reinterpret_cast<const void* const*>(arg1) == kXboxD3D12DescriptorHeapProxyVtbl.data()) {
        obj = reinterpret_cast<XboxD3D12DescriptorHeapProxy*>(arg1);
        sret_ptr = reinterpret_cast<uintptr_t>(arg0);
    }
    if (obj && obj->pc_heap) {
        const D3D12_CPU_DESCRIPTOR_HANDLE h = obj->pc_heap->GetCPUDescriptorHandleForHeapStart();
        if (IsLikelyDescriptorHandleSretPtr(sret_ptr, obj)) {
            *reinterpret_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(sret_ptr) = h;
            return static_cast<intptr_t>(sret_ptr);
        }
        return static_cast<intptr_t>(h.ptr);
    }
#else
    (void)arg0;
    (void)arg1;
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyGetGPUStart(void* arg0, uintptr_t arg1, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    XboxD3D12DescriptorHeapProxy* obj = nullptr;
    uintptr_t sret_ptr = 0;
    if (IsReadableMemory(arg0, sizeof(void*)) &&
        *reinterpret_cast<const void* const*>(arg0) == kXboxD3D12DescriptorHeapProxyVtbl.data()) {
        obj = static_cast<XboxD3D12DescriptorHeapProxy*>(arg0);
        sret_ptr = arg1;
    } else if (IsReadableMemory(reinterpret_cast<const void*>(arg1), sizeof(void*)) &&
               *reinterpret_cast<const void* const*>(arg1) == kXboxD3D12DescriptorHeapProxyVtbl.data()) {
        obj = reinterpret_cast<XboxD3D12DescriptorHeapProxy*>(arg1);
        sret_ptr = reinterpret_cast<uintptr_t>(arg0);
    }
    if (obj && obj->pc_heap) {
        const D3D12_GPU_DESCRIPTOR_HANDLE h = obj->pc_heap->GetGPUDescriptorHandleForHeapStart();
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] DescHeap::GetGPUStart(self=" << obj
                      << ", sret=0x" << std::hex << sret_ptr << ", h=0x" << h.ptr << std::dec << ")\n";
        }
        if (IsLikelyDescriptorHandleSretPtr(sret_ptr, obj)) {
            *reinterpret_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(sret_ptr) = h;
            return static_cast<intptr_t>(sret_ptr);
        }
        return static_cast<intptr_t>(h.ptr);
    }
#else
    (void)arg0;
    (void)arg1;
#endif
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12DescriptorHeap::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeDescHeapProxyVtable(std::index_sequence<Is...>) {
    return {
        &DescHeapProxyQueryInterface,
        &DescHeapProxyAddRef,
        &DescHeapProxyRelease,
        &DescHeapProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<ComVtableFn, 16> BuildDescHeapProxyVtable() {
    auto vtbl = MakeDescHeapProxyVtable(std::make_index_sequence<13>{});
    vtbl[6] = &DescHeapProxySetName;      // +0x30
    vtbl[8] = &DescHeapProxyGetDesc;      // +0x40
    vtbl[9] = &DescHeapProxyGetCPUStart;  // +0x48 (supports both MSVC sret and Xbox register return)
    vtbl[10] = &DescHeapProxyGetGPUStart; // +0x50 (supports both MSVC sret and Xbox register return)
    return vtbl;
}

const std::array<ComVtableFn, 16> kXboxD3D12DescriptorHeapProxyVtbl = BuildDescHeapProxyVtable();

struct XboxD3D12FenceProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12Fence* pc_fence = nullptr;
#endif
};

intptr_t SCARLETTX_GDK_CALLBACK FenceProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (!IsD3D12FenceIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12Fence", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12FenceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_fence) obj->pc_fence->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    if (obj->pc_fence && name_ptr) return static_cast<intptr_t>(obj->pc_fence->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxyGetCompletedValue(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    if (obj->pc_fence) return static_cast<intptr_t>(obj->pc_fence->GetCompletedValue());
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxySetEventOnCompletion(void* self, uintptr_t val, uintptr_t hEvent, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    if (obj->pc_fence) return static_cast<intptr_t>(obj->pc_fence->SetEventOnCompletion(val, reinterpret_cast<HANDLE>(hEvent)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxySignal(void* self, uintptr_t val, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    if (obj->pc_fence) return static_cast<intptr_t>(obj->pc_fence->Signal(val));
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK FenceProxyTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Fence::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeFenceProxyVtable(std::index_sequence<Is...>) {
    return {&FenceProxyQueryInterface, &FenceProxyAddRef, &FenceProxyRelease, &FenceProxyTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 16> BuildFenceProxyVtable() {
    auto vtbl = MakeFenceProxyVtable(std::make_index_sequence<13>{});
    vtbl[6] = &FenceProxySetName;
    vtbl[8] = &FenceProxyGetCompletedValue;
    vtbl[9] = &FenceProxySetEventOnCompletion;
    vtbl[10] = &FenceProxySignal;
    return vtbl;
}
const auto kXboxFenceProxyVtbl = BuildFenceProxyVtable();

#if defined(_WIN32)
std::mutex g_alloc_epoch_mu;
std::unordered_map<ID3D12CommandAllocator*, uint64_t> g_alloc_epochs;

uint64_t GetAllocatorEpoch(ID3D12CommandAllocator* alloc) {
    if (!alloc) return 0;
    std::lock_guard<std::mutex> lk(g_alloc_epoch_mu);
    return g_alloc_epochs[alloc];
}
#endif

struct XboxD3D12CommandAllocatorProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12CommandAllocator* pc_alloc = nullptr;
#endif
};
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12CommandAllocatorProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (!IsD3D12CommandAllocatorIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12CommandAllocator", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12CommandAllocatorProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12CommandAllocatorProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_alloc) {
            {
                std::lock_guard<std::mutex> lk(g_alloc_epoch_mu);
                g_alloc_epochs.erase(obj->pc_alloc);
            }
            obj->pc_alloc->Release();
        }
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
intptr_t SCARLETTX_GDK_CALLBACK AllocProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandAllocatorProxy*>(self);
    if (obj->pc_alloc && name_ptr) return static_cast<intptr_t>(obj->pc_alloc->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyReset(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandAllocatorProxy*>(self);
    if (obj->pc_alloc) {
        const HRESULT hr = obj->pc_alloc->Reset();
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lk(g_alloc_epoch_mu);
            ++g_alloc_epochs[obj->pc_alloc];
        }
        return static_cast<intptr_t>(hr);
    }
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12CommandAllocator::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeAllocProxyVtable(std::index_sequence<Is...>) {
    return {&AllocProxyQueryInterface, &AllocProxyAddRef, &AllocProxyRelease, &AllocProxyTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 16> BuildAllocProxyVtable() {
    auto vtbl = MakeAllocProxyVtable(std::make_index_sequence<13>{});
    vtbl[6] = &AllocProxySetName;
    vtbl[8] = &AllocProxyReset;
    return vtbl;
}
const auto kXboxCommandAllocatorProxyVtbl = BuildAllocProxyVtable();

struct RootTableUavSpan {
    uint32_t param_idx = 0;
    uint32_t offset_in_table = 0;
    uint32_t count = 0;
};

// Generic simple proxy for RootSignature / PipelineState / QueryHeap / CommandSignature
struct XboxD3D12SimpleObjectProxy {
    const ComVtableFn* lpVtbl;
    // Xbox d3d12_xs inline fast-paths (e.g. SetPipelineState) read header bytes at
    // +0x10..+0x2F on PSO objects before checking the command-list fast-path buffer.
    uint8_t xbox_inline_header[0x80] = {};
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12Object* pc_obj = nullptr;
    std::vector<RootTableUavSpan> uav_table_spans;
#endif
    const char* kind = "Object";
};

intptr_t SCARLETTX_GDK_CALLBACK SimpleObjRelease(void* self, uintptr_t, uintptr_t, uintptr_t);

struct XboxD3D12StateObjectPropertiesProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XboxD3D12SimpleObjectProxy* parent_so = nullptr;
#if defined(_WIN32)
    ID3D12StateObjectProperties* pc_props = nullptr;
#endif
};

intptr_t SCARLETTX_GDK_CALLBACK StateObjPropsQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12StateObjectPropertiesProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid)) ||
        IsBlockedDiagnosticsOrComIid(riid)) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (ProxyGuidEquals(riid, kIidIUnknown) || ProxyGuidEquals(riid, kIidID3D12StateObjectProperties)) {
        obj->ref_count.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return 0;
    }
    if ((ProxyGuidEquals(riid, kIidID3D12StateObject) ||
         ProxyGuidEquals(riid, kIidID3D12Object) ||
         ProxyGuidEquals(riid, kIidID3D12DeviceChild)) &&
        obj->parent_so) {
        obj->parent_so->ref_count.fetch_add(1, std::memory_order_relaxed);
        *out = obj->parent_so;
        return 0;
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
}

intptr_t SCARLETTX_GDK_CALLBACK StateObjPropsAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12StateObjectPropertiesProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK StateObjPropsRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12StateObjectPropertiesProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_props) obj->pc_props->Release();
#endif
        if (obj->parent_so) SimpleObjRelease(obj->parent_so, 0, 0, 0);
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK StateObjPropsGetShaderIdentifier(void* self, uintptr_t pExportName, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12StateObjectPropertiesProxy*>(self);
    if (obj && obj->pc_props && pExportName) {
        return reinterpret_cast<intptr_t>(obj->pc_props->GetShaderIdentifier(reinterpret_cast<LPCWSTR>(pExportName)));
    }
#else
    (void)self;
    (void)pExportName;
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK StateObjPropsGetShaderStackSize(void* self, uintptr_t pExportName, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12StateObjectPropertiesProxy*>(self);
    if (obj && obj->pc_props && pExportName) {
        return static_cast<intptr_t>(obj->pc_props->GetShaderStackSize(reinterpret_cast<LPCWSTR>(pExportName)));
    }
#else
    (void)self;
    (void)pExportName;
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK StateObjPropsGetPipelineStackSize(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12StateObjectPropertiesProxy*>(self);
    if (obj && obj->pc_props) {
        return static_cast<intptr_t>(obj->pc_props->GetPipelineStackSize());
    }
#else
    (void)self;
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK StateObjPropsSetPipelineStackSize(void* self, uintptr_t stackSizeBytes, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12StateObjectPropertiesProxy*>(self);
    if (obj && obj->pc_props) {
        obj->pc_props->SetPipelineStackSize(static_cast<UINT64>(stackSizeBytes));
    }
#else
    (void)self;
    (void)stackSizeBytes;
#endif
    return 0;
}

const std::array<ComVtableFn, 7> kXboxStateObjPropsProxyVtbl = {
    &StateObjPropsQueryInterface,
    &StateObjPropsAddRef,
    &StateObjPropsRelease,
    &StateObjPropsGetShaderIdentifier,
    &StateObjPropsGetShaderStackSize,
    &StateObjPropsGetPipelineStackSize,
    &StateObjPropsSetPipelineStackSize,
};

intptr_t SCARLETTX_GDK_CALLBACK SimpleObjQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12SimpleObjectProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
#if defined(_WIN32)
    if (ProxyGuidEquals(riid, kIidID3D12StateObjectProperties)) {
        if (!obj->pc_obj) {
            return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
        }
        ID3D12StateObjectProperties* pc_props = nullptr;
        if (FAILED(obj->pc_obj->QueryInterface(__uuidof(ID3D12StateObjectProperties),
                                               reinterpret_cast<void**>(&pc_props))) ||
            !pc_props) {
            scarlettx::FlightRecorder::Instance().RecordComQiMiss(
                obj->kind, reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
            return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
        }
        auto* props_proxy = new XboxD3D12StateObjectPropertiesProxy();
        props_proxy->lpVtbl = kXboxStateObjPropsProxyVtbl.data();
        props_proxy->parent_so = obj;
        obj->ref_count.fetch_add(1, std::memory_order_relaxed);
        props_proxy->pc_props = pc_props;
        *out = props_proxy;
        return 0;
    }
#endif
    if (!IsD3D12SimpleObjIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            obj->kind, reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12SimpleObjectProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12SimpleObjectProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_obj) obj->pc_obj->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjSetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12SimpleObjectProxy*>(self);
    if (obj->pc_obj && name_ptr) return static_cast<intptr_t>(obj->pc_obj->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjTracedSlot(void* self, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        auto* obj = static_cast<XboxD3D12SimpleObjectProxy*>(self);
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12" << obj->kind << "::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeSimpleObjVtable(std::index_sequence<Is...>) {
    return {&SimpleObjQueryInterface, &SimpleObjAddRef, &SimpleObjRelease, &SimpleObjTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 16> BuildSimpleObjVtable() {
    auto vtbl = MakeSimpleObjVtable(std::make_index_sequence<13>{});
    vtbl[6] = &SimpleObjSetName;
    return vtbl;
}
const auto kXboxSimpleObjProxyVtbl = BuildSimpleObjVtable();

// Guest-facing D3D12 heap backed by a real host ID3D12Heap so overlapping
// CreatePlacedResource allocations genuinely alias physical GPU memory.
struct XboxD3D12HeapProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    D3D12_HEAP_DESC desc{};
    D3D12_HEAP_DESC pc_desc{};
    ID3D12Heap* pc_heap = nullptr;
    std::mutex tile_mu;
#endif
};
const ProxyGuid kIidID3D12Heap = {0x6b3b2502u, 0x6e51u, 0x45b3u, {0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3}};
const ProxyGuid kIidID3D12Heap1 = {0x572f7389u, 0x2168u, 0x49e3u, {0x96, 0x93, 0xd6, 0xdf, 0x58, 0x71, 0xbf, 0x6d}};
const ProxyGuid kIidIUnknownForHeap = {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

bool IsD3D12HeapIid(uintptr_t riid) {
    return IsD3D12PageableBaseIid(riid) ||
           ProxyGuidEquals(riid, kIidID3D12Heap) ||
           ProxyGuidEquals(riid, kIidID3D12Heap1);
}

intptr_t SCARLETTX_GDK_CALLBACK HeapQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(static_cast<int32_t>(0x80004003u));
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (!IsD3D12HeapIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12Heap", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    static_cast<XboxD3D12HeapProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK HeapAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12HeapProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK HeapRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12HeapProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_heap) obj->pc_heap->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK HeapTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Heap::vtable[" << Slot << "](a1=0x" << std::hex << a1
                  << ", a2=0x" << a2 << ", a3=0x" << a3 << std::dec << ") -> E_NOTIMPL\n";
    }
    scarlettx::FlightRecorder::Instance().RecordVtableSlotMiss(
        "XboxID3D12Heap", Slot, static_cast<uint64_t>(a1), SCARLETTX_CALLER_RIP());
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeHeapVtable(std::index_sequence<Is...>) {
    return {&HeapQueryInterface, &HeapAddRef, &HeapRelease, &HeapTracedSlot<static_cast<int>(Is + 3)>...};
}
const auto kXboxHeapProxyVtbl = MakeHeapVtable(std::make_index_sequence<13>{});

struct GuestTextureMemory;
struct XboxD3D12GraphicsCommandListProxy;
struct XboxD3D12CommandQueueProxy;

struct XboxD3D12ResourceProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12Resource* pc_res = nullptr;
    ID3D12Heap* backing_pc_heap = nullptr;  // Keeps placed ID3D12Heap alive if guest releases heap early
    std::atomic<uint32_t> tracked_state{0};
    std::atomic<uint32_t> exec_state{0};
    DXGI_FORMAT guest_format = DXGI_FORMAT_UNKNOWN;
    bool promoted_typeless = false;
    bool is_default_buffer = false;
    uint64_t external_buffer_gpu_va = 0;
    GuestTextureMemory* guest_memory = nullptr;
#endif
    void* device = nullptr;  // the XboxD3D12DeviceProxy that created it, referenced
};
#if defined(_WIN32)
std::mutex g_live_resources_mutex;
std::unordered_set<XboxD3D12ResourceProxy*> g_live_resources;

std::mutex g_uav_desc_mutex;
std::unordered_map<SIZE_T, XboxD3D12ResourceProxy*> g_uav_cpu_descriptors;

void RecordUavCpuDescriptor(SIZE_T cpu_ptr, XboxD3D12ResourceProxy* res_proxy) {
    if (cpu_ptr == 0) return;
    std::lock_guard<std::mutex> lk(g_uav_desc_mutex);
    if (res_proxy) {
        g_uav_cpu_descriptors[cpu_ptr] = res_proxy;
    } else {
        g_uav_cpu_descriptors.erase(cpu_ptr);
    }
}

void CopyTrackedUavCpuDescriptors(SIZE_T dst_start, SIZE_T src_start, UINT count, UINT stride) {
    if (count == 0 || stride == 0 || dst_start == src_start) return;
    std::lock_guard<std::mutex> lk(g_uav_desc_mutex);
    if (g_uav_cpu_descriptors.empty()) return;
    for (UINT i = 0; i < count; ++i) {
        const SIZE_T s = src_start + static_cast<SIZE_T>(i) * stride;
        const SIZE_T d = dst_start + static_cast<SIZE_T>(i) * stride;
        auto it = g_uav_cpu_descriptors.find(s);
        if (it != g_uav_cpu_descriptors.end()) {
            g_uav_cpu_descriptors[d] = it->second;
        } else {
            g_uav_cpu_descriptors.erase(d);
        }
    }
}

XboxD3D12ResourceProxy* LookupUavCpuDescriptor(SIZE_T cpu_ptr) {
    std::lock_guard<std::mutex> lk(g_uav_desc_mutex);
    auto it = g_uav_cpu_descriptors.find(cpu_ptr);
    return (it != g_uav_cpu_descriptors.end()) ? it->second : nullptr;
}

void RegisterLiveResourceProxy(XboxD3D12ResourceProxy* proxy) {
    if (!proxy) return;
    std::lock_guard<std::mutex> lk(g_live_resources_mutex);
    g_live_resources.insert(proxy);
}

void UnregisterLiveResourceProxy(XboxD3D12ResourceProxy* proxy) {
    if (!proxy) return;
    {
        std::lock_guard<std::mutex> lk(g_live_resources_mutex);
        g_live_resources.erase(proxy);
    }
    {
        std::lock_guard<std::mutex> lk(g_uav_desc_mutex);
        for (auto it = g_uav_cpu_descriptors.begin(); it != g_uav_cpu_descriptors.end();) {
            if (it->second == proxy) {
                it = g_uav_cpu_descriptors.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Fence-Retired Per-Device Upload Ring Buffer & Transient Slab Reclamation
// ---------------------------------------------------------------------------
std::atomic<uint64_t> g_upload_textures{0};
std::atomic<uint64_t> g_upload_bytes{0};

struct UploadPage {
    ID3D12Resource* pc_res = nullptr;
    uint8_t* mapped_ptr = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_va = 0;
    uint64_t capacity = 0;
    uint64_t write_offset = 0;

    ~UploadPage() {
        if (pc_res) {
            if (mapped_ptr) {
                D3D12_RANGE empty_read{0, 0};
                pc_res->Unmap(0, &empty_read);
                mapped_ptr = nullptr;
            }
            pc_res->Release();
            pc_res = nullptr;
        }
    }
};

struct InFlightUploadPage {
    ID3D12Fence* fence = nullptr;
    uint64_t fence_val = 0;
    std::unique_ptr<UploadPage> page;
};

struct DeviceUploadRing {
    static constexpr uint64_t kStandardPageBytes = 1024ull * 1024ull;  // 1 MiB page
    static constexpr size_t kMaxRetainedStandardPages = 8;             // 8 MiB warm pool; transient spikes reclaimed

    std::mutex mu;
    std::vector<std::unique_ptr<UploadPage>> free_pages;
    std::vector<InFlightUploadPage> in_flight_pages;
    std::unique_ptr<UploadPage> device_staging_page;
    std::vector<std::unique_ptr<UploadPage>> pending_device_pages;
    uint64_t reclaimed_transient_pages = 0;

    void ReclaimCompletedLocked() {
        for (auto it = in_flight_pages.begin(); it != in_flight_pages.end();) {
            const bool done = !it->fence || (it->fence->GetCompletedValue() >= it->fence_val);
            if (!done) {
                ++it;
                continue;
            }
            if (it->fence) {
                it->fence->Release();
                it->fence = nullptr;
            }
            if (it->page) {
                if (it->page->capacity == kStandardPageBytes &&
                    free_pages.size() < kMaxRetainedStandardPages) {
                    it->page->write_offset = 0;
                    free_pages.push_back(std::move(it->page));
                } else {
                    // Transient overflow or oversized slab: immediately release GPU memory!
                    ++reclaimed_transient_pages;
                    it->page.reset();
                }
            }
            it = in_flight_pages.erase(it);
        }
    }

    ~DeviceUploadRing() {
        device_staging_page.reset();
        pending_device_pages.clear();
        HANDLE drain_ev = nullptr;
        for (auto& inf : in_flight_pages) {
            if (inf.fence) {
                if (inf.fence->GetCompletedValue() < inf.fence_val) {
                    if (!drain_ev) drain_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (drain_ev && SUCCEEDED(inf.fence->SetEventOnCompletion(inf.fence_val, drain_ev))) {
                        WaitForSingleObject(drain_ev, 5000);
                    }
                }
                inf.fence->Release();
            }
            inf.page.reset();
        }
        if (drain_ev) CloseHandle(drain_ev);
        in_flight_pages.clear();
        free_pages.clear();
    }
};

std::mutex g_upload_rings_mu;
std::unordered_map<ID3D12Device*, std::unique_ptr<DeviceUploadRing>> g_upload_rings;

DeviceUploadRing* GetOrCreateDeviceUploadRing(ID3D12Device* pc_dev) {
    if (!pc_dev) return nullptr;
    std::lock_guard<std::mutex> lk(g_upload_rings_mu);
    auto& slot = g_upload_rings[pc_dev];
    if (!slot) slot = std::make_unique<DeviceUploadRing>();
    return slot.get();
}

void DestroyDeviceUploadRing(ID3D12Device* pc_dev) {
    if (!pc_dev) return;
    std::unique_ptr<DeviceUploadRing> doomed;
    {
        std::lock_guard<std::mutex> lk(g_upload_rings_mu);
        auto it = g_upload_rings.find(pc_dev);
        if (it != g_upload_rings.end()) {
            doomed = std::move(it->second);
            g_upload_rings.erase(it);
        }
    }
    doomed.reset();
}

std::unique_ptr<UploadPage> AcquireUploadPage(ID3D12Device* pc_dev, uint64_t min_bytes) {
    DeviceUploadRing* ring = GetOrCreateDeviceUploadRing(pc_dev);
    if (!ring) return nullptr;
    const uint64_t alloc_bytes = (min_bytes <= DeviceUploadRing::kStandardPageBytes)
        ? DeviceUploadRing::kStandardPageBytes
        : AlignUp(min_bytes, 65536ull);
    {
        std::lock_guard<std::mutex> lk(ring->mu);
        ring->ReclaimCompletedLocked();
        if (alloc_bytes == DeviceUploadRing::kStandardPageBytes && !ring->free_pages.empty()) {
            auto page = std::move(ring->free_pages.back());
            ring->free_pages.pop_back();
            page->write_offset = 0;
            return page;
        }
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = alloc_bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* res = nullptr;
    if (FAILED(pc_dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&res))) ||
        !res) {
        return nullptr;
    }
    D3D12_RANGE empty_read{0, 0};
    void* mapped = nullptr;
    if (FAILED(res->Map(0, &empty_read, &mapped)) || !mapped) {
        res->Release();
        return nullptr;
    }
    auto page = std::make_unique<UploadPage>();
    page->pc_res = res;
    page->mapped_ptr = static_cast<uint8_t*>(mapped);
    page->gpu_va = res->GetGPUVirtualAddress();
    page->capacity = alloc_bytes;
    page->write_offset = 0;
    return page;
}

void ReturnUnusedUploadPages(ID3D12Device* pc_dev, std::vector<std::unique_ptr<UploadPage>>& pages) {
    if (pages.empty()) return;
    DeviceUploadRing* ring = GetOrCreateDeviceUploadRing(pc_dev);
    if (!ring) {
        pages.clear();
        return;
    }
    std::lock_guard<std::mutex> lk(ring->mu);
    ring->ReclaimCompletedLocked();
    for (auto& p : pages) {
        if (!p) continue;
        if (p->capacity == DeviceUploadRing::kStandardPageBytes &&
            ring->free_pages.size() < DeviceUploadRing::kMaxRetainedStandardPages) {
            p->write_offset = 0;
            ring->free_pages.push_back(std::move(p));
        } else {
            ++ring->reclaimed_transient_pages;
            p.reset();
        }
    }
    pages.clear();
}

void RetireUploadPagesOnFence(
    ID3D12Device* pc_dev,
    ID3D12Fence* fence,
    uint64_t fence_val,
    std::vector<std::unique_ptr<UploadPage>>& pages) {
    DeviceUploadRing* ring = GetOrCreateDeviceUploadRing(pc_dev);
    if (!ring) {
        pages.clear();
        return;
    }
    std::lock_guard<std::mutex> lk(ring->mu);
    ring->ReclaimCompletedLocked();
    if (ring->device_staging_page && ring->device_staging_page->write_offset > 0) {
        ring->pending_device_pages.push_back(std::move(ring->device_staging_page));
    }
    auto push_in_flight = [&](std::unique_ptr<UploadPage> p) {
        if (!p) return;
        if (fence) fence->AddRef();
        ring->in_flight_pages.push_back(InFlightUploadPage{fence, fence_val, std::move(p)});
    };
    for (auto& p : pages) {
        push_in_flight(std::move(p));
    }
    pages.clear();
    for (auto& p : ring->pending_device_pages) {
        push_in_flight(std::move(p));
    }
    ring->pending_device_pages.clear();
}

D3D12_GPU_VIRTUAL_ADDRESS SnapshotToDeviceUploadRing(
    ID3D12Device* pc_dev,
    const void* src_bytes,
    size_t copy_bytes,
    size_t alignment = 256u) {
    if (!pc_dev || !src_bytes || copy_bytes == 0) return 0;
    DeviceUploadRing* ring = GetOrCreateDeviceUploadRing(pc_dev);
    if (!ring) return 0;

    std::unique_lock<std::mutex> lk(ring->mu);
    ring->ReclaimCompletedLocked();
    if (ring->device_staging_page) {
        const uint64_t aligned = AlignUp(ring->device_staging_page->write_offset, alignment);
        if (aligned + copy_bytes <= ring->device_staging_page->capacity) {
            std::memcpy(ring->device_staging_page->mapped_ptr + aligned, src_bytes, copy_bytes);
            ring->device_staging_page->write_offset = aligned + copy_bytes;
            g_upload_bytes.fetch_add(copy_bytes, std::memory_order_relaxed);
            return ring->device_staging_page->gpu_va + aligned;
        }
        ring->pending_device_pages.push_back(std::move(ring->device_staging_page));
    }
    lk.unlock();

    auto fresh = AcquireUploadPage(pc_dev, copy_bytes);
    if (!fresh) return 0;
    std::memcpy(fresh->mapped_ptr, src_bytes, copy_bytes);
    fresh->write_offset = copy_bytes;
    const D3D12_GPU_VIRTUAL_ADDRESS out_va = fresh->gpu_va;
    g_upload_bytes.fetch_add(copy_bytes, std::memory_order_relaxed);

    lk.lock();
    if (!ring->device_staging_page) {
        ring->device_staging_page = std::move(fresh);
    } else {
        ring->pending_device_pages.push_back(std::move(fresh));
    }
    return out_va;
}

// Shared ID3D12Heap regions backing CreatePlacedResourceX so overlapping guest
// virtual/KernelX addresses physically alias GPU memory on the host.
struct VirtualPlacedHeapRegion {
    ID3D12Device* pc_device = nullptr;
    uint64_t base_gpu_va = 0;
    uint64_t size_bytes = 0;
    ID3D12Heap* pc_heap = nullptr;
};

std::mutex g_placed_x_heaps_mu;
std::vector<VirtualPlacedHeapRegion> g_placed_x_heaps;

ID3D12Heap* AcquireOrCreatePlacedXHeap(
    ID3D12Device* pc_dev,
    uint64_t region_base_gpu_va,
    uint64_t min_required_bytes,
    uint64_t* out_heap_offset,
    uint64_t requested_gpu_va) {
    if (!pc_dev || requested_gpu_va < region_base_gpu_va) return nullptr;
    const uint64_t offset = requested_gpu_va - region_base_gpu_va;
    const uint64_t needed = AlignUp(offset + min_required_bytes, 65536ull);
    std::lock_guard<std::mutex> lk(g_placed_x_heaps_mu);
    for (auto& r : g_placed_x_heaps) {
        if (r.pc_device == pc_dev &&
            requested_gpu_va >= r.base_gpu_va &&
            (requested_gpu_va - r.base_gpu_va) + min_required_bytes <= r.size_bytes &&
            r.pc_heap != nullptr) {
            if (out_heap_offset) *out_heap_offset = requested_gpu_va - r.base_gpu_va;
            r.pc_heap->AddRef();
            return r.pc_heap;
        }
    }

    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = std::max<uint64_t>(65536ull, needed);
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_NONE;
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if (SUCCEEDED(pc_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))) &&
        opts.ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_1) {
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    }

    ID3D12Heap* heap = nullptr;
    if (FAILED(pc_dev->CreateHeap(&hd, IID_PPV_ARGS(&heap))) || !heap) {
        return nullptr;
    }
    g_placed_x_heaps.push_back(VirtualPlacedHeapRegion{pc_dev, region_base_gpu_va, hd.SizeInBytes, heap});
    if (out_heap_offset) *out_heap_offset = offset;
    heap->AddRef();
    return heap;
}

void DestroyDevicePlacedXHeaps(ID3D12Device* pc_dev) {
    if (!pc_dev) return;
    std::lock_guard<std::mutex> lk(g_placed_x_heaps_mu);
    for (auto it = g_placed_x_heaps.begin(); it != g_placed_x_heaps.end();) {
        if (it->pc_device == pc_dev) {
            if (it->pc_heap) it->pc_heap->Release();
            it = g_placed_x_heaps.erase(it);
        } else {
            ++it;
        }
    }
}

// Textures the title fills through the address returned by
// GetGPUVirtualAddress. On the console that address is the texture's own
// memory, and titles copy cooked texel data (console layout) straight into it.
// Here it is a write-watched guest buffer that is converted to rows and
// uploaded into the host texture whenever the title has written to it.
struct GuestTextureMemory {
    void* base = nullptr;
    size_t size = 0;
    bool owns_memory = true;
    uint64_t external_gpu_va = 0;
    std::vector<uint64_t> subresource_hashes;
    XG_RESOURCE_LAYOUT layout{};
};
std::mutex g_guest_textures_mutex;
std::vector<XboxD3D12ResourceProxy*> g_guest_textures;

uint64_t HashMemoryRegionFast(const uint8_t* ptr, size_t len) {
    uint64_t h = 14695981039346656037ull;
    const size_t n8 = len / 8u;
    const auto* w = reinterpret_cast<const uint64_t*>(ptr);
    for (size_t i = 0; i < n8; ++i) {
        h = (h ^ w[i]) * 1099511628211ull;
    }
    for (size_t i = n8 * 8u; i < len; ++i) {
        h = (h ^ ptr[i]) * 1099511628211ull;
    }
    return h ? h : 1ull;
}

struct GuestPlacedBufferEntry {
    XboxD3D12ResourceProxy* owner_proxy = nullptr;
    uint8_t* external_cpu_va = nullptr;
    uint64_t external_gpu_va = 0;
    size_t size = 0;
    ID3D12Device* pc_device = nullptr;
    ID3D12Resource* pc_res = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS host_gpu_va = 0;
    uint64_t last_hash = 0;
};

struct HostBufferVaRange {
    XboxD3D12ResourceProxy* owner = nullptr;
    ID3D12Resource* pc_res = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS start = 0;
    uint64_t size = 0;
    uint8_t* upload_cpu_shadow = nullptr;  // Non-null for CPU-mapped UPLOAD buffers
};

std::mutex g_guest_buffers_mutex;
std::vector<GuestPlacedBufferEntry> g_guest_buffers;
std::vector<HostBufferVaRange> g_host_buffer_vas;

void RegisterHostBufferVaRange(
    XboxD3D12ResourceProxy* owner,
    ID3D12Resource* pc_res,
    uint64_t width,
    bool is_upload_heap = false) {
    if (!owner || !pc_res || width == 0) return;
    const D3D12_GPU_VIRTUAL_ADDRESS va = pc_res->GetGPUVirtualAddress();
    if (va == 0) return;
    uint8_t* shadow = nullptr;
    if (is_upload_heap) {
        D3D12_RANGE empty_read{0, 0};
        void* mapped = nullptr;
        if (SUCCEEDED(pc_res->Map(0, &empty_read, &mapped)) && mapped) {
            shadow = static_cast<uint8_t*>(mapped);
        }
    }
    std::lock_guard<std::mutex> lk(g_guest_buffers_mutex);
    g_host_buffer_vas.push_back(HostBufferVaRange{owner, pc_res, va, width, shadow});
}

void RegisterPlacedBufferAllocation(
    XboxD3D12ResourceProxy* owner,
    ID3D12Device* pc_device,
    ID3D12Resource* pc_res,
    void* external_cpu_va,
    uint64_t external_gpu_va,
    size_t size) {
    if (!pc_res || size == 0) return;
    GuestPlacedBufferEntry entry{};
    entry.owner_proxy = owner;
    entry.external_cpu_va = static_cast<uint8_t*>(external_cpu_va);
    entry.external_gpu_va = external_gpu_va;
    entry.size = size;
    entry.pc_device = pc_device;
    entry.pc_res = pc_res;
    entry.pc_res->AddRef();
    entry.host_gpu_va = pc_res->GetGPUVirtualAddress();
    entry.last_hash = 0;  // Initial upload happens cleanly via fence-tracked upload ring on submit
    std::lock_guard<std::mutex> lk(g_guest_buffers_mutex);
    g_guest_buffers.push_back(entry);
}

void UnregisterResourceBufferTracking(XboxD3D12ResourceProxy* owner) {
    if (!owner) return;
    std::lock_guard<std::mutex> lk(g_guest_buffers_mutex);
    for (auto it = g_host_buffer_vas.begin(); it != g_host_buffer_vas.end();) {
        if (it->owner == owner) {
            if (it->upload_cpu_shadow && it->pc_res) {
                D3D12_RANGE empty_write{0, 0};
                it->pc_res->Unmap(0, &empty_write);
            }
            it = g_host_buffer_vas.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_guest_buffers.begin(); it != g_guest_buffers.end();) {
        if (it->owner_proxy == owner) {
            if (it->pc_res) {
                it->pc_res->Release();
            }
            it = g_guest_buffers.erase(it);
        } else {
            ++it;
        }
    }
}

void ReleaseDeviceAutoGuestBuffers(ID3D12Device* pc_device) {
    DestroyDeviceUploadRing(pc_device);
    DestroyDevicePlacedXHeaps(pc_device);
}

bool QueryKernelXUnifiedAddresses(
    uint64_t candidate_va,
    uint8_t** out_cpu_va,
    uint64_t* out_gpu_va) {
    if (out_cpu_va) *out_cpu_va = nullptr;
    if (out_gpu_va) *out_gpu_va = 0;
    HMODULE kx = GetModuleHandleW(L"kernelx.dll");
    if (!kx) return false;
    using MapGpuToVaFn = int32_t(__stdcall*)(uint64_t, void**);
    using MapVaToGpuFn = int32_t(__stdcall*)(const void*, uint64_t*);
    auto map_gpu_to_va = reinterpret_cast<MapGpuToVaFn>(
        reinterpret_cast<void*>(GetProcAddress(kx, "KernelXMapGpuToVirtual")));
    auto map_va_to_gpu = reinterpret_cast<MapVaToGpuFn>(
        reinterpret_cast<void*>(GetProcAddress(kx, "KernelXMapVirtualToGpu")));

    void* cpu_ptr = nullptr;
    if (map_gpu_to_va && map_gpu_to_va(candidate_va, &cpu_ptr) == 0 && cpu_ptr) {
        if (out_cpu_va) *out_cpu_va = static_cast<uint8_t*>(cpu_ptr);
        if (out_gpu_va) *out_gpu_va = candidate_va;
        return true;
    }
    uint64_t gpu_addr = 0;
    if (map_va_to_gpu && map_va_to_gpu(reinterpret_cast<const void*>(candidate_va), &gpu_addr) == 0 &&
        gpu_addr != 0) {
        if (out_cpu_va) *out_cpu_va = reinterpret_cast<uint8_t*>(candidate_va);
        if (out_gpu_va) *out_gpu_va = gpu_addr;
        return true;
    }
    return false;
}

bool HasPendingGuestBufferWrites() {
    std::lock_guard<std::mutex> lk(g_guest_buffers_mutex);
    for (const GuestPlacedBufferEntry& e : g_guest_buffers) {
        if (!e.external_cpu_va || e.size == 0 || !e.pc_res) continue;
        if (!IsReadableMemory(e.external_cpu_va, e.size)) continue;
        if (HashMemoryRegionFast(e.external_cpu_va, e.size) != e.last_hash) {
            return true;
        }
    }
    return false;
}

size_t ProbeReadableCopyLength(const uint8_t* ptr, size_t desired) {
    if (!ptr || desired == 0) return 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) return 0;
    const DWORD kReadableMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || (mbi.Protect & kReadableMask) == 0) return 0;
    const auto* region_end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    if (ptr >= region_end) return 0;
    const size_t avail = static_cast<size_t>(region_end - ptr);
    const size_t n = std::min<size_t>(desired, avail);
    return IsReadableMemory(ptr, n) ? n : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS TranslateGuestGpuVa(
    ID3D12Device* pc_dev,
    D3D12_GPU_VIRTUAL_ADDRESS va,
    size_t size_hint) {
    if (va == 0) return 0;

    {
        std::lock_guard<std::mutex> lk(g_guest_buffers_mutex);
        for (const HostBufferVaRange& hb : g_host_buffer_vas) {
            if (va >= hb.start && va < hb.start + hb.size) {
                return va;
            }
        }
        for (const GuestPlacedBufferEntry& e : g_guest_buffers) {
            if (pc_dev && e.pc_device && e.pc_device != pc_dev) continue;
            size_t offset = 0;
            bool matched = false;
            if (e.external_gpu_va != 0 && va >= e.external_gpu_va && va < e.external_gpu_va + e.size) {
                offset = static_cast<size_t>(va - e.external_gpu_va);
                matched = true;
            } else if (e.external_cpu_va != nullptr) {
                const uint64_t cpu_base = reinterpret_cast<uint64_t>(e.external_cpu_va);
                if (va >= cpu_base && va < cpu_base + e.size) {
                    offset = static_cast<size_t>(va - cpu_base);
                    matched = true;
                }
            }
            if (!matched && e.host_gpu_va != 0 && va >= e.host_gpu_va && va < e.host_gpu_va + e.size) {
                offset = static_cast<size_t>(va - e.host_gpu_va);
                matched = true;
            }
            if (matched) {
                if (e.external_cpu_va && offset < e.size && pc_dev) {
                    const size_t want = std::min<size_t>(e.size - offset, std::max<size_t>(size_hint, 4096u));
                    if (IsReadableMemory(e.external_cpu_va + offset, want)) {
                        const auto ring_va = SnapshotToDeviceUploadRing(pc_dev, e.external_cpu_va + offset, want, 256u);
                        if (ring_va != 0) return ring_va;
                    }
                }
                return e.host_gpu_va + offset;
            }
        }
    }

    if (!pc_dev) return va;

    uint8_t* cpu_ptr = nullptr;
    uint64_t gpu_ptr = 0;
    bool resolved = QueryKernelXUnifiedAddresses(va, &cpu_ptr, &gpu_ptr);
    if (!resolved && RealTitleMode()) {
        if (IsReadableMemory(reinterpret_cast<const void*>(va), std::max<size_t>(size_hint, 16u))) {
            cpu_ptr = reinterpret_cast<uint8_t*>(va);
            resolved = true;
        }
    }
    if (!resolved || !cpu_ptr) {
        return va;
    }

    const size_t copy_bytes = ProbeReadableCopyLength(cpu_ptr, std::max<size_t>(size_hint, 4096u));
    if (copy_bytes == 0) return va;
    const D3D12_GPU_VIRTUAL_ADDRESS ring_va = SnapshotToDeviceUploadRing(pc_dev, cpu_ptr, copy_bytes, 256u);
    return ring_va != 0 ? ring_va : va;
}

// True when the guest layout describes the same rows as the host texture's
// subresources. Rules out planar formats and element sizes the layout code
// does not know, whose conversion would not fit the host footprints.
bool GuestLayoutMatchesHost(ID3D12Resource* res, const D3D12_RESOURCE_DESC& d, const XG_RESOURCE_LAYOUT& layout) {
    ID3D12Device* device = nullptr;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
    D3D12_FEATURE_DATA_FORMAT_INFO info = {};
    info.Format = d.Format;
    bool ok = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) &&
              info.PlaneCount == 1;
    const UINT subresources = static_cast<UINT>(d.MipLevels) * d.DepthOrArraySize;
    std::vector<UINT> rows(subresources);
    std::vector<UINT64> row_bytes(subresources);
    if (ok) {
        device->GetCopyableFootprints(&d, 0, subresources, 0, nullptr, rows.data(), row_bytes.data(), nullptr);
        const XG_PLANE_LAYOUT& plane = layout.Plane[0];
        for (UINT s = 0; ok && s < subresources; ++s) {
            const XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[s % d.MipLevels];
            ok = rows[s] == ml.HeightElements &&
                 row_bytes[s] == static_cast<UINT64>(ml.WidthElements) * plane.BytesPerElement;
        }
    }
    device->Release();
    return ok;
}

void* EnsureGuestTextureMemory(XboxD3D12ResourceProxy* obj) {
    std::lock_guard<std::mutex> lock(g_guest_textures_mutex);
    if (obj->guest_memory) return obj->guest_memory->base;
    const D3D12_RESOURCE_DESC d = obj->pc_res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count > 1 ||
        (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        return nullptr;
    }
    XG_TEXTURE2D_DESC xd{};
    xd.Width = static_cast<uint32_t>(d.Width);
    xd.Height = d.Height;
    xd.MipLevels = d.MipLevels;
    xd.ArraySize = d.DepthOrArraySize;
    xd.Format = static_cast<uint32_t>(obj->guest_format != DXGI_FORMAT_UNKNOWN ? obj->guest_format : d.Format);
    xd.SampleDesc.Count = 1;
    xd.BindFlags = 0x8;
    xd.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    auto mem = std::make_unique<GuestTextureMemory>();
    scarlettx::gpu::xg::ComputeTexture2DLayout(xd, &mem->layout);
    if (!scarlettx::gpu::xg::IsAddressable(mem->layout) || mem->layout.SizeBytes == 0 ||
        !GuestLayoutMatchesHost(obj->pc_res, d, mem->layout)) {
        return nullptr;
    }
    mem->size = static_cast<size_t>(mem->layout.SizeBytes);
    mem->base = VirtualAlloc(nullptr, mem->size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!mem->base) return nullptr;
    if (TraceEnabled()) {
        std::ostringstream os;
        os << "[ScarlettX][d3d12_xs] texture " << d.Width << "x" << d.Height << " mips=" << d.MipLevels
           << " fmt=" << xd.Format << ": guest memory " << mem->base << " size=0x" << std::hex << mem->size
           << "\n";
        std::cerr << os.str();
    }
    obj->guest_memory = mem.release();
    g_guest_textures.push_back(obj);
    return obj->guest_memory->base;
}

bool BindExternalGuestTextureMemory(XboxD3D12ResourceProxy* obj, void* guest_cpu_va, uint64_t guest_gpu_va) {
    if (!obj || !obj->pc_res || !guest_cpu_va) return false;
    std::lock_guard<std::mutex> lock(g_guest_textures_mutex);
    if (obj->guest_memory) return true;
    const D3D12_RESOURCE_DESC d = obj->pc_res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count > 1 ||
        (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        return false;
    }
    XG_TEXTURE2D_DESC xd{};
    xd.Width = static_cast<uint32_t>(d.Width);
    xd.Height = d.Height;
    xd.MipLevels = d.MipLevels;
    xd.ArraySize = d.DepthOrArraySize;
    xd.Format = static_cast<uint32_t>(obj->guest_format != DXGI_FORMAT_UNKNOWN ? obj->guest_format : d.Format);
    xd.SampleDesc.Count = 1;
    xd.BindFlags = 0x8;
    xd.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    auto mem = std::make_unique<GuestTextureMemory>();
    scarlettx::gpu::xg::ComputeTexture2DLayout(xd, &mem->layout);
    if (!scarlettx::gpu::xg::IsAddressable(mem->layout) || mem->layout.SizeBytes == 0 ||
        !GuestLayoutMatchesHost(obj->pc_res, d, mem->layout)) {
        return false;
    }
    mem->base = guest_cpu_va;
    mem->size = static_cast<size_t>(mem->layout.SizeBytes);
    mem->owns_memory = false;
    mem->external_gpu_va = guest_gpu_va ? guest_gpu_va : reinterpret_cast<uint64_t>(guest_cpu_va);
    mem->subresource_hashes.assign(static_cast<size_t>(d.MipLevels) * d.DepthOrArraySize, 0ull);
    obj->guest_memory = mem.release();
    g_guest_textures.push_back(obj);
    return true;
}

void ReleaseGuestTextureMemory(XboxD3D12ResourceProxy* obj) {
    std::lock_guard<std::mutex> lock(g_guest_textures_mutex);
    if (!obj->guest_memory) return;
    g_guest_textures.erase(std::remove(g_guest_textures.begin(), g_guest_textures.end(), obj),
                           g_guest_textures.end());
    if (obj->guest_memory->owns_memory && obj->guest_memory->base) {
        VirtualFree(obj->guest_memory->base, 0, MEM_RELEASE);
    }
    delete obj->guest_memory;
    obj->guest_memory = nullptr;
}

// Converts the given subresources of a texture's guest memory to rows and
// copies them into the host texture. Blocks until the copy has run so the
// temporaries can be released.
void UploadGuestTexture(ID3D12CommandQueue* queue, XboxD3D12ResourceProxy* tex, const std::vector<UINT>& dirty) {
    ID3D12Device* device = nullptr;
    if (FAILED(tex->pc_res->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
    const D3D12_RESOURCE_DESC d = tex->pc_res->GetDesc();
    const UINT subresources = static_cast<UINT>(d.MipLevels) * d.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(subresources);
    UINT64 total = 0;
    device->GetCopyableFootprints(&d, 0, subresources, 0, fps.data(), nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    void* mapped = nullptr;
    const bool ready =
        total != 0 &&
        SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&upload))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
        SUCCEEDED(upload->Map(0, nullptr, &mapped));
    if (ready) {
        const GuestTextureMemory& mem = *tex->guest_memory;
        for (const UINT s : dirty) {
            scarlettx::gpu::xg::CopyFromSubresource(mem.layout, mem.base, s % d.MipLevels, s / d.MipLevels,
                                                    static_cast<uint8_t*>(mapped) + fps[s].Offset,
                                                    fps[s].Footprint.RowPitch);
        }
        upload->Unmap(0, nullptr);
        // The state committed on the GPU queue from previously executed command lists.
        const auto state = static_cast<D3D12_RESOURCE_STATES>(tex->exec_state.load(std::memory_order_relaxed));
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex->pc_res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &b);
        for (const UINT s : dirty) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = tex->pc_res;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fps[s];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &b);
        if (SUCCEEDED(list->Close())) {
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            // The temporaries below must outlive the copy: block until it has
            // run (a null event makes SetEventOnCompletion wait; a removed
            // device completes its fences).
            if (SUCCEEDED(queue->Signal(fence, 1))) fence->SetEventOnCompletion(1, nullptr);
        }
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (upload) upload->Release();
    device->Release();
}

struct GuestTextureUpload {
    XboxD3D12ResourceProxy* tex = nullptr;
    std::vector<UINT> dirty;
    D3D12_RESOURCE_DESC desc{};
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps;
    uint64_t offset = 0;
    uint64_t size = 0;
};

// Uploads all dirty textures in one command list and one fence wait. The
// per-texture version used to allocate, submit and block once per texture,
// which turned a handful of streamed textures into a long CPU stall.
void UploadGuestTextureBatch(ID3D12CommandQueue* queue,
                             std::vector<GuestTextureUpload>& uploads) {
    if (uploads.empty()) return;
    ID3D12Device* device = nullptr;
    if (FAILED(uploads.front().tex->pc_res->GetDevice(IID_PPV_ARGS(&device))) || !device) return;

    uint64_t total = 0;
    for (GuestTextureUpload& u : uploads) {
        u.desc = u.tex->pc_res->GetDesc();
        const UINT subresources = static_cast<UINT>(u.desc.MipLevels) * u.desc.DepthOrArraySize;
        u.fps.resize(subresources);
        u.offset = AlignUp(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        device->GetCopyableFootprints(&u.desc, 0, subresources, 0, u.fps.data(), nullptr, nullptr,
                                      &u.size);
        total = u.offset + u.size;
    }
    if (total == 0) {
        device->Release();
        return;
    }
    g_upload_bytes.fetch_add(total, std::memory_order_relaxed);
    g_upload_textures.fetch_add(uploads.size(), std::memory_order_relaxed);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    void* mapped = nullptr;
    const bool ready =
        SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&upload))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                            IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
        SUCCEEDED(upload->Map(0, nullptr, &mapped));

    if (ready) {
        for (const GuestTextureUpload& u : uploads) {
            const GuestTextureMemory& mem = *u.tex->guest_memory;
            for (const UINT s : u.dirty) {
                scarlettx::gpu::xg::CopyFromSubresource(
                    mem.layout, mem.base, s % u.desc.MipLevels, s / u.desc.MipLevels,
                    static_cast<uint8_t*>(mapped) + u.offset + u.fps[s].Offset,
                    u.fps[s].Footprint.RowPitch);
            }
        }
        upload->Unmap(0, nullptr);

        for (const GuestTextureUpload& u : uploads) {
            const auto state =
                static_cast<D3D12_RESOURCE_STATES>(u.tex->exec_state.load(std::memory_order_relaxed));
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = u.tex->pc_res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            if (state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &b);
            for (const UINT s : u.dirty) {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = u.fps[s];
                fp.Offset += u.offset;
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = u.tex->pc_res;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = s;
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = fp;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            if (state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &b);
        }

        if (SUCCEEDED(list->Close())) {
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            if (SUCCEEDED(queue->Signal(fence, 1))) fence->SetEventOnCompletion(1, nullptr);
        }
    }

    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (upload) upload->Release();
    device->Release();
}

// Uploads every guest-memory texture the title has written to since the last call.
void FlushGuestTextureWrites(ID3D12CommandQueue* queue) {
    std::lock_guard<std::mutex> lock(g_guest_textures_mutex);
    if (!queue || g_guest_textures.empty() || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    std::vector<GuestTextureUpload> uploads;
    std::vector<void*> pages;
    std::vector<UINT> dirty;
    for (XboxD3D12ResourceProxy* tex : g_guest_textures) {
        GuestTextureMemory& mem = *tex->guest_memory;
        const D3D12_RESOURCE_DESC d = tex->pc_res->GetDesc();
        const UINT subres_count = static_cast<UINT>(d.MipLevels) * d.DepthOrArraySize;
        dirty.clear();
        if (!mem.owns_memory) {
            if (mem.subresource_hashes.size() < subres_count) {
                mem.subresource_hashes.resize(subres_count, 0ull);
            }
            for (UINT s = 0; s < subres_count; ++s) {
                uint64_t offset = 0, size = 0;
                if (!scarlettx::gpu::xg::SubresourceRange(mem.layout, s % d.MipLevels, s / d.MipLevels, &offset, &size) ||
                    offset + size > mem.size) {
                    continue;
                }
                const uint64_t cur_hash =
                    HashMemoryRegionFast(static_cast<const uint8_t*>(mem.base) + offset, static_cast<size_t>(size));
                if (cur_hash != mem.subresource_hashes[s]) {
                    mem.subresource_hashes[s] = cur_hash;
                    dirty.push_back(s);
                }
            }
        } else {
            pages.resize(mem.size / 4096 + 1);
            ULONG_PTR count = pages.size();
            DWORD granularity = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem.base, mem.size, pages.data(), &count, &granularity) != 0 ||
                count == 0) {
                continue;
            }
            pages.resize(count);
            for (UINT s = 0; s < subres_count; ++s) {
                uint64_t offset = 0, size = 0;
                if (!scarlettx::gpu::xg::SubresourceRange(mem.layout, s % d.MipLevels, s / d.MipLevels, &offset, &size)) {
                    continue;
                }
                const auto* first = static_cast<const uint8_t*>(mem.base) + offset;
                const bool written = std::any_of(pages.begin(), pages.end(), [&](void* page) {
                    const auto* p = static_cast<const uint8_t*>(page);
                    return p + granularity > first && p < first + size;
                });
                if (written) dirty.push_back(s);
            }
        }
        if (dirty.empty()) continue;
        GuestTextureUpload u;
        u.tex = tex;
        u.dirty = dirty;
        uploads.push_back(std::move(u));
    }
    UploadGuestTextureBatch(queue, uploads);
    if (TraceEnabled() && !uploads.empty()) {
        std::cerr << "[ScarlettX][d3d12_xs] uploaded " << uploads.size()
                  << " guest texture(s) in one batch\n";
    }
}

// Lightweight non-reset check for any mid-frame guest texture or placed buffer writes.
bool HasPendingGuestTextureWrites() {
    if (HasPendingGuestBufferWrites()) return true;
    std::lock_guard<std::mutex> lock(g_guest_textures_mutex);
    if (g_guest_textures.empty()) return false;
    ULONG_PTR count = 1;
    void* single_page = nullptr;
    DWORD granularity = 0;
    for (XboxD3D12ResourceProxy* tex : g_guest_textures) {
        if (!tex || !tex->guest_memory || !tex->guest_memory->base) continue;
        if (!tex->guest_memory->owns_memory) {
            const GuestTextureMemory& mem = *tex->guest_memory;
            const D3D12_RESOURCE_DESC d = tex->pc_res->GetDesc();
            const UINT subres_count = static_cast<UINT>(d.MipLevels) * d.DepthOrArraySize;
            for (UINT s = 0; s < subres_count && s < mem.subresource_hashes.size(); ++s) {
                uint64_t offset = 0, size = 0;
                if (scarlettx::gpu::xg::SubresourceRange(mem.layout, s % d.MipLevels, s / d.MipLevels, &offset, &size) &&
                    offset + size <= mem.size) {
                    const uint64_t h =
                        HashMemoryRegionFast(static_cast<const uint8_t*>(mem.base) + offset, static_cast<size_t>(size));
                    if (h != mem.subresource_hashes[s]) return true;
                }
            }
            continue;
        }
        count = 1;
        if (GetWriteWatch(0, tex->guest_memory->base, tex->guest_memory->size, &single_page, &count, &granularity) == 0 && count > 0) {
            return true;
        }
    }
    return false;
}
#endif
extern const std::array<ComVtableFn, 24> kXboxResourceProxyVtbl;

intptr_t SCARLETTX_GDK_CALLBACK ResProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (!IsD3D12ResourceIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12Resource", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12ResourceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        UnregisterLiveResourceProxy(obj);
        UnregisterResourceBufferTracking(obj);
        ReleaseGuestTextureMemory(obj);
        if (obj->pc_res) obj->pc_res->Release();
        if (obj->backing_pc_heap) obj->backing_pc_heap->Release();
#endif
        if (obj->device) (*static_cast<const ComVtableFn* const*>(obj->device))[2](obj->device, 0, 0, 0);
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
// ID3D12DeviceChild::GetDevice (+0x38), observed at 0x14052510b in a title.
intptr_t SCARLETTX_GDK_CALLBACK ResProxyGetDevice(void* self, uintptr_t /*riid*/, uintptr_t ppDevice, uintptr_t) {
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppDevice);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!obj->device) return static_cast<intptr_t>(static_cast<int32_t>(0x80004005u));
    (*static_cast<const ComVtableFn* const*>(obj->device))[1](obj->device, 0, 0, 0);
    *out = obj->device;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->pc_res && name_ptr) return static_cast<intptr_t>(obj->pc_res->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyMap(void* self, uintptr_t subres, uintptr_t pReadRange, uintptr_t ppData) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->pc_res) {
        return static_cast<intptr_t>(obj->pc_res->Map(
            static_cast<UINT>(subres),
            reinterpret_cast<const D3D12_RANGE*>(pReadRange),
            reinterpret_cast<void**>(ppData)));
    }
#endif
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyUnmap(void* self, uintptr_t subres, uintptr_t pWrittenRange, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->pc_res) {
        obj->pc_res->Unmap(static_cast<UINT>(subres), reinterpret_cast<const D3D12_RANGE*>(pWrittenRange));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyGetDesc(void* arg0, uintptr_t arg1, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    XboxD3D12ResourceProxy* obj = nullptr;
    uintptr_t sret_ptr = 0;
    if (IsReadableMemory(arg0, sizeof(void*)) &&
        *reinterpret_cast<const void* const*>(arg0) == kXboxResourceProxyVtbl.data()) {
        obj = static_cast<XboxD3D12ResourceProxy*>(arg0);
        sret_ptr = arg1;
    } else if (IsReadableMemory(reinterpret_cast<const void*>(arg1), sizeof(void*)) &&
               *reinterpret_cast<const void* const*>(arg1) == kXboxResourceProxyVtbl.data()) {
        obj = reinterpret_cast<XboxD3D12ResourceProxy*>(arg1);
        sret_ptr = reinterpret_cast<uintptr_t>(arg0);
    }
    if (obj && obj->pc_res && IsWritableMemory(reinterpret_cast<void*>(sret_ptr), sizeof(D3D12_RESOURCE_DESC))) {
        D3D12_RESOURCE_DESC desc = obj->pc_res->GetDesc();
        if (obj->promoted_typeless && obj->guest_format != DXGI_FORMAT_UNKNOWN) {
            desc.Format = obj->guest_format;
        }
        *reinterpret_cast<D3D12_RESOURCE_DESC*>(sret_ptr) = desc;
    }
    return static_cast<intptr_t>(sret_ptr);
#else
    (void)arg0;
    return static_cast<intptr_t>(arg1);
#endif
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyGetGPUVirtualAddress(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->external_buffer_gpu_va != 0) {
        return static_cast<intptr_t>(obj->external_buffer_gpu_va);
    }
    if (obj->guest_memory && obj->guest_memory->external_gpu_va != 0) {
        return static_cast<intptr_t>(obj->guest_memory->external_gpu_va);
    }
    if (obj->pc_res) {
        const auto va = obj->pc_res->GetGPUVirtualAddress();
        if (va != 0) return static_cast<intptr_t>(va);
        // Host textures have no address; hand out guest memory the title can fill.
        return reinterpret_cast<intptr_t>(EnsureGuestTextureMemory(obj));
    }
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK ResProxyTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Resource::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeResProxyVtable(std::index_sequence<Is...>) {
    return {&ResProxyQueryInterface, &ResProxyAddRef, &ResProxyRelease, &ResProxyTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 24> BuildResProxyVtable() {
    auto vtbl = MakeResProxyVtable(std::make_index_sequence<21>{});
    vtbl[6] = &ResProxySetName;
    vtbl[7] = &ResProxyGetDevice;
    vtbl[8] = &ResProxyMap;
    vtbl[9] = &ResProxyUnmap;
    vtbl[10] = &ResProxyGetDesc;
    vtbl[11] = &ResProxyGetGPUVirtualAddress;
    return vtbl;
}
const std::array<ComVtableFn, 24> kXboxResourceProxyVtbl = BuildResProxyVtable();

#if defined(_WIN32)
struct RetiredScratchBuffer {
    ID3D12CommandAllocator* pc_alloc = nullptr;
    uint64_t epoch = 0;
    ID3D12Resource* res = nullptr;
};
#endif

struct XboxD3D12GraphicsCommandListProxy {
    const ComVtableFn* lpVtbl;
    uint8_t xbox_inline_cmdbuf[0x1000] = {};
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12GraphicsCommandList* pc_list = nullptr;
    ID3D12GraphicsCommandList4* pc_list4 = nullptr;
    ID3D12GraphicsCommandList5* pc_list5 = nullptr;
    ID3D12GraphicsCommandList6* pc_list6 = nullptr;
    ID3D12GraphicsCommandList7* pc_list7 = nullptr;
    ID3D12Device* pc_device = nullptr;
    ID3D12CommandAllocator* active_pc_alloc = nullptr;
    uint64_t active_alloc_epoch = 0;
    ID3D12Resource* scratch_buf = nullptr;
    uint64_t scratch_size = 0;
    std::vector<RetiredScratchBuffer> retired_scratch;
    std::unique_ptr<UploadPage> cur_upload_page;
    std::vector<std::unique_ptr<UploadPage>> used_upload_pages;
    XboxD3D12DescriptorHeapProxy* bound_cbv_srv_uav_heap = nullptr;
    XboxD3D12SimpleObjectProxy* bound_compute_rs = nullptr;
    XboxD3D12SimpleObjectProxy* bound_graphics_rs = nullptr;
    std::vector<std::pair<XboxD3D12ResourceProxy*, D3D12_RESOURCE_STATES>> cmd_buffer_states;
    std::mutex state_mu;
    std::vector<std::pair<XboxD3D12ResourceProxy*, uint32_t>> pending_final_states;
#endif
    XboxD3D12GraphicsCommandListProxy() {
        g_live_command_lists.fetch_add(1, std::memory_order_relaxed);
    }
    ~XboxD3D12GraphicsCommandListProxy() {
        g_live_command_lists.fetch_sub(1, std::memory_order_relaxed);
    }
};

#if defined(_WIN32)
void InitCmdListExtendedInterfaces(XboxD3D12GraphicsCommandListProxy* proxy) {
    if (!proxy || !proxy->pc_list) return;
    (void)proxy->pc_list->QueryInterface(IID_PPV_ARGS(&proxy->pc_list4));
    (void)proxy->pc_list->QueryInterface(IID_PPV_ARGS(&proxy->pc_list5));
#if defined(__ID3D12GraphicsCommandList6_INTERFACE_DEFINED__)
    (void)proxy->pc_list->QueryInterface(IID_PPV_ARGS(&proxy->pc_list6));
#endif
#if defined(__ID3D12GraphicsCommandList7_INTERFACE_DEFINED__)
    (void)proxy->pc_list->QueryInterface(IID_PPV_ARGS(&proxy->pc_list7));
#endif
}

D3D12_GPU_VIRTUAL_ADDRESS AllocateCmdListUploadSlice(
    XboxD3D12GraphicsCommandListProxy* cmd,
    const void* src_bytes,
    size_t copy_bytes,
    size_t alignment = 256u) {
    if (!cmd || !cmd->pc_device || !src_bytes || copy_bytes == 0) return 0;
    if (cmd->cur_upload_page) {
        const uint64_t aligned = AlignUp(cmd->cur_upload_page->write_offset, alignment);
        if (aligned + copy_bytes <= cmd->cur_upload_page->capacity) {
            std::memcpy(cmd->cur_upload_page->mapped_ptr + aligned, src_bytes, copy_bytes);
            cmd->cur_upload_page->write_offset = aligned + copy_bytes;
            g_upload_bytes.fetch_add(copy_bytes, std::memory_order_relaxed);
            return cmd->cur_upload_page->gpu_va + aligned;
        }
        cmd->used_upload_pages.push_back(std::move(cmd->cur_upload_page));
    }
    cmd->cur_upload_page = AcquireUploadPage(cmd->pc_device, copy_bytes);
    if (!cmd->cur_upload_page) return 0;
    std::memcpy(cmd->cur_upload_page->mapped_ptr, src_bytes, copy_bytes);
    cmd->cur_upload_page->write_offset = copy_bytes;
    g_upload_bytes.fetch_add(copy_bytes, std::memory_order_relaxed);
    return cmd->cur_upload_page->gpu_va;
}

void RecordCmdListPendingFinalState(XboxD3D12GraphicsCommandListProxy* obj, XboxD3D12ResourceProxy* res, uint32_t st) {
    if (!obj || !res) return;
    res->tracked_state.store(st, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(obj->state_mu);
    for (auto& kv : obj->pending_final_states) {
        if (kv.first == res) {
            kv.second = st;
            return;
        }
    }
    obj->pending_final_states.emplace_back(res, st);
}

void MarkCmdListUavBufferUsed(XboxD3D12GraphicsCommandListProxy* obj, XboxD3D12ResourceProxy* res_proxy) {
    if (!obj || !obj->pc_list || !res_proxy || !res_proxy->pc_res || !res_proxy->is_default_buffer) return;
    for (auto& kv : obj->cmd_buffer_states) {
        if (kv.first == res_proxy) {
            if (kv.second != D3D12_RESOURCE_STATE_COMMON &&
                kv.second != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = res_proxy->pc_res;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = kv.second;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                obj->pc_list->ResourceBarrier(1, &b);
            }
            kv.second = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            return;
        }
    }
    const auto init_state = static_cast<D3D12_RESOURCE_STATES>(
        res_proxy->exec_state.load(std::memory_order_relaxed));
    if (init_state != D3D12_RESOURCE_STATE_COMMON &&
        init_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res_proxy->pc_res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = init_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        obj->pc_list->ResourceBarrier(1, &b);
    }
    obj->cmd_buffer_states.emplace_back(res_proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void TrackDescriptorTableUavBindings(
    XboxD3D12GraphicsCommandListProxy* obj,
    XboxD3D12SimpleObjectProxy* rs_proxy,
    uint32_t root_param_idx,
    UINT64 gpu_handle_ptr) {
    if (!obj || !rs_proxy || !obj->bound_cbv_srv_uav_heap || gpu_handle_ptr == 0) return;
    const auto* heap = obj->bound_cbv_srv_uav_heap;
    if (heap->gpu_start == 0 || heap->descriptor_stride == 0 || gpu_handle_ptr < heap->gpu_start) return;
    const UINT64 byte_delta = gpu_handle_ptr - heap->gpu_start;
    const UINT64 base_slot = byte_delta / heap->descriptor_stride;
    if (base_slot >= heap->num_descriptors) return;
    for (const auto& span : rs_proxy->uav_table_spans) {
        if (span.param_idx != root_param_idx) continue;
        const uint32_t max_check = std::min<uint32_t>(span.count, 64u);
        for (uint32_t i = 0; i < max_check; ++i) {
            const UINT64 idx = base_slot + span.offset_in_table + i;
            if (idx >= heap->num_descriptors) break;
            const SIZE_T cpu_ptr = heap->cpu_start + static_cast<SIZE_T>(idx) * heap->descriptor_stride;
            if (XboxD3D12ResourceProxy* uav_res = LookupUavCpuDescriptor(cpu_ptr)) {
                MarkCmdListUavBufferUsed(obj, uav_res);
            }
        }
    }
}

D3D12_GPU_VIRTUAL_ADDRESS TranslateCmdListGpuVa(
    XboxD3D12GraphicsCommandListProxy* cmd,
    D3D12_GPU_VIRTUAL_ADDRESS va,
    size_t size_hint,
    bool is_uav_write,
    bool is_cbv = false) {
    if (va == 0 || !cmd) return 0;

    {
        std::unique_lock<std::mutex> lk(g_guest_buffers_mutex);
        for (const HostBufferVaRange& hb : g_host_buffer_vas) {
            if (va >= hb.start && va < hb.start + hb.size) {
                const uint64_t offset = va - hb.start;
                if (is_uav_write) {
                    XboxD3D12ResourceProxy* owner = hb.owner;
                    lk.unlock();
                    MarkCmdListUavBufferUsed(cmd, owner);
                    return va;
                }
                if (hb.upload_cpu_shadow != nullptr) {
                    const uint64_t avail = hb.size - offset;
                    const size_t desired = (size_hint > 0)
                        ? std::min<size_t>(size_hint, static_cast<size_t>(avail))
                        : static_cast<size_t>(std::min<uint64_t>(avail, is_cbv ? 65536ull : 4096ull));
                    if (desired > 0 && desired <= 65536u) {
                        const uint8_t* src_ptr = hb.upload_cpu_shadow + offset;
                        lk.unlock();
                        const auto ring_va = AllocateCmdListUploadSlice(cmd, src_ptr, desired, 256u);
                        return ring_va != 0 ? ring_va : va;
                    }
                }
                return va;
            }
        }

        for (const GuestPlacedBufferEntry& e : g_guest_buffers) {
            if (cmd->pc_device && e.pc_device && e.pc_device != cmd->pc_device) continue;
            size_t offset = 0;
            bool matched = false;
            if (e.external_gpu_va != 0 && va >= e.external_gpu_va && va < e.external_gpu_va + e.size) {
                offset = static_cast<size_t>(va - e.external_gpu_va);
                matched = true;
            } else if (e.external_cpu_va != nullptr) {
                const uint64_t cpu_base = reinterpret_cast<uint64_t>(e.external_cpu_va);
                if (va >= cpu_base && va < cpu_base + e.size) {
                    offset = static_cast<size_t>(va - cpu_base);
                    matched = true;
                }
            }
            if (!matched && e.host_gpu_va != 0 && va >= e.host_gpu_va && va < e.host_gpu_va + e.size) {
                offset = static_cast<size_t>(va - e.host_gpu_va);
                matched = true;
            }
            if (matched) {
                if (is_uav_write) {
                    XboxD3D12ResourceProxy* owner = e.owner_proxy;
                    const D3D12_GPU_VIRTUAL_ADDRESS out_va = e.host_gpu_va + offset;
                    lk.unlock();
                    MarkCmdListUavBufferUsed(cmd, owner);
                    return out_va;
                }
                if (e.external_cpu_va && offset < e.size) {
                    const size_t want = (size_hint > 0)
                        ? std::min<size_t>(e.size - offset, size_hint)
                        : std::min<size_t>(e.size - offset, is_cbv ? 65536u : 4096u);
                    const uint8_t* src_ptr = e.external_cpu_va + offset;
                    const D3D12_GPU_VIRTUAL_ADDRESS fallback_va = e.host_gpu_va + offset;
                    lk.unlock();
                    if (IsReadableMemory(src_ptr, want)) {
                        const auto ring_va = AllocateCmdListUploadSlice(cmd, src_ptr, want, 256u);
                        if (ring_va != 0) return ring_va;
                    }
                    return fallback_va;
                }
                return e.host_gpu_va + offset;
            }
        }
    }

    if (is_uav_write || !cmd->pc_device) return va;

    uint8_t* cpu_ptr = nullptr;
    uint64_t gpu_ptr = 0;
    bool resolved = QueryKernelXUnifiedAddresses(va, &cpu_ptr, &gpu_ptr);
    if (!resolved && RealTitleMode()) {
        if (IsReadableMemory(reinterpret_cast<const void*>(va), std::max<size_t>(size_hint, 16u))) {
            cpu_ptr = reinterpret_cast<uint8_t*>(va);
            resolved = true;
        }
    }
    if (!resolved || !cpu_ptr) return va;

    const size_t copy_bytes = ProbeReadableCopyLength(cpu_ptr, size_hint > 0 ? size_hint : 4096u);
    if (copy_bytes == 0) return va;
    const auto ring_va = AllocateCmdListUploadSlice(cmd, cpu_ptr, copy_bytes, 256u);
    return ring_va != 0 ? ring_va : va;
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK CmdListQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (!IsD3D12GraphicsCommandListIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12GraphicsCommandList", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12GraphicsCommandListProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->cur_upload_page) {
            obj->used_upload_pages.push_back(std::move(obj->cur_upload_page));
        }
        ReturnUnusedUploadPages(obj->pc_device, obj->used_upload_pages);
        if (obj->scratch_buf) obj->scratch_buf->Release();
        for (auto& r : obj->retired_scratch) {
            if (r.res) r.res->Release();
        }
        if (obj->pc_list7) obj->pc_list7->Release();
        if (obj->pc_list6) obj->pc_list6->Release();
        if (obj->pc_list5) obj->pc_list5->Release();
        if (obj->pc_list4) obj->pc_list4->Release();
        if (obj->pc_list) obj->pc_list->Release();
        if (obj->pc_device) obj->pc_device->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && name_ptr) return static_cast<intptr_t>(obj->pc_list->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListGetType(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) return static_cast<intptr_t>(obj->pc_list->GetType());
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListClose(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        const HRESULT hr = obj->pc_list->Close();
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] CmdList::Close(self=" << self << ") -> 0x"
                      << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
        }
        return static_cast<intptr_t>(hr);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListReset(void* self, uintptr_t pAlloc, uintptr_t pPso, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj->pc_list || !pAlloc) return static_cast<intptr_t>(E_INVALIDARG);
    auto* alloc_proxy = reinterpret_cast<XboxD3D12CommandAllocatorProxy*>(pAlloc);
    ID3D12PipelineState* pc_pso = nullptr;
    if (pPso) {
        auto* pso_proxy = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pPso);
        pc_pso = reinterpret_cast<ID3D12PipelineState*>(pso_proxy->pc_obj);
    }
    const HRESULT hr = obj->pc_list->Reset(alloc_proxy->pc_alloc, pc_pso);
    if (SUCCEEDED(hr)) {
        if (obj->cur_upload_page) {
            obj->used_upload_pages.push_back(std::move(obj->cur_upload_page));
        }
        ReturnUnusedUploadPages(obj->pc_device, obj->used_upload_pages);
        obj->bound_cbv_srv_uav_heap = nullptr;
        obj->bound_compute_rs = nullptr;
        obj->bound_graphics_rs = nullptr;
        obj->cmd_buffer_states.clear();
        {
            std::lock_guard<std::mutex> lk(obj->state_mu);
            obj->pending_final_states.clear();
        }
        if (!obj->retired_scratch.empty()) {
            std::lock_guard<std::mutex> lk(g_alloc_epoch_mu);
            for (auto it = obj->retired_scratch.begin(); it != obj->retired_scratch.end();) {
                auto ep_it = g_alloc_epochs.find(it->pc_alloc);
                const bool alloc_gone_or_reset = (ep_it == g_alloc_epochs.end()) || (ep_it->second > it->epoch);
                if (alloc_gone_or_reset) {
                    if (it->res) it->res->Release();
                    it = obj->retired_scratch.erase(it);
                } else {
                    ++it;
                }
            }
        }
        obj->active_pc_alloc = alloc_proxy->pc_alloc;
        obj->active_alloc_epoch = GetAllocatorEpoch(alloc_proxy->pc_alloc);
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::Reset(self=" << self << ", alloc=" << alloc_proxy
                  << ", pso=" << reinterpret_cast<void*>(pPso) << ") -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    return static_cast<intptr_t>(hr);
#else
    return 0;
#endif
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetDescriptorHeaps(void* self, uintptr_t num, uintptr_t ppHeaps, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj->pc_list || !ppHeaps || num == 0) return 0;
    auto** guest_heaps = reinterpret_cast<XboxD3D12DescriptorHeapProxy**>(ppHeaps);
    std::array<ID3D12DescriptorHeap*, 8> stack_heaps{};
    std::unique_ptr<ID3D12DescriptorHeap*[]> heap_fallback;
    ID3D12DescriptorHeap** pc_heaps = (num <= stack_heaps.size())
        ? stack_heaps.data()
        : (heap_fallback = std::make_unique<ID3D12DescriptorHeap*[]>(static_cast<size_t>(num))).get();
    UINT pc_count = 0;
    obj->bound_cbv_srv_uav_heap = nullptr;
    for (uintptr_t i = 0; i < num; ++i) {
        if (guest_heaps[i] && guest_heaps[i]->pc_heap) {
            if (guest_heaps[i]->orig_desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                static_cast<uint32_t>(guest_heaps[i]->orig_desc.Type) >= 4u) {
                obj->bound_cbv_srv_uav_heap = guest_heaps[i];
            }
            pc_heaps[pc_count++] = guest_heaps[i]->pc_heap;
        }
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::SetDescriptorHeaps(self=" << self
                  << ", num=" << num << ", pc_count=" << pc_count << ")\n";
    }
    if (pc_count > 0) {
        obj->pc_list->SetDescriptorHeaps(pc_count, pc_heaps);
    }
#endif
    return 0;
}
D3D12_RESOURCE_STATES SanitizePcResourceState(D3D12_RESOURCE_STATES state, D3D12_RESOURCE_DIMENSION dim) {
    uint32_t s = static_cast<uint32_t>(state);
    s &= ~0x100000u;
    if (dim != D3D12_RESOURCE_DIMENSION_BUFFER) {
        s &= ~(static_cast<uint32_t>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
               static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
               static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) |
               static_cast<uint32_t>(D3D12_RESOURCE_STATE_STREAM_OUT));
    }
    return static_cast<D3D12_RESOURCE_STATES>(s);
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListResourceBarrier(void* self, uintptr_t num, uintptr_t pBarriers, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj->pc_list || !pBarriers || num == 0) return 0;
    const auto* in = reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(pBarriers);
    std::array<D3D12_RESOURCE_BARRIER, 32> stack_barriers{};
    std::unique_ptr<D3D12_RESOURCE_BARRIER[]> heap_barriers;
    D3D12_RESOURCE_BARRIER* out_barriers = (num <= stack_barriers.size())
        ? stack_barriers.data()
        : (heap_barriers = std::make_unique<D3D12_RESOURCE_BARRIER[]>(static_cast<size_t>(num))).get();
    UINT out_count = 0;

    for (size_t i = 0; i < static_cast<size_t>(num); ++i) {
        D3D12_RESOURCE_BARRIER b = in[i];
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] CmdList::ResourceBarrier(self=" << self
                      << ", i=" << i << "/" << num << ", type=" << b.Type << ", flags=0x"
                      << std::hex << b.Flags;
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                std::cerr << ", res=" << b.Transition.pResource << ", sub=" << b.Transition.Subresource
                          << ", before=0x" << b.Transition.StateBefore << ", after=0x" << b.Transition.StateAfter;
            }
            std::cerr << std::dec << ")\n";
        }
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource) {
            auto* res_proxy = reinterpret_cast<XboxD3D12ResourceProxy*>(b.Transition.pResource);
            auto* pc_res = res_proxy->pc_res;
            if (!pc_res) continue;
            b.Transition.pResource = pc_res;
            const auto dim = pc_res->GetDesc().Dimension;
            b.Transition.StateBefore = SanitizePcResourceState(b.Transition.StateBefore, dim);
            b.Transition.StateAfter = SanitizePcResourceState(b.Transition.StateAfter, dim);
            if (res_proxy->is_default_buffer) {
                for (auto& kv : obj->cmd_buffer_states) {
                    if (kv.first == res_proxy) {
                        b.Transition.StateBefore = kv.second;
                        kv.second = b.Transition.StateAfter;
                        break;
                    }
                }
            }
            RecordCmdListPendingFinalState(obj, res_proxy, static_cast<uint32_t>(b.Transition.StateAfter));
            if (b.Transition.StateBefore == b.Transition.StateAfter) continue;
            out_barriers[out_count++] = b;
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            if (b.UAV.pResource) {
                b.UAV.pResource = reinterpret_cast<XboxD3D12ResourceProxy*>(b.UAV.pResource)->pc_res;
                if (!b.UAV.pResource) continue;
            }
            out_barriers[out_count++] = b;
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            if (b.Aliasing.pResourceBefore) b.Aliasing.pResourceBefore = reinterpret_cast<XboxD3D12ResourceProxy*>(b.Aliasing.pResourceBefore)->pc_res;
            if (b.Aliasing.pResourceAfter) b.Aliasing.pResourceAfter = reinterpret_cast<XboxD3D12ResourceProxy*>(b.Aliasing.pResourceAfter)->pc_res;
            out_barriers[out_count++] = b;
        }
    }
    if (out_count > 0) {
        obj->pc_list->ResourceBarrier(out_count, out_barriers);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListCopyBufferRegion(
    void* self, uintptr_t pDstBuf, uintptr_t dstOffset, uintptr_t pSrcBuf, uintptr_t srcOffset, uintptr_t numBytes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj || !obj->pc_list || !pDstBuf || !pSrcBuf) return 0;
    auto* dst = reinterpret_cast<XboxD3D12ResourceProxy*>(pDstBuf)->pc_res;
    auto* src = reinterpret_cast<XboxD3D12ResourceProxy*>(pSrcBuf)->pc_res;
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::CopyBufferRegion(self=" << self
                  << ", dst=" << reinterpret_cast<void*>(pDstBuf) << "+" << dstOffset
                  << ", src=" << reinterpret_cast<void*>(pSrcBuf) << "+" << srcOffset
                  << ", bytes=" << numBytes << ")\n";
    }
    if (dst && src) {
        obj->pc_list->CopyBufferRegion(dst, dstOffset, src, srcOffset, numBytes);
    }
#endif
    return 0;
}
std::atomic<uint64_t> g_copy_texture_count{0};

intptr_t SCARLETTX_GDK_CALLBACK CmdListCopyTextureRegion(
    void* self, uintptr_t pDst, uintptr_t dstX, uintptr_t dstY, uintptr_t dstZ, uintptr_t pSrc, uintptr_t pSrcBox) {
    g_copy_texture_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj || !obj->pc_list || !pDst || !pSrc) return 0;
    D3D12_TEXTURE_COPY_LOCATION dst = *reinterpret_cast<const D3D12_TEXTURE_COPY_LOCATION*>(pDst);
    D3D12_TEXTURE_COPY_LOCATION src = *reinterpret_cast<const D3D12_TEXTURE_COPY_LOCATION*>(pSrc);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::CopyTextureRegion(self=" << self
                  << ", dstRes=" << dst.pResource << "(type=" << dst.Type << ",sub=" << dst.SubresourceIndex
                  << "), xyz=(" << static_cast<UINT>(dstX) << "," << static_cast<UINT>(dstY) << "," << static_cast<UINT>(dstZ)
                  << "), srcRes=" << src.pResource << "(type=" << src.Type;
        if (src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            const auto& fp = src.PlacedFootprint;
            std::cerr << ",off=" << fp.Offset << ",fmt=" << fp.Footprint.Format
                      << ",w=" << fp.Footprint.Width << ",h=" << fp.Footprint.Height
                      << ",d=" << fp.Footprint.Depth << ",pitch=" << fp.Footprint.RowPitch;
        } else {
            std::cerr << ",sub=" << src.SubresourceIndex;
        }
        std::cerr << "), box=" << reinterpret_cast<void*>(pSrcBox) << ")\n";
    }
    if (dst.pResource) dst.pResource = reinterpret_cast<XboxD3D12ResourceProxy*>(dst.pResource)->pc_res;
    if (src.pResource) src.pResource = reinterpret_cast<XboxD3D12ResourceProxy*>(src.pResource)->pc_res;
    if (dst.pResource && src.pResource) {
        if (src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
            (src.PlacedFootprint.Offset & 511ULL) != 0 && obj->pc_device) {
            const auto& fp = src.PlacedFootprint;
            const uint32_t fmt = static_cast<uint32_t>(fp.Footprint.Format);
            const bool is_bc = (fmt >= 70u && fmt <= 84u) || (fmt >= 94u && fmt <= 99u);
            const uint64_t rows = is_bc ? ((static_cast<uint64_t>(fp.Footprint.Height) + 3ULL) / 4ULL)
                                        : static_cast<uint64_t>(fp.Footprint.Height);
            const uint64_t depth = std::max<uint64_t>(1ULL, fp.Footprint.Depth);
            uint64_t copy_bytes = rows * depth * static_cast<uint64_t>(fp.Footprint.RowPitch);
            const D3D12_RESOURCE_DESC src_desc = src.pResource->GetDesc();
            if (fp.Offset < src_desc.Width) {
                copy_bytes = std::min<uint64_t>(copy_bytes, src_desc.Width - fp.Offset);
            }
            if (copy_bytes > obj->scratch_size) {
                const uint64_t alloc_size = std::max<uint64_t>(65536ULL, (copy_bytes + 65535ULL) & ~65535ULL);
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = alloc_size;
                bd.Height = 1;
                bd.DepthOrArraySize = 1;
                bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                ID3D12Resource* new_buf = nullptr;
                if (SUCCEEDED(obj->pc_device->CreateCommittedResource(
                        &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr, IID_PPV_ARGS(&new_buf))) && new_buf) {
                    if (obj->scratch_buf) {
                        obj->retired_scratch.push_back(
                            RetiredScratchBuffer{obj->active_pc_alloc, obj->active_alloc_epoch, obj->scratch_buf});
                    }
                    obj->scratch_buf = new_buf;
                    obj->scratch_size = alloc_size;
                }
            }
            if (obj->scratch_buf && copy_bytes > 0 && copy_bytes <= obj->scratch_size) {
                obj->pc_list->CopyBufferRegion(obj->scratch_buf, 0, src.pResource, fp.Offset, copy_bytes);
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = obj->scratch_buf;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                obj->pc_list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION aligned_src = src;
                aligned_src.pResource = obj->scratch_buf;
                aligned_src.PlacedFootprint.Offset = 0;
                obj->pc_list->CopyTextureRegion(
                    &dst, static_cast<UINT>(dstX), static_cast<UINT>(dstY), static_cast<UINT>(dstZ),
                    &aligned_src, reinterpret_cast<const D3D12_BOX*>(pSrcBox));
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                obj->pc_list->ResourceBarrier(1, &b);
                return 0;
            }
        }
        obj->pc_list->CopyTextureRegion(
            &dst, static_cast<UINT>(dstX), static_cast<UINT>(dstY), static_cast<UINT>(dstZ),
            &src, reinterpret_cast<const D3D12_BOX*>(pSrcBox));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListClearState(void* self, uintptr_t pPso, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        ID3D12PipelineState* pc_pso = nullptr;
        if (pPso) {
            auto* pso = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pPso);
            pc_pso = reinterpret_cast<ID3D12PipelineState*>(pso->pc_obj);
        }
        obj->pc_list->ClearState(pc_pso);
    }
#endif
    return 0;
}
std::atomic<uint64_t> g_draw_instanced_count{0};
std::atomic<uint64_t> g_draw_indexed_count{0};
std::atomic<uint64_t> g_dispatch_count{0};
std::atomic<uint64_t> g_clear_rtv_count{0};
std::atomic<uint64_t> g_om_set_rt_count{0};
std::atomic<uint64_t> g_set_pso_count{0};
std::atomic<uint64_t> g_set_grs_count{0};
std::atomic<uint64_t> g_set_gdt_count{0};
std::atomic<uint64_t> g_set_vb_count{0};
std::atomic<uint64_t> g_set_ib_count{0};
std::atomic<uint64_t> g_set_vp_count{0};
std::atomic<uint64_t> g_flush_count{0};
std::atomic<uint64_t> g_flush_us{0};
std::atomic<uint64_t> g_frame_epoch{0};
std::atomic<uint64_t> g_flushed_epoch{~0ull};

bool StatsEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_STATS");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

void MaybePrintStats(uint64_t frame) {
    if (!StatsEnabled() || frame == 0) return;
    static LARGE_INTEGER last_time{};
    static uint64_t last_frame = 0;
    static uint64_t last_draw = 0, last_dispatch = 0, last_pso = 0, last_grs = 0,
                    last_gdt = 0, last_copy = 0, last_submit = 0, last_flush = 0,
                    last_flush_us = 0, last_bytes = 0;
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last_time.QuadPart != 0) {
        const double seconds = static_cast<double>(now.QuadPart - last_time.QuadPart) /
                               static_cast<double>(freq.QuadPart);
        if (seconds < 1.0) return;
        const uint64_t frames = frame - last_frame;
        const double f = frames ? static_cast<double>(frames) : 1.0;
        const uint64_t draw = g_draw_instanced_count.load() + g_draw_indexed_count.load();
        const uint64_t dispatch = g_dispatch_count.load();
        const uint64_t pso = g_set_pso_count.load();
        const uint64_t grs = g_set_grs_count.load();
        const uint64_t gdt = g_set_gdt_count.load();
        const uint64_t copy = g_copy_texture_count.load();
        const uint64_t submit = g_submit_count.load();
        const uint64_t flush = g_flush_count.load();
        const uint64_t flush_us = g_flush_us.load();
        const uint64_t bytes = g_upload_bytes.load();
        std::fprintf(stderr,
                     "[STATS] fps=%.1f draws/f=%.1f disp/f=%.1f pso/f=%.1f grs/f=%.1f "
                     "gdt/f=%.1f copytex/f=%.1f submit/f=%.1f flush/f=%.2f "
                     "flush_ms/f=%.2f uploadMB/f=%.2f\n",
                     frames / seconds, (draw - last_draw) / f, (dispatch - last_dispatch) / f,
                     (pso - last_pso) / f, (grs - last_grs) / f, (gdt - last_gdt) / f,
                     (copy - last_copy) / f, (submit - last_submit) / f,
                     (flush - last_flush) / f, (flush_us - last_flush_us) / 1000.0 / f,
                     (bytes - last_bytes) / 1048576.0 / f);
        std::fflush(stderr);
        last_draw = draw; last_dispatch = dispatch; last_pso = pso; last_grs = grs;
        last_gdt = gdt; last_copy = copy; last_submit = submit; last_flush = flush;
        last_flush_us = flush_us; last_bytes = bytes;
        last_frame = frame;
        last_time = now;
        return;
    }
    last_time = now;
    last_frame = frame;
    last_draw = g_draw_instanced_count.load() + g_draw_indexed_count.load();
    last_dispatch = g_dispatch_count.load();
    last_pso = g_set_pso_count.load();
    last_grs = g_set_grs_count.load();
    last_gdt = g_set_gdt_count.load();
    last_copy = g_copy_texture_count.load();
    last_submit = g_submit_count.load();
    last_flush = g_flush_count.load();
    last_flush_us = g_flush_us.load();
    last_bytes = g_upload_bytes.load();
}

intptr_t SCARLETTX_GDK_CALLBACK CmdListDrawInstanced(
    void* self, uintptr_t vCount, uintptr_t iCount, uintptr_t startV, uintptr_t startI) {
    g_draw_instanced_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->DrawInstanced(static_cast<UINT>(vCount), static_cast<UINT>(iCount),
                                    static_cast<UINT>(startV), static_cast<UINT>(startI));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListDrawIndexedInstanced(
    void* self, uintptr_t idxCount, uintptr_t instCount, uintptr_t startIdx,
    uintptr_t baseVertex, uintptr_t startInst) {
    const uint64_t n = g_draw_indexed_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 5) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::DrawIndexedInstanced(#" << n
                  << ", self=" << self << ", idx=" << idxCount << ", inst=" << instCount
                  << ", startIdx=" << startIdx << ", baseV=" << static_cast<int64_t>(baseVertex)
                  << ", startI=" << startInst
                  << ", pso_cnt=" << g_set_pso_count.load(std::memory_order_relaxed)
                  << ", grs_cnt=" << g_set_grs_count.load(std::memory_order_relaxed)
                  << ", gdt_cnt=" << g_set_gdt_count.load(std::memory_order_relaxed)
                  << ", vb_cnt=" << g_set_vb_count.load(std::memory_order_relaxed)
                  << ", ib_cnt=" << g_set_ib_count.load(std::memory_order_relaxed)
                  << ", vp_cnt=" << g_set_vp_count.load(std::memory_order_relaxed) << ")\n";
        // Check if Xbox inline cmdbuf has non-zero qwords written by guest fast-paths
        const auto* qw = reinterpret_cast<const uint64_t*>(obj->xbox_inline_cmdbuf);
        for (size_t i = 0; i < 0x1000 / 8; ++i) {
            if (qw[i] != 0) {
                std::cerr << "  [inline_cmdbuf +0x" << std::hex << (8 + i * 8)
                          << "] = 0x" << qw[i] << std::dec << "\n";
            }
        }
    }
    if (obj->pc_list) {
        obj->pc_list->DrawIndexedInstanced(static_cast<UINT>(idxCount), static_cast<UINT>(instCount),
                                           static_cast<UINT>(startIdx), static_cast<INT>(baseVertex),
                                           static_cast<UINT>(startInst));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListDispatch(void* self, uintptr_t x, uintptr_t y, uintptr_t z) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->Dispatch(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListCopyResource(void* self, uintptr_t pDst, uintptr_t pSrc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pDst && pSrc) {
        auto* dst = reinterpret_cast<XboxD3D12ResourceProxy*>(pDst);
        auto* src = reinterpret_cast<XboxD3D12ResourceProxy*>(pSrc);
        if (dst->pc_res && src->pc_res) {
            obj->pc_list->CopyResource(dst->pc_res, src->pc_res);
        }
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListIASetPrimitiveTopology(void* self, uintptr_t topo, uintptr_t, uintptr_t) {
    static std::atomic<uint64_t> topo_cnt{0};
    const uint64_t n = topo_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 8) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::IASetPrimitiveTopology(#" << n
                  << ", self=" << self << ", topo=" << topo << ")\n";
    }
    if (obj->pc_list) {
        obj->pc_list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(topo));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListRSSetViewports(void* self, uintptr_t num, uintptr_t pViewports, uintptr_t) {
    const uint64_t n = g_set_vp_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pViewports && num > 0) {
        auto* vp = reinterpret_cast<const D3D12_VIEWPORT*>(pViewports);
        if (TraceEnabled() && n <= 4) {
            std::cerr << "[ScarlettX][d3d12_xs] CmdList::RSSetViewports(#" << n
                      << ", num=" << num << ", vp0=(" << vp[0].TopLeftX << "," << vp[0].TopLeftY
                      << "," << vp[0].Width << "x" << vp[0].Height
                      << ", z=" << vp[0].MinDepth << ".." << vp[0].MaxDepth << "))\n";
        }
        obj->pc_list->RSSetViewports(static_cast<UINT>(num), vp);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListRSSetScissorRects(void* self, uintptr_t num, uintptr_t pRects, uintptr_t) {
    static std::atomic<uint64_t> sc_cnt{0};
    const uint64_t n = sc_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pRects && num > 0) {
        auto* r = reinterpret_cast<const D3D12_RECT*>(pRects);
        if (TraceEnabled() && n <= 4) {
            std::cerr << "[ScarlettX][d3d12_xs] CmdList::RSSetScissorRects(#" << n
                      << ", num=" << num << ", r0=(" << r[0].left << "," << r[0].top
                      << "," << r[0].right << "," << r[0].bottom << "))\n";
        }
        obj->pc_list->RSSetScissorRects(static_cast<UINT>(num), r);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListOMSetBlendFactor(void* self, uintptr_t pFactor, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->OMSetBlendFactor(reinterpret_cast<const FLOAT*>(pFactor));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListOMSetStencilRef(void* self, uintptr_t stencilRef, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->OMSetStencilRef(static_cast<UINT>(stencilRef));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetPipelineState(void* self, uintptr_t pPso, uintptr_t, uintptr_t) {
    g_set_pso_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pPso) {
        auto* pso = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pPso);
        if (pso->pc_obj) {
            obj->pc_list->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(pso->pc_obj));
        }
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootSignature(void* self, uintptr_t pRs, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        ID3D12RootSignature* pc_rs = nullptr;
        obj->bound_compute_rs = nullptr;
        if (pRs) {
            auto* rs = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pRs);
            obj->bound_compute_rs = rs;
            pc_rs = reinterpret_cast<ID3D12RootSignature*>(rs->pc_obj);
        }
        obj->pc_list->SetComputeRootSignature(pc_rs);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootSignature(void* self, uintptr_t pRs, uintptr_t, uintptr_t) {
    g_set_grs_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        ID3D12RootSignature* pc_rs = nullptr;
        obj->bound_graphics_rs = nullptr;
        if (pRs) {
            auto* rs = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pRs);
            obj->bound_graphics_rs = rs;
            pc_rs = reinterpret_cast<ID3D12RootSignature*>(rs->pc_obj);
        }
        obj->pc_list->SetGraphicsRootSignature(pc_rs);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootDescriptorTable(
    void* self, uintptr_t rootParamIdx, uintptr_t baseDesc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        D3D12_GPU_DESCRIPTOR_HANDLE h{static_cast<UINT64>(baseDesc)};
        TrackDescriptorTableUavBindings(obj, obj->bound_compute_rs, static_cast<uint32_t>(rootParamIdx), h.ptr);
        obj->pc_list->SetComputeRootDescriptorTable(static_cast<UINT>(rootParamIdx), h);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootDescriptorTable(
    void* self, uintptr_t rootParamIdx, uintptr_t baseDesc, uintptr_t) {
    const uint64_t n = g_set_gdt_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 8) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::SetGraphicsRootDescTable(#" << n
                  << ", param=" << rootParamIdx << ", gpu=0x" << std::hex << baseDesc << std::dec << ")\n";
    }
    if (obj->pc_list) {
        D3D12_GPU_DESCRIPTOR_HANDLE h{static_cast<UINT64>(baseDesc)};
        TrackDescriptorTableUavBindings(obj, obj->bound_graphics_rs, static_cast<uint32_t>(rootParamIdx), h.ptr);
        obj->pc_list->SetGraphicsRootDescriptorTable(static_cast<UINT>(rootParamIdx), h);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRoot32BitConstant(
    void* self, uintptr_t rootParamIdx, uintptr_t srcData, uintptr_t destOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->SetComputeRoot32BitConstant(
            static_cast<UINT>(rootParamIdx), static_cast<UINT>(srcData), static_cast<UINT>(destOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRoot32BitConstant(
    void* self, uintptr_t rootParamIdx, uintptr_t srcData, uintptr_t destOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->SetGraphicsRoot32BitConstant(
            static_cast<UINT>(rootParamIdx), static_cast<UINT>(srcData), static_cast<UINT>(destOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRoot32BitConstants(
    void* self, uintptr_t rootParamIdx, uintptr_t numVals, uintptr_t pSrcData, uintptr_t destOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pSrcData) {
        obj->pc_list->SetComputeRoot32BitConstants(
            static_cast<UINT>(rootParamIdx), static_cast<UINT>(numVals),
            reinterpret_cast<const void*>(pSrcData), static_cast<UINT>(destOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRoot32BitConstants(
    void* self, uintptr_t rootParamIdx, uintptr_t numVals, uintptr_t pSrcData, uintptr_t destOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pSrcData) {
        obj->pc_list->SetGraphicsRoot32BitConstants(
            static_cast<UINT>(rootParamIdx), static_cast<UINT>(numVals),
            reinterpret_cast<const void*>(pSrcData), static_cast<UINT>(destOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootCBV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        const auto pc_loc = TranslateCmdListGpuVa(obj, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(loc), 0u, false, true);
        obj->pc_list->SetComputeRootConstantBufferView(static_cast<UINT>(idx), pc_loc);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootCBV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        const auto pc_loc = TranslateCmdListGpuVa(obj, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(loc), 0u, false, true);
        obj->pc_list->SetGraphicsRootConstantBufferView(static_cast<UINT>(idx), pc_loc);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootSRV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        const auto pc_loc = TranslateCmdListGpuVa(obj, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(loc), 0u, false, false);
        obj->pc_list->SetComputeRootShaderResourceView(static_cast<UINT>(idx), pc_loc);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootSRV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        const auto pc_loc = TranslateCmdListGpuVa(obj, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(loc), 0u, false, false);
        obj->pc_list->SetGraphicsRootShaderResourceView(static_cast<UINT>(idx), pc_loc);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootUAV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        const auto pc_loc = TranslateCmdListGpuVa(obj, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(loc), 0u, true, false);
        obj->pc_list->SetComputeRootUnorderedAccessView(static_cast<UINT>(idx), pc_loc);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootUAV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        const auto pc_loc = TranslateCmdListGpuVa(obj, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(loc), 0u, true, false);
        obj->pc_list->SetGraphicsRootUnorderedAccessView(static_cast<UINT>(idx), pc_loc);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListIASetIndexBuffer(void* self, uintptr_t pView, uintptr_t, uintptr_t) {
    const uint64_t n = g_set_ib_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 4 && pView) {
        auto* ib = reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(pView);
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::IASetIndexBuffer(#" << n
                  << ", loc=0x" << std::hex << ib->BufferLocation << std::dec
                  << ", size=" << ib->SizeInBytes << ", fmt=" << ib->Format << ")\n";
    }
    if (obj->pc_list) {
        if (!pView) {
            obj->pc_list->IASetIndexBuffer(nullptr);
        } else {
            D3D12_INDEX_BUFFER_VIEW ib = *reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(pView);
            if (ib.BufferLocation != 0 && ib.SizeInBytes > 0) {
                ib.BufferLocation = TranslateCmdListGpuVa(obj, ib.BufferLocation, ib.SizeInBytes, false, false);
            }
            obj->pc_list->IASetIndexBuffer(&ib);
        }
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListIASetVertexBuffers(
    void* self, uintptr_t startSlot, uintptr_t numViews, uintptr_t pViews) {
    const uint64_t n = g_set_vb_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 4 && pViews && numViews > 0) {
        auto* vb = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(pViews);
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::IASetVertexBuffers(#" << n
                  << ", start=" << startSlot << ", num=" << numViews
                  << ", vb0=(loc=0x" << std::hex << vb[0].BufferLocation << std::dec
                  << ", size=" << vb[0].SizeInBytes << ", stride=" << vb[0].StrideInBytes << "))\n";
    }
    if (obj->pc_list) {
        if (!pViews || numViews == 0 || numViews > 32u) {
            obj->pc_list->IASetVertexBuffers(
                static_cast<UINT>(startSlot), static_cast<UINT>(numViews),
                reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(pViews));
        } else {
            D3D12_VERTEX_BUFFER_VIEW translated[32]{};
            const auto* src_views = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(pViews);
            for (size_t i = 0; i < static_cast<size_t>(numViews); ++i) {
                translated[i] = src_views[i];
                if (translated[i].BufferLocation != 0 && translated[i].SizeInBytes > 0) {
                    translated[i].BufferLocation = TranslateCmdListGpuVa(
                        obj, translated[i].BufferLocation, translated[i].SizeInBytes, false, false);
                }
            }
            obj->pc_list->IASetVertexBuffers(
                static_cast<UINT>(startSlot), static_cast<UINT>(numViews), translated);
        }
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListOMSetRenderTargets(
    void* self, uintptr_t numRTVs, uintptr_t pRTVs, uintptr_t singleHandle, uintptr_t pDSV) {
    const uint64_t n = g_om_set_rt_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 6) {
        uintptr_t rtv0 = (pRTVs && numRTVs > 0) ? reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pRTVs)[0].ptr : 0;
        uintptr_t dsv0 = pDSV ? reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pDSV)->ptr : 0;
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::OMSetRenderTargets(#" << n
                  << ", num=" << numRTVs << ", rtv0=0x" << std::hex << rtv0
                  << ", single=" << singleHandle << ", dsv=0x" << dsv0 << std::dec << ")\n";
    }
    if (obj->pc_list) {
        obj->pc_list->OMSetRenderTargets(
            static_cast<UINT>(numRTVs),
            reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pRTVs),
            static_cast<BOOL>(singleHandle != 0),
            reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pDSV));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListClearDepthStencilView(
    void* self, uintptr_t dsv, uintptr_t flags, uintptr_t depthBits, uintptr_t stencil,
    uintptr_t numRects, uintptr_t pRects) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{static_cast<SIZE_T>(dsv)};
        // Note: on Win64 ABI float arg3 is passed in XMM3; use ClearDepthStencilView with
        // depth=1.0f fallback unless we capture XMM3 in a typed signature below.
        (void)depthBits;
        obj->pc_list->ClearDepthStencilView(
            h, static_cast<D3D12_CLEAR_FLAGS>(flags), 1.0f, static_cast<UINT8>(stencil),
            static_cast<UINT>(numRects), reinterpret_cast<const D3D12_RECT*>(pRects));
    }
#endif
    return 0;
}
void SCARLETTX_GDK_CALLBACK CmdListClearDepthStencilViewTyped(
    void* self, uintptr_t dsv, uint32_t flags, float depth, uint8_t stencil,
    uint32_t numRects, const void* pRects) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{static_cast<SIZE_T>(dsv)};
        obj->pc_list->ClearDepthStencilView(
            h, static_cast<D3D12_CLEAR_FLAGS>(flags), depth, stencil,
            static_cast<UINT>(numRects), reinterpret_cast<const D3D12_RECT*>(pRects));
    }
#endif
}
void SCARLETTX_GDK_CALLBACK CmdListClearRenderTargetViewTyped(
    void* self, uintptr_t rtv, const float* colorRgba, uint32_t numRects, const void* pRects) {
    const uint64_t n = g_clear_rtv_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 5 && colorRgba) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::ClearRTV(#" << n
                  << ", rtv=0x" << std::hex << rtv << std::dec
                  << ", rgba=(" << colorRgba[0] << "," << colorRgba[1] << "," << colorRgba[2] << "," << colorRgba[3]
                  << "), rects=" << numRects << ")\n";
    }
    if (obj->pc_list && colorRgba) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{static_cast<SIZE_T>(rtv)};
        obj->pc_list->ClearRenderTargetView(
            h, colorRgba, static_cast<UINT>(numRects), reinterpret_cast<const D3D12_RECT*>(pRects));
    }
#endif
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListNoop(void*, uintptr_t, uintptr_t, uintptr_t) { return 0; }
intptr_t SCARLETTX_GDK_CALLBACK CmdListResolveSubresource(
    void* self, uintptr_t pDstResource, uintptr_t dstSubresource,
    uintptr_t pSrcResource, uintptr_t srcSubresource, uintptr_t format) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto* dst = reinterpret_cast<XboxD3D12ResourceProxy*>(pDstResource);
    auto* src = reinterpret_cast<XboxD3D12ResourceProxy*>(pSrcResource);
    if (obj && obj->pc_list && dst && dst->pc_res && src && src->pc_res) {
        obj->pc_list->ResolveSubresource(
            dst->pc_res, static_cast<UINT>(dstSubresource),
            src->pc_res, static_cast<UINT>(srcSubresource),
            static_cast<DXGI_FORMAT>(format));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListExecuteBundle(void* self, uintptr_t pBundle, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto* bundle = reinterpret_cast<XboxD3D12GraphicsCommandListProxy*>(pBundle);
    if (obj && obj->pc_list && bundle && bundle->pc_list) {
        obj->pc_list->ExecuteBundle(bundle->pc_list);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListDiscardResource(
    void* self, uintptr_t pResource, uintptr_t pRegion, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto* res = reinterpret_cast<XboxD3D12ResourceProxy*>(pResource);
    if (obj && obj->pc_list && res && res->pc_res) {
        obj->pc_list->DiscardResource(
            res->pc_res, reinterpret_cast<const D3D12_DISCARD_REGION*>(pRegion));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListBeginQuery(
    void* self, uintptr_t pQueryHeap, uintptr_t type, uintptr_t index) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto* qh = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pQueryHeap);
    if (obj && obj->pc_list && qh && qh->pc_obj) {
        obj->pc_list->BeginQuery(
            reinterpret_cast<ID3D12QueryHeap*>(qh->pc_obj),
            static_cast<D3D12_QUERY_TYPE>(type), static_cast<UINT>(index));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListEndQuery(
    void* self, uintptr_t pQueryHeap, uintptr_t type, uintptr_t index) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto* qh = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pQueryHeap);
    if (obj && obj->pc_list && qh && qh->pc_obj) {
        obj->pc_list->EndQuery(
            reinterpret_cast<ID3D12QueryHeap*>(qh->pc_obj),
            static_cast<D3D12_QUERY_TYPE>(type), static_cast<UINT>(index));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListResolveQueryData(
    void* self, uintptr_t pQueryHeap, uintptr_t type, uintptr_t startIndex,
    uintptr_t numQueries, uintptr_t pDstBuffer, uintptr_t alignedDstOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto* qh = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pQueryHeap);
    auto* dst = reinterpret_cast<XboxD3D12ResourceProxy*>(pDstBuffer);
    if (obj && obj->pc_list && qh && qh->pc_obj && dst && dst->pc_res) {
        obj->pc_list->ResolveQueryData(
            reinterpret_cast<ID3D12QueryHeap*>(qh->pc_obj),
            static_cast<D3D12_QUERY_TYPE>(type), static_cast<UINT>(startIndex),
            static_cast<UINT>(numQueries), dst->pc_res, static_cast<UINT64>(alignedDstOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListExecuteIndirect(
    void* self, uintptr_t pCommandSignature, uintptr_t maxCommandCount,
    uintptr_t pArgumentBuffer, uintptr_t argumentBufferOffset,
    uintptr_t pCountBuffer, uintptr_t countBufferOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto* sig = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pCommandSignature);
    auto* arg = reinterpret_cast<XboxD3D12ResourceProxy*>(pArgumentBuffer);
    auto* cnt = reinterpret_cast<XboxD3D12ResourceProxy*>(pCountBuffer);
    if (obj && obj->pc_list && sig && sig->pc_obj && arg && arg->pc_res) {
        obj->pc_list->ExecuteIndirect(
            reinterpret_cast<ID3D12CommandSignature*>(sig->pc_obj),
            static_cast<UINT>(maxCommandCount),
            arg->pc_res, static_cast<UINT64>(argumentBufferOffset),
            cnt ? cnt->pc_res : nullptr, static_cast<UINT64>(countBufferOffset));
    }
#endif
    return 0;
}
#if defined(_WIN32)
D3D12_GPU_VIRTUAL_ADDRESS UnwrapResourceProxyOrTranslateGpuVa(
    XboxD3D12GraphicsCommandListProxy* cmd,
    D3D12_GPU_VIRTUAL_ADDRESS addr,
    size_t size_hint = 4096u,
    bool is_uav_write = false) {
    if (addr == 0) return 0;
    const auto* cand = reinterpret_cast<const XboxD3D12ResourceProxy*>(addr);
    if (IsReadableMemory(cand, sizeof(XboxD3D12ResourceProxy)) &&
        cand->lpVtbl == kXboxResourceProxyVtbl.data()) {
        if (is_uav_write) {
            MarkCmdListUavBufferUsed(cmd, const_cast<XboxD3D12ResourceProxy*>(cand));
        }
        return cand->pc_res ? cand->pc_res->GetGPUVirtualAddress() : 0;
    }
    return TranslateCmdListGpuVa(cmd, addr, size_hint, is_uav_write, false);
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK CmdListSetPipelineState1(
    void* self, uintptr_t pStateObject, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto* so = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pStateObject);
    if (obj && obj->pc_list4 && so && so->pc_obj) {
        obj->pc_list4->SetPipelineState1(reinterpret_cast<ID3D12StateObject*>(so->pc_obj));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListDispatchRays(
    void* self, uintptr_t pDesc, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj && obj->pc_list4 && pDesc) {
        D3D12_DISPATCH_RAYS_DESC d = *reinterpret_cast<const D3D12_DISPATCH_RAYS_DESC*>(pDesc);
        if (d.RayGenerationShaderRecord.StartAddress != 0) {
            d.RayGenerationShaderRecord.StartAddress = UnwrapResourceProxyOrTranslateGpuVa(
                obj, d.RayGenerationShaderRecord.StartAddress,
                static_cast<size_t>(d.RayGenerationShaderRecord.SizeInBytes));
        }
        if (d.MissShaderTable.StartAddress != 0) {
            d.MissShaderTable.StartAddress = UnwrapResourceProxyOrTranslateGpuVa(
                obj, d.MissShaderTable.StartAddress,
                static_cast<size_t>(d.MissShaderTable.SizeInBytes));
        }
        if (d.HitGroupTable.StartAddress != 0) {
            d.HitGroupTable.StartAddress = UnwrapResourceProxyOrTranslateGpuVa(
                obj, d.HitGroupTable.StartAddress,
                static_cast<size_t>(d.HitGroupTable.SizeInBytes));
        }
        if (d.CallableShaderTable.StartAddress != 0) {
            d.CallableShaderTable.StartAddress = UnwrapResourceProxyOrTranslateGpuVa(
                obj, d.CallableShaderTable.StartAddress,
                static_cast<size_t>(d.CallableShaderTable.SizeInBytes));
        }
        obj->pc_list4->DispatchRays(&d);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListBuildRaytracingAccelerationStructure(
    void* self, uintptr_t pDesc, uintptr_t numPostbuildInfoDescs, uintptr_t pPostbuildInfoDescs) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj && obj->pc_list4 && pDesc) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc =
            *reinterpret_cast<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*>(pDesc);
        build_desc.DestAccelerationStructureData = UnwrapResourceProxyOrTranslateGpuVa(
            obj, build_desc.DestAccelerationStructureData, 4096u, true);
        build_desc.SourceAccelerationStructureData = UnwrapResourceProxyOrTranslateGpuVa(
            obj, build_desc.SourceAccelerationStructureData, 4096u, false);
        build_desc.ScratchAccelerationStructureData = UnwrapResourceProxyOrTranslateGpuVa(
            obj, build_desc.ScratchAccelerationStructureData, 4096u, true);

        std::array<D3D12_RAYTRACING_GEOMETRY_DESC, 16> stack_geoms{};
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> heap_geoms;
        if (build_desc.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
            if (build_desc.Inputs.InstanceDescs != 0) {
                build_desc.Inputs.InstanceDescs = UnwrapResourceProxyOrTranslateGpuVa(
                    obj, build_desc.Inputs.InstanceDescs,
                    static_cast<size_t>(build_desc.Inputs.NumDescs) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            }
        } else if (build_desc.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL &&
                   build_desc.Inputs.NumDescs > 0) {
            D3D12_RAYTRACING_GEOMETRY_DESC* geoms = (build_desc.Inputs.NumDescs <= stack_geoms.size())
                ? stack_geoms.data()
                : (heap_geoms.resize(build_desc.Inputs.NumDescs), heap_geoms.data());
            for (UINT i = 0; i < build_desc.Inputs.NumDescs; ++i) {
                if (build_desc.Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS &&
                    build_desc.Inputs.ppGeometryDescs && build_desc.Inputs.ppGeometryDescs[i]) {
                    geoms[i] = *build_desc.Inputs.ppGeometryDescs[i];
                } else if (build_desc.Inputs.pGeometryDescs) {
                    geoms[i] = build_desc.Inputs.pGeometryDescs[i];
                }
                if (geoms[i].Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
                    if (geoms[i].Triangles.Transform3x4 != 0) {
                        geoms[i].Triangles.Transform3x4 = UnwrapResourceProxyOrTranslateGpuVa(
                            obj, geoms[i].Triangles.Transform3x4, 48u);
                    }
                    if (geoms[i].Triangles.IndexBuffer != 0) {
                        geoms[i].Triangles.IndexBuffer = UnwrapResourceProxyOrTranslateGpuVa(
                            obj, geoms[i].Triangles.IndexBuffer,
                            static_cast<size_t>(geoms[i].Triangles.IndexCount) * 4u);
                    }
                    if (geoms[i].Triangles.VertexBuffer.StartAddress != 0) {
                        const size_t vstride = geoms[i].Triangles.VertexBuffer.StrideInBytes
                                                   ? static_cast<size_t>(geoms[i].Triangles.VertexBuffer.StrideInBytes)
                                                   : 12u;
                        geoms[i].Triangles.VertexBuffer.StartAddress = UnwrapResourceProxyOrTranslateGpuVa(
                            obj, geoms[i].Triangles.VertexBuffer.StartAddress,
                            static_cast<size_t>(geoms[i].Triangles.VertexCount) * vstride);
                    }
                } else if (geoms[i].Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
                    if (geoms[i].AABBs.AABBs.StartAddress != 0) {
                        const size_t astride = geoms[i].AABBs.AABBs.StrideInBytes
                                                   ? static_cast<size_t>(geoms[i].AABBs.AABBs.StrideInBytes)
                                                   : 24u;
                        geoms[i].AABBs.AABBs.StartAddress = UnwrapResourceProxyOrTranslateGpuVa(
                            obj, geoms[i].AABBs.AABBs.StartAddress,
                            static_cast<size_t>(geoms[i].AABBs.AABBCount) * astride);
                    }
                }
            }
            build_desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            build_desc.Inputs.pGeometryDescs = geoms;
        }

        std::array<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC, 8> stack_post{};
        std::vector<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC> heap_post;
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* post_ptr = nullptr;
        UINT post_cnt = 0;
        if (numPostbuildInfoDescs > 0 && pPostbuildInfoDescs) {
            post_cnt = static_cast<UINT>(numPostbuildInfoDescs);
            auto* out_post = (post_cnt <= stack_post.size())
                ? stack_post.data()
                : (heap_post.resize(post_cnt), heap_post.data());
            const auto* in_post =
                reinterpret_cast<const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*>(
                    pPostbuildInfoDescs);
            for (UINT i = 0; i < post_cnt; ++i) {
                out_post[i] = in_post[i];
                out_post[i].DestBuffer = UnwrapResourceProxyOrTranslateGpuVa(obj, out_post[i].DestBuffer, 256u, true);
            }
            post_ptr = out_post;
        }

        obj->pc_list4->BuildRaytracingAccelerationStructure(&build_desc, post_cnt, post_ptr);
    }
#else
    (void)self;
    (void)pDesc;
    (void)numPostbuildInfoDescs;
    (void)pPostbuildInfoDescs;
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListCopyRaytracingAccelerationStructure(
    void* self, uintptr_t destData, uintptr_t srcData, uintptr_t mode) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj && obj->pc_list4 && destData != 0 && srcData != 0) {
        const auto dst = UnwrapResourceProxyOrTranslateGpuVa(
            obj, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(destData), 4096u, true);
        const auto src = UnwrapResourceProxyOrTranslateGpuVa(
            obj, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(srcData), 4096u, false);
        obj->pc_list4->CopyRaytracingAccelerationStructure(
            dst, src, static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE>(mode));
    }
#else
    (void)self;
    (void)destData;
    (void)srcData;
    (void)mode;
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListRSSetShadingRate(
    void* self, uintptr_t baseShadingRate, uintptr_t pCombiners, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj && obj->pc_list5 && obj->pc_device) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 opts6{};
        if (SUCCEEDED(obj->pc_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opts6, sizeof(opts6))) &&
            opts6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED) {
            auto rate = static_cast<D3D12_SHADING_RATE>(baseShadingRate);
            if (!opts6.AdditionalShadingRatesSupported && rate > D3D12_SHADING_RATE_2X2) {
                rate = D3D12_SHADING_RATE_2X2;
            }
            obj->pc_list5->RSSetShadingRate(rate, reinterpret_cast<const D3D12_SHADING_RATE_COMBINER*>(pCombiners));
        }
    }
#else
    (void)self;
    (void)baseShadingRate;
    (void)pCombiners;
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListRSSetShadingRateImage(
    void* self, uintptr_t pShadingRateImage, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj && obj->pc_list5 && obj->pc_device) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 opts6{};
        if (SUCCEEDED(obj->pc_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opts6, sizeof(opts6))) &&
            opts6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2) {
            ID3D12Resource* img = pShadingRateImage
                ? reinterpret_cast<XboxD3D12ResourceProxy*>(pShadingRateImage)->pc_res
                : nullptr;
            obj->pc_list5->RSSetShadingRateImage(img);
        }
    }
#else
    (void)self;
    (void)pShadingRateImage;
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListDispatchMesh(
    void* self, uintptr_t x, uintptr_t y, uintptr_t z) {
#if defined(_WIN32) && defined(__ID3D12GraphicsCommandList6_INTERFACE_DEFINED__)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj && obj->pc_list6) {
        obj->pc_list6->DispatchMesh(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    }
#else
    (void)self;
    (void)x;
    (void)y;
    (void)z;
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListBarrier(
    void* self, uintptr_t numBarrierGroups, uintptr_t pBarrierGroups, uintptr_t) {
#if defined(_WIN32) && defined(__ID3D12GraphicsCommandList7_INTERFACE_DEFINED__)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj && obj->pc_list7 && numBarrierGroups > 0 && pBarrierGroups) {
        const auto* in_groups = reinterpret_cast<const D3D12_BARRIER_GROUP*>(pBarrierGroups);
        size_t total_tex = 0;
        size_t total_buf = 0;
        for (size_t g = 0; g < static_cast<size_t>(numBarrierGroups); ++g) {
            if (in_groups[g].Type == D3D12_BARRIER_TYPE_TEXTURE) total_tex += in_groups[g].NumBarriers;
            else if (in_groups[g].Type == D3D12_BARRIER_TYPE_BUFFER) total_buf += in_groups[g].NumBarriers;
        }

        std::array<D3D12_BARRIER_GROUP, 16> stack_groups{};
        std::array<D3D12_TEXTURE_BARRIER, 32> stack_tex{};
        std::array<D3D12_BUFFER_BARRIER, 32> stack_buf{};
        std::unique_ptr<D3D12_BARRIER_GROUP[]> heap_groups;
        std::unique_ptr<D3D12_TEXTURE_BARRIER[]> heap_tex;
        std::unique_ptr<D3D12_BUFFER_BARRIER[]> heap_buf;

        D3D12_BARRIER_GROUP* out_groups = (numBarrierGroups <= stack_groups.size())
            ? stack_groups.data()
            : (heap_groups = std::make_unique<D3D12_BARRIER_GROUP[]>(static_cast<size_t>(numBarrierGroups))).get();
        D3D12_TEXTURE_BARRIER* out_tex = (total_tex <= stack_tex.size())
            ? stack_tex.data()
            : (heap_tex = std::make_unique<D3D12_TEXTURE_BARRIER[]>(total_tex)).get();
        D3D12_BUFFER_BARRIER* out_buf = (total_buf <= stack_buf.size())
            ? stack_buf.data()
            : (heap_buf = std::make_unique<D3D12_BUFFER_BARRIER[]>(total_buf)).get();

        UINT32 grp_count = 0;
        size_t tex_cursor = 0;
        size_t buf_cursor = 0;

        for (size_t g = 0; g < static_cast<size_t>(numBarrierGroups); ++g) {
            D3D12_BARRIER_GROUP grp = in_groups[g];
            if (grp.Type == D3D12_BARRIER_TYPE_TEXTURE) {
                if (grp.NumBarriers == 0 || !grp.pTextureBarriers) continue;
                const size_t start = tex_cursor;
                for (UINT32 b = 0; b < grp.NumBarriers; ++b) {
                    D3D12_TEXTURE_BARRIER tb = grp.pTextureBarriers[b];
                    if (!tb.pResource) continue;
                    auto* rp = reinterpret_cast<XboxD3D12ResourceProxy*>(tb.pResource);
                    if (!IsReadableMemory(rp, sizeof(*rp)) || !rp->pc_res) continue;
                    tb.pResource = rp->pc_res;
                    out_tex[tex_cursor++] = tb;
                }
                const UINT32 n = static_cast<UINT32>(tex_cursor - start);
                if (n == 0) continue;
                grp.NumBarriers = n;
                grp.pTextureBarriers = &out_tex[start];
                out_groups[grp_count++] = grp;
            } else if (grp.Type == D3D12_BARRIER_TYPE_BUFFER) {
                if (grp.NumBarriers == 0 || !grp.pBufferBarriers) continue;
                const size_t start = buf_cursor;
                for (UINT32 b = 0; b < grp.NumBarriers; ++b) {
                    D3D12_BUFFER_BARRIER bb = grp.pBufferBarriers[b];
                    if (!bb.pResource) continue;
                    auto* rp = reinterpret_cast<XboxD3D12ResourceProxy*>(bb.pResource);
                    if (!IsReadableMemory(rp, sizeof(*rp)) || !rp->pc_res) continue;
                    bb.pResource = rp->pc_res;
                    out_buf[buf_cursor++] = bb;
                }
                const UINT32 n = static_cast<UINT32>(buf_cursor - start);
                if (n == 0) continue;
                grp.NumBarriers = n;
                grp.pBufferBarriers = &out_buf[start];
                out_groups[grp_count++] = grp;
            } else if (grp.Type == D3D12_BARRIER_TYPE_GLOBAL) {
                if (grp.NumBarriers == 0 || !grp.pGlobalBarriers) continue;
                out_groups[grp_count++] = grp;
            }
        }
        if (grp_count > 0) {
            obj->pc_list7->Barrier(grp_count, out_groups);
        }
    }
#else
    (void)self;
    (void)numBarrierGroups;
    (void)pBarrierGroups;
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListFlushPipelineX(void* self, uintptr_t flags, uintptr_t a2, uintptr_t a3) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::FlushPipelineX(self=" << self
                  << ", flags=0x" << std::hex << flags << ", a2=0x" << a2
                  << ", a3=0x" << a3 << std::dec << ")\n";
    }
    if (obj && obj->pc_list && obj->pc_list->GetType() != D3D12_COMMAND_LIST_TYPE_COPY) {
        const size_t max_barriers = obj->cmd_buffer_states.size() + 2u;
        std::array<D3D12_RESOURCE_BARRIER, 32> stack_barriers{};
        std::unique_ptr<D3D12_RESOURCE_BARRIER[]> heap_barriers;
        D3D12_RESOURCE_BARRIER* barriers = (max_barriers <= stack_barriers.size())
            ? stack_barriers.data()
            : (heap_barriers = std::make_unique<D3D12_RESOURCE_BARRIER[]>(max_barriers)).get();
        UINT count = 0;

        // Transition any UAV-promoted DEFAULT buffers back to COMMON so subsequent
        // ExecuteIndirect (INDIRECT_ARGUMENT), SRV, CBV, VB/IB, or UAV ops promote
        // cleanly without per-resource state mismatch errors under D3D12 Debug Layer.
        for (auto& kv : obj->cmd_buffer_states) {
            if (kv.first && kv.first->pc_res && kv.second != D3D12_RESOURCE_STATE_COMMON) {
                D3D12_RESOURCE_BARRIER tr{};
                tr.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tr.Transition.pResource = kv.first->pc_res;
                tr.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tr.Transition.StateBefore = kv.second;
                tr.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                barriers[count++] = tr;
                kv.second = D3D12_RESOURCE_STATE_COMMON;
                RecordCmdListPendingFinalState(obj, kv.first, D3D12_RESOURCE_STATE_COMMON);
            }
        }

        // Global UAV + global Aliasing barrier to drain shader writes & L2 caches
        // across UAV dispatches, indirect arguments, and aliased ID3D12Heap resources.
        D3D12_RESOURCE_BARRIER uav_b{};
        uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_b.UAV.pResource = nullptr;
        barriers[count++] = uav_b;

        if (obj->pc_list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
            obj->pc_list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
            D3D12_RESOURCE_BARRIER alias_b{};
            alias_b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            alias_b.Aliasing.pResourceBefore = nullptr;
            alias_b.Aliasing.pResourceAfter = nullptr;
            barriers[count++] = alias_b;
        }

        obj->pc_list->ResourceBarrier(count, barriers);
    }
#else
    (void)self;
    (void)flags;
    (void)a2;
    (void)a3;
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK CmdListTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::ostringstream line;
        line << "[ScarlettX][d3d12_xs] XboxID3D12GraphicsCommandList::vtable[" << Slot
             << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
             << ", caller=0x" << reinterpret_cast<uintptr_t>(SCARLETTX_RETURN_ADDRESS())
             << std::dec << ") -> E_NOTIMPL";
#if defined(_WIN32)
        // Unknown slot: show what a pointer argument refers to, to help identify it.
        MEMORY_BASIC_INFORMATION mbi = {};
        if (a1 > 0x10000 && VirtualQuery(reinterpret_cast<void*>(a1), &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            a1 + 0x80 <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
            line << " [a1:" << std::hex;
            for (int i = 0; i < 16; ++i) {
                uint64_t q = 0;
                std::memcpy(&q, reinterpret_cast<const void*>(a1 + i * 8), 8);
                line << ' ' << q;
            }
            line << std::dec << ']';
        }
#endif
        line << "\n";
        std::cerr << line.str();
    }
    scarlettx::FlightRecorder::Instance().RecordVtableSlotMiss(
        "XboxID3D12GraphicsCommandList", Slot, static_cast<uint64_t>(a1), SCARLETTX_CALLER_RIP());
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeCmdListVtable(std::index_sequence<Is...>) {
    return {&CmdListQueryInterface, &CmdListAddRef, &CmdListRelease, &CmdListTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 128> BuildCmdListVtable() {
    auto vtbl = MakeCmdListVtable(std::make_index_sequence<125>{});
    vtbl[6] = &CmdListSetName;
    vtbl[8] = &CmdListGetType;
    vtbl[9] = &CmdListClose;
    vtbl[10] = &CmdListReset;
    vtbl[11] = &CmdListClearState;
    vtbl[12] = reinterpret_cast<ComVtableFn>(&CmdListDrawInstanced);
    vtbl[13] = reinterpret_cast<ComVtableFn>(&CmdListDrawIndexedInstanced);
    vtbl[14] = &CmdListDispatch;
    vtbl[15] = reinterpret_cast<ComVtableFn>(&CmdListCopyBufferRegion);
    vtbl[16] = reinterpret_cast<ComVtableFn>(&CmdListCopyTextureRegion);
    // Slot 83 (+0x298) is CopyTextureRegionX: the argument list of slot 16
    // plus copy flags (0 at every observed call). Titles upload textures
    // with it.
    vtbl[83] = reinterpret_cast<ComVtableFn>(&CmdListCopyTextureRegion);
    vtbl[17] = &CmdListCopyResource;
    vtbl[19] = reinterpret_cast<ComVtableFn>(&CmdListResolveSubresource);
    vtbl[20] = &CmdListIASetPrimitiveTopology;
    vtbl[21] = &CmdListRSSetViewports;
    vtbl[22] = &CmdListRSSetScissorRects;
    vtbl[23] = &CmdListOMSetBlendFactor;
    vtbl[24] = &CmdListOMSetStencilRef;
    vtbl[25] = &CmdListSetPipelineState;
    vtbl[26] = &CmdListResourceBarrier;
    vtbl[27] = &CmdListExecuteBundle;
    // Xbox d3d12_xs shifts PC slots >= 28 by +4 (verified from Concept Destruction traces).
    vtbl[32] = &CmdListSetDescriptorHeaps;
    vtbl[33] = &CmdListSetComputeRootSignature;
    vtbl[34] = &CmdListSetGraphicsRootSignature;
    vtbl[35] = &CmdListSetComputeRootDescriptorTable;
    vtbl[36] = &CmdListSetGraphicsRootDescriptorTable;
    vtbl[37] = &CmdListSetComputeRoot32BitConstant;
    vtbl[38] = &CmdListSetGraphicsRoot32BitConstant;
    vtbl[39] = reinterpret_cast<ComVtableFn>(&CmdListSetComputeRoot32BitConstants);
    vtbl[40] = reinterpret_cast<ComVtableFn>(&CmdListSetGraphicsRoot32BitConstants);
    vtbl[41] = &CmdListSetComputeRootCBV;
    vtbl[42] = &CmdListSetGraphicsRootCBV;
    vtbl[43] = &CmdListSetComputeRootSRV;
    vtbl[44] = &CmdListSetGraphicsRootSRV;
    vtbl[45] = &CmdListSetComputeRootUAV;
    vtbl[46] = &CmdListSetGraphicsRootUAV;
    vtbl[47] = &CmdListIASetIndexBuffer;
    vtbl[48] = &CmdListIASetVertexBuffers;
    vtbl[50] = reinterpret_cast<ComVtableFn>(&CmdListOMSetRenderTargets);
    vtbl[51] = reinterpret_cast<ComVtableFn>(&CmdListClearDepthStencilViewTyped);
    vtbl[52] = reinterpret_cast<ComVtableFn>(&CmdListClearRenderTargetViewTyped);
    vtbl[55] = &CmdListDiscardResource;
    vtbl[56] = &CmdListBeginQuery;
    vtbl[57] = &CmdListEndQuery;
    vtbl[58] = reinterpret_cast<ComVtableFn>(&CmdListResolveQueryData);
    vtbl[60] = &CmdListNoop;  // SetMarker
    vtbl[61] = &CmdListNoop;  // BeginEvent
    vtbl[62] = &CmdListNoop;  // EndEvent
    vtbl[63] = reinterpret_cast<ComVtableFn>(&CmdListExecuteIndirect);
    vtbl[67] = &CmdListFlushPipelineX;
    vtbl[70] = &CmdListSetPipelineState1;
    vtbl[71] = &CmdListDispatchRays;
    vtbl[72] = &CmdListBuildRaytracingAccelerationStructure;
    vtbl[73] = &CmdListCopyRaytracingAccelerationStructure;
    vtbl[74] = &CmdListRSSetShadingRate;
    vtbl[75] = &CmdListRSSetShadingRateImage;
    vtbl[76] = &CmdListDispatchMesh;
    vtbl[90] = &CmdListBarrier;
    return vtbl;
}
const auto kXboxCmdListProxyVtbl = BuildCmdListVtable();
// A later SDK (D3D12XboxCreateDevice version 0x20004+) added two methods at
// slots 59 and 60, so each method after them sits two slots further on:
// CopyTextureRegionX is slot 85. Checked against the method names in the
// debug driver each SDK's titles ship with.
std::array<ComVtableFn, 128> BuildCmdListVtableV4() {
    const auto older = BuildCmdListVtable();
    const auto unnamed = MakeCmdListVtable(std::make_index_sequence<125>{});
    auto vtbl = older;
    for (size_t slot = 59; slot < vtbl.size(); ++slot) {
        const bool named = slot >= 61 && older[slot - 2] != unnamed[slot - 2];
        vtbl[slot] = named ? older[slot - 2] : unnamed[slot];
    }
    return vtbl;
}
const auto kXboxCmdListProxyVtblV4 = BuildCmdListVtableV4();
std::array<ComVtableFn, 128> BuildDmaCmdListVtable() {
    auto vtbl = MakeCmdListVtable(std::make_index_sequence<125>{});
    vtbl[6] = &CmdListSetName;
    vtbl[8] = &CmdListGetType;
    vtbl[9] = &CmdListClose;
    vtbl[10] = &CmdListReset;
    vtbl[11] = reinterpret_cast<ComVtableFn>(&CmdListCopyBufferRegion);   // +0x58
    vtbl[12] = reinterpret_cast<ComVtableFn>(&CmdListCopyTextureRegion);  // +0x60
    vtbl[15] = &CmdListResourceBarrier;                                   // +0x78
    return vtbl;
}
const auto kXboxDmaCmdListProxyVtbl = BuildDmaCmdListVtable();

struct XboxD3D12CommandQueueProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12CommandQueue* pc_queue = nullptr;
    ID3D12Device* pc_device = nullptr;
    HWND present_hwnd = nullptr;
    HANDLE present_window_thread = nullptr;
    HANDLE present_window_ready = nullptr;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12CommandAllocator* present_alloc = nullptr;
    ID3D12GraphicsCommandList* present_list = nullptr;
    ID3D12Fence* present_fence = nullptr;
    HANDLE present_event = nullptr;
    uint64_t present_fence_val = 0;
    UINT swap_w = 0;
    UINT swap_h = 0;
    UINT swap_flags = 0;
    DXGI_FORMAT swap_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool present_init_attempted = false;

    ID3D12Fence* upload_fence = nullptr;
    uint64_t upload_fence_val = 0;
    std::mutex staging_mu;
    std::vector<std::pair<ID3D12CommandAllocator*, uint64_t>> staging_alloc_pool;
    ID3D12GraphicsCommandList* staging_list = nullptr;
#endif
};

#if defined(_WIN32)
void StopPresentWindow(XboxD3D12CommandQueueProxy* obj);

ID3D12CommandList* RecordDirtyGuestBufferStagingOnQueue(
    XboxD3D12CommandQueueProxy* q_obj,
    std::vector<std::unique_ptr<UploadPage>>& out_staging_pages,
    ID3D12CommandAllocator** out_used_alloc) {
    if (out_used_alloc) *out_used_alloc = nullptr;
    if (!q_obj || !q_obj->pc_device || !q_obj->pc_queue) return nullptr;

    struct PendingBufCopy {
        XboxD3D12ResourceProxy* owner = nullptr;
        ID3D12Resource* dst_res = nullptr;
        size_t size = 0;
        std::unique_ptr<UploadPage> page;
        uint64_t page_offset = 0;
    };
    std::vector<PendingBufCopy> copies;
    std::unique_ptr<UploadPage> cur_page;

    {
        std::lock_guard<std::mutex> lk(g_guest_buffers_mutex);
        for (GuestPlacedBufferEntry& e : g_guest_buffers) {
            if (e.pc_device != q_obj->pc_device || !e.external_cpu_va || !e.pc_res || e.size == 0) continue;
            if (!IsReadableMemory(e.external_cpu_va, e.size)) continue;
            const uint64_t cur_hash = HashMemoryRegionFast(e.external_cpu_va, e.size);
            if (cur_hash == e.last_hash) continue;
            e.last_hash = cur_hash;

            uint64_t slice_off = 0;
            if (!cur_page || AlignUp(cur_page->write_offset, 256ull) + e.size > cur_page->capacity) {
                if (cur_page) out_staging_pages.push_back(std::move(cur_page));
                cur_page = AcquireUploadPage(q_obj->pc_device, e.size);
                if (!cur_page) continue;
            }
            slice_off = AlignUp(cur_page->write_offset, 256ull);
            std::memcpy(cur_page->mapped_ptr + slice_off, e.external_cpu_va, e.size);
            cur_page->write_offset = slice_off + e.size;
            g_upload_bytes.fetch_add(e.size, std::memory_order_relaxed);

            PendingBufCopy pc{};
            pc.owner = e.owner_proxy;
            pc.dst_res = e.pc_res;
            pc.size = e.size;
            pc.page_offset = slice_off;
            copies.push_back(std::move(pc));
            // Attach current page resource via raw pointer from out_staging_pages
            copies.back().page.reset();
        }
    }

    if (copies.empty()) {
        if (cur_page) out_staging_pages.push_back(std::move(cur_page));
        return nullptr;
    }

    ID3D12Resource* active_page_res = cur_page ? cur_page->pc_res : nullptr;
    if (cur_page) out_staging_pages.push_back(std::move(cur_page));
    // Re-bind each copy's source resource across out_staging_pages in order:
    // if all fit in the last page (99.9% case), active_page_res handles all; for multi-page,
    // match by walking cumulative sizes.
    const D3D12_COMMAND_LIST_TYPE q_type = q_obj->pc_queue->GetDesc().Type;
    ID3D12CommandAllocator* alloc = nullptr;
    const uint64_t completed = q_obj->upload_fence ? q_obj->upload_fence->GetCompletedValue() : ~0ull;

    std::lock_guard<std::mutex> slk(q_obj->staging_mu);
    for (auto it = q_obj->staging_alloc_pool.begin(); it != q_obj->staging_alloc_pool.end(); ++it) {
        if (it->second <= completed) {
            alloc = it->first;
            q_obj->staging_alloc_pool.erase(it);
            break;
        }
    }
    if (!alloc) {
        if (FAILED(q_obj->pc_device->CreateCommandAllocator(q_type, IID_PPV_ARGS(&alloc))) || !alloc) {
            return nullptr;
        }
    } else {
        (void)alloc->Reset();
    }

    if (!q_obj->staging_list) {
        if (FAILED(q_obj->pc_device->CreateCommandList(0, q_type, alloc, nullptr, IID_PPV_ARGS(&q_obj->staging_list))) ||
            !q_obj->staging_list) {
            alloc->Release();
            return nullptr;
        }
    } else {
        if (FAILED(q_obj->staging_list->Reset(alloc, nullptr))) {
            alloc->Release();
            return nullptr;
        }
    }

    size_t page_idx = 0;
    uint64_t prev_off = 0;
    for (size_t i = 0; i < copies.size(); ++i) {
        if (i > 0 && copies[i].page_offset <= prev_off && page_idx + 1 < out_staging_pages.size()) {
            ++page_idx;
        }
        prev_off = copies[i].page_offset;
        ID3D12Resource* src_res = (page_idx < out_staging_pages.size() && out_staging_pages[page_idx])
            ? out_staging_pages[page_idx]->pc_res
            : active_page_res;
        if (!src_res || !copies[i].dst_res) continue;

        const auto prev_state = copies[i].owner
            ? static_cast<D3D12_RESOURCE_STATES>(copies[i].owner->exec_state.load(std::memory_order_relaxed))
            : D3D12_RESOURCE_STATE_COMMON;
        if (q_type != D3D12_COMMAND_LIST_TYPE_COPY &&
            prev_state != D3D12_RESOURCE_STATE_COMMON &&
            prev_state != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = copies[i].dst_res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = prev_state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            q_obj->staging_list->ResourceBarrier(1, &b);
        }

        q_obj->staging_list->CopyBufferRegion(
            copies[i].dst_res, 0ull, src_res, copies[i].page_offset, copies[i].size);

        if (q_type != D3D12_COMMAND_LIST_TYPE_COPY) {
            const D3D12_RESOURCE_STATES restore_state =
                (prev_state == D3D12_RESOURCE_STATE_COPY_DEST) ? D3D12_RESOURCE_STATE_COPY_DEST : prev_state;
            if (restore_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = copies[i].dst_res;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = restore_state;
                q_obj->staging_list->ResourceBarrier(1, &b);
            }
        }
    }

    if (FAILED(q_obj->staging_list->Close())) {
        q_obj->staging_alloc_pool.emplace_back(alloc, 0ull);
        return nullptr;
    }
    if (out_used_alloc) *out_used_alloc = alloc;
    return q_obj->staging_list;
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (TraceEnabled()) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12CommandQueue::QueryInterface("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ")\n";
    }
    if (!IsD3D12CommandQueueIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12CommandQueue", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12CommandQueueProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_queue && obj->pc_device) {
            ID3D12Fence* drain_fence = nullptr;
            if (SUCCEEDED(obj->pc_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain_fence))) &&
                drain_fence) {
                HANDLE drain_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (SUCCEEDED(obj->pc_queue->Signal(drain_fence, 1ull)) &&
                    drain_fence->GetCompletedValue() < 1ull && drain_ev &&
                    SUCCEEDED(drain_fence->SetEventOnCompletion(1ull, drain_ev))) {
                    WaitForSingleObject(drain_ev, 5000);
                }
                if (drain_ev) CloseHandle(drain_ev);
                drain_fence->Release();
            }
        }
        if (obj->pc_device) {
            if (DeviceUploadRing* ring = GetOrCreateDeviceUploadRing(obj->pc_device)) {
                std::lock_guard<std::mutex> lk(ring->mu);
                ring->ReclaimCompletedLocked();
            }
        }
        if (obj->staging_list) obj->staging_list->Release();
        for (auto& kv : obj->staging_alloc_pool) {
            if (kv.first) kv.first->Release();
        }
        obj->staging_alloc_pool.clear();
        if (obj->upload_fence) obj->upload_fence->Release();
        if (obj->present_event) CloseHandle(obj->present_event);
        if (obj->present_fence) obj->present_fence->Release();
        if (obj->present_list) obj->present_list->Release();
        if (obj->present_alloc) obj->present_alloc->Release();
        if (obj->swapchain) obj->swapchain->Release();
        StopPresentWindow(obj);
        if (obj->pc_queue) obj->pc_queue->Release();
        if (obj->pc_device) obj->pc_device->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (obj->pc_queue && name_ptr) {
        return static_cast<intptr_t>(obj->pc_queue->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
    }
#endif
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK QueueProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12CommandQueue::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    scarlettx::FlightRecorder::Instance().RecordVtableSlotMiss(
        "XboxID3D12CommandQueue", Slot, static_cast<uint64_t>(a1), SCARLETTX_CALLER_RIP());
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeQueueProxyVtable(std::index_sequence<Is...>) {
    return {
        &QueueProxyQueryInterface,
        &QueueProxyAddRef,
        &QueueProxyRelease,
        &QueueProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyUpdateTileMappings(
    void* self, uintptr_t pResource, uintptr_t numResourceRegions,
    uintptr_t pResourceRegionStartCoordinates, uintptr_t pResourceRegionSizes,
    uintptr_t pHeap, uintptr_t numRanges, uintptr_t pRangeFlags,
    uintptr_t pHeapRangeStartOffsets, uintptr_t pRangeTileCounts, uintptr_t flags) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    auto* res_proxy = reinterpret_cast<XboxD3D12ResourceProxy*>(pResource);
    if (!obj || !obj->pc_queue || !obj->pc_device || !res_proxy || !res_proxy->pc_res || numResourceRegions == 0) {
        return 0;
    }
    ID3D12Heap* pc_heap = nullptr;
    if (pHeap) {
        auto* heap_proxy = reinterpret_cast<XboxD3D12HeapProxy*>(pHeap);
        std::lock_guard<std::mutex> lk(heap_proxy->tile_mu);
        if (!heap_proxy->pc_heap) {
            D3D12_HEAP_DESC hd = heap_proxy->desc;
            if (hd.Properties.Type == D3D12_HEAP_TYPE_CUSTOM) {
                hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            }
            if (hd.SizeInBytes < 65536ull) hd.SizeInBytes = 65536ull;
            hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            const D3D12_RESOURCE_DESC rd = res_proxy->pc_res->GetDesc();
            if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            } else if ((rd.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0) {
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
            } else {
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
            }
            (void)obj->pc_device->CreateHeap(&hd, IID_PPV_ARGS(&heap_proxy->pc_heap));
        }
        pc_heap = heap_proxy->pc_heap;
    }
    const D3D12_TILE_RANGE_FLAGS default_null_flag = D3D12_TILE_RANGE_FLAG_NULL;
    UINT eff_num_ranges = static_cast<UINT>(numRanges);
    const auto* eff_range_flags = reinterpret_cast<const D3D12_TILE_RANGE_FLAGS*>(pRangeFlags);
    if (!pc_heap && (eff_num_ranges == 0 || !eff_range_flags)) {
        eff_num_ranges = 1u;
        eff_range_flags = &default_null_flag;
    }
    obj->pc_queue->UpdateTileMappings(
        res_proxy->pc_res,
        static_cast<UINT>(numResourceRegions),
        reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(pResourceRegionStartCoordinates),
        reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(pResourceRegionSizes),
        pc_heap,
        eff_num_ranges,
        eff_range_flags,
        reinterpret_cast<const UINT*>(pHeapRangeStartOffsets),
        reinterpret_cast<const UINT*>(pRangeTileCounts),
        static_cast<D3D12_TILE_MAPPING_FLAGS>(flags));
#else
    (void)self; (void)pResource; (void)numResourceRegions;
    (void)pResourceRegionStartCoordinates; (void)pResourceRegionSizes;
    (void)pHeap; (void)numRanges; (void)pRangeFlags;
    (void)pHeapRangeStartOffsets; (void)pRangeTileCounts; (void)flags;
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyCopyTileMappings(
    void* self, uintptr_t pDstResource, uintptr_t pDstStartCoord,
    uintptr_t pSrcResource, uintptr_t pSrcStartCoord,
    uintptr_t pRegionSize, uintptr_t flags) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    auto* dst = reinterpret_cast<XboxD3D12ResourceProxy*>(pDstResource);
    auto* src = reinterpret_cast<XboxD3D12ResourceProxy*>(pSrcResource);
    if (obj && obj->pc_queue && dst && dst->pc_res && src && src->pc_res) {
        obj->pc_queue->CopyTileMappings(
            dst->pc_res,
            reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(pDstStartCoord),
            src->pc_res,
            reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(pSrcStartCoord),
            reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(pRegionSize),
            static_cast<D3D12_TILE_MAPPING_FLAGS>(flags));
    }
#else
    (void)self; (void)pDstResource; (void)pDstStartCoord;
    (void)pSrcResource; (void)pSrcStartCoord; (void)pRegionSize; (void)flags;
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyExecuteCommandLists(
    void* self, uintptr_t num, uintptr_t ppLists, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !ppLists || num == 0) return 0;
    auto** guest_lists = reinterpret_cast<XboxD3D12GraphicsCommandListProxy**>(ppLists);

    std::array<ID3D12CommandList*, 32> stack_lists{};
    std::unique_ptr<ID3D12CommandList*[]> heap_lists;
    const size_t max_slots = static_cast<size_t>(num) + 1u;
    ID3D12CommandList** pc_lists = (max_slots <= stack_lists.size())
        ? stack_lists.data()
        : (heap_lists = std::make_unique<ID3D12CommandList*[]>(max_slots)).get();

    // Reserve slot 0 in case dirty guest placed buffers require a fence-retired staging copy.
    UINT guest_count = 0;
    for (uintptr_t i = 0; i < num; ++i) {
        if (guest_lists[i] && guest_lists[i]->pc_list) {
            pc_lists[1u + guest_count++] = guest_lists[i]->pc_list;
        }
    }
    if (guest_count > 0) {
        g_submit_count.fetch_add(1, std::memory_order_relaxed);
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] Queue::ExecuteCommandLists(q=" << self
                      << ", count=" << guest_count << ")\n";
        }

        std::vector<std::unique_ptr<UploadPage>> retired_pages;
        ID3D12CommandAllocator* used_staging_alloc = nullptr;
        ID3D12CommandList* staging_cl = nullptr;

        const uint64_t epoch = g_frame_epoch.load(std::memory_order_relaxed);
        const bool first_submit = (epoch == 0 || g_flushed_epoch.load(std::memory_order_relaxed) != epoch);
        if (first_submit || HasPendingGuestTextureWrites()) {
            LARGE_INTEGER freq{}, start{}, end{};
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&start);
            staging_cl = RecordDirtyGuestBufferStagingOnQueue(obj, retired_pages, &used_staging_alloc);
            FlushGuestTextureWrites(obj->pc_queue);
            QueryPerformanceCounter(&end);
            g_flush_us.fetch_add(static_cast<uint64_t>(
                                     (end.QuadPart - start.QuadPart) * 1000000ll / freq.QuadPart),
                                 std::memory_order_relaxed);
            g_flush_count.fetch_add(1, std::memory_order_relaxed);
            if (first_submit) {
                g_flushed_epoch.store(epoch, std::memory_order_relaxed);
            }
        }

        ID3D12CommandList** submit_ptr = pc_lists + 1u;
        UINT submit_cnt = guest_count;
        if (staging_cl) {
            pc_lists[0] = staging_cl;
            submit_ptr = pc_lists;
            submit_cnt = guest_count + 1u;
        }

        obj->pc_queue->ExecuteCommandLists(submit_cnt, submit_ptr);

        // Collect all UploadPages referenced by the submitted command lists and retire on upload_fence.
        for (uintptr_t i = 0; i < num; ++i) {
            auto* gl = guest_lists[i];
            if (!gl) continue;
            for (auto& up : gl->used_upload_pages) {
                if (up) retired_pages.push_back(std::move(up));
            }
            gl->used_upload_pages.clear();
            if (gl->cur_upload_page) {
                retired_pages.push_back(std::move(gl->cur_upload_page));
            }
        }

        if ((!retired_pages.empty() || used_staging_alloc != nullptr) && obj->pc_device) {
            if (!obj->upload_fence) {
                (void)obj->pc_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&obj->upload_fence));
            }
            const uint64_t ufv = ++obj->upload_fence_val;
            if (obj->upload_fence) {
                (void)obj->pc_queue->Signal(obj->upload_fence, ufv);
            }
            if (used_staging_alloc) {
                std::lock_guard<std::mutex> slk(obj->staging_mu);
                obj->staging_alloc_pool.emplace_back(used_staging_alloc, ufv);
            }
            RetireUploadPagesOnFence(obj->pc_device, obj->upload_fence, ufv, retired_pages);
        }

        scarlettx::FlightRecorder::Instance().BeatProgress("ExecuteCommandLists");
        MaybeRecordDeviceRemoved(obj->pc_device, S_OK, "QueueProxyExecuteCommandLists");
        {
            std::lock_guard<std::mutex> live_lk(g_live_resources_mutex);
            for (uintptr_t i = 0; i < num; ++i) {
                auto* gl = guest_lists[i];
                if (!gl) continue;
                std::lock_guard<std::mutex> st_lk(gl->state_mu);
                for (const auto& kv : gl->pending_final_states) {
                    if (g_live_resources.count(kv.first) != 0) {
                        kv.first->exec_state.store(kv.second, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyNoop(void*, uintptr_t, uintptr_t, uintptr_t) { return 0; }

intptr_t SCARLETTX_GDK_CALLBACK QueueProxySignal(void* self, uintptr_t pFence, uintptr_t val, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !pFence) return static_cast<intptr_t>(E_INVALIDARG);
    auto* f = reinterpret_cast<XboxD3D12FenceProxy*>(pFence);
    if (!f->pc_fence) return static_cast<intptr_t>(E_INVALIDARG);
    return static_cast<intptr_t>(obj->pc_queue->Signal(f->pc_fence, val));
#else
    return 0;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyWait(void* self, uintptr_t pFence, uintptr_t val, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !pFence) return static_cast<intptr_t>(E_INVALIDARG);
    auto* f = reinterpret_cast<XboxD3D12FenceProxy*>(pFence);
    if (!f->pc_fence) return static_cast<intptr_t>(E_INVALIDARG);
    return static_cast<intptr_t>(obj->pc_queue->Wait(f->pc_fence, val));
#else
    return 0;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyGetTimestampFrequency(
    void* self, uintptr_t pFreq, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !pFreq) return static_cast<intptr_t>(E_INVALIDARG);
    const HRESULT hr = obj->pc_queue->GetTimestampFrequency(reinterpret_cast<UINT64*>(pFreq));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12CommandQueue::GetTimestampFrequency() -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec
                  << " (freq=" << *reinterpret_cast<UINT64*>(pFreq) << ")\n";
    }
    return static_cast<intptr_t>(hr);
#else
    return 0;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyGetClockCalibration(
    void* self, uintptr_t pGpu, uintptr_t pCpu, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !pGpu || !pCpu) return static_cast<intptr_t>(E_INVALIDARG);
    return static_cast<intptr_t>(obj->pc_queue->GetClockCalibration(
        reinterpret_cast<UINT64*>(pGpu), reinterpret_cast<UINT64*>(pCpu)));
#else
    return 0;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyGetDesc(void* self, uintptr_t sret_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (obj->pc_queue && sret_ptr) {
        *reinterpret_cast<D3D12_COMMAND_QUEUE_DESC*>(sret_ptr) = obj->pc_queue->GetDesc();
    }
#endif
    return static_cast<intptr_t>(sret_ptr);
}

#if defined(_WIN32)
constexpr UINT kMsgDestroyPresentWindow = WM_APP + 1;

// Borderless fullscreen on the window's monitor; the swap chain stretches.
void ToggleFullscreen(HWND hwnd) {
    static WINDOWPLACEMENT restore = {sizeof(restore)};
    const LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetWindowPlacement(hwnd, &restore) ||
            !GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            return;
        }
        SetWindowLongPtrA(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrA(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &restore);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK ScarlettXPresentWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {  // Alt+Enter
            if (!(lp & (1 << 30))) ToggleFullscreen(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        return 0;  // the title owns the lifetime of the output
    case kMsgDestroyPresentWindow:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        // Alt and F10 would put the window into menu mode and take the
        // keyboard. Alt+Space, the system menu, is left alone.
        if ((wp & 0xFFF0) == SC_KEYMENU && lp != ' ') return 0;
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// The window has its own thread. DefWindowProc runs modal loops (system menu,
// moving or sizing the window) that do not return until the user is done; on
// the thread that calls PresentX that would stop the title's submissions, and
// a title treats a fence that does not advance as a lost device.
DWORD WINAPI PresentWindowThread(void* param) {
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(param);
    // Keep this module loaded for as long as the thread runs its code.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCSTR>(&ScarlettXPresentWndProc), &self);

    const char* cls_name = "ScarlettXPresentWindowClass";
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ScarlettXPresentWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = cls_name;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);

    RECT rc{0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const char* offscreen_env = std::getenv("SCARLETTX_PRESENT_OFFSCREEN");
    const bool offscreen = offscreen_env && offscreen_env[0] != '\0' && offscreen_env[0] != '0';
    const DWORD win_style = offscreen ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    const int init_x = offscreen ? -32000 : CW_USEDEFAULT;
    const int init_y = offscreen ? -32000 : CW_USEDEFAULT;
    const HWND hwnd = CreateWindowExA(
        0, cls_name, "ScarlettX - Xbox Series X|S",
        win_style,
        init_x, init_y, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) {
        if (offscreen) {
            SetWindowPos(hwnd, HWND_BOTTOM, -32000, -32000, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        } else {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
    }
    obj->present_hwnd = hwnd;
    SetEvent(obj->present_window_ready);  // obj is not used past this point

    if (hwnd) {
        MSG msg = {};
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

void StartPresentWindow(XboxD3D12CommandQueueProxy* obj) {
    obj->present_window_ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!obj->present_window_ready) return;
    obj->present_window_thread = CreateThread(nullptr, 0, &PresentWindowThread, obj, 0, nullptr);
    if (obj->present_window_thread) WaitForSingleObject(obj->present_window_ready, INFINITE);
}

void StopPresentWindow(XboxD3D12CommandQueueProxy* obj) {
    if (obj->present_window_thread) {
        if (obj->present_hwnd) PostMessageA(obj->present_hwnd, kMsgDestroyPresentWindow, 0, 0);
        // The thread no longer touches obj; do not hang a Release on it.
        WaitForSingleObject(obj->present_window_thread, 5000);
        CloseHandle(obj->present_window_thread);
        obj->present_window_thread = nullptr;
    }
    if (obj->present_window_ready) {
        CloseHandle(obj->present_window_ready);
        obj->present_window_ready = nullptr;
    }
    obj->present_hwnd = nullptr;
}

DXGI_FORMAT NormalizeSwapChainFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void DumpResourceToBmp(
    XboxD3D12CommandQueueProxy* obj,
    ID3D12Resource* src_res,
    D3D12_RESOURCE_STATES cur_state,
    const std::string& path) {
    if (!obj || !obj->pc_device || !obj->pc_queue || !obj->present_alloc || !obj->present_list || !src_res) return;
    D3D12_RESOURCE_DESC desc = src_res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0) return;
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    obj->pc_device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &num_rows, &row_size, &total_bytes);
    if (total_bytes == 0 || fp.Footprint.RowPitch == 0) return;

    D3D12_HEAP_PROPERTIES rb_heap = {};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc = {};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = total_bytes;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* rb = nullptr;
    if (FAILED(obj->pc_device->CreateCommittedResource(
            &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb))) || !rb) {
        std::cerr << "[ScarlettX][d3d12_xs] frame dump: readback buffer creation failed (" << path << ")" << std::endl;
        return;
    }

    if (obj->present_fence && obj->present_fence->GetCompletedValue() < obj->present_fence_val) {
        if (obj->present_event) {
            obj->present_fence->SetEventOnCompletion(obj->present_fence_val, obj->present_event);
            WaitForSingleObject(obj->present_event, 5000);
        } else {
            obj->present_fence->SetEventOnCompletion(obj->present_fence_val, nullptr);
        }
    }
    if (FAILED(obj->present_alloc->Reset()) ||
        FAILED(obj->present_list->Reset(obj->present_alloc, nullptr))) {
        rb->Release();
        return;
    }

    if (cur_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src_res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = cur_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        obj->present_list->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = rb;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = src_res;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    obj->present_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    if (cur_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src_res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = cur_state;
        obj->present_list->ResourceBarrier(1, &b);
    }

    if (SUCCEEDED(obj->present_list->Close())) {
        ID3D12CommandList* lists[] = {obj->present_list};
        obj->pc_queue->ExecuteCommandLists(1, lists);
        const uint64_t val = ++obj->present_fence_val;
        obj->pc_queue->Signal(obj->present_fence, val);
        if (obj->present_fence->GetCompletedValue() < val) {
            obj->present_fence->SetEventOnCompletion(val, obj->present_event);
            WaitForSingleObject(obj->present_event, 5000);
        }

        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(total_bytes)};
        if (SUCCEEDED(rb->Map(0, &range, &mapped)) && mapped) {
            const uint32_t w = static_cast<uint32_t>(desc.Width);
            const uint32_t h = desc.Height;
            const uint32_t bmp_stride = ((w * 3u + 3u) & ~3u);
            const uint32_t pixel_bytes = bmp_stride * h;
            const uint32_t file_size = 54u + pixel_bytes;

            uint8_t hdr[54] = {};
            hdr[0] = 'B';
            hdr[1] = 'M';
            std::memcpy(hdr + 2, &file_size, 4);
            const uint32_t off = 54u;
            std::memcpy(hdr + 10, &off, 4);
            const uint32_t dib = 40u;
            std::memcpy(hdr + 14, &dib, 4);
            std::memcpy(hdr + 18, &w, 4);
            std::memcpy(hdr + 22, &h, 4);
            const uint16_t planes = 1u;
            const uint16_t bpp = 24u;
            std::memcpy(hdr + 26, &planes, 2);
            std::memcpy(hdr + 28, &bpp, 2);
            std::memcpy(hdr + 34, &pixel_bytes, 4);

            const bool is_bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                  desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                                  desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                std::cerr << "[ScarlettX][d3d12_xs] frame dump: cannot open " << path << std::endl;
            }
            if (out) {
                out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
                std::vector<uint8_t> row_buf(bmp_stride, 0);
                const auto* base = static_cast<const uint8_t*>(mapped);
                uint64_t non_black = 0;
                for (uint32_t y = 0; y < h; ++y) {
                    const uint32_t src_y = h - 1u - y;
                    const uint8_t* srow = base + static_cast<size_t>(src_y) * fp.Footprint.RowPitch;
                    for (uint32_t x = 0; x < w; ++x) {
                        const uint8_t c0 = srow[x * 4 + 0];
                        const uint8_t c1 = srow[x * 4 + 1];
                        const uint8_t c2 = srow[x * 4 + 2];
                        const uint8_t r = is_bgra ? c2 : c0;
                        const uint8_t g = c1;
                        const uint8_t b = is_bgra ? c0 : c2;
                        row_buf[x * 3 + 0] = b;
                        row_buf[x * 3 + 1] = g;
                        row_buf[x * 3 + 2] = r;
                        if (r || g || b) ++non_black;
                    }
                    out.write(reinterpret_cast<const char*>(row_buf.data()), static_cast<std::streamsize>(bmp_stride));
                }
                std::cerr << "[ScarlettX][d3d12_xs] Dumped frame to " << path
                          << " (" << w << "x" << h << ", fmt=" << desc.Format
                          << ", non_black_pixels=" << non_black << "/" << (static_cast<uint64_t>(w) * h) << ")\n";
            }
            D3D12_RANGE empty_range{0, 0};
            rb->Unmap(0, &empty_range);
        }
    }
    rb->Release();
}

std::atomic<int64_t> g_last_wait_frame_event_qpc{0};
std::atomic<uint32_t> g_last_wait_frame_interval_us{0};

void EnsurePresentSwapChain(XboxD3D12CommandQueueProxy* obj, UINT w, UINT h, DXGI_FORMAT src_fmt) {
    if (!obj || !obj->pc_device || !obj->pc_queue) return;

    const UINT target_w = (w > 0) ? w : 1920;
    const UINT target_h = (h > 0) ? h : 1080;
    const DXGI_FORMAT target_fmt = NormalizeSwapChainFormat(src_fmt);

    if (obj->present_init_attempted) {
        if (obj->swapchain &&
            (target_w != obj->swap_w || target_h != obj->swap_h || target_fmt != obj->swap_fmt)) {
            if (obj->present_fence && obj->present_event) {
                const uint64_t drain_val = ++obj->present_fence_val;
                if (SUCCEEDED(obj->pc_queue->Signal(obj->present_fence, drain_val)) &&
                    obj->present_fence->GetCompletedValue() < drain_val) {
                    obj->present_fence->SetEventOnCompletion(drain_val, obj->present_event);
                    WaitForSingleObject(obj->present_event, 5000);
                }
            }
            if (obj->present_alloc) {
                (void)obj->present_alloc->Reset();
            }
            const HRESULT rhr = obj->swapchain->ResizeBuffers(
                0, target_w, target_h, target_fmt, obj->swap_flags);
            if (SUCCEEDED(rhr)) {
                obj->swap_w = target_w;
                obj->swap_h = target_h;
                obj->swap_fmt = target_fmt;
                if (obj->present_hwnd) {
                    RECT rc{0, 0, static_cast<LONG>(target_w), static_cast<LONG>(target_h)};
                    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
                    SetWindowPos(obj->present_hwnd, nullptr, 0, 0,
                                 rc.right - rc.left, rc.bottom - rc.top,
                                 SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
                }
                if (TraceEnabled()) {
                    std::cerr << "[ScarlettX][d3d12_xs] SwapChain resized (" << obj->swap_w << "x" << obj->swap_h
                              << ", fmt=" << obj->swap_fmt << ")\n";
                }
            } else {
                MaybeRecordDeviceRemoved(obj->pc_device, rhr, "EnsurePresentSwapChain::ResizeBuffers");
            }
        }
        return;
    }
    obj->present_init_attempted = true;

    const char* headless_env = std::getenv("SCARLETTX_HEADLESS");
    if (headless_env && headless_env[0] != '\0' && headless_env[0] != '0') {
        return;
    }

    StartPresentWindow(obj);
    if (!obj->present_hwnd) return;

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || !factory) {
        return;
    }

    UINT sc_flags = 0;
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5))) && factory5) {
        BOOL allow_tearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing))) &&
            allow_tearing) {
            sc_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
        factory5->Release();
    }

    obj->swap_w = target_w;
    obj->swap_h = target_h;
    obj->swap_fmt = target_fmt;
    obj->swap_flags = sc_flags;

    if (obj->present_hwnd) {
        RECT rc{0, 0, static_cast<LONG>(target_w), static_cast<LONG>(target_h)};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(obj->present_hwnd, nullptr, 0, 0,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
    sc_desc.Width = obj->swap_w;
    sc_desc.Height = obj->swap_h;
    sc_desc.Format = obj->swap_fmt;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 3;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.Flags = obj->swap_flags;

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(
        obj->pc_queue, obj->present_hwnd, &sc_desc, nullptr, nullptr, &sc1);
    // DXGI's own Alt+Enter switch leaves every Present failing.
    factory->MakeWindowAssociation(obj->present_hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    if (FAILED(hr) || !sc1) {
        std::cerr << "[ScarlettX][d3d12_xs] CreateSwapChainForHwnd failed hr=0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
        return;
    }
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&obj->swapchain));
    sc1->Release();

    obj->pc_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
        reinterpret_cast<void**>(&obj->present_alloc));
    if (obj->present_alloc) {
        obj->pc_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, obj->present_alloc, nullptr,
            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&obj->present_list));
        if (obj->present_list) obj->present_list->Close();
    }
    obj->pc_device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&obj->present_fence));
    obj->present_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    std::cerr << "[ScarlettX][d3d12_xs] SwapChain created (" << obj->swap_w << "x" << obj->swap_h
              << ", fmt=" << obj->swap_fmt << ", sc=" << obj->swapchain << ")\n";
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyPresentX(
    void* self, uintptr_t planeCount, uintptr_t pPlaneParams, uintptr_t pPresentParams) {
    (void)pPresentParams;
    if (scarlettx::IsAsyncShutdownRequested()) {
        scarlettx::TryAbortActiveInProcessGuest(0);
        return 0;
    }
    static std::atomic<uint64_t> present_count{0};
    const uint64_t n = present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    g_frame_epoch.fetch_add(1, std::memory_order_relaxed);
    g_gpu_profiler.RecordQpcTick();
#if defined(_WIN32)
    LARGE_INTEGER now_qpc{}, freq_qpc{};
    QueryPerformanceCounter(&now_qpc);
    QueryPerformanceFrequency(&freq_qpc);
    {
        static std::atomic<int64_t> last_present_qpc{0};
        const int64_t prev_qpc = last_present_qpc.exchange(now_qpc.QuadPart, std::memory_order_relaxed);
        if (prev_qpc > 0 && freq_qpc.QuadPart > 0 && now_qpc.QuadPart > prev_qpc) {
            const uint64_t delta_us = static_cast<uint64_t>(
                ((now_qpc.QuadPart - prev_qpc) * 1000000ll) / freq_qpc.QuadPart);
            scarlettx::FlightRecorder::Instance().RecordFrameTimeUs(delta_us);
        } else {
            scarlettx::FlightRecorder::Instance().BeatProgress("PresentX");
        }
    }
#else
    scarlettx::FlightRecorder::Instance().BeatProgress("PresentX");
#endif
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    XboxD3D12ResourceProxy* src_proxy = nullptr;
    if (pPlaneParams && planeCount > 0 &&
        IsReadableMemory(reinterpret_cast<const void*>(pPlaneParams), 24u)) {
        const auto* qwords = reinterpret_cast<const uint64_t*>(pPlaneParams);
        if (qwords[1] > 0 && qwords[2] != 0) {
            const void* q2_ptr = reinterpret_cast<const void*>(qwords[2]);
            if (IsReadableMemory(q2_ptr, sizeof(XboxD3D12ResourceProxy))) {
                auto* cand = reinterpret_cast<XboxD3D12ResourceProxy*>(qwords[2]);
                if (cand->lpVtbl == kXboxResourceProxyVtbl.data()) {
                    src_proxy = cand;
                }
            }
            if (!src_proxy && IsReadableMemory(q2_ptr, sizeof(void*))) {
                auto* deref = *reinterpret_cast<XboxD3D12ResourceProxy* const*>(q2_ptr);
                if (IsReadableMemory(deref, sizeof(XboxD3D12ResourceProxy)) &&
                    deref->lpVtbl == kXboxResourceProxyVtbl.data()) {
                    src_proxy = deref;
                }
            }
        }
    }

    if (src_proxy && src_proxy->pc_res && obj && obj->pc_device && obj->pc_queue) {
        const D3D12_RESOURCE_DESC sdesc = src_proxy->pc_res->GetDesc();
        EnsurePresentSwapChain(obj, static_cast<UINT>(sdesc.Width), sdesc.Height, sdesc.Format);

        const auto cur_state = static_cast<D3D12_RESOURCE_STATES>(
            src_proxy->exec_state.load(std::memory_order_relaxed));

        if (obj->swapchain && obj->present_alloc && obj->present_list && obj->present_fence && obj->present_event) {
            if (obj->present_fence->GetCompletedValue() < obj->present_fence_val) {
                obj->present_fence->SetEventOnCompletion(obj->present_fence_val, obj->present_event);
                WaitForSingleObject(obj->present_event, 2000);
            }
            const UINT bb_idx = obj->swapchain->GetCurrentBackBufferIndex();
            ID3D12Resource* bb = nullptr;
            if (SUCCEEDED(obj->swapchain->GetBuffer(bb_idx, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&bb))) && bb) {
                obj->present_alloc->Reset();
                obj->present_list->Reset(obj->present_alloc, nullptr);

                D3D12_RESOURCE_BARRIER pre[2] = {};
                UINT pre_n = 0;
                if (cur_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                    pre[pre_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    pre[pre_n].Transition.pResource = src_proxy->pc_res;
                    pre[pre_n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    pre[pre_n].Transition.StateBefore = cur_state;
                    pre[pre_n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    ++pre_n;
                }
                pre[pre_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pre[pre_n].Transition.pResource = bb;
                pre[pre_n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                pre[pre_n].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                pre[pre_n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                ++pre_n;
                obj->present_list->ResourceBarrier(pre_n, pre);

                obj->present_list->CopyResource(bb, src_proxy->pc_res);

                D3D12_RESOURCE_BARRIER post[2] = {};
                UINT post_n = 0;
                if (cur_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                    post[post_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    post[post_n].Transition.pResource = src_proxy->pc_res;
                    post[post_n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    post[post_n].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    post[post_n].Transition.StateAfter = cur_state;
                    ++post_n;
                }
                post[post_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                post[post_n].Transition.pResource = bb;
                post[post_n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                post[post_n].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                post[post_n].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                ++post_n;
                obj->present_list->ResourceBarrier(post_n, post);

                const HRESULT close_hr = obj->present_list->Close();
                bb->Release();
                bb = nullptr;

                if (SUCCEEDED(close_hr)) {
                    ID3D12CommandList* lists[] = {obj->present_list};
                    obj->pc_queue->ExecuteCommandLists(1, lists);

                    // Prevent double-throttling when the title paces via WaitFrameEventX!
                    const int64_t last_wait = g_last_wait_frame_event_qpc.load(std::memory_order_relaxed);
                    const uint32_t wait_us = g_last_wait_frame_interval_us.load(std::memory_order_relaxed);
                    const bool paced_by_wait_event =
                        (last_wait > 0 && wait_us > 0 && freq_qpc.QuadPart > 0 &&
                         now_qpc.QuadPart >= last_wait &&
                         (now_qpc.QuadPart - last_wait) <=
                             (freq_qpc.QuadPart * static_cast<int64_t>(wait_us) * 4ll) / 1000000ll);

                    const char* vsync_env = std::getenv("SCARLETTX_VSYNC");
                    const bool force_vsync = vsync_env && vsync_env[0] != '\0' && vsync_env[0] != '0';
                    const bool force_nosync = vsync_env && vsync_env[0] == '0';

                    const UINT sync_interval = force_nosync
                        ? 0u
                        : (force_vsync
                               ? 1u
                               : (paced_by_wait_event
                                      ? 0u
                                      : (g_frame_interval_us.load(std::memory_order_relaxed) > 0 ? 1u : 0u)));
                    const UINT present_flags =
                        (sync_interval == 0u && (obj->swap_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0u)
                            ? DXGI_PRESENT_ALLOW_TEARING
                            : 0u;

                    const HRESULT present_hr = obj->swapchain->Present(sync_interval, present_flags);
                    if (FAILED(present_hr)) {
                        MaybeRecordDeviceRemoved(obj->pc_device, present_hr, "QueueProxyPresentX");
                    }
                    if (present_hr != S_OK) {
                        static std::atomic<uint64_t> not_ok{0};
                        const uint64_t k = not_ok.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (k == 1 || (k % 1000) == 0) {
                            std::cerr << "[ScarlettX][d3d12_xs] Present returned 0x" << std::hex
                                      << static_cast<uint32_t>(present_hr) << std::dec << " (frame " << n << ", "
                                      << k << " so far)\n";
                        }
                    }
                    const uint64_t val = ++obj->present_fence_val;
                    obj->pc_queue->Signal(obj->present_fence, val);
                }
            }
        }

        if (const char* dump_path = std::getenv("SCARLETTX_DUMP_FRAME"); dump_path && dump_path[0] != '\0') {
            // Frames to dump: SCARLETTX_DUMP_FRAME_AT="5,30,90" (the default).
            static const std::vector<uint64_t> dump_frames = [] {
                std::vector<uint64_t> frames;
                const char* at = std::getenv("SCARLETTX_DUMP_FRAME_AT");
                std::istringstream list(at && at[0] ? at : "5,30,90");
                for (std::string item; std::getline(list, item, ',');) {
                    char* end = nullptr;
                    const unsigned long long v = std::strtoull(item.c_str(), &end, 10);
                    if (end != item.c_str()) frames.push_back(v);
                }
                return frames;
            }();
            if (std::find(dump_frames.begin(), dump_frames.end(), static_cast<uint64_t>(n)) != dump_frames.end()) {
                DumpResourceToBmp(obj, src_proxy->pc_res, cur_state, std::string(dump_path) + "_" + std::to_string(n) + ".bmp");
            }
        }
    }
#endif
    if (TraceEnabled() && (n <= 10 || (n % 60) == 0)) {
        std::cerr << "[ScarlettX][d3d12_xs] Queue::PresentX(#" << n
                  << ", q=" << self << ", src=" << src_proxy
                  << ", planes=" << planeCount
                  << ", draws=" << g_draw_instanced_count.load(std::memory_order_relaxed)
                  << "/" << g_draw_indexed_count.load(std::memory_order_relaxed)
                  << ", clears=" << g_clear_rtv_count.load(std::memory_order_relaxed)
                  << ", setRT=" << g_om_set_rt_count.load(std::memory_order_relaxed)
                  << ") -> S_OK\n";
    }
    MaybePrintStats(n);
#if defined(_WIN32)
    if (obj && obj->present_hwnd) {
        static std::atomic<uint64_t> hud_last_tick{0};
        static std::atomic<uint64_t> hud_last_frame{0};
        const uint64_t now_tick = GetTickCount64();
        const uint64_t prev_tick = hud_last_tick.load(std::memory_order_relaxed);
        if (prev_tick == 0) {
            hud_last_tick.store(now_tick, std::memory_order_relaxed);
            hud_last_frame.store(n, std::memory_order_relaxed);
        } else if (now_tick >= prev_tick + 500ull) {
            const uint64_t prev_frame = hud_last_frame.exchange(n, std::memory_order_relaxed);
            hud_last_tick.store(now_tick, std::memory_order_relaxed);
            const double dt_sec = static_cast<double>(now_tick - prev_tick) / 1000.0;
            const double fps = (dt_sec > 0.0) ? (static_cast<double>(n - prev_frame) / dt_sec) : 0.0;
            const auto fp = scarlettx::FlightRecorder::Instance().GetFramePacingSummary();
            const uint64_t draws = g_draw_instanced_count.load(std::memory_order_relaxed) +
                                   g_draw_indexed_count.load(std::memory_order_relaxed);
            char hud[256];
            std::snprintf(hud, sizeof(hud),
                          "ScarlettEdge | %.1f FPS | p95 %.2f ms | Draws: %llu",
                          fps, fp.p95_ms, static_cast<unsigned long long>(draws));
            SetWindowTextA(obj->present_hwnd, hud);
        }
    }
#endif
    return 0;
}

std::array<ComVtableFn, 64> BuildQueueProxyVtable() {
    auto vtbl = MakeQueueProxyVtable(std::make_index_sequence<61>{});
    vtbl[6] = &QueueProxySetName;
    vtbl[8] = reinterpret_cast<ComVtableFn>(&QueueProxyUpdateTileMappings);
    vtbl[9] = reinterpret_cast<ComVtableFn>(&QueueProxyCopyTileMappings);
    vtbl[10] = &QueueProxyExecuteCommandLists;
    vtbl[11] = &QueueProxyNoop;  // SetMarker
    vtbl[12] = &QueueProxyNoop;  // BeginEvent
    vtbl[13] = &QueueProxyNoop;  // EndEvent
    vtbl[14] = &QueueProxySignal;
    vtbl[15] = &QueueProxyWait;
    vtbl[16] = &QueueProxyGetTimestampFrequency;
    vtbl[17] = &QueueProxyGetClockCalibration;
    vtbl[18] = &QueueProxyGetDesc;
    vtbl[30] = &QueueProxyPresentX;
    return vtbl;
}

const auto kXboxCommandQueueProxyVtbl = BuildQueueProxyVtable();

// Titles built against a later SDK (D3D12XboxCreateDevice version 0x20004,
// against 0x20001 for the table above) find two more methods ahead of
// Signal, shifting slots >= 14 by +2.
std::array<ComVtableFn, 64> BuildQueueProxyVtableV4() {
    auto vtbl = MakeQueueProxyVtable(std::make_index_sequence<61>{});
    vtbl[6] = &QueueProxySetName;
    vtbl[8] = reinterpret_cast<ComVtableFn>(&QueueProxyUpdateTileMappings);
    vtbl[9] = reinterpret_cast<ComVtableFn>(&QueueProxyCopyTileMappings);
    vtbl[10] = &QueueProxyExecuteCommandLists;
    vtbl[11] = &QueueProxyNoop;
    vtbl[12] = &QueueProxyNoop;
    vtbl[13] = &QueueProxyNoop;
    vtbl[14] = &QueueProxyNoop;
    vtbl[15] = &QueueProxyNoop;
    vtbl[16] = &QueueProxySignal;                // +0x80, observed at 0x1404f6166
    vtbl[17] = &QueueProxyWait;                  // +0x88
    vtbl[18] = &QueueProxyGetTimestampFrequency; // +0x90
    vtbl[19] = &QueueProxyGetClockCalibration;   // +0x98
    vtbl[20] = &QueueProxyGetDesc;               // +0xa0
    vtbl[32] = &QueueProxyPresentX;              // +0x100, observed at 0x1404f60d3
    return vtbl;
}

const auto kXboxCommandQueueProxyVtblV4 = BuildQueueProxyVtableV4();

struct XboxD3D12DeviceProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    uint32_t sdk_version = 0;
#if defined(_WIN32)
    ID3D12Device* pc_device = nullptr;
    IDXGIAdapter1* pc_adapter = nullptr;
    std::mutex pso_cache_mu;
    std::unordered_map<uint64_t, ID3D12PipelineState*> pso_cache;
#endif
};

struct XboxDXGIDeviceProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XboxD3D12DeviceProxy* parent_device = nullptr;
};

intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxDXGIDeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (TraceEnabled()) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIDevice::QueryInterface("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ")\n";
    }
    if (IsD3D12DeviceIid(riid)) {
        if (!obj->parent_device) {
            return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
        }
        obj->parent_device->ref_count.fetch_add(1, std::memory_order_relaxed);
        *out = obj->parent_device;
        return 0;
    }
    if (ProxyGuidEquals(riid, kIidIUnknown) ||
        ProxyGuidEquals(riid, kIidIDXGIObject) ||
        IsDxgiDeviceIid(riid)) {
        obj->ref_count.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return 0;
    }
    scarlettx::FlightRecorder::Instance().RecordComQiMiss(
        "XboxIDXGIDevice", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
}

intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxDXGIDeviceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxDXGIDeviceProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        if (obj->parent_device) {
            obj->parent_device->lpVtbl[2](obj->parent_device, 0, 0, 0);
        }
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

XboxDXGIAdapterProxy* NewAdapterProxy(const XboxDXGIDeviceProxy* obj) {
    auto* adapter_proxy = new XboxDXGIAdapterProxy();
    adapter_proxy->lpVtbl = kXboxDXGIAdapterProxyVtbl.data();
#if defined(_WIN32)
    if (obj->parent_device && obj->parent_device->pc_adapter) {
        adapter_proxy->pc_adapter = obj->parent_device->pc_adapter;
        adapter_proxy->pc_adapter->AddRef();
    }
#endif
    return adapter_proxy;
}

intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyGetParent(void* self, uintptr_t riid, uintptr_t ppParent, uintptr_t) {
    auto* obj = static_cast<XboxDXGIDeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppParent);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (TraceEnabled() && riid) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIDevice::GetParent("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ") -> S_OK\n";
    }
    *out = NewAdapterProxy(obj);
    return 0;  // S_OK
}

// IDXGIDevice::GetAdapter (+0x38), observed at 0x1404f5274 in a title.
intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyGetAdapter(void* self, uintptr_t ppAdapter, uintptr_t, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppAdapter);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = NewAdapterProxy(static_cast<XboxDXGIDeviceProxy*>(self));
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIDevice::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeDxgiDevProxyVtable(std::index_sequence<Is...>) {
    return {
        &DxgiDevProxyQueryInterface,
        &DxgiDevProxyAddRef,
        &DxgiDevProxyRelease,
        &DxgiDevProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<ComVtableFn, 24> BuildDxgiDevProxyVtable() {
    auto vtbl = MakeDxgiDevProxyVtable(std::make_index_sequence<21>{});
    vtbl[6] = &DxgiDevProxyGetParent;  // IDXGIObject::GetParent (+0x30)
    vtbl[7] = &DxgiDevProxyGetAdapter;
    return vtbl;
}

const auto kXboxDXGIDeviceProxyVtbl = BuildDxgiDevProxyVtable();

intptr_t SCARLETTX_GDK_CALLBACK ProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);  // E_POINTER
    *out = nullptr;
    if (!riid || !IsReadableMemory(reinterpret_cast<const void*>(riid), sizeof(ProxyGuid))) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    if (TraceEnabled()) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::QueryInterface("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ")\n";
    }
    if (IsDxgiDeviceIid(riid)) {
        auto* dxgi = new XboxDXGIDeviceProxy();
        dxgi->lpVtbl = kXboxDXGIDeviceProxyVtbl.data();
        dxgi->parent_device = obj;
        obj->ref_count.fetch_add(1, std::memory_order_relaxed);
        *out = dxgi;
        return 0;  // S_OK
    }
    if (ProxyGuidEquals(riid, kIidIUnknown) ||
        ProxyGuidEquals(riid, kIidID3D12Object) ||
        IsD3D12DeviceIid(riid)) {
        obj->ref_count.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return 0;  // S_OK
    }
    scarlettx::FlightRecorder::Instance().RecordComQiMiss(
        "XboxID3D12Device", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12DeviceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        {
            std::lock_guard<std::mutex> lk(obj->pso_cache_mu);
            for (auto& kv : obj->pso_cache) {
                if (kv.second) kv.second->Release();
            }
            obj->pso_cache.clear();
        }
        ReleaseDeviceAutoGuestBuffers(obj->pc_device);
        if (obj->pc_device) obj->pc_device->Release();
        if (obj->pc_adapter) obj->pc_adapter->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK ProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj->pc_device && name_ptr) {
        return static_cast<intptr_t>(obj->pc_device->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetNodeCount(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj->pc_device) {
        return static_cast<intptr_t>(obj->pc_device->GetNodeCount());
    }
#endif
    return 1;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandQueue(void* self, uintptr_t pDesc, uintptr_t riid, uintptr_t ppQueue) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppQueue);
    if (!pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const auto* desc = reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(pDesc);
    D3D12_COMMAND_QUEUE_DESC pc_desc = *desc;
    if (pc_desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        pc_desc.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        pc_desc.Type != D3D12_COMMAND_LIST_TYPE_COPY) {
        pc_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    pc_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    pc_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    pc_desc.NodeMask = 0;
    ID3D12CommandQueue* pc_q = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandQueue(
        &pc_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&pc_q));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandQueue(type="
                  << static_cast<int>(desc->Type) << ", riid=" << std::hex << riid
                  << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_q) return static_cast<intptr_t>(hr);
    auto* q_proxy = new XboxD3D12CommandQueueProxy();
    q_proxy->lpVtbl = obj->sdk_version >= 0x20004u ? kXboxCommandQueueProxyVtblV4.data()
                                                   : kXboxCommandQueueProxyVtbl.data();
    q_proxy->pc_queue = pc_q;
    q_proxy->pc_device = obj->pc_device;
    if (q_proxy->pc_device) q_proxy->pc_device->AddRef();
    *out = q_proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandQueueX(void* self, uintptr_t pDesc, uintptr_t riid, uintptr_t ppQueue) {
    if (TraceEnabled()) {
        const uint32_t raw_type = pDesc ? *reinterpret_cast<const uint32_t*>(pDesc) : 0;
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::vtable[48]/+0x180(raw_type=0x"
                  << std::hex << raw_type << std::dec << ")\n";
    }
    return ProxyCreateCommandQueue(self, pDesc, riid, ppQueue);
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCheckFeatureSupport(
    void* self, uintptr_t feature_raw, uintptr_t data_ptr, uintptr_t data_size_raw) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (!obj || !obj->pc_device || !data_ptr) return static_cast<intptr_t>(E_INVALIDARG);
    const auto feature = static_cast<D3D12_FEATURE>(feature_raw);
    const auto data_size = static_cast<UINT>(data_size_raw);
    HRESULT hr = obj->pc_device->CheckFeatureSupport(
        feature, reinterpret_cast<void*>(data_ptr), data_size);
    if (FAILED(hr) && feature == D3D12_FEATURE_D3D12_OPTIONS &&
        data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) {
        // Xbox D3D12_FEATURE_DATA_D3D12_OPTIONS may differ slightly in trailing size from PC.
        D3D12_FEATURE_DATA_D3D12_OPTIONS pc_opts{};
        hr = obj->pc_device->CheckFeatureSupport(feature, &pc_opts, sizeof(pc_opts));
        if (SUCCEEDED(hr)) {
            std::memset(reinterpret_cast<void*>(data_ptr), 0, data_size);
            const size_t copy_bytes =
                data_size < sizeof(pc_opts) ? data_size : sizeof(pc_opts);
            std::memcpy(reinterpret_cast<void*>(data_ptr), &pc_opts, copy_bytes);
        }
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CheckFeatureSupport(feature="
                  << static_cast<uint32_t>(feature) << ", size=0x" << std::hex << data_size
                  << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    return static_cast<intptr_t>(hr);
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateDescriptorHeap(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppvHeap) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvHeap);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const auto* guest_desc = reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(pDesc);
    D3D12_DESCRIPTOR_HEAP_DESC pc_desc = *guest_desc;
    if (static_cast<uint32_t>(pc_desc.Type) >= 4) {
        pc_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    }
    if (pc_desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
        pc_desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) {
        pc_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    }
    ID3D12DescriptorHeap* pc_heap = nullptr;
    const HRESULT hr = obj->pc_device->CreateDescriptorHeap(&pc_desc, IID_PPV_ARGS(&pc_heap));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateDescriptorHeap(type="
                  << static_cast<uint32_t>(guest_desc->Type)
                  << ", num=" << guest_desc->NumDescriptors
                  << ", flags=0x" << std::hex << static_cast<uint32_t>(guest_desc->Flags)
                  << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_heap) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12DescriptorHeapProxy();
    proxy->lpVtbl = kXboxD3D12DescriptorHeapProxyVtbl.data();
    proxy->pc_heap = pc_heap;
    proxy->orig_desc = *guest_desc;
    proxy->cpu_start = pc_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if ((pc_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        proxy->gpu_start = pc_heap->GetGPUDescriptorHandleForHeapStart().ptr;
    }
    proxy->descriptor_stride = obj->pc_device->GetDescriptorHandleIncrementSize(pc_desc.Type);
    proxy->num_descriptors = pc_desc.NumDescriptors;
    *out = proxy;
    return 0;  // S_OK
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetDescriptorHandleIncrementSize(
    void* self, uintptr_t heap_type_raw, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (!obj || !obj->pc_device) return 32;
    auto pc_type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heap_type_raw);
    if (heap_type_raw >= 4) {
        pc_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    }
    const UINT inc = obj->pc_device->GetDescriptorHandleIncrementSize(pc_type);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::GetDescriptorHandleIncrementSize(type="
                  << static_cast<uint32_t>(heap_type_raw) << ") -> " << inc << "\n";
    }
    return static_cast<intptr_t>(inc);
#else
    return 32;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxySlot69XboxSetting(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::vtable[69]/+0x228(id=0x"
                  << std::hex << a1 << ", val=0x" << a2 << std::dec << ") -> S_OK\n";
    }
    return 0;  // S_OK
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK ProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        uintptr_t norm_ret = 0;
#if defined(_WIN32)
        const auto ret = reinterpret_cast<uintptr_t>(SCARLETTX_RETURN_ADDRESS());
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        if (ret >= base && ret < base + 0x20000000u) {
            norm_ret = 0x140000000ull + (ret - base);
        }
#endif
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << ", ret=0x" << norm_ret
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    scarlettx::FlightRecorder::Instance().RecordVtableSlotMiss(
        "XboxID3D12Device", Slot, static_cast<uint64_t>(a1), SCARLETTX_CALLER_RIP());
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));  // E_NOTIMPL
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeProxyVtable(std::index_sequence<Is...>) {
    return {
        &ProxyQueryInterface,
        &ProxyAddRef,
        &ProxyRelease,
        &ProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandAllocator(
    void* self, uintptr_t type_raw, uintptr_t /*riid*/, uintptr_t ppAlloc) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppAlloc);
    if (!obj || !obj->pc_device || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(type_raw);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        type != D3D12_COMMAND_LIST_TYPE_COPY) {
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    ID3D12CommandAllocator* pc_alloc = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandAllocator(type, IID_PPV_ARGS(&pc_alloc));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandAllocator(type="
                  << static_cast<uint32_t>(type_raw) << ") -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << ", pc_alloc=" << pc_alloc;
        if (FAILED(hr)) {
            std::cerr << ", removed_reason=0x"
                      << static_cast<uint32_t>(obj->pc_device->GetDeviceRemovedReason());
        }
        std::cerr << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_alloc) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12CommandAllocatorProxy();
    proxy->lpVtbl = kXboxCommandAllocatorProxyVtbl.data();
    proxy->pc_alloc = pc_alloc;
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

#if defined(_WIN32)
void TranslateGuestShaderStageIfNeeded(
    D3D12_SHADER_BYTECODE& stage,
    std::vector<uint8_t>& out_owned) {
    if (!stage.pShaderBytecode || stage.BytecodeLength < 20) return;
    const auto* bytes = static_cast<const uint8_t*>(stage.pShaderBytecode);
    std::vector<uint8_t> src(bytes, bytes + stage.BytecodeLength);
    scarlettx::ShaderTranslationResult tr{};
    if (scarlettx::TranslateXboxShaderToPc(src, tr).ok() &&
        (tr.stripped_xbox_chunks > 0 || tr.lifted_from_rdna2)) {
        out_owned = std::move(tr.pc_bytecode);
        stage.pShaderBytecode = out_owned.data();
        stage.BytecodeLength = out_owned.size();
    }
}

void FnvMixBytes(uint64_t& h, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
}

void FnvMixShaderStage(uint64_t& h, const D3D12_SHADER_BYTECODE& s) {
    const uint64_t len = s.BytecodeLength;
    FnvMixBytes(h, &len, sizeof(len));
    if (s.pShaderBytecode && s.BytecodeLength > 0) {
        const uint64_t sh = HashMemoryRegionFast(static_cast<const uint8_t*>(s.pShaderBytecode), s.BytecodeLength);
        FnvMixBytes(h, &sh, sizeof(sh));
    }
}

uint64_t ComputeGraphicsPsoCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    uint64_t h = 14695981039346656037ull;
    FnvMixBytes(h, &d.pRootSignature, sizeof(d.pRootSignature));
    FnvMixShaderStage(h, d.VS);
    FnvMixShaderStage(h, d.PS);
    FnvMixShaderStage(h, d.DS);
    FnvMixShaderStage(h, d.HS);
    FnvMixShaderStage(h, d.GS);
    FnvMixBytes(h, &d.BlendState, sizeof(d.BlendState));
    FnvMixBytes(h, &d.SampleMask, sizeof(d.SampleMask));
    FnvMixBytes(h, &d.RasterizerState, sizeof(d.RasterizerState));
    FnvMixBytes(h, &d.DepthStencilState, sizeof(d.DepthStencilState));
    FnvMixBytes(h, &d.InputLayout.NumElements, sizeof(d.InputLayout.NumElements));
    if (d.InputLayout.pInputElementDescs && d.InputLayout.NumElements > 0 && d.InputLayout.NumElements <= 64u) {
        for (UINT i = 0; i < d.InputLayout.NumElements; ++i) {
            const auto& el = d.InputLayout.pInputElementDescs[i];
            if (el.SemanticName) {
                FnvMixBytes(h, el.SemanticName, std::strlen(el.SemanticName) + 1u);
            }
            FnvMixBytes(h, &el.SemanticIndex, sizeof(el.SemanticIndex));
            FnvMixBytes(h, &el.Format, sizeof(el.Format));
            FnvMixBytes(h, &el.InputSlot, sizeof(el.InputSlot));
            FnvMixBytes(h, &el.AlignedByteOffset, sizeof(el.AlignedByteOffset));
            FnvMixBytes(h, &el.InputSlotClass, sizeof(el.InputSlotClass));
            FnvMixBytes(h, &el.InstanceDataStepRate, sizeof(el.InstanceDataStepRate));
        }
    }
    FnvMixBytes(h, &d.IBStripCutValue, sizeof(d.IBStripCutValue));
    FnvMixBytes(h, &d.PrimitiveTopologyType, sizeof(d.PrimitiveTopologyType));
    FnvMixBytes(h, &d.NumRenderTargets, sizeof(d.NumRenderTargets));
    FnvMixBytes(h, d.RTVFormats, sizeof(d.RTVFormats));
    FnvMixBytes(h, &d.DSVFormat, sizeof(d.DSVFormat));
    FnvMixBytes(h, &d.SampleDesc, sizeof(d.SampleDesc));
    FnvMixBytes(h, &d.NodeMask, sizeof(d.NodeMask));
    FnvMixBytes(h, &d.Flags, sizeof(d.Flags));
    return h ? h : 1ull;
}

uint64_t ComputeComputePsoCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC& d) {
    uint64_t h = 14695981039346656037ull ^ 0xC0DE0001ull;
    FnvMixBytes(h, &d.pRootSignature, sizeof(d.pRootSignature));
    FnvMixShaderStage(h, d.CS);
    FnvMixBytes(h, &d.NodeMask, sizeof(d.NodeMask));
    FnvMixBytes(h, &d.Flags, sizeof(d.Flags));
    return h ? h : 1ull;
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateGraphicsPipelineState(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppPso) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppPso);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const auto& raw_desc = *reinterpret_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(pDesc);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = raw_desc;
    if (desc.pRootSignature) {
        desc.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(
            reinterpret_cast<XboxD3D12SimpleObjectProxy*>(desc.pRootSignature)->pc_obj);
    }
    std::vector<uint8_t> vs_owned, ps_owned, ds_owned, hs_owned, gs_owned;
    TranslateGuestShaderStageIfNeeded(desc.VS, vs_owned);
    TranslateGuestShaderStageIfNeeded(desc.PS, ps_owned);
    TranslateGuestShaderStageIfNeeded(desc.DS, ds_owned);
    TranslateGuestShaderStageIfNeeded(desc.HS, hs_owned);
    TranslateGuestShaderStageIfNeeded(desc.GS, gs_owned);

    const bool can_cache = (desc.StreamOutput.NumEntries == 0);
    const uint64_t pso_key = can_cache ? ComputeGraphicsPsoCacheKey(desc) : 0ull;
    ID3D12PipelineState* pc_pso = nullptr;
    HRESULT hr = S_OK;
    if (can_cache) {
        std::lock_guard<std::mutex> lk(obj->pso_cache_mu);
        auto it = obj->pso_cache.find(pso_key);
        if (it != obj->pso_cache.end() && it->second) {
            pc_pso = it->second;
            pc_pso->AddRef();
            g_proxy_pso_cache_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!pc_pso) {
        hr = obj->pc_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pc_pso));
        if (SUCCEEDED(hr) && pc_pso) {
            g_proxy_pso_cache_misses.fetch_add(1, std::memory_order_relaxed);
            if (can_cache) {
                std::lock_guard<std::mutex> lk(obj->pso_cache_mu);
                auto& slot = obj->pso_cache[pso_key];
                if (!slot) {
                    pc_pso->AddRef();
                    slot = pc_pso;
                }
            }
        }
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateGraphicsPipelineState("
                  << "vs=" << desc.VS.BytecodeLength << ", ps=" << desc.PS.BytecodeLength
                  << ", rtvs=" << desc.NumRenderTargets << ", rtv0=" << desc.RTVFormats[0]
                  << ", dsv=" << desc.DSVFormat << ", depth=" << desc.DepthStencilState.DepthEnable
                  << ", cull=" << desc.RasterizerState.CullMode
                  << ", topo=" << desc.PrimitiveTopologyType
                  << ", blend0=" << desc.BlendState.RenderTarget[0].BlendEnable
                  << ") -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_pso) {
        const uint64_t caller_rip = SCARLETTX_CALLER_RIP();
        if (raw_desc.VS.pShaderBytecode && raw_desc.VS.BytecodeLength > 0) {
            scarlettx::FlightRecorder::Instance().DumpFailingShaderArtifact(
                "GraphicsPSO_VS", static_cast<int32_t>(hr),
                raw_desc.VS.pShaderBytecode, raw_desc.VS.BytecodeLength,
                vs_owned.empty() ? nullptr : vs_owned.data(), vs_owned.size(), caller_rip);
        }
        if (raw_desc.PS.pShaderBytecode && raw_desc.PS.BytecodeLength > 0) {
            scarlettx::FlightRecorder::Instance().DumpFailingShaderArtifact(
                "GraphicsPSO_PS", static_cast<int32_t>(hr),
                raw_desc.PS.pShaderBytecode, raw_desc.PS.BytecodeLength,
                ps_owned.empty() ? nullptr : ps_owned.data(), ps_owned.size(), caller_rip);
        }
        MaybeRecordDeviceRemoved(obj->pc_device, hr, "ProxyCreateGraphicsPipelineState");
        return static_cast<intptr_t>(hr);
    }
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_pso;
    proxy->kind = "PipelineState";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateComputePipelineState(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppPso) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppPso);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const auto& raw_desc = *reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(pDesc);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = raw_desc;
    if (desc.pRootSignature) {
        desc.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(
            reinterpret_cast<XboxD3D12SimpleObjectProxy*>(desc.pRootSignature)->pc_obj);
    }
    std::vector<uint8_t> cs_owned;
    TranslateGuestShaderStageIfNeeded(desc.CS, cs_owned);

    const uint64_t pso_key = ComputeComputePsoCacheKey(desc);
    ID3D12PipelineState* pc_pso = nullptr;
    HRESULT hr = S_OK;
    {
        std::lock_guard<std::mutex> lk(obj->pso_cache_mu);
        auto it = obj->pso_cache.find(pso_key);
        if (it != obj->pso_cache.end() && it->second) {
            pc_pso = it->second;
            pc_pso->AddRef();
            g_proxy_pso_cache_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!pc_pso) {
        hr = obj->pc_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pc_pso));
        if (SUCCEEDED(hr) && pc_pso) {
            g_proxy_pso_cache_misses.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(obj->pso_cache_mu);
            auto& slot = obj->pso_cache[pso_key];
            if (!slot) {
                pc_pso->AddRef();
                slot = pc_pso;
            }
        }
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateComputePipelineState() -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_pso) {
        scarlettx::FlightRecorder::Instance().DumpFailingShaderArtifact(
            "ComputePSO_CS", static_cast<int32_t>(hr),
            raw_desc.CS.pShaderBytecode, raw_desc.CS.BytecodeLength,
            cs_owned.empty() ? nullptr : cs_owned.data(), cs_owned.size(),
            SCARLETTX_CALLER_RIP());
        MaybeRecordDeviceRemoved(obj->pc_device, hr, "ProxyCreateComputePipelineState");
        return static_cast<intptr_t>(hr);
    }
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_pso;
    proxy->kind = "PipelineState";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandList(
    void* self, uintptr_t nodeMask, uintptr_t type_raw, uintptr_t pAlloc,
    uintptr_t pInitialState, uintptr_t /*riid*/, uintptr_t ppList) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppList);
    if (!obj || !obj->pc_device || !pAlloc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(type_raw);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        type != D3D12_COMMAND_LIST_TYPE_COPY) {
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    auto* alloc_proxy = reinterpret_cast<XboxD3D12CommandAllocatorProxy*>(pAlloc);
    ID3D12PipelineState* pc_pso = nullptr;
    if (pInitialState) {
        pc_pso = reinterpret_cast<ID3D12PipelineState*>(
            reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pInitialState)->pc_obj);
    }
    ID3D12GraphicsCommandList* pc_list = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandList(
        static_cast<UINT>(nodeMask), type, alloc_proxy->pc_alloc, pc_pso, IID_PPV_ARGS(&pc_list));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandList(type="
                  << static_cast<uint32_t>(type_raw) << ") -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_list) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12GraphicsCommandListProxy();
    proxy->lpVtbl = (obj->sdk_version >= 0x20004u ? kXboxCmdListProxyVtblV4 : kXboxCmdListProxyVtbl).data();
    proxy->pc_list = pc_list;
    InitCmdListExtendedInterfaces(proxy);
    proxy->pc_device = obj->pc_device;
    if (proxy->pc_device) proxy->pc_device->AddRef();
    proxy->active_pc_alloc = alloc_proxy->pc_alloc;
    proxy->active_alloc_epoch = GetAllocatorEpoch(alloc_proxy->pc_alloc);
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

#if defined(_WIN32)
void PopulateRootSignatureUavTableSpans(
    const void* pBlob,
    size_t blobLen,
    std::vector<RootTableUavSpan>& out_spans) {
    out_spans.clear();
    if (!pBlob || blobLen == 0) return;
    ID3D12VersionedRootSignatureDeserializer* deser = nullptr;
    if (FAILED(CallSystemD3D12CreateVersionedRootSignatureDeserializer(
            pBlob, blobLen, __uuidof(ID3D12VersionedRootSignatureDeserializer),
            reinterpret_cast<void**>(&deser))) ||
        !deser) {
        return;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vdesc = deser->GetUnconvertedRootSignatureDesc();
    if (vdesc) {
        if (vdesc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
            const auto& d0 = vdesc->Desc_1_0;
            for (UINT p = 0; p < d0.NumParameters; ++p) {
                if (d0.pParameters[p].ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
                const auto& tbl = d0.pParameters[p].DescriptorTable;
                UINT running_off = 0;
                for (UINT r = 0; r < tbl.NumDescriptorRanges; ++r) {
                    const auto& rng = tbl.pDescriptorRanges[r];
                    const UINT off = (rng.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                        ? running_off
                        : rng.OffsetInDescriptorsFromTableStart;
                    if (rng.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV && rng.NumDescriptors > 0) {
                        out_spans.push_back(RootTableUavSpan{p, off, rng.NumDescriptors});
                    }
                    if (rng.NumDescriptors != UINT_MAX) {
                        running_off = off + rng.NumDescriptors;
                    }
                }
            }
        } else if (vdesc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
            const auto& d1 = vdesc->Desc_1_1;
            for (UINT p = 0; p < d1.NumParameters; ++p) {
                if (d1.pParameters[p].ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
                const auto& tbl = d1.pParameters[p].DescriptorTable;
                UINT running_off = 0;
                for (UINT r = 0; r < tbl.NumDescriptorRanges; ++r) {
                    const auto& rng = tbl.pDescriptorRanges[r];
                    const UINT off = (rng.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                        ? running_off
                        : rng.OffsetInDescriptorsFromTableStart;
                    if (rng.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV && rng.NumDescriptors > 0) {
                        out_spans.push_back(RootTableUavSpan{p, off, rng.NumDescriptors});
                    }
                    if (rng.NumDescriptors != UINT_MAX) {
                        running_off = off + rng.NumDescriptors;
                    }
                }
            }
        }
    }
    deser->Release();
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateRootSignature(
    void* self, uintptr_t nodeMask, uintptr_t pBlob, uintptr_t blobLen,
    uintptr_t /*riid*/, uintptr_t ppRootSig) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppRootSig);
    if (!obj || !obj->pc_device || !pBlob || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12RootSignature* pc_rs = nullptr;
    const HRESULT hr = obj->pc_device->CreateRootSignature(
        static_cast<UINT>(nodeMask), reinterpret_cast<const void*>(pBlob),
        static_cast<SIZE_T>(blobLen), IID_PPV_ARGS(&pc_rs));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateRootSignature(len="
                  << blobLen << ") -> 0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_rs) {
        scarlettx::FlightRecorder::Instance().DumpFailingShaderArtifact(
            "RootSignature", static_cast<int32_t>(hr),
            reinterpret_cast<const void*>(pBlob), static_cast<size_t>(blobLen),
            nullptr, 0, SCARLETTX_CALLER_RIP());
        MaybeRecordDeviceRemoved(obj->pc_device, hr, "ProxyCreateRootSignature");
        return static_cast<intptr_t>(hr);
    }
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_rs;
    proxy->kind = "RootSignature";
    PopulateRootSignatureUavTableSpans(
        reinterpret_cast<const void*>(pBlob), static_cast<size_t>(blobLen), proxy->uav_table_spans);
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateConstantBufferView(
    void* self, uintptr_t pDesc, uintptr_t destDesc, uintptr_t) {
    static std::atomic<uint64_t> cbv_cnt{0};
    const uint64_t n = cbv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        RecordUavCpuDescriptor(static_cast<SIZE_T>(destDesc), nullptr);
        auto* d = reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(pDesc);
        D3D12_CONSTANT_BUFFER_VIEW_DESC translated_cbv{};
        if (d) {
            translated_cbv = *d;
            if (translated_cbv.BufferLocation != 0 && translated_cbv.SizeInBytes > 0) {
                translated_cbv.BufferLocation = TranslateGuestGpuVa(
                    obj->pc_device, translated_cbv.BufferLocation, translated_cbv.SizeInBytes);
            }
            d = &translated_cbv;
        }
        if (TraceEnabled() && n <= 8) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CreateCBV(#" << n
                      << ", loc=0x" << std::hex << (d ? d->BufferLocation : 0) << std::dec
                      << ", size=" << (d ? d->SizeInBytes : 0)
                      << ", dst=0x" << std::hex << destDesc << std::dec << ")\n";
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{destDesc};
        obj->pc_device->CreateConstantBufferView(d, h);
    }
#endif
    return 0;
}

#if defined(_WIN32)
DXGI_FORMAT StripSrgbForUav(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return fmt;
    }
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateShaderResourceView(
    void* self, uintptr_t pResource, uintptr_t pDesc, uintptr_t destDesc) {
    static std::atomic<uint64_t> srv_cnt{0};
    const uint64_t n = srv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        RecordUavCpuDescriptor(static_cast<SIZE_T>(destDesc), nullptr);
        auto* res_proxy = pResource ? reinterpret_cast<XboxD3D12ResourceProxy*>(pResource) : nullptr;
        ID3D12Resource* pc_res = res_proxy ? res_proxy->pc_res : nullptr;
        auto* d = reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(pDesc);
        D3D12_SHADER_RESOURCE_VIEW_DESC synth_desc = {};
        if (!d && res_proxy && pc_res && res_proxy->promoted_typeless && res_proxy->guest_format != DXGI_FORMAT_UNKNOWN) {
            const D3D12_RESOURCE_DESC rd = pc_res->GetDesc();
            synth_desc.Format = res_proxy->guest_format;
            synth_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (rd.SampleDesc.Count > 1) {
                if (rd.DepthOrArraySize > 1) {
                    synth_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                    synth_desc.Texture2DMSArray.FirstArraySlice = 0;
                    synth_desc.Texture2DMSArray.ArraySize = rd.DepthOrArraySize;
                } else {
                    synth_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                }
            } else if (rd.DepthOrArraySize > 1) {
                synth_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                synth_desc.Texture2DArray.MostDetailedMip = 0;
                synth_desc.Texture2DArray.MipLevels = ~0u;
                synth_desc.Texture2DArray.FirstArraySlice = 0;
                synth_desc.Texture2DArray.ArraySize = rd.DepthOrArraySize;
            } else {
                synth_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                synth_desc.Texture2D.MipLevels = ~0u;
            }
            d = &synth_desc;
        }
        if (TraceEnabled() && n <= 8) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CreateSRV(#" << n
                      << ", res=0x" << std::hex << pResource << std::dec
                      << ", fmt=" << (d ? static_cast<int>(d->Format) : -1)
                      << ", dim=" << (d ? static_cast<int>(d->ViewDimension) : -1)
                      << ", dst=0x" << std::hex << destDesc << std::dec << ")\n";
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{destDesc};
        obj->pc_device->CreateShaderResourceView(pc_res, d, h);
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateUnorderedAccessView(
    void* self, uintptr_t pResource, uintptr_t pCounter, uintptr_t pDesc, uintptr_t destDesc) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        auto* res_proxy = pResource ? reinterpret_cast<XboxD3D12ResourceProxy*>(pResource) : nullptr;
        RecordUavCpuDescriptor(
            static_cast<SIZE_T>(destDesc),
            (res_proxy && res_proxy->is_default_buffer) ? res_proxy : nullptr);
        ID3D12Resource* pc_res = res_proxy ? res_proxy->pc_res : nullptr;
        ID3D12Resource* pc_cnt = pCounter ? reinterpret_cast<XboxD3D12ResourceProxy*>(pCounter)->pc_res : nullptr;
        auto* d = reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(pDesc);
        D3D12_UNORDERED_ACCESS_VIEW_DESC synth_uav = {};
        if (!d && res_proxy && pc_res && res_proxy->promoted_typeless && res_proxy->guest_format != DXGI_FORMAT_UNKNOWN) {
            const D3D12_RESOURCE_DESC rd = pc_res->GetDesc();
            synth_uav.Format = StripSrgbForUav(res_proxy->guest_format);
            if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                synth_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                synth_uav.Texture3D.MipSlice = 0;
                synth_uav.Texture3D.FirstWSlice = 0;
                synth_uav.Texture3D.WSize = rd.DepthOrArraySize;
            } else if (rd.DepthOrArraySize > 1) {
                synth_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                synth_uav.Texture2DArray.MipSlice = 0;
                synth_uav.Texture2DArray.FirstArraySlice = 0;
                synth_uav.Texture2DArray.ArraySize = rd.DepthOrArraySize;
            } else {
                synth_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                synth_uav.Texture2D.MipSlice = 0;
            }
            d = &synth_uav;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{destDesc};
        obj->pc_device->CreateUnorderedAccessView(pc_res, pc_cnt, d, h);
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateRenderTargetView(
    void* self, uintptr_t pResource, uintptr_t pDesc, uintptr_t destDesc) {
    static std::atomic<uint64_t> rtv_cnt{0};
    const uint64_t n = rtv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        auto* res_proxy = pResource ? reinterpret_cast<XboxD3D12ResourceProxy*>(pResource) : nullptr;
        ID3D12Resource* pc_res = res_proxy ? res_proxy->pc_res : nullptr;
        auto* d = reinterpret_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(pDesc);
        D3D12_RENDER_TARGET_VIEW_DESC synth_desc = {};
        if (!d && res_proxy && pc_res && res_proxy->promoted_typeless && res_proxy->guest_format != DXGI_FORMAT_UNKNOWN) {
            const D3D12_RESOURCE_DESC rd = pc_res->GetDesc();
            synth_desc.Format = res_proxy->guest_format;
            if (rd.SampleDesc.Count > 1) {
                if (rd.DepthOrArraySize > 1) {
                    synth_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    synth_desc.Texture2DMSArray.FirstArraySlice = 0;
                    synth_desc.Texture2DMSArray.ArraySize = rd.DepthOrArraySize;
                } else {
                    synth_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                }
            } else if (rd.DepthOrArraySize > 1) {
                synth_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                synth_desc.Texture2DArray.MipSlice = 0;
                synth_desc.Texture2DArray.FirstArraySlice = 0;
                synth_desc.Texture2DArray.ArraySize = rd.DepthOrArraySize;
            } else {
                synth_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            }
            d = &synth_desc;
        }
        if (TraceEnabled() && n <= 12) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CreateRTV(#" << n
                      << ", res=0x" << std::hex << pResource << std::dec
                      << ", fmt=" << (d ? static_cast<int>(d->Format) : -1)
                      << ", dim=" << (d ? static_cast<int>(d->ViewDimension) : -1)
                      << ", dst=0x" << std::hex << destDesc << std::dec << ")\n";
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{destDesc};
        obj->pc_device->CreateRenderTargetView(pc_res, d, h);
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateDepthStencilView(
    void* self, uintptr_t pResource, uintptr_t pDesc, uintptr_t destDesc) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        ID3D12Resource* pc_res = pResource ? reinterpret_cast<XboxD3D12ResourceProxy*>(pResource)->pc_res : nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE h{destDesc};
        obj->pc_device->CreateDepthStencilView(
            pc_res, reinterpret_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(pDesc), h);
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateSampler(
    void* self, uintptr_t pDesc, uintptr_t destDesc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device && pDesc) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{destDesc};
        obj->pc_device->CreateSampler(reinterpret_cast<const D3D12_SAMPLER_DESC*>(pDesc), h);
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCopyDescriptors(
    void* self, uintptr_t numDstRanges, uintptr_t pDstStarts, uintptr_t pDstSizes,
    uintptr_t numSrcRanges, uintptr_t pSrcStarts, uintptr_t pSrcSizes, uintptr_t type_raw) {
    static std::atomic<uint64_t> cd_cnt{0};
    const uint64_t n = cd_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        auto pc_type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type_raw);
        if (type_raw >= 4) pc_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        if (TraceEnabled() && n <= 8) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CopyDescriptors(#" << n
                      << ", dstRanges=" << numDstRanges << ", srcRanges=" << numSrcRanges
                      << ", type=" << static_cast<uint32_t>(pc_type) << ")\n";
        }
        obj->pc_device->CopyDescriptors(
            static_cast<UINT>(numDstRanges),
            reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pDstStarts),
            reinterpret_cast<const UINT*>(pDstSizes),
            static_cast<UINT>(numSrcRanges),
            reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pSrcStarts),
            reinterpret_cast<const UINT*>(pSrcSizes),
            pc_type);
        if (pc_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            numDstRanges == 1 && numSrcRanges == 1 && pDstStarts && pSrcStarts) {
            const UINT stride = obj->pc_device->GetDescriptorHandleIncrementSize(pc_type);
            const UINT cnt = pDstSizes ? *reinterpret_cast<const UINT*>(pDstSizes) : 1u;
            CopyTrackedUavCpuDescriptors(
                reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pDstStarts)->ptr,
                reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pSrcStarts)->ptr,
                cnt,
                stride);
        }
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCopyDescriptorsSimple(
    void* self, uintptr_t numDescs, uintptr_t destStart, uintptr_t srcStart, uintptr_t type_raw) {
    static std::atomic<uint64_t> cds_cnt{0};
    const uint64_t n = cds_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst{destStart};
        D3D12_CPU_DESCRIPTOR_HANDLE src{srcStart};
        auto pc_type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type_raw);
        if (type_raw >= 4) pc_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        if (TraceEnabled() && n <= 12) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CopyDescriptorsSimple(#" << n
                      << ", num=" << numDescs << ", dst=0x" << std::hex << destStart
                      << ", src=0x" << srcStart << std::dec
                      << ", type=" << static_cast<uint32_t>(pc_type) << ")\n";
        }
        obj->pc_device->CopyDescriptorsSimple(static_cast<UINT>(numDescs), dst, src, pc_type);
        if (pc_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && numDescs > 0) {
            const UINT stride = obj->pc_device->GetDescriptorHandleIncrementSize(pc_type);
            CopyTrackedUavCpuDescriptors(dst.ptr, src.ptr, static_cast<UINT>(numDescs), stride);
        }
    }
#endif
    return 0;
}

extern const std::array<ComVtableFn, 128> kXboxDeviceProxyVtbl;

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetResourceAllocationInfo(
    void* arg0, uintptr_t arg1, uintptr_t visibleMask, uintptr_t numDescs, uintptr_t pDescs) {
#if defined(_WIN32)
    XboxD3D12DeviceProxy* obj = nullptr;
    uintptr_t sret_ptr = 0;
    if (arg0 && *reinterpret_cast<const void* const*>(arg0) == kXboxDeviceProxyVtbl.data()) {
        obj = static_cast<XboxD3D12DeviceProxy*>(arg0);
        sret_ptr = arg1;
    } else if (arg1 && *reinterpret_cast<const void* const*>(arg1) == kXboxDeviceProxyVtbl.data()) {
        obj = reinterpret_cast<XboxD3D12DeviceProxy*>(arg1);
        sret_ptr = reinterpret_cast<uintptr_t>(arg0);
    }
    if (obj && obj->pc_device && sret_ptr && pDescs && numDescs > 0 && numDescs <= 256) {
        std::array<D3D12_RESOURCE_DESC, 16> stack_descs{};
        std::unique_ptr<D3D12_RESOURCE_DESC[]> heap_descs;
        D3D12_RESOURCE_DESC* descs = (numDescs <= stack_descs.size())
            ? stack_descs.data()
            : (heap_descs = std::make_unique<D3D12_RESOURCE_DESC[]>(static_cast<size_t>(numDescs))).get();
        const auto* src = reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDescs);
        for (size_t i = 0; i < static_cast<size_t>(numDescs); ++i) {
            descs[i] = src[i];
            descs[i].Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<uint32_t>(descs[i].Flags) & 0x3Fu);
            if ((descs[i].Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
                !(descs[i].Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
                descs[i].Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                    descs[i].Flags & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
            }
        }
        *reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(sret_ptr) =
            obj->pc_device->GetResourceAllocationInfo(
                static_cast<UINT>(visibleMask), static_cast<UINT>(numDescs), descs);
    }
    return static_cast<intptr_t>(sret_ptr);
#else
    return static_cast<intptr_t>(arg1);
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetCustomHeapProperties(
    void* arg0, uintptr_t arg1, uintptr_t nodeMask, uintptr_t heapType) {
#if defined(_WIN32)
    XboxD3D12DeviceProxy* obj = nullptr;
    uintptr_t sret_ptr = 0;
    if (arg0 && *reinterpret_cast<const void* const*>(arg0) == kXboxDeviceProxyVtbl.data()) {
        obj = static_cast<XboxD3D12DeviceProxy*>(arg0);
        sret_ptr = arg1;
    } else if (arg1 && *reinterpret_cast<const void* const*>(arg1) == kXboxDeviceProxyVtbl.data()) {
        obj = reinterpret_cast<XboxD3D12DeviceProxy*>(arg1);
        sret_ptr = reinterpret_cast<uintptr_t>(arg0);
    }
    if (obj && obj->pc_device && sret_ptr) {
        *reinterpret_cast<D3D12_HEAP_PROPERTIES*>(sret_ptr) =
            obj->pc_device->GetCustomHeapProperties(
                static_cast<UINT>(nodeMask), static_cast<D3D12_HEAP_TYPE>(heapType));
    }
    return static_cast<intptr_t>(sret_ptr);
#else
    return static_cast<intptr_t>(arg1);
#endif
}

#if defined(_WIN32)
D3D12_HEAP_PROPERTIES SanitizeGuestHeapPropsForPc(const D3D12_HEAP_PROPERTIES& orig_hp) {
    D3D12_HEAP_PROPERTIES hp = orig_hp;
    if (hp.Type == D3D12_HEAP_TYPE_CUSTOM) {
        hp.Type = (hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE)
                      ? D3D12_HEAP_TYPE_UPLOAD
                      : (hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK
                             ? D3D12_HEAP_TYPE_READBACK
                             : D3D12_HEAP_TYPE_DEFAULT);
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    }
    return hp;
}

void SanitizeGuestResourceDescAndStateForPc(
    const D3D12_HEAP_PROPERTIES& hp,
    uintptr_t heapFlags,
    D3D12_RESOURCE_DESC& desc,
    uintptr_t initialState,
    DXGI_FORMAT& out_orig_fmt,
    bool& out_promoted_typeless,
    D3D12_RESOURCE_STATES& out_pc_state) {
    const auto orig_flags = static_cast<uint32_t>(desc.Flags);
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(orig_flags & 0x3Fu);
    if (hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_READBACK) {
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            static_cast<uint32_t>(desc.Flags) & ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    }
    if ((static_cast<uint32_t>(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0u) {
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            static_cast<uint32_t>(desc.Flags) & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    }
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    }

    out_pc_state = SanitizePcResourceState(
        static_cast<D3D12_RESOURCE_STATES>(static_cast<uint32_t>(initialState)), desc.Dimension);
    if (hp.Type == D3D12_HEAP_TYPE_UPLOAD) {
        out_pc_state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (hp.Type == D3D12_HEAP_TYPE_READBACK) {
        out_pc_state = D3D12_RESOURCE_STATE_COPY_DEST;
    } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        // PC D3D12 buffers on DEFAULT heaps always begin in COMMON (passing non-COMMON
        // emits D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED under SDKLayers).
        out_pc_state = D3D12_RESOURCE_STATE_COMMON;
    }

    out_orig_fmt = desc.Format;
    out_promoted_typeless = false;
    const bool is_display_or_rt =
        ((heapFlags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) != 0) ||
        ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        hp.Type == D3D12_HEAP_TYPE_DEFAULT && !is_display_or_rt) {
        switch (desc.Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
                out_promoted_typeless = true;
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
                out_promoted_typeless = true;
                break;
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_BC1_TYPELESS;
                out_promoted_typeless = true;
                break;
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_BC2_TYPELESS;
                out_promoted_typeless = true;
                break;
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_BC3_TYPELESS;
                out_promoted_typeless = true;
                break;
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_BC7_TYPELESS;
                out_promoted_typeless = true;
                break;
            default:
                break;
        }
    }
}
#endif

// ID3D12Device::CreateHeap (slot 28): backed by a real host ID3D12Heap so
// resources placed at overlapping offsets alias physical GPU memory.
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateHeap(void* self, uintptr_t pDesc, uintptr_t riid, uintptr_t ppvHeap) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvHeap);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    if (riid && !IsD3D12HeapIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12Device::CreateHeap", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(E_NOINTERFACE);
    }

    const D3D12_HEAP_DESC orig_desc = *reinterpret_cast<const D3D12_HEAP_DESC*>(pDesc);
    D3D12_HEAP_DESC pc_hd = orig_desc;
    pc_hd.Properties = SanitizeGuestHeapPropsForPc(orig_desc.Properties);
    pc_hd.Alignment = (orig_desc.Alignment == D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
        ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
        : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    pc_hd.SizeInBytes = std::max<UINT64>(pc_hd.Alignment, AlignUp(orig_desc.SizeInBytes, 65536ull));

    if (pc_hd.Properties.Type == D3D12_HEAP_TYPE_UPLOAD ||
        pc_hd.Properties.Type == D3D12_HEAP_TYPE_READBACK) {
        pc_hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    } else {
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
        const bool tier2 =
            SUCCEEDED(obj->pc_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))) &&
            opts.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
        if (tier2) {
            // Tier 2 allows buffers, non-RT/DS textures, and RT/DS textures in one ID3D12Heap
            // while stripping Xbox-specific heap flag bits (e.g. 0x84).
            pc_hd.Flags = D3D12_HEAP_FLAG_NONE;
        } else {
            pc_hd.Flags = static_cast<D3D12_HEAP_FLAGS>(
                static_cast<uint32_t>(orig_desc.Flags) &
                (D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES |
                 D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES));
            if (pc_hd.Flags == D3D12_HEAP_FLAG_NONE) {
                pc_hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            }
        }
    }

    ID3D12Heap* pc_heap = nullptr;
    const HRESULT hr = obj->pc_device->CreateHeap(&pc_hd, IID_PPV_ARGS(&pc_heap));
    if (FAILED(hr) || !pc_heap) {
        MaybeRecordDeviceRemoved(obj->pc_device, hr, "ProxyCreateHeap");
        return static_cast<intptr_t>(hr);
    }

    auto* proxy = new (std::nothrow) XboxD3D12HeapProxy();
    if (!proxy) {
        pc_heap->Release();
        return static_cast<intptr_t>(E_OUTOFMEMORY);
    }
    proxy->lpVtbl = kXboxHeapProxyVtbl.data();
    proxy->desc = orig_desc;
    proxy->pc_desc = pc_hd;
    proxy->pc_heap = pc_heap;
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateHeap(size=0x" << std::hex << pc_hd.SizeInBytes
                  << ", type=" << std::dec << static_cast<uint32_t>(pc_hd.Properties.Type)
                  << ", align=0x" << std::hex << pc_hd.Alignment
                  << ", flags=0x" << static_cast<uint32_t>(pc_hd.Flags)
                  << std::dec << ") -> S_OK (proxy=" << proxy << ", pc_heap=" << pc_heap << ")\n";
    }
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommittedResource(
    void* self, uintptr_t pHeapProps, uintptr_t heapFlags, uintptr_t pDesc,
    uintptr_t initialState, uintptr_t pClearVal, uintptr_t /*riid*/, uintptr_t ppvRes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvRes);
    if (!obj || !obj->pc_device || !pHeapProps || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const D3D12_HEAP_PROPERTIES orig_hp = *reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(pHeapProps);
    D3D12_HEAP_PROPERTIES hp = SanitizeGuestHeapPropsForPc(orig_hp);
    D3D12_RESOURCE_DESC desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc);
    const auto orig_flags = static_cast<uint32_t>(desc.Flags);

    DXGI_FORMAT orig_fmt = desc.Format;
    bool promoted_typeless = false;
    D3D12_RESOURCE_STATES pc_state = D3D12_RESOURCE_STATE_COMMON;
    SanitizeGuestResourceDescAndStateForPc(
        hp, heapFlags, desc, initialState, orig_fmt, promoted_typeless, pc_state);

    const D3D12_CLEAR_VALUE* pc_clear_val = promoted_typeless
        ? nullptr
        : reinterpret_cast<const D3D12_CLEAR_VALUE*>(pClearVal);
    // Strip Xbox 0x80 and PC ALLOW_DISPLAY (0x8) bits rejected on non-display CreateCommittedResource.
    const auto pc_heap_flags = static_cast<D3D12_HEAP_FLAGS>(
        (heapFlags & 0x3FFFu) & ~static_cast<uintptr_t>(0x88u));
    ID3D12Resource* pc_res = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommittedResource(
        &hp, pc_heap_flags, &desc, pc_state,
        pc_clear_val, IID_PPV_ARGS(&pc_res));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommittedResource(dim="
                  << static_cast<uint32_t>(desc.Dimension) << ", w=" << desc.Width
                  << ", h=" << desc.Height << ", hp=" << static_cast<uint32_t>(orig_hp.Type)
                  << ", hflags=0x" << std::hex << heapFlags
                  << ", rflags=0x" << orig_flags
                  << ", state=0x" << initialState
                  << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_res) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12ResourceProxy();
    proxy->lpVtbl = kXboxResourceProxyVtbl.data();
    proxy->device = obj;
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    proxy->pc_res = pc_res;
    proxy->tracked_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
    proxy->exec_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
    proxy->guest_format = orig_fmt;
    proxy->promoted_typeless = promoted_typeless;
    proxy->is_default_buffer =
        (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && hp.Type == D3D12_HEAP_TYPE_DEFAULT);
    RegisterLiveResourceProxy(proxy);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        RegisterHostBufferVaRange(proxy, pc_res, desc.Width, hp.Type == D3D12_HEAP_TYPE_UPLOAD);
    }
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// ID3D12Device::CreatePlacedResource (slot 29): places the resource directly into
// `heap->pc_heap` at `heapOffset` so overlapping placed resources alias GPU memory!
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreatePlacedResource(
    void* self, uintptr_t pHeap, uintptr_t heapOffset, uintptr_t pDesc,
    uintptr_t initialState, uintptr_t pClearVal, uintptr_t riid, uintptr_t ppvRes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto* heap = reinterpret_cast<XboxD3D12HeapProxy*>(pHeap);
    auto** out = reinterpret_cast<void**>(ppvRes);
    if (out) *out = nullptr;
    if (!obj || !obj->pc_device || !heap || !IsReadableMemory(heap, sizeof(*heap)) ||
        heap->lpVtbl != kXboxHeapProxyVtbl.data() || !heap->pc_heap || !pDesc || !out) {
        return static_cast<intptr_t>(E_INVALIDARG);
    }
    if (riid && !IsD3D12ResourceIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12Device::CreatePlacedResource", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(E_NOINTERFACE);
    }

    const D3D12_HEAP_PROPERTIES& hp = heap->pc_desc.Properties;
    D3D12_RESOURCE_DESC desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc);
    DXGI_FORMAT orig_fmt = desc.Format;
    bool promoted_typeless = false;
    D3D12_RESOURCE_STATES pc_state = D3D12_RESOURCE_STATE_COMMON;
    SanitizeGuestResourceDescAndStateForPc(
        hp, static_cast<uintptr_t>(heap->desc.Flags), desc, initialState,
        orig_fmt, promoted_typeless, pc_state);

    const D3D12_CLEAR_VALUE* pc_clear_val = promoted_typeless
        ? nullptr
        : reinterpret_cast<const D3D12_CLEAR_VALUE*>(pClearVal);

    ID3D12Resource* pc_res = nullptr;
    HRESULT hr = obj->pc_device->CreatePlacedResource(
        heap->pc_heap, static_cast<UINT64>(heapOffset), &desc, pc_state,
        pc_clear_val, IID_PPV_ARGS(&pc_res));
    if (FAILED(hr) && desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        (heapOffset & 0xFFFull) == 0 && (heapOffset & 0xFFFFull) != 0) {
        D3D12_RESOURCE_DESC small_desc = desc;
        small_desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
        hr = obj->pc_device->CreatePlacedResource(
            heap->pc_heap, static_cast<UINT64>(heapOffset), &small_desc, pc_state,
            pc_clear_val, IID_PPV_ARGS(&pc_res));
    }

    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreatePlacedResource(heap=" << heap
                  << ", pc_heap=" << heap->pc_heap << ", offset=0x" << std::hex << heapOffset
                  << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_res) {
        MaybeRecordDeviceRemoved(obj->pc_device, hr, "ProxyCreatePlacedResource");
        return static_cast<intptr_t>(hr);
    }

    auto* proxy = new XboxD3D12ResourceProxy();
    proxy->lpVtbl = kXboxResourceProxyVtbl.data();
    proxy->device = obj;
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    proxy->pc_res = pc_res;
    proxy->backing_pc_heap = heap->pc_heap;
    proxy->backing_pc_heap->AddRef();
    proxy->tracked_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
    proxy->exec_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
    proxy->guest_format = orig_fmt;
    proxy->promoted_typeless = promoted_typeless;
    proxy->is_default_buffer =
        (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && hp.Type == D3D12_HEAP_TYPE_DEFAULT);
    RegisterLiveResourceProxy(proxy);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        RegisterHostBufferVaRange(proxy, pc_res, desc.Width, hp.Type == D3D12_HEAP_TYPE_UPLOAD);
    }
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateFence(
    void* self, uintptr_t initialValue, uintptr_t flags, uintptr_t /*riid*/, uintptr_t ppFence) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppFence);
    if (!obj || !obj->pc_device || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12Fence* pc_fence = nullptr;
    const HRESULT hr = obj->pc_device->CreateFence(
        initialValue, static_cast<D3D12_FENCE_FLAGS>(flags), IID_PPV_ARGS(&pc_fence));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateFence(init="
                  << initialValue << ", flags=0x" << std::hex << flags << ") -> 0x"
                  << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_fence) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12FenceProxy();
    proxy->lpVtbl = kXboxFenceProxyVtbl.data();
    proxy->pc_fence = pc_fence;
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetDeviceRemovedReason(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        const HRESULT rem = obj->pc_device->GetDeviceRemovedReason();
        if (rem != S_OK) {
            MaybeRecordDeviceRemoved(obj->pc_device, rem, "ProxyGetDeviceRemovedReason");
        }
        return static_cast<intptr_t>(rem);
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetCopyableFootprints(
    void* self, uintptr_t pDesc, uintptr_t firstSub, uintptr_t numSub, uintptr_t baseOffset,
    uintptr_t pLayouts, uintptr_t pNumRows, uintptr_t pRowSizes, uintptr_t pTotalBytes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device && pDesc) {
        obj->pc_device->GetCopyableFootprints(
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc),
            static_cast<UINT>(firstSub), static_cast<UINT>(numSub), baseOffset,
            reinterpret_cast<D3D12_PLACED_SUBRESOURCE_FOOTPRINT*>(pLayouts),
            reinterpret_cast<UINT*>(pNumRows),
            reinterpret_cast<UINT64*>(pRowSizes),
            reinterpret_cast<UINT64*>(pTotalBytes));
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateQueryHeap(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppvHeap) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvHeap);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12QueryHeap* pc_qh = nullptr;
    const HRESULT hr = obj->pc_device->CreateQueryHeap(
        reinterpret_cast<const D3D12_QUERY_HEAP_DESC*>(pDesc), IID_PPV_ARGS(&pc_qh));
    if (FAILED(hr) || !pc_qh) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_qh;
    proxy->kind = "QueryHeap";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandSignature(
    void* self, uintptr_t pDesc, uintptr_t pRootSig, uintptr_t /*riid*/, uintptr_t ppvSig) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvSig);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12RootSignature* pc_rs = pRootSig
        ? reinterpret_cast<ID3D12RootSignature*>(reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pRootSig)->pc_obj)
        : nullptr;
    ID3D12CommandSignature* pc_sig = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandSignature(
        reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(pDesc), pc_rs, IID_PPV_ARGS(&pc_sig));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandSignature() -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_sig) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_sig;
    proxy->kind = "CommandSignature";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// Provisional Xbox device slot 68 (+0x220), observed at 0x1405383b0 with a stack struct ptr; return value ignored.
intptr_t SCARLETTX_GDK_CALLBACK ProxySlot68XboxInfo(
    void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::vtable[68](a1=0x" << std::hex << a1
                  << ", a2=0x" << a2 << ", a3=0x" << a3 << std::dec << ") -> S_OK (provisional)\n";
    }
    return 0;
}

// Verified Xbox device slot 73 (+0x248) from 0x140094a4c..0x140094a88:
// CreateCommandListX(const D3D12XBOX_COMMAND_LIST_DESC* pDesc, ID3D12CommandAllocator* pAlloc,
//                    ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList)
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandListX(
    void* self, uintptr_t pDesc, uintptr_t pAlloc, uintptr_t pInitialState,
    uintptr_t /*riid*/, uintptr_t ppList) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppList);
    if (!obj || !obj->pc_device || !pDesc || !pAlloc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const uint32_t type_raw = *reinterpret_cast<const uint32_t*>(pDesc);
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(type_raw);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        type != D3D12_COMMAND_LIST_TYPE_COPY) {
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    auto* alloc_proxy = reinterpret_cast<XboxD3D12CommandAllocatorProxy*>(pAlloc);
    ID3D12PipelineState* pc_pso = nullptr;
    if (pInitialState) {
        pc_pso = reinterpret_cast<ID3D12PipelineState*>(
            reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pInitialState)->pc_obj);
    }
    ID3D12GraphicsCommandList* pc_list = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandList(
        0u, type, alloc_proxy->pc_alloc, pc_pso, IID_PPV_ARGS(&pc_list));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandListX(type="
                  << type_raw << ") -> 0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_list) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12GraphicsCommandListProxy();
    proxy->lpVtbl = (type_raw == 16u) ? kXboxDmaCmdListProxyVtbl.data()
                                      : (obj->sdk_version >= 0x20004u ? kXboxCmdListProxyVtblV4 : kXboxCmdListProxyVtbl).data();
    proxy->pc_list = pc_list;
    InitCmdListExtendedInterfaces(proxy);
    proxy->pc_device = obj->pc_device;
    if (proxy->pc_device) proxy->pc_device->AddRef();
    proxy->active_pc_alloc = alloc_proxy->pc_alloc;
    proxy->active_alloc_epoch = GetAllocatorEpoch(alloc_proxy->pc_alloc);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandListX(type="
                  << type_raw << ") -> 0x0 (proxy=" << proxy << ", alloc=" << alloc_proxy << ")\n";
    }
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// Observed Xbox device slot 78 (+0x270) at 0x14052f501: SetFrameIntervalX(pOutput, intervalUs=0x411b, period=2, flags=0).
intptr_t SCARLETTX_GDK_CALLBACK ProxySetFrameIntervalX(
    void* /*self*/, uintptr_t pOutput, uintptr_t intervalUs, uintptr_t period) {
    // The period is the allowed latency in intervals, not a rate divisor.
    g_frame_interval_us.store(intervalUs <= 1000000 ? static_cast<uint32_t>(intervalUs) : 0,
                              std::memory_order_relaxed);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::SetFrameIntervalX(out=0x" << std::hex << pOutput
                  << ", interval_us=" << std::dec << intervalUs << ", period=" << period << ") -> S_OK\n";
    }
    return 0;
}

}  // namespace

namespace scarlettx::d3d12_xs_internal {

#if defined(_WIN32)
void EnqueueDelayedEventSignal(HANDLE hEvent, uint64_t offset_us) {
    if (!hEvent) return;
    if (offset_us == 0) {
        SetEvent(hEvent);
        return;
    }
    struct DelayedSignalScheduler {
        using Clock = std::chrono::steady_clock;
        struct Item {
            Clock::time_point due;
            HANDLE ev;
            bool operator>(const Item& o) const { return due > o.due; }
        };
        std::mutex mu;
        std::condition_variable cv;
        std::vector<Item> heap;
        DelayedSignalScheduler() {
            std::thread([this]() {
                std::unique_lock<std::mutex> lk(mu);
                for (;;) {
                    if (heap.empty()) {
                        cv.wait(lk);
                        continue;
                    }
                    const auto now = Clock::now();
                    if (heap.front().due > now) {
                        cv.wait_until(lk, heap.front().due);
                        continue;
                    }
                    std::pop_heap(heap.begin(), heap.end(), std::greater<Item>{});
                    HANDLE target = heap.back().ev;
                    heap.pop_back();
                    lk.unlock();
                    SetEvent(target);
                    lk.lock();
                }
            }).detach();
        }
        void Schedule(HANDLE ev, uint64_t us) {
            std::lock_guard<std::mutex> lk(mu);
            heap.push_back(Item{Clock::now() + std::chrono::microseconds(us), ev});
            std::push_heap(heap.begin(), heap.end(), std::greater<Item>{});
            cv.notify_one();
        }
    };
    static DelayedSignalScheduler* sched = new DelayedSignalScheduler();
    sched->Schedule(hEvent, offset_us);
}
#endif

bool TryGetDeviceProxyRemovedReason(uint64_t candidate_ptr, int32_t* out_reason) {
    if (out_reason) *out_reason = 0;
#if defined(_WIN32)
    if (candidate_ptr <= 0x10000ull) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(candidate_ptr), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT) {
        return false;
    }
    const void* vtbl = *reinterpret_cast<const void* const*>(candidate_ptr);
    if (vtbl != kXboxDeviceProxyVtbl.data()) return false;
    const auto* proxy = reinterpret_cast<const XboxD3D12DeviceProxy*>(candidate_ptr);
    if (proxy->pc_device && out_reason) {
        *out_reason = static_cast<int32_t>(proxy->pc_device->GetDeviceRemovedReason());
    }
    return true;
#else
    (void)candidate_ptr;
    return false;
#endif
}

}  // namespace scarlettx::d3d12_xs_internal

namespace {

// Observed Xbox device slot 79 (+0x278) at 0x14052f51e: ScheduleFrameEventX(eventType, offset, pEvent, flags).
intptr_t SCARLETTX_GDK_CALLBACK ProxyScheduleFrameEventX(
    void* /*self*/, uintptr_t eventType, uintptr_t offset, uintptr_t pEvent) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::ScheduleFrameEventX(type=" << eventType
                  << ", offset=" << offset << ", event=0x" << std::hex << pEvent << std::dec << ") -> S_OK\n";
    }
#if defined(_WIN32)
    if (pEvent) {
        EnqueueDelayedEventSignal(reinterpret_cast<HANDLE>(pEvent), static_cast<uint64_t>(offset));
    }
#endif
    return 0;
}

// Observed Xbox device slot 80 (+0x280) at 0x14008ced6:
//   WaitFrameEventX(eventType=0/1, timeoutMs=INFINITE, pEvent=NULL, flags=0, pToken=&out_u64)
std::atomic<uint64_t> g_frame_pipeline_token{1};

// Blocks until the next QPC frame origin or VBlank (VSYNC) using a high-resolution
// kernel waitable timer (< 1% CPU usage, zero spin-wait, zero double-throttle with PresentX).
void PaceFrameCadenceQpc(bool is_vsync, DWORD timeout_ms) {
#if defined(_WIN32)
    // Re-read SCARLETTX_FCAP dynamically so per-test or runtime changes take effect cleanly.
    const char* fcap_env = std::getenv("SCARLETTX_FCAP");
    const long fcap = (fcap_env && fcap_env[0] != '\0') ? std::strtol(fcap_env, nullptr, 10) : -1L;
    if (fcap == 0) return;

    const uint32_t configured_us = g_frame_interval_us.load(std::memory_order_relaxed);
    const uint32_t interval_us = (fcap > 0)
        ? static_cast<uint32_t>(1000000L / fcap)
        : (configured_us > 0 ? configured_us : (is_vsync ? 16667u : 0u));
    if (interval_us == 0) return;

    static std::mutex mu;
    static int64_t next_origin_qpc = 0;
    static int64_t next_vsync_qpc = 0;
    static const int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1000000ll;
    }();

    thread_local struct ThreadHighResTimer {
        HANDLE handle = nullptr;
        ThreadHighResTimer() {
            handle = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
            if (!handle) {
                handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            }
        }
        ~ThreadHighResTimer() {
            if (handle) CloseHandle(handle);
        }
    } tl_timer;

    int64_t target_qpc = 0;
    const int64_t interval_qpc = std::max<int64_t>(1ll, (freq * static_cast<int64_t>(interval_us)) / 1000000ll);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    {
        std::lock_guard<std::mutex> lock(mu);
        int64_t& cursor = is_vsync ? next_vsync_qpc : next_origin_qpc;
        if (now.QuadPart > cursor + interval_qpc) {
            cursor = now.QuadPart;  // fell behind, snap without catch-up burst
        }
        target_qpc = cursor;
        cursor += interval_qpc;
    }

    if (now.QuadPart < target_qpc && timeout_ms != 0) {
        LARGE_INTEGER due{};
        due.QuadPart = -((target_qpc - now.QuadPart) * 10000000ll / freq);
        if (tl_timer.handle && SetWaitableTimer(tl_timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(tl_timer.handle, timeout_ms);
        }
    }

    LARGE_INTEGER done_qpc{};
    QueryPerformanceCounter(&done_qpc);
    g_last_wait_frame_interval_us.store(interval_us, std::memory_order_relaxed);
    g_last_wait_frame_event_qpc.store(done_qpc.QuadPart, std::memory_order_relaxed);
    g_gpu_profiler.RecordQpcTick();
    scarlettx::FlightRecorder::Instance().BeatProgress("WaitFrameEventX");
#else
    (void)is_vsync;
    (void)timeout_ms;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyWaitFrameEventX(
    void* /*self*/, uintptr_t eventType, uintptr_t timeoutMs, uintptr_t pEvent,
    uintptr_t flags, uintptr_t pToken) {
    if (eventType == 0 || eventType == 1) {
#if defined(_WIN32)
        const DWORD wait_ms = (timeoutMs >= 0xFFFFFFFFull) ? INFINITE : static_cast<DWORD>(timeoutMs);
        PaceFrameCadenceQpc(eventType == 1, wait_ms);
        if (pEvent) {
            SetEvent(reinterpret_cast<HANDLE>(pEvent));
        }
#endif
    }
    uint64_t tok = g_frame_pipeline_token.fetch_add(1, std::memory_order_relaxed);
    if (pToken) {
        *reinterpret_cast<uint64_t*>(pToken) = tok;
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::WaitFrameEventX(type=" << eventType
                  << ", timeout=0x" << std::hex << timeoutMs << ", event=0x" << pEvent
                  << ", flags=0x" << flags << std::dec << ") -> S_OK (tok=" << tok << ")\n";
    }
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxySetStablePowerState(void* self, uintptr_t enable, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        // Ignore E_FAIL when developer mode is disabled on host; console titles expect S_OK.
        (void)obj->pc_device->SetStablePowerState(enable ? TRUE : FALSE);
    }
#endif
    return 0;
}

// Win64 COM aggregate return ABI: RCX = XboxD3D12DeviceProxy* this, RDX = LUID* sret.
intptr_t SCARLETTX_GDK_CALLBACK ProxyGetAdapterLuid(void* self, uintptr_t sret_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto* out_luid = reinterpret_cast<LUID*>(sret_ptr);
    if (out_luid) {
        *out_luid = (obj && obj->pc_device) ? obj->pc_device->GetAdapterLuid() : LUID{};
    }
#endif
    return static_cast<intptr_t>(sret_ptr);
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateStateObject(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppStateObject) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppStateObject);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12Device5* dev5 = nullptr;
    if (FAILED(obj->pc_device->QueryInterface(IID_PPV_ARGS(&dev5))) || !dev5) {
        return static_cast<intptr_t>(E_NOINTERFACE);
    }

    const auto* in_desc = reinterpret_cast<const D3D12_STATE_OBJECT_DESC*>(pDesc);
    D3D12_STATE_OBJECT_DESC desc = *in_desc;
    std::vector<D3D12_STATE_SUBOBJECT> subobjs;
    std::vector<D3D12_GLOBAL_ROOT_SIGNATURE> global_rs;
    std::vector<D3D12_LOCAL_ROOT_SIGNATURE> local_rs;
    std::vector<D3D12_DXIL_LIBRARY_DESC> dxil_libs;
    std::vector<std::vector<uint8_t>> dxil_blobs;
    if (in_desc->NumSubobjects > 0 && in_desc->pSubobjects) {
        subobjs.assign(in_desc->pSubobjects, in_desc->pSubobjects + in_desc->NumSubobjects);
        global_rs.reserve(subobjs.size());
        local_rs.reserve(subobjs.size());
        dxil_libs.reserve(subobjs.size());
        dxil_blobs.reserve(subobjs.size());
        for (size_t i = 0; i < subobjs.size(); ++i) {
            if (subobjs[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY && subobjs[i].pDesc) {
                auto lib = *static_cast<const D3D12_DXIL_LIBRARY_DESC*>(subobjs[i].pDesc);
                std::vector<uint8_t> translated_dxil;
                TranslateGuestShaderStageIfNeeded(lib.DXILLibrary, translated_dxil);
                if (!translated_dxil.empty()) {
                    dxil_blobs.push_back(std::move(translated_dxil));
                    lib.DXILLibrary.pShaderBytecode = dxil_blobs.back().data();
                    lib.DXILLibrary.BytecodeLength = dxil_blobs.back().size();
                }
                dxil_libs.push_back(lib);
                subobjs[i].pDesc = &dxil_libs.back();
            } else if (subobjs[i].Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE && subobjs[i].pDesc) {
                auto grs = *static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(subobjs[i].pDesc);
                if (grs.pGlobalRootSignature) {
                    auto* rp = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(grs.pGlobalRootSignature);
                    grs.pGlobalRootSignature = reinterpret_cast<ID3D12RootSignature*>(rp->pc_obj);
                }
                global_rs.push_back(grs);
                subobjs[i].pDesc = &global_rs.back();
            } else if (subobjs[i].Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && subobjs[i].pDesc) {
                auto lrs = *static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(subobjs[i].pDesc);
                if (lrs.pLocalRootSignature) {
                    auto* rp = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(lrs.pLocalRootSignature);
                    lrs.pLocalRootSignature = reinterpret_cast<ID3D12RootSignature*>(rp->pc_obj);
                }
                local_rs.push_back(lrs);
                subobjs[i].pDesc = &local_rs.back();
            } else if (subobjs[i].Type == D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION &&
                       subobjs[i].pDesc) {
                // Keep pointer to translated subobject slot if pointing into original array
                const auto* assoc = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(subobjs[i].pDesc);
                if (assoc->pSubobjectToAssociate >= in_desc->pSubobjects &&
                    assoc->pSubobjectToAssociate < in_desc->pSubobjects + in_desc->NumSubobjects) {
                    const size_t idx = static_cast<size_t>(assoc->pSubobjectToAssociate - in_desc->pSubobjects);
                    auto* copy_assoc = new D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION(*assoc);
                    copy_assoc->pSubobjectToAssociate = &subobjs[idx];
                    subobjs[i].pDesc = copy_assoc;
                }
            }
        }
        desc.pSubobjects = subobjs.data();
    }

    ID3D12StateObject* so = nullptr;
    const HRESULT hr = dev5->CreateStateObject(&desc, IID_PPV_ARGS(&so));
    if (in_desc->NumSubobjects > 0 && in_desc->pSubobjects) {
        for (size_t i = 0; i < subobjs.size(); ++i) {
            if (subobjs[i].Type == D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION &&
                subobjs[i].pDesc != in_desc->pSubobjects[i].pDesc) {
                delete static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(subobjs[i].pDesc);
            }
        }
    }
    dev5->Release();
    if (FAILED(hr) || !so) {
        const void* first_raw = nullptr;
        size_t first_raw_sz = 0;
        if (in_desc->NumSubobjects > 0 && in_desc->pSubobjects) {
            for (UINT i = 0; i < in_desc->NumSubobjects; ++i) {
                if (in_desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY &&
                    in_desc->pSubobjects[i].pDesc) {
                    const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(in_desc->pSubobjects[i].pDesc);
                    first_raw = ld->DXILLibrary.pShaderBytecode;
                    first_raw_sz = ld->DXILLibrary.BytecodeLength;
                    break;
                }
            }
        }
        scarlettx::FlightRecorder::Instance().DumpFailingShaderArtifact(
            "StateObject_DXR", static_cast<int32_t>(hr),
            first_raw, first_raw_sz,
            dxil_blobs.empty() ? nullptr : dxil_blobs.front().data(),
            dxil_blobs.empty() ? 0u : dxil_blobs.front().size(),
            SCARLETTX_CALLER_RIP());
        MaybeRecordDeviceRemoved(obj->pc_device, hr, "ProxyCreateStateObject");
        return static_cast<intptr_t>(hr);
    }
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = so;
    proxy->kind = "StateObject";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetRaytracingPrebuildInfo(
    void* self, uintptr_t pDesc, uintptr_t pInfo, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device && pDesc && pInfo) {
        ID3D12Device5* dev5 = nullptr;
        if (SUCCEEDED(obj->pc_device->QueryInterface(IID_PPV_ARGS(&dev5))) && dev5) {
            dev5->GetRaytracingAccelerationStructurePrebuildInfo(
                reinterpret_cast<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*>(pDesc),
                reinterpret_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*>(pInfo));
            dev5->Release();
        }
    }
#endif
    return 0;
}

#if defined(_WIN32)
template <typename Inner>
struct alignas(void*) PsoStreamToken {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    Inner inner;
};

size_t PsoStreamSubobjectByteSize(uint32_t raw_type) {
    switch (raw_type) {
        case 0:  return sizeof(PsoStreamToken<ID3D12RootSignature*>);               // ROOT_SIGNATURE
        case 1:                                                                     // VS
        case 2:                                                                     // PS
        case 3:                                                                     // DS
        case 4:                                                                     // HS
        case 5:                                                                     // GS
        case 6:                                                                     // CS
        case 24:                                                                    // AS (Amplification)
        case 25: return sizeof(PsoStreamToken<D3D12_SHADER_BYTECODE>);              // MS (Mesh)
        case 7:  return sizeof(PsoStreamToken<D3D12_STREAM_OUTPUT_DESC>);           // STREAM_OUTPUT
        case 8:  return sizeof(PsoStreamToken<D3D12_BLEND_DESC>);                   // BLEND
        case 9:  return sizeof(PsoStreamToken<UINT>);                               // SAMPLE_MASK
        case 10: return sizeof(PsoStreamToken<D3D12_RASTERIZER_DESC>);              // RASTERIZER
        case 11: return sizeof(PsoStreamToken<D3D12_DEPTH_STENCIL_DESC>);           // DEPTH_STENCIL
        case 12: return sizeof(PsoStreamToken<D3D12_INPUT_LAYOUT_DESC>);            // INPUT_LAYOUT
        case 13: return sizeof(PsoStreamToken<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>); // IB_STRIP_CUT_VALUE
        case 14: return sizeof(PsoStreamToken<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);      // PRIMITIVE_TOPOLOGY
        case 15: return sizeof(PsoStreamToken<D3D12_RT_FORMAT_ARRAY>);              // RENDER_TARGET_FORMATS
        case 16: return sizeof(PsoStreamToken<DXGI_FORMAT>);                        // DEPTH_STENCIL_FORMAT
        case 17: return sizeof(PsoStreamToken<DXGI_SAMPLE_DESC>);                   // SAMPLE_DESC
        case 18: return sizeof(PsoStreamToken<UINT>);                               // NODE_MASK
        case 19: return sizeof(PsoStreamToken<D3D12_CACHED_PIPELINE_STATE>);        // CACHED_PSO
        case 20: return sizeof(PsoStreamToken<D3D12_PIPELINE_STATE_FLAGS>);         // FLAGS
        case 21: return sizeof(PsoStreamToken<D3D12_DEPTH_STENCIL_DESC1>);          // DEPTH_STENCIL1
        case 22: return sizeof(PsoStreamToken<D3D12_VIEW_INSTANCING_DESC>);         // VIEW_INSTANCING
        default: return 0;
    }
}
#endif

// ID3D12Device2::CreatePipelineState (stream-based PSOs for Mesh Shaders, VRS, modern D3D12.2):
// Unwraps XboxD3D12SimpleObjectProxy* root signatures and translates Xbox/XS2 shader stages in-stream.
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreatePipelineState(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppPso) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppPso);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;

    ID3D12Device2* dev2 = nullptr;
    if (FAILED(obj->pc_device->QueryInterface(IID_PPV_ARGS(&dev2))) || !dev2) {
        return static_cast<intptr_t>(E_NOINTERFACE);
    }

    const auto* in_stream = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DESC*>(pDesc);
    if (!in_stream->pPipelineStateSubobjectStream || in_stream->SizeInBytes == 0) {
        dev2->Release();
        return static_cast<intptr_t>(E_INVALIDARG);
    }

    const auto* src_bytes = static_cast<const uint8_t*>(in_stream->pPipelineStateSubobjectStream);
    std::vector<uint8_t> stream_copy(src_bytes, src_bytes + in_stream->SizeInBytes);
    std::vector<std::vector<uint8_t>> translated_stages;
    translated_stages.reserve(6);

    size_t pos = 0;
    while (pos + sizeof(uint32_t) <= stream_copy.size()) {
        uint32_t raw_type = 0;
        std::memcpy(&raw_type, stream_copy.data() + pos, sizeof(uint32_t));
        const size_t tok_sz = PsoStreamSubobjectByteSize(raw_type);
        if (tok_sz == 0 || pos + tok_sz > stream_copy.size()) {
            break;  // Unknown vendor extension tail; pass stream prefix/remainder untouched
        }
        if (raw_type == 0) {
            auto* tok = reinterpret_cast<PsoStreamToken<ID3D12RootSignature*>*>(stream_copy.data() + pos);
            if (tok->inner) {
                auto* rs_proxy = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(tok->inner);
                tok->inner = reinterpret_cast<ID3D12RootSignature*>(rs_proxy->pc_obj);
            }
        } else if ((raw_type >= 1 && raw_type <= 6) || raw_type == 24 || raw_type == 25) {
            auto* tok = reinterpret_cast<PsoStreamToken<D3D12_SHADER_BYTECODE>*>(stream_copy.data() + pos);
            std::vector<uint8_t> stage_blob;
            TranslateGuestShaderStageIfNeeded(tok->inner, stage_blob);
            if (!stage_blob.empty()) {
                translated_stages.push_back(std::move(stage_blob));
                tok->inner.pShaderBytecode = translated_stages.back().data();
                tok->inner.BytecodeLength = translated_stages.back().size();
            }
        }
        pos += tok_sz;
    }

    D3D12_PIPELINE_STATE_STREAM_DESC mod_desc{};
    mod_desc.SizeInBytes = stream_copy.size();
    mod_desc.pPipelineStateSubobjectStream = stream_copy.data();

    ID3D12PipelineState* pc_pso = nullptr;
    const HRESULT hr = dev2->CreatePipelineState(&mod_desc, IID_PPV_ARGS(&pc_pso));
    dev2->Release();
    if (FAILED(hr) || !pc_pso) {
        scarlettx::FlightRecorder::Instance().DumpFailingShaderArtifact(
            "PipelineStateStream", static_cast<int32_t>(hr),
            in_stream->pPipelineStateSubobjectStream, in_stream->SizeInBytes,
            translated_stages.empty() ? nullptr : translated_stages.front().data(),
            translated_stages.empty() ? 0u : translated_stages.front().size(),
            SCARLETTX_CALLER_RIP());
        MaybeRecordDeviceRemoved(obj->pc_device, hr, "ProxyCreatePipelineState");
        return static_cast<intptr_t>(hr);
    }

    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_pso;
    proxy->kind = "PipelineState";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// Xbox ID3D12Device::CreatePlacedResourceX (slot 74, +0x250):
// Places a D3D12 resource onto a shared host ID3D12Heap corresponding to the
// guest Unified Memory / KernelX GPU Virtual Address region at exact byte offsets
// (so overlapping CreatePlacedResourceX calls alias GPU memory), while syncing
// console-swizzled or CPU writes cleanly via fence-tracked staging!
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreatePlacedResourceX(
    void* self, uintptr_t baseAddress, uintptr_t pDesc, uintptr_t initialState,
    uintptr_t pClearVal, uintptr_t riid, uintptr_t ppvRes) {
#if defined(_WIN32)
    auto* dev_proxy = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvRes);
    if (out) *out = nullptr;
    if (!dev_proxy || !dev_proxy->pc_device || baseAddress == 0 || !pDesc || !out) {
        return static_cast<intptr_t>(E_INVALIDARG);
    }
    if (riid && !IsD3D12ResourceIid(riid)) {
        scarlettx::FlightRecorder::Instance().RecordComQiMiss(
            "XboxID3D12Device::CreatePlacedResourceX", reinterpret_cast<const void*>(riid), SCARLETTX_CALLER_RIP());
        return static_cast<intptr_t>(E_NOINTERFACE);
    }

    uint8_t* guest_cpu_va = nullptr;
    uint64_t guest_gpu_va = static_cast<uint64_t>(baseAddress);
    if (!QueryKernelXUnifiedAddresses(static_cast<uint64_t>(baseAddress), &guest_cpu_va, &guest_gpu_va)) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(baseAddress), &mbi, sizeof(mbi)) != 0 &&
            mbi.State == MEM_COMMIT) {
            guest_cpu_va = reinterpret_cast<uint8_t*>(baseAddress);
        }
    }
    if (!guest_cpu_va) {
        return static_cast<intptr_t>(E_INVALIDARG);
    }

    D3D12_HEAP_PROPERTIES default_hp{};
    default_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc);
    DXGI_FORMAT orig_fmt = desc.Format;
    bool promoted_typeless = false;
    D3D12_RESOURCE_STATES pc_state = D3D12_RESOURCE_STATE_COMMON;
    SanitizeGuestResourceDescAndStateForPc(
        default_hp, 0u, desc, initialState, orig_fmt, promoted_typeless, pc_state);
    const D3D12_CLEAR_VALUE* pc_clear_val = promoted_typeless
        ? nullptr
        : reinterpret_cast<const D3D12_CLEAR_VALUE*>(pClearVal);

    const D3D12_RESOURCE_ALLOCATION_INFO alloc_info =
        dev_proxy->pc_device->GetResourceAllocationInfo(0, 1, &desc);

    uint64_t region_base_gpu_va = static_cast<uint64_t>(baseAddress) & ~0xFFFFull;
    uint64_t region_span_bytes = alloc_info.SizeInBytes;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(guest_cpu_va, &mbi, sizeof(mbi)) != 0 && mbi.AllocationBase != nullptr) {
        const auto* alloc_base = static_cast<const uint8_t*>(mbi.AllocationBase);
        if (guest_cpu_va >= alloc_base) {
            const uint64_t cpu_off = static_cast<uint64_t>(guest_cpu_va - alloc_base);
            if (static_cast<uint64_t>(baseAddress) >= cpu_off) {
                region_base_gpu_va = (static_cast<uint64_t>(baseAddress) - cpu_off) & ~0xFFFFull;
                region_span_bytes = std::max<uint64_t>(
                    region_span_bytes,
                    (static_cast<uint64_t>(baseAddress) - region_base_gpu_va) + mbi.RegionSize);
            }
        }
    }

    uint64_t heap_offset = 0;
    ID3D12Heap* shared_heap = AcquireOrCreatePlacedXHeap(
        dev_proxy->pc_device, region_base_gpu_va, region_span_bytes, &heap_offset,
        static_cast<uint64_t>(baseAddress));

    ID3D12Resource* pc_res = nullptr;
    if (shared_heap && alloc_info.Alignment > 0 && (heap_offset % alloc_info.Alignment) == 0) {
        (void)dev_proxy->pc_device->CreatePlacedResource(
            shared_heap, heap_offset, &desc, pc_state, pc_clear_val, IID_PPV_ARGS(&pc_res));
    }
    if (!pc_res) {
        if (shared_heap) {
            shared_heap->Release();
            shared_heap = nullptr;
        }
        scarlettx::FlightRecorder::Instance().RecordEvent(
            scarlettx::FlightEventCategory::HleCall, "d3d12_xs",
            "CreatePlacedResourceX_UnalignedFallback", 0, SCARLETTX_CALLER_RIP(),
            static_cast<uint64_t>(baseAddress), heap_offset);
        const HRESULT chr = dev_proxy->pc_device->CreateCommittedResource(
            &default_hp, D3D12_HEAP_FLAG_NONE, &desc, pc_state, pc_clear_val, IID_PPV_ARGS(&pc_res));
        if (FAILED(chr) || !pc_res) {
            return static_cast<intptr_t>(chr);
        }
    }

    auto* res_proxy = new XboxD3D12ResourceProxy();
    res_proxy->lpVtbl = kXboxResourceProxyVtbl.data();
    res_proxy->device = dev_proxy;
    dev_proxy->ref_count.fetch_add(1, std::memory_order_relaxed);
    res_proxy->pc_res = pc_res;
    res_proxy->backing_pc_heap = shared_heap;  // ownership transferred
    res_proxy->tracked_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
    res_proxy->exec_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
    res_proxy->guest_format = orig_fmt;
    res_proxy->promoted_typeless = promoted_typeless;
    res_proxy->is_default_buffer = (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
    RegisterLiveResourceProxy(res_proxy);

    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        (void)BindExternalGuestTextureMemory(res_proxy, guest_cpu_va, static_cast<uint64_t>(baseAddress));
    } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        res_proxy->external_buffer_gpu_va = static_cast<uint64_t>(baseAddress);
        RegisterHostBufferVaRange(res_proxy, pc_res, desc.Width, false);
        RegisterPlacedBufferAllocation(
            res_proxy,
            dev_proxy->pc_device,
            pc_res,
            guest_cpu_va,
            static_cast<uint64_t>(baseAddress),
            static_cast<size_t>(desc.Width));
    }
    *out = res_proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// ID3D12Device::CreateReservedResource (slot 30, +0xF0) & ID3D12Device8::CreateSamplerFeedbackUnorderedAccessView (slot 65)
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateReservedResource(
    void* self, uintptr_t pDesc, uintptr_t initialState,
    uintptr_t pClearVal, uintptr_t riid, uintptr_t ppvRes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvRes);
    if (out) *out = nullptr;
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);

    D3D12_RESOURCE_DESC desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc);
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<uint32_t>(desc.Flags) & 0x3Fu);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    } else if (desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN) {
        desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    }
    const auto pc_state = SanitizePcResourceState(static_cast<D3D12_RESOURCE_STATES>(initialState), desc.Dimension);
    ID3D12Resource* pc_res = nullptr;
    const HRESULT hr = obj->pc_device->CreateReservedResource(
        &desc, pc_state, reinterpret_cast<const D3D12_CLEAR_VALUE*>(pClearVal), IID_PPV_ARGS(&pc_res));
    if (SUCCEEDED(hr) && pc_res) {
        auto* proxy = new XboxD3D12ResourceProxy();
        proxy->lpVtbl = kXboxResourceProxyVtbl.data();
        proxy->device = obj;
        obj->ref_count.fetch_add(1, std::memory_order_relaxed);
        proxy->pc_res = pc_res;
        proxy->tracked_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
        proxy->exec_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
        proxy->guest_format = desc.Format;
        RegisterLiveResourceProxy(proxy);
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            RegisterHostBufferVaRange(proxy, pc_res, desc.Width);
        }
        *out = proxy;
        return 0;
    }
    // Clean fallback to committed resource when host adapter lacks TiledResourcesTier for this format/dimension
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC fb_desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc);
    if (fb_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        fb_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    return ProxyCreateCommittedResource(
        self, reinterpret_cast<uintptr_t>(&hp), 0u, reinterpret_cast<uintptr_t>(&fb_desc),
        initialState, pClearVal, riid, ppvRes);
#else
    (void)self; (void)pDesc; (void)initialState; (void)pClearVal; (void)riid; (void)ppvRes;
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateSamplerFeedbackUAV(
    void* self, uintptr_t pTargetedResource, uintptr_t pFeedbackResource, uintptr_t destDescriptor) {
#if defined(_WIN32) && defined(__ID3D12Device8_INTERFACE_DEFINED__)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device && pFeedbackResource && destDescriptor) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
        if (SUCCEEDED(obj->pc_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7))) &&
            opts7.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED) {
            ID3D12Device8* dev8 = nullptr;
            if (SUCCEEDED(obj->pc_device->QueryInterface(IID_PPV_ARGS(&dev8))) && dev8) {
                ID3D12Resource* tgt = pTargetedResource
                    ? reinterpret_cast<XboxD3D12ResourceProxy*>(pTargetedResource)->pc_res
                    : nullptr;
                ID3D12Resource* fb = reinterpret_cast<XboxD3D12ResourceProxy*>(pFeedbackResource)->pc_res;
                D3D12_CPU_DESCRIPTOR_HANDLE h{destDescriptor};
                if (fb) dev8->CreateSamplerFeedbackUnorderedAccessView(tgt, fb, h);
                dev8->Release();
            }
        }
    }
#else
    (void)self; (void)pTargetedResource; (void)pFeedbackResource; (void)destDescriptor;
#endif
    return 0;
}

std::array<ComVtableFn, 128> BuildXboxDeviceProxyVtable() {
    auto vtbl = MakeProxyVtable(std::make_index_sequence<125>{});
    vtbl[6] = &ProxySetName;                                                           // +0x30
    vtbl[7] = &ProxyGetNodeCount;                                                      // +0x38
    vtbl[8] = &ProxyCreateCommandQueue;                                                // +0x40
    vtbl[9] = &ProxyCreateCommandAllocator;                                            // +0x48
    vtbl[10] = &ProxyCreateGraphicsPipelineState;                                      // +0x50
    vtbl[11] = &ProxyCreateComputePipelineState;                                       // +0x58
    vtbl[12] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommandList);                 // +0x60
    vtbl[13] = &ProxyCheckFeatureSupport;                                              // +0x68
    vtbl[14] = &ProxyCreateDescriptorHeap;                                             // +0x70
    vtbl[15] = &ProxyGetDescriptorHandleIncrementSize;                                 // +0x78
    vtbl[16] = reinterpret_cast<ComVtableFn>(&ProxyCreateRootSignature);               // +0x80
    vtbl[17] = &ProxyCreateConstantBufferView;                                         // +0x88
    vtbl[18] = &ProxyCreateShaderResourceView;                                         // +0x90
    vtbl[19] = reinterpret_cast<ComVtableFn>(&ProxyCreateUnorderedAccessView);         // +0x98
    vtbl[20] = &ProxyCreateRenderTargetView;                                           // +0xa0
    vtbl[21] = &ProxyCreateDepthStencilView;                                           // +0xa8
    vtbl[22] = &ProxyCreateSampler;                                                    // +0xb0
    vtbl[23] = reinterpret_cast<ComVtableFn>(&ProxyCopyDescriptors);                   // +0xb8
    vtbl[24] = reinterpret_cast<ComVtableFn>(&ProxyCopyDescriptorsSimple);             // +0xc0
    vtbl[25] = reinterpret_cast<ComVtableFn>(&ProxyGetResourceAllocationInfo);         // +0xc8
    vtbl[26] = &ProxyGetCustomHeapProperties;                                          // +0xd0
    vtbl[27] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommittedResource);           // +0xd8
    vtbl[28] = &ProxyCreateHeap;                                                       // +0xe0
    vtbl[29] = reinterpret_cast<ComVtableFn>(&ProxyCreatePlacedResource);              // +0xe8
    vtbl[30] = reinterpret_cast<ComVtableFn>(&ProxyCreateReservedResource);            // +0xf0
    vtbl[36] = reinterpret_cast<ComVtableFn>(&ProxyCreateFence);                       // +0x120
    vtbl[37] = &ProxyGetDeviceRemovedReason;                                           // +0x128
    vtbl[38] = reinterpret_cast<ComVtableFn>(&ProxyGetCopyableFootprints);             // +0x130
    vtbl[39] = &ProxyCreateQueryHeap;                                                  // +0x138
    vtbl[40] = &ProxySetStablePowerState;                                              // +0x140
    vtbl[41] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommandSignature);            // +0x148
    vtbl[44] = &ProxyGetAdapterLuid;                                                   // +0x160
    vtbl[48] = &ProxyCreateCommandQueueX;                                              // +0x180
    vtbl[50] = &ProxyCreatePipelineState;                                              // +0x190 (ID3D12Device2 stream PSO)
    vtbl[55] = &ProxyCreateStateObject;                                                // +0x1b8
    vtbl[57] = &ProxyGetRaytracingPrebuildInfo;                                        // +0x1c8
    vtbl[65] = &ProxyCreateSamplerFeedbackUAV;                                         // +0x208 (ID3D12Device8 SamplerFeedback)
    vtbl[68] = &ProxySlot68XboxInfo;                                                   // +0x220
    vtbl[69] = &ProxySlot69XboxSetting;                                                // +0x228
    vtbl[73] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommandListX);                // +0x248
    vtbl[74] = reinterpret_cast<ComVtableFn>(&ProxyCreatePlacedResourceX);             // +0x250 (CreatePlacedResourceX)
    vtbl[78] = &ProxySetFrameIntervalX;                                                // +0x270
    vtbl[79] = &ProxyScheduleFrameEventX;                                              // +0x278
    vtbl[80] = reinterpret_cast<ComVtableFn>(&ProxyWaitFrameEventX);                   // +0x280
    return vtbl;
}

const std::array<ComVtableFn, 128> kXboxDeviceProxyVtbl = BuildXboxDeviceProxyVtable();

#if defined(_WIN32)
IUnknown* UnwrapGuestDxgiAdapter(void* pAdapter) {
    if (!pAdapter) return nullptr;
    if (IsReadableMemory(pAdapter, sizeof(XboxDXGIAdapterProxy))) {
        const auto* cand = static_cast<const XboxDXGIAdapterProxy*>(pAdapter);
        if (cand->lpVtbl == kXboxDXGIAdapterProxyVtbl.data()) {
            return cand->pc_adapter;
        }
    }
    return static_cast<IUnknown*>(pAdapter);
}

HRESULT CreatePcDeviceHelper(
    void* pAdapter,
    bool prefer_warp,
    D3D_FEATURE_LEVEL min_fl,
    IDXGIAdapter1** out_adapter,
    REFIID riid,
    void** ppvDevice) {
    if (out_adapter) *out_adapter = nullptr;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        return hr;
    }

    IDXGIAdapter1* adapter = nullptr;
    if (prefer_warp) {
        hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
    } else {
        if (pAdapter) {
            IUnknown* raw_adapter = UnwrapGuestDxgiAdapter(pAdapter);
            if (raw_adapter) {
                raw_adapter->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
            }
            if (adapter) {
                DXGI_ADAPTER_DESC1 requested{};
                if (SUCCEEDED(adapter->GetDesc1(&requested)) &&
                    (requested.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    adapter->Release();
                    adapter = nullptr;
                }
            }
        }
        if (!adapter) {
            IDXGIFactory6* factory6 = nullptr;
            if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6),
                                                  reinterpret_cast<void**>(&factory6))) &&
                factory6) {
                factory6->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                    reinterpret_cast<void**>(&adapter));
                factory6->Release();
            }
        }
        if (!adapter) {
            hr = factory->EnumAdapters1(0, &adapter);
        }
        if (FAILED(hr) || !adapter) {
            hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
        }
    }

    {
        DXGI_ADAPTER_DESC1 chosen{};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&chosen))) {
            std::fprintf(stderr, "[ScarlettX][d3d12] Adapter: %ls%s\n", chosen.Description,
                         (chosen.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (WARP/software)"
                                                                     : " (hardware)");
            std::fflush(stderr);
        } else {
            std::fprintf(stderr, "[ScarlettX][d3d12] Adapter: none\n");
            std::fflush(stderr);
        }
    }

    scarlettx::FlightRecorder::Instance().EnableD3D12DredIfConfigured();
    const D3D_FEATURE_LEVEL primary_fl = (min_fl != 0) ? min_fl : D3D_FEATURE_LEVEL_12_0;
    hr = CallSystemD3D12CreateDevice(adapter, primary_fl, riid, ppvDevice);
    if (FAILED(hr) && primary_fl != D3D_FEATURE_LEVEL_11_0) {
        hr = CallSystemD3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, riid, ppvDevice);
    }

    factory->Release();
    if (SUCCEEDED(hr) && out_adapter) {
        *out_adapter = adapter;
    } else if (adapter) {
        adapter->Release();
    }
    return hr;
}
#endif

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// Verified Xbox Series X|S d3d12_xs.dll entry points
// ---------------------------------------------------------------------------
void SCARLETTX_GDK_CALLBACK D3D12XboxSetProcessDebugFlags(uint32_t flags) {
    g_process_debug_flags.store(flags, std::memory_order_relaxed);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] D3D12XboxSetProcessDebugFlags(0x"
                  << std::hex << flags << std::dec << ")\n";
    }
}

uint32_t SCARLETTX_GDK_CALLBACK D3D12XboxGetProcessDebugFlags(void) {
    return g_process_debug_flags.load(std::memory_order_relaxed);
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XboxCreateDevice(
    void* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    const void* riid,
    void** ppvDevice) {
#if defined(_WIN32)
    if (!riid || !ppvDevice) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    *ppvDevice = nullptr;

    if (TraceEnabled()) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] D3D12XboxCreateDevice(adapter=" << pAdapter
                  << ", params=" << pParameters
                  << ", riid=" << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3]
                  << std::dec << ")\n";
    }

    const bool prefer_warp =
        PreferWarpFromEnv(pParameters ? (pParameters->PreferWarp != 0) : true);
    const uint32_t dbg_flags =
        (pParameters ? pParameters->ProcessDebugFlags : 0u) |
        g_process_debug_flags.load(std::memory_order_relaxed);
    const bool debug_layer = (dbg_flags & D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG) != 0;

    const char* env_d3d_debug = std::getenv("SCARLETTX_D3D12_DEBUG");
    const bool force_debug_layer = env_d3d_debug && env_d3d_debug[0] == '1';
    if ((debug_layer && !RealTitleMode()) || force_debug_layer) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(CallSystemD3D12GetDebugInterface(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&dbg))) && dbg) {
            dbg->EnableDebugLayer();
            dbg->Release();
        }
    }

    if (RealTitleMode()) {
        if (IsBlockedDiagnosticsOrComIid(reinterpret_cast<uintptr_t>(riid))) {
            return static_cast<int32_t>(E_NOINTERFACE);
        }
        // Real Scarlett guest binary: NEVER hand back a raw PC ID3D12Device* vtable.
        ID3D12Device* pc_dev = nullptr;
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT hr = CreatePcDeviceHelper(
            pAdapter, prefer_warp, D3D_FEATURE_LEVEL_12_0, &adapter,
            __uuidof(ID3D12Device), reinterpret_cast<void**>(&pc_dev));
        if (FAILED(hr) || !pc_dev) {
            if (adapter) adapter->Release();
            return static_cast<int32_t>(hr);
        }
        auto* proxy = new XboxD3D12DeviceProxy();
        proxy->lpVtbl = kXboxDeviceProxyVtbl.data();
        proxy->sdk_version = pParameters ? pParameters->Version : 0;
        proxy->pc_device = pc_dev;
        proxy->pc_adapter = adapter;  // transfers ownership
        if (IsDxgiDeviceIid(reinterpret_cast<uintptr_t>(riid))) {
            const intptr_t qi_hr = ProxyQueryInterface(
                proxy, reinterpret_cast<uintptr_t>(riid), reinterpret_cast<uintptr_t>(ppvDevice), 0);
            ProxyRelease(proxy, 0, 0, 0);
            return static_cast<int32_t>(qi_hr);
        }
        *ppvDevice = proxy;
        return S_OK;
    }

    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    const HRESULT hr = CreatePcDeviceHelper(
        pAdapter, prefer_warp, D3D_FEATURE_LEVEL_12_0, nullptr, target_iid, ppvDevice);
    return static_cast<int32_t>(hr);
#else
    (void)pAdapter;
    (void)pParameters;
    (void)riid;
    (void)ppvDevice;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateDevice(
    void* pAdapter,
    uint32_t minimumFeatureLevel,
    const void* riid,
    void** ppDevice) {
#if defined(_WIN32)
    if (!riid || !ppDevice) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    *ppDevice = nullptr;
    if (RealTitleMode()) {
        if (IsBlockedDiagnosticsOrComIid(reinterpret_cast<uintptr_t>(riid))) {
            return static_cast<int32_t>(E_NOINTERFACE);
        }
        const bool prefer_warp = PreferWarpFromEnv(false);
        ID3D12Device* pc_dev = nullptr;
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT hr = CreatePcDeviceHelper(
            pAdapter,
            prefer_warp,
            static_cast<D3D_FEATURE_LEVEL>(minimumFeatureLevel),
            &adapter,
            __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&pc_dev));
        if (FAILED(hr) || !pc_dev) {
            if (adapter) adapter->Release();
            return static_cast<int32_t>(hr);
        }
        auto* proxy = new XboxD3D12DeviceProxy();
        proxy->lpVtbl = kXboxDeviceProxyVtbl.data();
        proxy->sdk_version = 0;
        proxy->pc_device = pc_dev;
        proxy->pc_adapter = adapter;
        if (IsDxgiDeviceIid(reinterpret_cast<uintptr_t>(riid))) {
            const intptr_t qi_hr = ProxyQueryInterface(
                proxy, reinterpret_cast<uintptr_t>(riid), reinterpret_cast<uintptr_t>(ppDevice), 0);
            ProxyRelease(proxy, 0, 0, 0);
            return static_cast<int32_t>(qi_hr);
        }
        *ppDevice = proxy;
        return S_OK;
    }
    scarlettx::FlightRecorder::Instance().EnableD3D12DredIfConfigured();
    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    IUnknown* pc_adapter = UnwrapGuestDxgiAdapter(pAdapter);
    return static_cast<int32_t>(CallSystemD3D12CreateDevice(
        pc_adapter,
        static_cast<D3D_FEATURE_LEVEL>(minimumFeatureLevel),
        target_iid,
        ppDevice));
#else
    (void)pAdapter; (void)minimumFeatureLevel; (void)riid; (void)ppDevice;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsGetDebugInterface(const void* riid, void** ppvDebug) {
#if defined(_WIN32)
    if (!riid || !ppvDebug) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    return static_cast<int32_t>(CallSystemD3D12GetDebugInterface(target_iid, ppvDebug));
#else
    (void)riid; (void)ppvDebug;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsSerializeRootSignature(
    const void* pRootSignature,
    uint32_t version,
    void** ppBlob,
    void** ppErrorBlob) {
#if defined(_WIN32)
    if (!pRootSignature || !ppBlob) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    return static_cast<int32_t>(CallSystemD3D12SerializeRootSignature(
        static_cast<const D3D12_ROOT_SIGNATURE_DESC*>(pRootSignature),
        static_cast<D3D_ROOT_SIGNATURE_VERSION>(version),
        reinterpret_cast<ID3DBlob**>(ppBlob),
        reinterpret_cast<ID3DBlob**>(ppErrorBlob)));
#else
    (void)pRootSignature; (void)version; (void)ppBlob; (void)ppErrorBlob;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsSerializeVersionedRootSignature(
    const void* pRootSignature,
    void** ppBlob,
    void** ppErrorBlob) {
#if defined(_WIN32)
    if (!pRootSignature || !ppBlob) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    return static_cast<int32_t>(CallSystemD3D12SerializeVersionedRootSignature(
        static_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*>(pRootSignature),
        reinterpret_cast<ID3DBlob**>(ppBlob),
        reinterpret_cast<ID3DBlob**>(ppErrorBlob)));
#else
    (void)pRootSignature; (void)ppBlob; (void)ppErrorBlob;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
#if defined(_WIN32)
    if (!pSrcData || !pRootSignatureDeserializerInterface || !ppRootSignatureDeserializer) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(pRootSignatureDeserializerInterface);
    return static_cast<int32_t>(CallSystemD3D12CreateRootSignatureDeserializer(
        pSrcData,
        srcDataSizeInBytes,
        target_iid,
        ppRootSignatureDeserializer));
#else
    (void)pSrcData; (void)srcDataSizeInBytes;
    (void)pRootSignatureDeserializerInterface; (void)ppRootSignatureDeserializer;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateVersionedRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
#if defined(_WIN32)
    if (!pSrcData || !pRootSignatureDeserializerInterface || !ppRootSignatureDeserializer) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(pRootSignatureDeserializerInterface);
    return static_cast<int32_t>(CallSystemD3D12CreateVersionedRootSignatureDeserializer(
        pSrcData,
        srcDataSizeInBytes,
        target_iid,
        ppRootSignatureDeserializer));
#else
    (void)pSrcData; (void)srcDataSizeInBytes;
    (void)pRootSignatureDeserializerInterface; (void)ppRootSignatureDeserializer;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsEnableExperimentalFeatures(
    uint32_t numFeatures,
    const void* pIIDs,
    void* pConfigurationStructs,
    uint32_t* pConfigurationStructSizes) {
#if defined(_WIN32)
    HRESULT hr = CallSystemD3D12EnableExperimentalFeatures(
        numFeatures,
        static_cast<const IID*>(pIIDs),
        pConfigurationStructs,
        reinterpret_cast<UINT*>(pConfigurationStructSizes));
    // On headless WARP/CI without developer mode, swallow E_NOINTERFACE gracefully.
    if (hr == E_NOINTERFACE) {
        return S_OK;
    }
    return static_cast<int32_t>(hr);
#else
    (void)numFeatures; (void)pIIDs; (void)pConfigurationStructs; (void)pConfigurationStructSizes;
    return XGR_S_OK;
#endif
}

}
