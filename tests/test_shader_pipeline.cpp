#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/shader_ir.h"
#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_shader_pipeline] FAILED: " #cond                \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void AppendLe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

// Builds a valid synthetic DXBC container with custom chunks (for testing Xbox stripping & DXIL inspection).
std::vector<uint8_t> BuildSyntheticDxbc(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& chunks) {
    const uint32_t n = static_cast<uint32_t>(chunks.size());
    std::vector<uint32_t> offsets(n, 0);
    uint32_t cur = 32u + n * 4u;
    for (uint32_t i = 0; i < n; ++i) {
        offsets[i] = cur;
        cur += 8u + static_cast<uint32_t>(chunks[i].second.size());
    }

    std::vector<uint8_t> out;
    out.reserve(cur);
    out.insert(out.end(), {'D', 'X', 'B', 'C'});
    out.insert(out.end(), 16, 0u);  // MD5 placeholder
    AppendLe32(out, 1u);            // version
    AppendLe32(out, cur);           // total size
    AppendLe32(out, n);             // chunk count
    for (uint32_t off : offsets) AppendLe32(out, off);
    for (const auto& c : chunks) {
        out.insert(out.end(), c.first.begin(), c.first.begin() + 4);
        AppendLe32(out, static_cast<uint32_t>(c.second.size()));
        out.insert(out.end(), c.second.begin(), c.second.end());
    }
    return out;
}

}  // namespace

