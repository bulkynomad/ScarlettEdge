#include <cstdlib>
#include <iostream>
#include <vector>

#include "scarlettx/gpu/d3d12_xs_abi.h"

namespace {

std::vector<SCARLETT_BVH_TRIANGLE> MakeSyntheticRtScene() {
    std::vector<SCARLETT_BVH_TRIANGLE> tris;
    auto push_tri = [&](float x0, float y0, float z0,
                        float x1, float y1, float z1,
                        float x2, float y2, float z2,
                        float r, float g, float b, float refl) {
        SCARLETT_BVH_TRIANGLE t{};
        t.v0[0] = x0; t.v0[1] = y0; t.v0[2] = z0;
        t.v1[0] = x1; t.v1[1] = y1; t.v1[2] = z1;
        t.v2[0] = x2; t.v2[1] = y2; t.v2[2] = z2;
        t.color[0] = r; t.color[1] = g; t.color[2] = b;
        t.reflectivity = refl;
        tris.push_back(t);
    };

    // Reflective floor
    push_tri(-3.0f, 0.0f, -3.0f,  3.0f, 0.0f, -3.0f,  3.0f, 0.0f, 3.0f, 0.72f, 0.75f, 0.80f, 0.45f);
    push_tri(-3.0f, 0.0f, -3.0f,  3.0f, 0.0f,  3.0f, -3.0f, 0.0f, 3.0f, 0.72f, 0.75f, 0.80f, 0.45f);
    // Back wall
    push_tri(-3.0f, 0.0f,  2.5f,  3.0f, 0.0f,  2.5f,  3.0f, 3.0f, 2.5f, 0.25f, 0.30f, 0.38f, 0.10f);
    push_tri(-3.0f, 0.0f,  2.5f,  3.0f, 3.0f,  2.5f, -3.0f, 3.0f, 2.5f, 0.25f, 0.30f, 0.38f, 0.10f);
    // Red left wall & Xbox-green right wall
    push_tri(-2.5f, 0.0f, -2.5f, -2.5f, 0.0f,  2.5f, -2.5f, 3.0f, 2.5f, 0.85f, 0.18f, 0.18f, 0.05f);
    push_tri(-2.5f, 0.0f, -2.5f, -2.5f, 3.0f,  2.5f, -2.5f, 3.0f,-2.5f, 0.85f, 0.18f, 0.18f, 0.05f);
    push_tri( 2.5f, 0.0f,  2.5f,  2.5f, 0.0f, -2.5f,  2.5f, 3.0f,-2.5f, 0.10f, 0.78f, 0.22f, 0.05f);
    push_tri( 2.5f, 0.0f,  2.5f,  2.5f, 3.0f, -2.5f,  2.5f, 3.0f, 2.5f, 0.10f, 0.78f, 0.22f, 0.05f);
    // Glossy pyramid
    push_tri(-0.65f, 0.35f, -0.35f,  0.65f, 0.35f, -0.35f, 0.0f, 1.55f, 0.2f, 0.95f, 0.78f, 0.18f, 0.30f);
    push_tri( 0.65f, 0.35f, -0.35f,  0.65f, 0.35f,  0.75f, 0.0f, 1.55f, 0.2f, 0.18f, 0.72f, 0.95f, 0.30f);
    push_tri( 0.65f, 0.35f,  0.75f, -0.65f, 0.35f,  0.75f, 0.0f, 1.55f, 0.2f, 0.85f, 0.25f, 0.85f, 0.30f);
    push_tri(-0.65f, 0.35f,  0.75f, -0.65f, 0.35f, -0.35f, 0.0f, 1.55f, 0.2f, 0.95f, 0.42f, 0.18f, 0.30f);
    return tris;
}

}  // namespace

int main() {
    if (XGameRuntimeInitialize() != 0) {
        return 1;
    }

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    uint64_t d3d_dev = 0;
    if (D3D12XboxCreateSyntheticDevice(&params, &d3d_dev) != 0) {
        return 2;
    }

    auto tris = MakeSyntheticRtScene();
    SCARLETT_BVH_BUILD_DESC build_desc{};
    build_desc.Triangles = tris.data();
    build_desc.TriangleCount = static_cast<uint32_t>(tris.size());

    uint64_t blas = 0;
    if (XboxBvhBuildBlas(&build_desc, &blas) != 0) {
        std::cerr << "[rt_showcase] XboxBvhBuildBlas failed\n";
        return 3;
    }

    uint64_t tlas = 0;
    if (XboxBvhBuildTlas(&blas, 1, &tlas) != 0) {
        std::cerr << "[rt_showcase] XboxBvhBuildTlas failed\n";
        return 4;
    }

    const char* rt_dump = std::getenv("SCARLETTX_RT_DUMP");
    uint32_t frames = 2;
    if (const char* env_frames = std::getenv("SCARLETTX_MAX_FRAMES")) {
        frames = static_cast<uint32_t>(std::strtoul(env_frames, nullptr, 10));
        if (frames == 0) frames = 1;
    }

    SCARLETT_RT_DISPATCH_DESC rt_desc{};
    rt_desc.Width = 160;
    rt_desc.Height = 120;
    rt_desc.CameraPos[0] = 0.0f; rt_desc.CameraPos[1] = 1.1f; rt_desc.CameraPos[2] = -3.4f;
    rt_desc.CameraLookAt[0] = 0.0f; rt_desc.CameraLookAt[1] = 0.65f; rt_desc.CameraLookAt[2] = 0.2f;
    rt_desc.LightPos[0] = 1.5f; rt_desc.LightPos[1] = 2.8f; rt_desc.LightPos[2] = -1.2f;
    rt_desc.MaxBounces = 2;

    uint64_t expected_hash = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        uint64_t token = 0;
        D3D12XboxWaitFrameEventX(d3d_dev, &token);

        uint64_t frame_hash = 0;
        const char* dump = (f == 0) ? rt_dump : nullptr;
        if (XboxBvhDispatchRays(tlas, &rt_desc, dump, &frame_hash) != 0) {
            std::cerr << "[rt_showcase] XboxBvhDispatchRays failed\n";
            return 5;
        }
        if (f == 0) {
            expected_hash = frame_hash;
        } else if (frame_hash != expected_hash) {
            std::cerr << "[rt_showcase] Non-deterministic RT frame hash!\n";
            return 6;
        }

        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = token;
        plane.Width = rt_desc.Width;
        plane.Height = rt_desc.Height;
        D3D12XboxPresentX(d3d_dev, &plane);

        std::cout << "[rt_showcase] RT Frame " << f << " token=" << token
                  << " hash=0x" << std::hex << frame_hash << std::dec << "\n";
    }

    XboxBvhDestroy(tlas);
    XboxBvhDestroy(blas);
    D3D12XboxDestroyDevice(d3d_dev);
    XGameRuntimeUninitialize();
    return 0;
}
