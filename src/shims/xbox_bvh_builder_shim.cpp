#define SCARLETTX_BUILDING_XBOX_BVH 1
#include "scarlettx/gpu/d3d12_xs_abi.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "scarlettx/gpu/rt_bvh.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

uint64_t AlignUp(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

bool IsZeroMatrix3x4(const float (&m)[3][4]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (m[r][c] != 0.0f) return false;
        }
    }
    return true;
}

scarlettx::Vec3 TransformPoint3x4(const float (&m)[3][4], scarlettx::Vec3 p) {
    return {
        m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
        m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
        m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3],
    };
}

struct BlasEntry {
    std::vector<scarlettx::RtTriangle> triangles;
    uint32_t build_flags = 0;
    scarlettx::BvhAccelerationStructure bvh;
};

struct TlasEntry {
    scarlettx::BvhAccelerationStructure bvh;
};

std::mutex g_bvh_mutex;
uint64_t g_next_bvh_handle = 0xB7000001ull;
std::unordered_map<uint64_t, BlasEntry> g_blas_map;
std::unordered_map<uint64_t, std::unique_ptr<TlasEntry>> g_tlas_map;

std::vector<scarlettx::RtTriangle> ConvertTriangles(const SCARLETT_BVH_TRIANGLE* tris, uint32_t count) {
    std::vector<scarlettx::RtTriangle> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& src = tris[i];
        scarlettx::RtTriangle tri{};
        tri.v0 = {src.v0[0], src.v0[1], src.v0[2]};
        tri.v1 = {src.v1[0], src.v1[1], src.v1[2]};
        tri.v2 = {src.v2[0], src.v2[1], src.v2[2]};
        tri.color = {src.color[0], src.color[1], src.color[2]};
        tri.reflectivity = src.reflectivity;
        out.push_back(tri);
    }
    return out;
}

}  // namespace