int main() {
    using namespace scarlettx;

    // 1. Synthetic DXIL raytracing library with Xbox private chunks + embedded root signature
    {
        std::vector<uint8_t> dxil_payload(24, 0);
        // DXIL header: program version token at offset 0: (type=7 Library << 16) | (major=6 << 4) | minor=5
        const uint32_t ver_token = (7u << 16) | (6u << 4) | 5u;
        std::memcpy(dxil_payload.data(), &ver_token, 4);

        auto xbox_dxil = BuildSyntheticDxbc({
            {"DXIL", dxil_payload},
            {"RDAT", {1, 2, 3, 4}},
            {"RTS0", {5, 6, 7, 8}},
            {"XBOX", {0xAA, 0xBB, 0xCC, 0xDD}},
            {"XS2M", {0x11, 0x22, 0x33, 0x44}},
        });

        ShaderModuleInfo info{};
        SX_CHECK(InspectShaderContainer(xbox_dxil, info).ok());
        SX_CHECK(info.kind == ShaderContainerKind::Dxil);
        SX_CHECK(info.stage == ShaderStage::Library);
        SX_CHECK(info.shader_model_major == 6);
        SX_CHECK(info.shader_model_minor == 5);
        SX_CHECK(info.has_raytracing_exports);
        SX_CHECK(info.has_embedded_root_signature);
        SX_CHECK(info.has_xbox_private_chunks);
        SX_CHECK(info.chunk_count == 5);

        ShaderTranslationResult translated{};
        SX_CHECK(TranslateXboxShaderToPc(xbox_dxil, translated).ok());
        SX_CHECK(translated.stripped_xbox_chunks == 2);

        ShaderModuleInfo pc_info{};
        SX_CHECK(InspectShaderContainer(translated.pc_bytecode, pc_info).ok());
        SX_CHECK(pc_info.kind == ShaderContainerKind::Dxil);
        SX_CHECK(!pc_info.has_xbox_private_chunks);
        SX_CHECK(pc_info.chunk_count == 3);
        SX_CHECK(pc_info.has_raytracing_exports);
        SX_CHECK(pc_info.has_embedded_root_signature);
    }

    // 2. Scarlett precompiled RDNA2 ISA container detection
    {
        std::vector<uint8_t> rdna2 = {'X', 'S', '2', '\0', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        ShaderModuleInfo info{};
        SX_CHECK(InspectShaderContainer(rdna2, info).ok());
        SX_CHECK(info.kind == ShaderContainerKind::ScarlettRdna2);
        ShaderTranslationResult tr{};
        Status st = TranslateXboxShaderToPc(rdna2, tr);
        SX_CHECK(!st.ok());
        SX_CHECK(st.code == StatusCode::UnsupportedFormat);
    }

    // 3. ShaderCache hit/miss tracking
    {
        ShaderCache cache;
        std::vector<uint8_t> out;
        SX_CHECK(!cache.Lookup(0x1234ull, out));
        SX_CHECK(cache.misses() == 1);
        cache.Store(0x1234ull, {1, 2, 3, 4});
        SX_CHECK(cache.Lookup(0x1234ull, out));
        SX_CHECK(cache.hits() == 1);
        SX_CHECK(out.size() == 4 && out[3] == 4);
    }

    // 4. Live D3D12 WARP shader compile + root signature + descriptor heap + graphics/compute/DXR PSO + cache
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.PreferWarp = 1;
    uint64_t dev = 0;
    SX_CHECK(D3D12XboxCreateSyntheticDevice(&dev_params, &dev) == 0);
    SX_CHECK(dev != 0);

    // Descriptor heaps
    D3D12XBOX_DESCRIPTOR_HEAP_DESC cbv_heap_desc{};
    cbv_heap_desc.Type = D3D12XBOX_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_heap_desc.NumDescriptors = 16;
    cbv_heap_desc.ShaderVisible = 1;
    uint64_t cbv_heap = 0;
    SX_CHECK(D3D12XboxCreateDescriptorHeap(dev, &cbv_heap_desc, &cbv_heap) == 0);
    SX_CHECK(cbv_heap != 0);
    SX_CHECK(D3D12XboxGetDescriptorHandleIncrementSize(
                 dev, D3D12XBOX_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) > 0);

    // Root signatures (graphics: empty; compute: 2 root constants + 1 UAV)
    D3D12XBOX_ROOT_SIGNATURE_DESC gfx_rs_desc{};
    uint64_t gfx_rs = 0;
    SX_CHECK(D3D12XboxCreateRootSignature(dev, &gfx_rs_desc, &gfx_rs) == 0);

    D3D12XBOX_ROOT_SIGNATURE_DESC cs_rs_desc{};
    cs_rs_desc.NumRootConstants32Bit = 2;
    cs_rs_desc.NumUavs = 1;
    uint64_t cs_rs = 0;
    SX_CHECK(D3D12XboxCreateRootSignature(dev, &cs_rs_desc, &cs_rs) == 0);

    // Compile real HLSL shaders to DXBC
    const std::string vs_src = R"(
        struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR0; };
        VSOut main(uint vid : SV_VertexID) {
            float2 p[3] = { float2(0.0, 0.7), float2(0.7, -0.7), float2(-0.7, -0.7) };
            float4 c[3] = { float4(0.8, 0.4, 1.0, 1.0), float4(0.4, 0.2, 0.9, 1.0), float4(0.2, 0.8, 0.9, 1.0) };
            VSOut o;
            o.pos = float4(p[vid], 0.0, 1.0);
            o.col = c[vid];
            return o;
        }
    )";
    const std::string ps_src = R"(
        struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR0; };
        float4 main(PSIn i) : SV_TARGET { return i.col; }
    )";
    const std::string cs_src = R"(
        cbuffer Params : register(b0) { uint multiplier; uint bias; };
        RWByteAddressBuffer OutBuf : register(u0);
        [numthreads(16, 1, 1)]
        void main(uint3 tid : SV_DispatchThreadID) {
            uint val = tid.x * multiplier + bias;
            OutBuf.Store(tid.x * 4, val);
        }
    )";

    std::vector<uint8_t> vs_dxbc, ps_dxbc, cs_dxbc;
    SX_CHECK(CompileHlslToDxbc(vs_src, "main", "vs_5_0", vs_dxbc).ok());
    SX_CHECK(CompileHlslToDxbc(ps_src, "main", "ps_5_0", ps_dxbc).ok());
    SX_CHECK(CompileHlslToDxbc(cs_src, "main", "cs_5_0", cs_dxbc).ok());

    // Verify retail DXBC modified-MD5 checksum matches Microsoft d3dcompiler byte-for-byte
    for (const auto* orig_blob : {&vs_dxbc, &ps_dxbc, &cs_dxbc}) {
        std::vector<uint8_t> copy_blob = *orig_blob;
        std::memset(copy_blob.data() + 4, 0, 16);
        SX_CHECK(UpdateDxbcContainerChecksum(copy_blob.data(), copy_blob.size()));
        SX_CHECK(std::memcmp(copy_blob.data() + 4, orig_blob->data() + 4, 16) == 0);
    }

    // Inspect compiled shaders through the d3d12_xs C ABI
    D3D12XBOX_SHADER_BYTECODE cs_bc{cs_dxbc.data(), cs_dxbc.size()};
    D3D12XBOX_SHADER_INSPECTION cs_insp{};
    SX_CHECK(D3D12XboxInspectShader(&cs_bc, &cs_insp) == 0);
    SX_CHECK(cs_insp.ContainerKind == static_cast<uint32_t>(ShaderContainerKind::Dxbc));
    SX_CHECK(cs_insp.Stage == static_cast<uint32_t>(ShaderStage::Compute));
    SX_CHECK(cs_insp.ShaderModelMajor == 5);

    // Create Graphics PSO twice to exercise PSO cache hit
    D3D12XBOX_GRAPHICS_PSO_DESC gfx_pso_desc{};
    gfx_pso_desc.RootSignatureHandle = gfx_rs;
    gfx_pso_desc.VS = {vs_dxbc.data(), vs_dxbc.size()};
    gfx_pso_desc.PS = {ps_dxbc.data(), ps_dxbc.size()};
    gfx_pso_desc.RtvFormat = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    uint64_t gfx_pso1 = 0, gfx_pso2 = 0;
    SX_CHECK(D3D12XboxCreateGraphicsPipelineState(dev, &gfx_pso_desc, &gfx_pso1) == 0);
    SX_CHECK(D3D12XboxCreateGraphicsPipelineState(dev, &gfx_pso_desc, &gfx_pso2) == 0);
    SX_CHECK(gfx_pso1 != 0 && gfx_pso2 != 0 && gfx_pso1 != gfx_pso2);

    // Render using the custom Graphics PSO and verify deterministic pixel hash
    const std::string ppm_path = std::string(SCARLETTX_BINARY_DIR) + "/shader_pso_frame.ppm";
    uint64_t hash1 = 0, hash2 = 0;
    SX_CHECK(D3D12XboxRenderWithPipelineState(dev, gfx_pso1, 128, 128, 3, ppm_path.c_str(), &hash1) == 0);
    SX_CHECK(D3D12XboxRenderWithPipelineState(dev, gfx_pso2, 128, 128, 3, nullptr, &hash2) == 0);
    SX_CHECK(hash1 != 0);
    SX_CHECK(hash1 == hash2);
    SX_CHECK(std::filesystem::exists(ppm_path));

    // Create Compute PSO & dispatch on WARP, then verify UAV readback values!
    D3D12XBOX_COMPUTE_PSO_DESC cs_pso_desc{};
    cs_pso_desc.RootSignatureHandle = cs_rs;
    cs_pso_desc.CS = cs_bc;
    uint64_t cs_pso = 0;
    SX_CHECK(D3D12XboxCreateComputePipelineState(dev, &cs_pso_desc, &cs_pso) == 0);

    const uint32_t root_consts[2] = {7u, 100u};
    uint32_t uav_out[16] = {};
    SX_CHECK(D3D12XboxDispatchCompute(
                 dev, cs_pso, 1, 1, 1, root_consts, 2, uav_out, sizeof(uav_out)) == 0);
    for (uint32_t i = 0; i < 16; ++i) {
        SX_CHECK(uav_out[i] == i * 7u + 100u);
    }

    // Raytracing PSO from Xbox DXIL library (auto-strips Xbox-private chunks)
    std::vector<uint8_t> rt_dxil_payload(24, 0);
    const uint32_t rt_ver_token = (7u << 16) | (6u << 4) | 5u;  // lib_6_5
    std::memcpy(rt_dxil_payload.data(), &rt_ver_token, 4);
    auto rt_xbox_dxil = BuildSyntheticDxbc({
        {"DXIL", rt_dxil_payload},
        {"RDAT", {1, 2, 3, 4}},
        {"XBOX", {0xAA, 0xBB, 0xCC, 0xDD}},
    });
    D3D12XBOX_RAYTRACING_PSO_DESC rt_pso_desc{};
    rt_pso_desc.GlobalRootSignatureHandle = gfx_rs;
    rt_pso_desc.DxilLibrary.Bytecode = rt_xbox_dxil.data();
    rt_pso_desc.DxilLibrary.BytecodeLength = rt_xbox_dxil.size();
    rt_pso_desc.MaxPayloadSizeInBytes = 32;
    rt_pso_desc.MaxAttributeSizeInBytes = 8;
    rt_pso_desc.MaxTraceRecursionDepth = 2;
    uint64_t rt_pso = 0;
    SX_CHECK(D3D12XboxCreateRaytracingPipelineState(dev, &rt_pso_desc, &rt_pso) == 0);
    SX_CHECK(rt_pso != 0);

    // Check pipeline cache stats (1 graphics hit + 3 misses: 1 gfx + 1 cs + 1 rt)
    D3D12XBOX_PIPELINE_CACHE_STATS stats{};
    SX_CHECK(D3D12XboxGetPipelineCacheStats(dev, &stats) == 0);
    SX_CHECK(stats.CacheHits == 1);
    SX_CHECK(stats.CacheMisses == 3);
    SX_CHECK(stats.LiveRootSignatures == 2);
    SX_CHECK(stats.LiveDescriptorHeaps == 1);
    SX_CHECK(stats.LiveGraphicsPsos == 2);
    SX_CHECK(stats.LiveComputePsos == 1);
    SX_CHECK(stats.LiveRaytracingPsos == 1);

    SX_CHECK(D3D12XboxDestroyPipelineState(dev, gfx_pso1) == 0);
    SX_CHECK(D3D12XboxDestroyPipelineState(dev, gfx_pso2) == 0);
    SX_CHECK(D3D12XboxDestroyPipelineState(dev, cs_pso) == 0);
    SX_CHECK(D3D12XboxDestroyPipelineState(dev, rt_pso) == 0);
    SX_CHECK(D3D12XboxDestroyDescriptorHeap(dev, cbv_heap) == 0);
    SX_CHECK(D3D12XboxDestroyRootSignature(dev, gfx_rs) == 0);
    SX_CHECK(D3D12XboxDestroyRootSignature(dev, cs_rs) == 0);
    SX_CHECK(D3D12XboxDestroyDevice(dev) == 0);

    std::cout << "[PASS] test_shader_pipeline (DXIL/Xbox strip + WARP graphics PSO + compute UAV readback)\n";
    return 0;
}
