#include "scarlettx/gpu/rt_bvh.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace scarlettx {

namespace {

inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 MulVec(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 Normalize(Vec3 v) {
    const float len = std::sqrt(Dot(v, v));
    return len > 1e-8f ? Mul(v, 1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
}
inline Vec3 Reflect(Vec3 dir, Vec3 n) {
    return Sub(dir, Mul(n, 2.0f * Dot(dir, n)));
}

inline void ExpandAabb(RtAabb& box, Vec3 p) {
    box.min.x = std::min(box.min.x, p.x);
    box.min.y = std::min(box.min.y, p.y);
    box.min.z = std::min(box.min.z, p.z);
    box.max.x = std::max(box.max.x, p.x);
    box.max.y = std::max(box.max.y, p.y);
    box.max.z = std::max(box.max.z, p.z);
}

inline Vec3 TriCentroid(const RtTriangle& t) {
    return Mul(Add(Add(t.v0, t.v1), t.v2), 1.0f / 3.0f);
}

inline float AxisVal(Vec3 v, int axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

bool IntersectAabb(const RtAabb& box, const RtRay& ray, float t_best) {
    float tmin = ray.t_min;
    float tmax = t_best;
    for (int a = 0; a < 3; ++a) {
        const float orig = AxisVal(ray.origin, a);
        const float dir = AxisVal(ray.direction, a);
        const float bmin = AxisVal(box.min, a);
        const float bmax = AxisVal(box.max, a);
        if (std::fabs(dir) < 1e-8f) {
            if (orig < bmin || orig > bmax) return false;
            continue;
        }
        const float inv = 1.0f / dir;
        float t0 = (bmin - orig) * inv;
        float t1 = (bmax - orig) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmax < tmin) return false;
    }
    return true;
}

bool IntersectTriangle(const RtTriangle& tri, const RtRay& ray, float t_best, float& out_t, Vec3& out_normal) {
    const Vec3 e1 = Sub(tri.v1, tri.v0);
    const Vec3 e2 = Sub(tri.v2, tri.v0);
    const Vec3 pvec = Cross(ray.direction, e2);
    const float det = Dot(e1, pvec);
    if (std::fabs(det) < 1e-7f) {
        return false;
    }
    const float inv_det = 1.0f / det;
    const Vec3 tvec = Sub(ray.origin, tri.v0);
    const float u = Dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 qvec = Cross(tvec, e1);
    const float v = Dot(ray.direction, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = Dot(e2, qvec) * inv_det;
    if (t < ray.t_min || t >= t_best) {
        return false;
    }
    out_t = t;
    Vec3 n = Normalize(Cross(e1, e2));
    if (Dot(n, ray.direction) > 0.0f) {
        n = Mul(n, -1.0f);
    }
    out_normal = n;
    return true;
}

uint8_t ToByte(float v) {
    const float clamped = std::max(0.0f, std::min(1.0f, v));
    return static_cast<uint8_t>(std::round(clamped * 255.0f));
}

}  // namespace

Status BvhAccelerationStructure::Build(const std::vector<RtTriangle>& triangles) {
    if (triangles.empty()) {
        return Status::Error(StatusCode::RaytracingError, "Cannot build BVH from 0 triangles");
    }
    triangles_ = triangles;
    tri_indices_.resize(triangles_.size());
    std::iota(tri_indices_.begin(), tri_indices_.end(), 0u);
    nodes_.clear();
    nodes_.reserve(triangles_.size() * 2);
    BuildRecursive(0, static_cast<uint32_t>(triangles_.size()));
    return Status::Ok();
}

uint32_t BvhAccelerationStructure::BuildRecursive(uint32_t first, uint32_t count) {
    const uint32_t node_idx = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(BvhNode{});

    RtAabb bounds;
    for (uint32_t i = 0; i < count; ++i) {
        const RtTriangle& t = triangles_[tri_indices_[first + i]];
        ExpandAabb(bounds, t.v0);
        ExpandAabb(bounds, t.v1);
        ExpandAabb(bounds, t.v2);
    }
    nodes_[node_idx].bounds = bounds;

    if (count <= 2) {
        nodes_[node_idx].left_first = first;
        nodes_[node_idx].tri_count = count;
        return node_idx;
    }

    const Vec3 extent = Sub(bounds.max, bounds.min);
    int axis = 0;
    if (extent.y > extent.x) axis = 1;
    if (extent.z > AxisVal(extent, axis)) axis = 2;

    const uint32_t mid = first + count / 2;
    std::nth_element(tri_indices_.begin() + first,
                     tri_indices_.begin() + mid,
                     tri_indices_.begin() + first + count,
                     [&](uint32_t a, uint32_t b) {
                         return AxisVal(TriCentroid(triangles_[a]), axis) <
                                AxisVal(TriCentroid(triangles_[b]), axis);
                     });

    const uint32_t left_child = BuildRecursive(first, mid - first);
    const uint32_t right_child = BuildRecursive(mid, first + count - mid);
    nodes_[node_idx].left_first = left_child;
    nodes_[node_idx].right_child = right_child;
    nodes_[node_idx].tri_count = 0;
    return node_idx;
}

bool BvhAccelerationStructure::TraceRay(const RtRay& ray, RtHit& out_hit, RtStats* stats) const {
    out_hit = RtHit{};
    out_hit.t = ray.t_max;
    if (nodes_.empty()) {
        return false;
    }

    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const uint32_t idx = stack[--sp];
        const BvhNode& node = nodes_[idx];
        if (stats) ++stats->aabb_tests;
        if (!IntersectAabb(node.bounds, ray, out_hit.t)) {
            continue;
        }
        if (node.tri_count > 0) {
            for (uint32_t i = 0; i < node.tri_count; ++i) {
                const uint32_t tri_idx = tri_indices_[node.left_first + i];
                const RtTriangle& tri = triangles_[tri_idx];
                if (stats) ++stats->triangle_tests;
                float t = 0.0f;
                Vec3 n{};
                if (IntersectTriangle(tri, ray, out_hit.t, t, n)) {
                    out_hit.hit = true;
                    out_hit.t = t;
                    out_hit.triangle_index = tri_idx;
                    out_hit.normal = n;
                    out_hit.color = tri.color;
                    out_hit.reflectivity = tri.reflectivity;
                }
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = node.left_first;
            stack[sp++] = node.right_child;
        }
    }

    return out_hit.hit;
}

Vec3 BvhAccelerationStructure::TraceColor(const RtRay& ray,
                                          const Vec3& light_pos,
                                          uint32_t depth,
                                          RtStats* stats) const {
    RtHit hit{};
    if (!TraceRay(ray, hit, stats)) {
        // Sky gradient miss shader
        const float t = 0.5f * (Normalize(ray.direction).y + 1.0f);
        return Add(Mul({0.06f, 0.09f, 0.16f}, 1.0f - t), Mul({0.18f, 0.34f, 0.58f}, t));
    }

    const Vec3 hit_pos = Add(ray.origin, Mul(ray.direction, hit.t));
    const Vec3 to_light = Sub(light_pos, hit_pos);
    const float light_dist = std::sqrt(Dot(to_light, to_light));
    const Vec3 light_dir = Mul(to_light, 1.0f / std::max(light_dist, 1e-6f));

    // Shadow ray
    RtRay shadow_ray{};
    shadow_ray.origin = Add(hit_pos, Mul(hit.normal, 0.002f));
    shadow_ray.direction = light_dir;
    shadow_ray.t_min = 0.001f;
    shadow_ray.t_max = light_dist - 0.002f;
    if (stats) ++stats->shadow_rays;
    RtHit shadow_hit{};
    const bool in_shadow = TraceRay(shadow_ray, shadow_hit, stats);

    const float ndotl = std::max(0.0f, Dot(hit.normal, light_dir));
    const float ambient = 0.22f;
    const float diffuse = in_shadow ? 0.0f : (0.78f * ndotl);
    Vec3 local_color = Mul(hit.color, ambient + diffuse);

    if (depth > 0 && hit.reflectivity > 0.01f) {
        RtRay refl_ray{};
        refl_ray.origin = Add(hit_pos, Mul(hit.normal, 0.002f));
        refl_ray.direction = Normalize(Reflect(ray.direction, hit.normal));
        if (stats) ++stats->reflection_rays;
        const Vec3 refl_col = TraceColor(refl_ray, light_pos, depth - 1, stats);
        local_color = Add(Mul(local_color, 1.0f - hit.reflectivity), Mul(refl_col, hit.reflectivity));
    }

    return local_color;
}

Status BvhAccelerationStructure::RenderScene(const RtDispatchParams& params,
                                             FrameBufferRgba8& out_frame,
                                             RtStats* out_stats) const {
    if (nodes_.empty()) {
        return Status::Error(StatusCode::RaytracingError, "BVH is empty");
    }
    if (params.width == 0 || params.height == 0) {
        return Status::Error(StatusCode::InvalidArgument, "RT dispatch dimensions must be > 0");
    }

    RtStats local_stats{};
    local_stats.bvh_nodes = NodeCount();

    const Vec3 forward = Normalize(Sub(params.camera_look_at, params.camera_pos));
    const Vec3 right = Normalize(Cross({0.0f, 1.0f, 0.0f}, forward));
    const Vec3 up = Cross(forward, right);
    const float aspect = static_cast<float>(params.width) / static_cast<float>(params.height);
    const float fov_scale = 0.65f;

    out_frame.width = params.width;
    out_frame.height = params.height;
    out_frame.pixels.resize(static_cast<size_t>(params.width) * params.height * 4);

    for (uint32_t y = 0; y < params.height; ++y) {
        const float ndc_y = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(params.height);
        for (uint32_t x = 0; x < params.width; ++x) {
            const float ndc_x = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(params.width) - 1.0f;
            RtRay ray{};
            ray.origin = params.camera_pos;
            ray.direction = Normalize(Add(Add(forward, Mul(right, ndc_x * aspect * fov_scale)),
                                          Mul(up, ndc_y * fov_scale)));
            ++local_stats.primary_rays;
            const Vec3 col = TraceColor(ray, params.light_pos, params.max_bounces, &local_stats);
            const size_t idx = (static_cast<size_t>(y) * params.width + x) * 4;
            out_frame.pixels[idx + 0] = ToByte(col.x);
            out_frame.pixels[idx + 1] = ToByte(col.y);
            out_frame.pixels[idx + 2] = ToByte(col.z);
            out_frame.pixels[idx + 3] = 255;
        }
    }

    if (out_stats) {
        *out_stats = local_stats;
    }
    return Status::Ok();
}

std::vector<RtTriangle> BuildDefaultRtTestScene() {
    std::vector<RtTriangle> tris;
    // Reflective floor (y = 0)
    tris.push_back({{-3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, 3.0f}, {0.72f, 0.75f, 0.80f}, 0.45f});
    tris.push_back({{-3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, 3.0f}, {-3.0f, 0.0f, 3.0f}, {0.72f, 0.75f, 0.80f}, 0.45f});

    // Back wall (z = 2.5)
    tris.push_back({{-3.0f, 0.0f, 2.5f}, {3.0f, 0.0f, 2.5f}, {3.0f, 3.0f, 2.5f}, {0.25f, 0.30f, 0.38f}, 0.10f});
    tris.push_back({{-3.0f, 0.0f, 2.5f}, {3.0f, 3.0f, 2.5f}, {-3.0f, 3.0f, 2.5f}, {0.25f, 0.30f, 0.38f}, 0.10f});

    // Red left wall
    tris.push_back({{-2.5f, 0.0f, -2.5f}, {-2.5f, 0.0f, 2.5f}, {-2.5f, 3.0f, 2.5f}, {0.85f, 0.18f, 0.18f}, 0.05f});
    tris.push_back({{-2.5f, 0.0f, -2.5f}, {-2.5f, 3.0f, 2.5f}, {-2.5f, 3.0f, -2.5f}, {0.85f, 0.18f, 0.18f}, 0.05f});

    // Green right wall (Xbox green accent)
    tris.push_back({{2.5f, 0.0f, 2.5f}, {2.5f, 0.0f, -2.5f}, {2.5f, 3.0f, -2.5f}, {0.10f, 0.78f, 0.22f}, 0.05f});
    tris.push_back({{2.5f, 0.0f, 2.5f}, {2.5f, 3.0f, -2.5f}, {2.5f, 3.0f, 2.5f}, {0.10f, 0.78f, 0.22f}, 0.05f});

    // Floating glossy pyramid in center (casts shadow onto floor + reflects walls)
    const Vec3 apex{0.0f, 1.55f, 0.2f};
    const Vec3 b0{-0.65f, 0.35f, -0.35f};
    const Vec3 b1{0.65f, 0.35f, -0.35f};
    const Vec3 b2{0.65f, 0.35f, 0.75f};
    const Vec3 b3{-0.65f, 0.35f, 0.75f};
    tris.push_back({b0, b1, apex, {0.95f, 0.78f, 0.18f}, 0.30f});
    tris.push_back({b1, b2, apex, {0.18f, 0.72f, 0.95f}, 0.30f});
    tris.push_back({b2, b3, apex, {0.85f, 0.25f, 0.85f}, 0.30f});
    tris.push_back({b3, b0, apex, {0.95f, 0.42f, 0.18f}, 0.30f});
    return tris;
}

}  // namespace scarlettx