extern "C" {

int32_t XboxBvhGetPrebuildInfo(uint32_t triangle_count,
                               uint32_t build_flags,
                               D3D12XBOX_RT_AS_PREBUILD_INFO* out_info) {
    if (!out_info || triangle_count == 0) {
        return -1;
    }
    const uint64_t n = triangle_count;
    out_info->ResultDataMaxSizeInBytes = AlignUp(n * 192 + 256, 256);
    out_info->ScratchDataSizeInBytes = AlignUp(n * 128 + 256, 256);
    out_info->UpdateScratchDataSizeInBytes =
        (build_flags & D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE) ? AlignUp(n * 64 + 256, 256) : 0;
    return 0;
}

int32_t XboxBvhBuildBlas(const SCARLETT_BVH_BUILD_DESC* desc, uint64_t* out_blas_handle) {
    if (!desc || !desc->Triangles || desc->TriangleCount == 0 || !out_blas_handle) {
        return -1;
    }
    BlasEntry entry;
    entry.build_flags = desc->BuildFlags;
    entry.triangles = ConvertTriangles(desc->Triangles, desc->TriangleCount);
    auto st = entry.bvh.Build(entry.triangles);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    const uint64_t handle = g_next_bvh_handle++;
    g_blas_map[handle] = std::move(entry);
    *out_blas_handle = handle;
    return 0;
}

int32_t XboxBvhUpdateBlas(uint64_t blas_handle, const SCARLETT_BVH_BUILD_DESC* desc) {
    if (!desc || !desc->Triangles || desc->TriangleCount == 0) {
        return -1;
    }
    auto new_tris = ConvertTriangles(desc->Triangles, desc->TriangleCount);
    scarlettx::BvhAccelerationStructure rebuilt;
    auto st = rebuilt.Build(new_tris);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    auto it = g_blas_map.find(blas_handle);
    if (it == g_blas_map.end()) {
        return -2;
    }
    it->second.triangles = std::move(new_tris);
    it->second.bvh = std::move(rebuilt);
    return 0;
}

int32_t XboxBvhBuildTlasInstances(const SCARLETT_BVH_INSTANCE_DESC* instances,
                                  uint32_t instance_count,
                                  uint64_t* out_tlas_handle) {
    if (!instances || instance_count == 0 || !out_tlas_handle) {
        return -1;
    }

    std::vector<scarlettx::RtTriangle> combined;
    {
        std::lock_guard<std::mutex> lock(g_bvh_mutex);
        for (uint32_t i = 0; i < instance_count; ++i) {
            const auto& inst = instances[i];
            auto it = g_blas_map.find(inst.BlasHandle);
            if (it == g_blas_map.end()) {
                return -2;
            }
            const bool identity = IsZeroMatrix3x4(inst.Transform);
            for (const auto& src_tri : it->second.triangles) {
                if (identity) {
                    combined.push_back(src_tri);
                } else {
                    scarlettx::RtTriangle wt = src_tri;
                    wt.v0 = TransformPoint3x4(inst.Transform, src_tri.v0);
                    wt.v1 = TransformPoint3x4(inst.Transform, src_tri.v1);
                    wt.v2 = TransformPoint3x4(inst.Transform, src_tri.v2);
                    combined.push_back(wt);
                }
            }
        }
    }

    auto tlas = std::make_unique<TlasEntry>();
    auto st = tlas->bvh.Build(combined);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    const uint64_t handle = g_next_bvh_handle++;
    g_tlas_map[handle] = std::move(tlas);
    *out_tlas_handle = handle;
    return 0;
}

int32_t XboxBvhBuildTlas(const uint64_t* blas_handles,
                         uint32_t blas_count,
                         uint64_t* out_tlas_handle) {
    if (!blas_handles || blas_count == 0 || !out_tlas_handle) {
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(g_bvh_mutex);
        if (g_blas_map.find(blas_handles[0]) == g_blas_map.end()) {
#if defined(_WIN32)
            MEMORY_BASIC_INFORMATION mbi{};
            const size_t inst_bytes = static_cast<size_t>(blas_count) * sizeof(SCARLETT_BVH_INSTANCE_DESC);
            if (VirtualQuery(blas_handles, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
                reinterpret_cast<uintptr_t>(blas_handles) + inst_bytes <=
                    reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
                const auto* as_inst = reinterpret_cast<const SCARLETT_BVH_INSTANCE_DESC*>(blas_handles);
                if (g_blas_map.find(as_inst[0].BlasHandle) != g_blas_map.end()) {
                    // Release lock before delegating
                    goto build_from_instances;
                }
            }
#endif
            return -2;
        }
    }

    if (false) {
    build_from_instances:
        return XboxBvhBuildTlasInstances(
            reinterpret_cast<const SCARLETT_BVH_INSTANCE_DESC*>(blas_handles), blas_count, out_tlas_handle);
    }

    std::vector<scarlettx::RtTriangle> combined;
    {
        std::lock_guard<std::mutex> lock(g_bvh_mutex);
        for (uint32_t i = 0; i < blas_count; ++i) {
            auto it = g_blas_map.find(blas_handles[i]);
            if (it == g_blas_map.end()) {
                return -2;
            }
            combined.insert(combined.end(),
                            it->second.triangles.begin(),
                            it->second.triangles.end());
        }
    }

    auto tlas = std::make_unique<TlasEntry>();
    auto st = tlas->bvh.Build(combined);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    const uint64_t handle = g_next_bvh_handle++;
    g_tlas_map[handle] = std::move(tlas);
    *out_tlas_handle = handle;
    return 0;
}

int32_t XboxBvhGetStats(uint64_t handle, SCARLETT_BVH_STATS* out_stats) {
    if (!out_stats) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    const scarlettx::BvhAccelerationStructure* bvh = nullptr;
    if (auto it = g_tlas_map.find(handle); it != g_tlas_map.end()) {
        bvh = &it->second->bvh;
    } else if (auto bit = g_blas_map.find(handle); bit != g_blas_map.end()) {
        bvh = &bit->second.bvh;
    }
    if (!bvh) {
        return -2;
    }
    const auto bounds = bvh->RootBounds();
    out_stats->NodeCount = bvh->NodeCount();
    out_stats->TriangleCount = bvh->TriangleCount();
    out_stats->BoundsMin[0] = bounds.min.x;
    out_stats->BoundsMin[1] = bounds.min.y;
    out_stats->BoundsMin[2] = bounds.min.z;
    out_stats->BoundsMax[0] = bounds.max.x;
    out_stats->BoundsMax[1] = bounds.max.y;
    out_stats->BoundsMax[2] = bounds.max.z;
    return 0;
}

int32_t XboxBvhTraceSingleRay(uint64_t tlas_handle,
                              const SCARLETT_RAY_DESC* ray,
                              SCARLETT_RAY_HIT* out_hit) {
    if (!ray || !out_hit) {
        return -1;
    }
    std::memset(out_hit, 0, sizeof(*out_hit));
    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    auto it = g_tlas_map.find(tlas_handle);
    if (it == g_tlas_map.end()) {
        return -2;
    }

    scarlettx::RtRay r{};
    r.origin = {ray->Origin[0], ray->Origin[1], ray->Origin[2]};
    r.direction = {ray->Direction[0], ray->Direction[1], ray->Direction[2]};
    r.t_min = ray->TMin;
    r.t_max = ray->TMax;

    scarlettx::RtHit hit{};
    if (it->second->bvh.TraceRay(r, hit, nullptr)) {
        out_hit->Hit = 1;
        out_hit->TriangleIndex = hit.triangle_index;
        out_hit->T = hit.t;
        out_hit->U = hit.u;
        out_hit->V = hit.v;
        out_hit->Normal[0] = hit.normal.x;
        out_hit->Normal[1] = hit.normal.y;
        out_hit->Normal[2] = hit.normal.z;
    }
    return 0;
}

int32_t XboxBvhDispatchRays(uint64_t tlas_handle,
                            const SCARLETT_RT_DISPATCH_DESC* desc,
                            const char* optional_ppm_path,
                            uint64_t* out_frame_hash) {
    if (!desc) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    auto it = g_tlas_map.find(tlas_handle);
    if (it == g_tlas_map.end()) {
        return -2;
    }

    scarlettx::RtDispatchParams params{};
    params.width = desc->Width;
    params.height = desc->Height;
    params.camera_pos = {desc->CameraPos[0], desc->CameraPos[1], desc->CameraPos[2]};
    params.camera_look_at = {desc->CameraLookAt[0], desc->CameraLookAt[1], desc->CameraLookAt[2]};
    params.light_pos = {desc->LightPos[0], desc->LightPos[1], desc->LightPos[2]};
    params.max_bounces = desc->MaxBounces;

    scarlettx::FrameBufferRgba8 fb;
    auto st = it->second->bvh.RenderScene(params, fb, nullptr);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (optional_ppm_path && *optional_ppm_path) {
        fb.WritePpm(optional_ppm_path);
    }
    if (out_frame_hash) {
        *out_frame_hash = fb.ComputeFnv1aHash();
    }
    return 0;
}

int32_t XboxBvhGetPostbuildInfo(uint64_t handle,
                                uint32_t postbuild_info_type,
                                uint64_t* out_info) {
    (void)postbuild_info_type;
    if (!out_info) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    const scarlettx::BvhAccelerationStructure* bvh = nullptr;
    if (auto tit = g_tlas_map.find(handle); tit != g_tlas_map.end()) {
        bvh = &tit->second->bvh;
    } else if (auto bit = g_blas_map.find(handle); bit != g_blas_map.end()) {
        bvh = &bit->second.bvh;
    }
    if (!bvh) {
        return -2;
    }
    const uint64_t size = AlignUp(static_cast<uint64_t>(bvh->NodeCount()) * 128 +
                                  static_cast<uint64_t>(bvh->TriangleCount()) * 64 + 256, 256);
    *out_info = size;
    return 0;
}

int32_t XboxBvhCopy(uint64_t source_handle, uint64_t* out_dest_handle) {
    if (!out_dest_handle) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    if (auto tit = g_tlas_map.find(source_handle); tit != g_tlas_map.end()) {
        auto copy = std::make_unique<TlasEntry>();
        copy->bvh = tit->second->bvh;
        const uint64_t handle = g_next_bvh_handle++;
        g_tlas_map[handle] = std::move(copy);
        *out_dest_handle = handle;
        return 0;
    }
    if (auto bit = g_blas_map.find(source_handle); bit != g_blas_map.end()) {
        BlasEntry copy;
        copy.triangles = bit->second.triangles;
        copy.build_flags = bit->second.build_flags;
        copy.bvh = bit->second.bvh;
        const uint64_t handle = g_next_bvh_handle++;
        g_blas_map[handle] = std::move(copy);
        *out_dest_handle = handle;
        return 0;
    }
    return -2;
}

int32_t XboxBvhDestroy(uint64_t handle) {
    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    if (g_blas_map.erase(handle) > 0 || g_tlas_map.erase(handle) > 0) {
        return 0;
    }
    return -2;
}

}  // extern "C"
