#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/gpu/d3d12_backend.h"

namespace scarlettx {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct RtTriangle {
    Vec3 v0;
    Vec3 v1;
    Vec3 v2;
    Vec3 color;
    float reflectivity = 0.0f;
};

struct RtAabb {
    Vec3 min{1e30f, 1e30f, 1e30f};
    Vec3 max{-1e30f, -1e30f, -1e30f};
};

struct RtRay {
    Vec3 origin;
    Vec3 direction;
    float t_min = 0.001f;
    float t_max = 1e30f;
};

struct RtHit {
    bool hit = false;
    float t = 1e30f;
    float u = 0.0f;
    float v = 0.0f;
    uint32_t triangle_index = 0;
    Vec3 normal{};
    Vec3 color{};
    float reflectivity = 0.0f;
};

struct RtDispatchParams {
    uint32_t width = 160;
    uint32_t height = 120;
    Vec3 camera_pos{0.0f, 1.0f, -3.5f};
    Vec3 camera_look_at{0.0f, 0.5f, 0.0f};
    Vec3 light_pos{1.5f, 3.0f, -1.5f};
    uint32_t max_bounces = 2;
};

struct RtStats {
    uint64_t primary_rays = 0;
    uint64_t shadow_rays = 0;
    uint64_t reflection_rays = 0;
    uint64_t aabb_tests = 0;
    uint64_t triangle_tests = 0;
    uint32_t bvh_nodes = 0;
};

// Emulates Scarlett's XboxBvhBuilder / DXR BLAS + TLAS acceleration structure & ray traversal.
class BvhAccelerationStructure {
public:
    Status Build(const std::vector<RtTriangle>& triangles);
    bool TraceRay(const RtRay& ray, RtHit& out_hit, RtStats* stats = nullptr) const;
    Status RenderScene(const RtDispatchParams& params,
                       FrameBufferRgba8& out_frame,
                       RtStats* out_stats = nullptr) const;

    uint32_t NodeCount() const noexcept { return static_cast<uint32_t>(nodes_.size()); }
    uint32_t TriangleCount() const noexcept { return static_cast<uint32_t>(triangles_.size()); }
    RtAabb RootBounds() const noexcept { return nodes_.empty() ? RtAabb{} : nodes_[0].bounds; }

private:
    struct BvhNode {
        RtAabb bounds;
        uint32_t left_first = 0;   // left child index if tri_count == 0, else first triangle index
        uint32_t right_child = 0;  // right child index if tri_count == 0
        uint32_t tri_count = 0;
    };

    uint32_t BuildRecursive(uint32_t first, uint32_t count);
    Vec3 TraceColor(const RtRay& ray, const Vec3& light_pos, uint32_t depth, RtStats* stats) const;

    std::vector<RtTriangle> triangles_;
    std::vector<uint32_t> tri_indices_;
    std::vector<BvhNode> nodes_;
};

// Creates a deterministic 3D Cornell-style test scene (reflective floor + colored walls + floating prism).
std::vector<RtTriangle> BuildDefaultRtTestScene();

}  // namespace scarlettx
