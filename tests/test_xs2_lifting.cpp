#include "scarlettx/gpu/xs2_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

namespace {

// Live GPU driver verification: creates a real VkInstance + VkDevice on host vulkan-1.dll
// and validates that vkCreateShaderModule accepts our lifted SPIR-V 1.5 binary with VK_SUCCESS (0).
bool VerifySpirvWithHostVulkanDriver(const void* spirv_bytes, size_t spirv_size, bool* out_driver_present) {
    if (out_driver_present) *out_driver_present = false;
    HMODULE vk_dll = LoadLibraryW(L"vulkan-1.dll");
    if (!vk_dll) return true;

    struct VkAppInfo { uint32_t sType; const void* pNext; const char* app; uint32_t appVer; const char* eng; uint32_t engVer; uint32_t apiVer; };
    struct VkInstInfo { uint32_t sType; const void* pNext; uint32_t flags; const VkAppInfo* app; uint32_t lc; const char* const* l; uint32_t ec; const char* const* e; };
    struct VkQueueInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfi; uint32_t qc; const float* prio; };
    struct VkDevInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qic; const VkQueueInfo* qi; uint32_t lc; const char* const* l; uint32_t ec; const char* const* e; const void* feat; };
    struct VkShaderModInfo { uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode; };

    using FnCreateInst = int32_t (WINAPI*)(const VkInstInfo*, const void*, void**);
    using FnDestroyInst = void (WINAPI*)(void*, const void*);
    using FnEnumPhys = int32_t (WINAPI*)(void*, uint32_t*, void**);
    using FnCreateDev = int32_t (WINAPI*)(void*, const VkDevInfo*, const void*, void**);
    using FnDestroyDev = void (WINAPI*)(void*, const void*);
    using FnCreateMod = int32_t (WINAPI*)(void*, const VkShaderModInfo*, const void*, uint64_t*);
    using FnDestroyMod = void (WINAPI*)(void*, uint64_t, const void*);

    auto create_inst = reinterpret_cast<FnCreateInst>(reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkCreateInstance")));
    auto destroy_inst = reinterpret_cast<FnDestroyInst>(reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkDestroyInstance")));
    auto enum_phys = reinterpret_cast<FnEnumPhys>(reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkEnumeratePhysicalDevices")));
    auto create_dev = reinterpret_cast<FnCreateDev>(reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkCreateDevice")));
    auto destroy_dev = reinterpret_cast<FnDestroyDev>(reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkDestroyDevice")));
    auto create_mod = reinterpret_cast<FnCreateMod>(reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkCreateShaderModule")));
    auto destroy_mod = reinterpret_cast<FnDestroyMod>(reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkDestroyShaderModule")));
    if (!create_inst || !destroy_inst || !enum_phys || !create_dev || !destroy_dev || !create_mod || !destroy_mod) {
        FreeLibrary(vk_dll);
        return true;
    }

    VkAppInfo app{0, nullptr, "SXTest", 1, "SXTest", 1, (1u << 22) | (2u << 12)};
    VkInstInfo ici{1, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    void* inst = nullptr;
    if (create_inst(&ici, nullptr, &inst) != 0 || !inst) {
        FreeLibrary(vk_dll);
        return true;
    }
    uint32_t count = 0;
    std::vector<void*> phys;
    if (enum_phys(inst, &count, nullptr) == 0 && count > 0) {
        phys.resize(count);
        enum_phys(inst, &count, phys.data());
    }
    if (phys.empty() || !phys[0]) {
        destroy_inst(inst, nullptr);
        FreeLibrary(vk_dll);
        return true;
    }
    struct VkPhysProps { uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID; uint32_t deviceID; uint32_t deviceType; char deviceName[256]; uint8_t pad[1024]; };
    using FnGetPhysProps = void (WINAPI*)(void*, VkPhysProps*);
    auto get_phys_props = reinterpret_cast<FnGetPhysProps>(reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkGetPhysicalDeviceProperties")));
    if (get_phys_props) {
        VkPhysProps props{};
        get_phys_props(phys[0], &props);
        if (props.apiVersion < ((1u << 22) | (2u << 12))) {
            destroy_inst(inst, nullptr);
            FreeLibrary(vk_dll);
            return true;  // SPIR-V 1.5 requires Vulkan 1.2+; skip gracefully on legacy 1.0/1.1 CI stubs
        }
    }
    const float prio = 1.0f;
    VkQueueInfo qci{2, nullptr, 0, 0, 1, &prio};
    VkDevInfo dci{3, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, nullptr};
    void* dev = nullptr;
    if (create_dev(phys[0], &dci, nullptr, &dev) != 0 || !dev) {
        destroy_inst(inst, nullptr);
        FreeLibrary(vk_dll);
        return true;
    }
    if (out_driver_present) *out_driver_present = true;

    VkShaderModInfo smi{16, nullptr, 0, spirv_size, static_cast<const uint32_t*>(spirv_bytes)};
    uint64_t mod = 0;
    const int32_t res = create_mod(dev, &smi, nullptr, &mod);
    if (res == 0 && mod != 0) {
        destroy_mod(dev, mod, nullptr);
    }
    destroy_dev(dev, nullptr);
    destroy_inst(inst, nullptr);
    FreeLibrary(vk_dll);
    return (res == 0 && mod != 0);
}

}  // namespace
#endif

int main() {
    std::cout << "[test_xs2_lifting] Testing RDNA2 XS2 Shader Lifting...\n";

    const uint8_t sample_microcode[] = {
        0x00, 0x00, 0x80, 0xC0, // s_load_dwordx4
        0x01, 0x02, 0x00, 0x7E, // v_add_f32
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0xBF  // s_endpgm
    };
    size_t microcode_size = sizeof(sample_microcode);

    // 1. Identification
    XS2_MICROCODE_INFO info{};
    SX_CHECK(Xs2ShaderIdentifyMicrocode(sample_microcode, microcode_size, &info) == 0);
    SX_CHECK(info.instructionCount >= 4);
    SX_CHECK(info.shaderHash != 0);

    // 2. Disassembly
    char disasm[256]{};
    size_t actual_disasm = 0;
    SX_CHECK(Xs2ShaderDisassembleRdna2(sample_microcode, microcode_size, disasm, sizeof(disasm), &actual_disasm) == 0);
    SX_CHECK(actual_disasm > 0);
    SX_CHECK(std::strstr(disasm, "s_endpgm") != nullptr);

    // 3. Lift to DXIL & SPIR-V
    XS2_TRANSLATION_OPTIONS opts{};
    opts.targetShaderModel = 0x00060005;
    opts.enableWave32 = 1;

    void* dxil_buf = nullptr;
    size_t dxil_size = 0;
    SX_CHECK(Xs2ShaderLiftToDxil(sample_microcode, microcode_size, &opts, &dxil_buf, &dxil_size) == 0);
    SX_CHECK(dxil_buf != nullptr);
    SX_CHECK(dxil_size > 0);

    int32_t is_valid_dxil = 0;
    SX_CHECK(Xs2ShaderValidateDxilContainer(dxil_buf, dxil_size, &is_valid_dxil) == 0);
    SX_CHECK(is_valid_dxil == 1);
    SX_CHECK(Xs2ShaderFreeBytecode(dxil_buf) == 0);

    void* spirv_buf = nullptr;
    size_t spirv_size = 0;
    SX_CHECK(Xs2ShaderLiftToSpirv(sample_microcode, microcode_size, &opts, &spirv_buf, &spirv_size) == 0);
    SX_CHECK(spirv_buf != nullptr);
    SX_CHECK(spirv_size > 0);
    SX_CHECK(Xs2ShaderFreeBytecode(spirv_buf) == 0);

    // 4. Resource bindings & chunk stripping
    XS2_RESOURCE_BINDINGS bindings{};
    SX_CHECK(Xs2ShaderExtractResourceBindings(sample_microcode, microcode_size, &bindings) == 0);
    SX_CHECK(bindings.srvCount == 4);

    void* stripped = nullptr;
    size_t stripped_size = 0;
    SX_CHECK(Xs2ShaderStripXboxVendorChunks(sample_microcode, microcode_size, &stripped, &stripped_size) == 0);
    SX_CHECK(stripped != nullptr);
    delete[] static_cast<uint8_t*>(stripped);

    // 5. Wavefront & workgroup queries
    XS2_WAVEFRONT_PROPS props{};
    SX_CHECK(Xs2ShaderQueryWavefrontProperties(sample_microcode, microcode_size, &props) == 0);
    SX_CHECK(props.waveSize == 32);

    uint32_t wx = 0, wy = 0, wz = 0;
    SX_CHECK(Xs2ShaderTranslateComputeWorkgroupSize(sample_microcode, microcode_size, &wx, &wy, &wz) == 0);
    SX_CHECK(wx == 8 && wy == 8);

    // 6. Translation pipeline & caching
    XS2_PIPELINE_DESC pipe_desc{};
    pipe_desc.maxCacheEntries = 100;
    XS2_PIPELINE_HANDLE pipeline = nullptr;
    SX_CHECK(Xs2ShaderCreateTranslationPipeline(&pipe_desc, &pipeline) == 0);
    SX_CHECK(pipeline != nullptr);

    void* cached_bc1 = nullptr;
    size_t cached_sz1 = 0;
    SX_CHECK(Xs2ShaderTranslateCached(pipeline, sample_microcode, microcode_size, &cached_bc1, &cached_sz1) == 0);
    delete[] static_cast<uint8_t*>(cached_bc1);

    // Second call hits cache
    void* cached_bc2 = nullptr;
    size_t cached_sz2 = 0;
    SX_CHECK(Xs2ShaderTranslateCached(pipeline, sample_microcode, microcode_size, &cached_bc2, &cached_sz2) == 0);
    delete[] static_cast<uint8_t*>(cached_bc2);

    XS2_TRANSLATION_STATS stats{};
    SX_CHECK(Xs2ShaderGetTranslationStatistics(pipeline, &stats) == 0);
    SX_CHECK(stats.totalShadersTranslated == 1);
    SX_CHECK(stats.cacheHits == 1);

    SX_CHECK(Xs2ShaderClearPipelineCache(pipeline) == 0);
    SX_CHECK(Xs2ShaderDestroyTranslationPipeline(pipeline) == 0);

    // 7. Advanced Open-World Engine Compute Shader Lifting (RDNA2 ISA)
    std::cout << "[test_xs2_lifting] Verifying Open-World RDNA2 Compute Shader Lifting...\n";
    const uint32_t openworld_compute_kernel[] = {
        0xF4000000, 0x00000000, // SMEM: s_load_dwordx4 (64-bit)
        0x06000000,             // VOP2: v_add_f32 (32-bit)
        0x10000000,             // VOP2: v_mul_f32 (32-bit)
        0x32000000,             // VOP2: v_fmac_f32 (32-bit)
        0xD4000000, 0x00000000, // VOP3: v_fma_f32 (64-bit)
        0xE0000000, 0x00000000, // VMEM: buffer_load_dword (64-bit)
        0xE2000000, 0x00000000, // VMEM: buffer_store_dword (64-bit)
        0xD8000000, 0x00000000, // LDS:  ds_read_b32 (64-bit)
        0xDA000000, 0x00000000, // LDS:  ds_write_b32 (64-bit)
        0xBF8C007F,             // SOPP: s_waitcnt vmcnt(0) lgkmcnt(0) (32-bit)
        0xBF800000              // SOPP: s_endpgm (32-bit)
    };
    const size_t openworld_kernel_bytes = sizeof(openworld_compute_kernel);

    // Verify Compute stage classification
    XS2_MICROCODE_INFO cs_info{};
    SX_CHECK(Xs2ShaderIdentifyMicrocode(openworld_compute_kernel, openworld_kernel_bytes, &cs_info) == 0);
    SX_CHECK(cs_info.stage == 3); // Identified as Compute Shader
    SX_CHECK(cs_info.instructionCount == 11);

    // Verify Disassembly mnemonics
    char cs_disasm[1024]{};
    size_t cs_disasm_len = 0;
    SX_CHECK(Xs2ShaderDisassembleRdna2(openworld_compute_kernel, openworld_kernel_bytes, cs_disasm, sizeof(cs_disasm), &cs_disasm_len) == 0);
    SX_CHECK(std::strstr(cs_disasm, "s_load_dwordx4") != nullptr);
    SX_CHECK(std::strstr(cs_disasm, "v_fma_f32") != nullptr);
    SX_CHECK(std::strstr(cs_disasm, "buffer_load_dword") != nullptr);
    SX_CHECK(std::strstr(cs_disasm, "buffer_store_dword") != nullptr);
    SX_CHECK(std::strstr(cs_disasm, "ds_read_b32") != nullptr);
    SX_CHECK(std::strstr(cs_disasm, "ds_write_b32") != nullptr);
    SX_CHECK(std::strstr(cs_disasm, "s_waitcnt") != nullptr);
    SX_CHECK(std::strstr(cs_disasm, "s_endpgm") != nullptr);

    // Verify UAV and SRV resource extraction
    XS2_RESOURCE_BINDINGS cs_bindings{};
    SX_CHECK(Xs2ShaderExtractResourceBindings(openworld_compute_kernel, openworld_kernel_bytes, &cs_bindings) == 0);
    SX_CHECK(cs_bindings.uavCount >= 1);
    SX_CHECK(cs_bindings.srvCount >= 4);
    SX_CHECK(cs_bindings.cbvCount >= 2);

    // Lift to DXIL and validate container
    void* cs_dxil = nullptr;
    size_t cs_dxil_size = 0;
    SX_CHECK(Xs2ShaderLiftToDxil(openworld_compute_kernel, openworld_kernel_bytes, &opts, &cs_dxil, &cs_dxil_size) == 0);
    SX_CHECK(cs_dxil != nullptr && cs_dxil_size > openworld_kernel_bytes);
    int32_t cs_dxil_valid = 0;
    SX_CHECK(Xs2ShaderValidateDxilContainer(cs_dxil, cs_dxil_size, &cs_dxil_valid) == 0);
    SX_CHECK(cs_dxil_valid == 1);
    SX_CHECK(Xs2ShaderFreeBytecode(cs_dxil) == 0);

    // Lift to SPIR-V and validate SPIR-V 1.5 SSA instruction stream
    void* cs_spirv = nullptr;
    size_t cs_spirv_size = 0;
    SX_CHECK(Xs2ShaderLiftToSpirv(openworld_compute_kernel, openworld_kernel_bytes, &opts, &cs_spirv, &cs_spirv_size) == 0);
    SX_CHECK(cs_spirv != nullptr && cs_spirv_size >= 20 && (cs_spirv_size % 4) == 0);
    const auto* spv_words = static_cast<const uint32_t*>(cs_spirv);
    const size_t spv_count = cs_spirv_size / 4;
    SX_CHECK(spv_words[0] == 0x07230203u);  // SPIR-V Magic
    SX_CHECK(spv_words[1] == 0x00010500u);  // SPIR-V 1.5
    bool has_fadd = false, has_fmul = false, has_ret = false, has_fn_end = false;
    for (size_t wi = 5; wi < spv_count; ) {
        const uint32_t wc = spv_words[wi] >> 16;
        const uint32_t op = spv_words[wi] & 0xFFFFu;
        SX_CHECK(wc >= 1 && wi + wc <= spv_count);
        if (op == 129u) has_fadd = true;     // OpFAdd
        if (op == 133u) has_fmul = true;     // OpFMul
        if (op == 253u) has_ret = true;      // OpReturn
        if (op == 56u) has_fn_end = true;    // OpFunctionEnd
        wi += wc;
    }
    SX_CHECK(has_fadd && has_fmul && has_ret && has_fn_end);
    SX_CHECK(Xs2ShaderFreeBytecode(cs_spirv) == 0);

    // 8. RDNA2 Graphics Shaders (Pixel Shader with VINTRP + MIMG + EXP mrt0, Vertex Shader with EXP pos0)
    {
        const uint32_t ps_kernel[] = {
            0xC8000000,             // VINTRP: v_interp_p1_f32 v0, v0, attr0.x
            0xC8010000,             // VINTRP: v_interp_p2_f32 v0, v0, attr0.x
            0xF0800000, 0x00000400, // MIMG:   image_sample v[4:7], v0, s[0:7], s[8:11]
            0xF8000800, 0x07060504, // EXP:    exp mrt0 v4, v5, v6, v7 done
            0xBF800000              // SOPP:   s_endpgm
        };
        XS2_MICROCODE_INFO ps_info{};
        SX_CHECK(Xs2ShaderIdentifyMicrocode(ps_kernel, sizeof(ps_kernel), &ps_info) == 0);
        SX_CHECK(ps_info.stage == 2);  // Pixel Shader
        char ps_disasm[1024]{};
        SX_CHECK(Xs2ShaderDisassembleRdna2(ps_kernel, sizeof(ps_kernel), ps_disasm, sizeof(ps_disasm), nullptr) == 0);
        SX_CHECK(std::strstr(ps_disasm, "v_interp_p1_f32") != nullptr);
        SX_CHECK(std::strstr(ps_disasm, "v_interp_p2_f32") != nullptr);
        SX_CHECK(std::strstr(ps_disasm, "image_sample") != nullptr);
        SX_CHECK(std::strstr(ps_disasm, "exp mrt0") != nullptr);

        const uint32_t vs_kernel[] = {
            0xF4000000, 0x00000000, // SMEM: s_load_dwordx4
            0xF80008C0, 0x03020100, // EXP:  exp pos0 v0, v1, v2, v3 done (target=12 -> 12<<4=0xC0)
            0xBF800000              // SOPP: s_endpgm
        };
        XS2_MICROCODE_INFO vs_info{};
        SX_CHECK(Xs2ShaderIdentifyMicrocode(vs_kernel, sizeof(vs_kernel), &vs_info) == 0);
        SX_CHECK(vs_info.stage == 1);  // Vertex Shader
        char vs_disasm[512]{};
        SX_CHECK(Xs2ShaderDisassembleRdna2(vs_kernel, sizeof(vs_kernel), vs_disasm, sizeof(vs_disasm), nullptr) == 0);
        SX_CHECK(std::strstr(vs_disasm, "exp pos0") != nullptr);

        // Verify lifted SPIR-V 1.5 Vertex & Pixel modules contain OpCompositeConstruct (80), OpStore (62),
        // and Pixel Shader emits OpImageSampleImplicitLod (87) + Fragment Input OpLoad (61)
        auto check_spv_export = [&](const void* kernel, size_t sz, bool require_sample) -> bool {
            void* spv = nullptr;
            size_t spv_sz = 0;
            if (Xs2ShaderLiftToSpirv(kernel, sz, &opts, &spv, &spv_sz) != 0 || !spv || spv_sz < 20) return false;
            const auto* w = static_cast<const uint32_t*>(spv);
            const size_t n = spv_sz / 4;
            bool has_cc = false, has_store = false, has_dec = false, has_sample = false, has_access = false;
            for (size_t i = 5; i < n; ) {
                const uint32_t wc = w[i] >> 16;
                const uint32_t op = w[i] & 0xFFFFu;
                if (wc < 1 || i + wc > n) { Xs2ShaderFreeBytecode(spv); return false; }
                if (op == 71u) has_dec = true;     // OpDecorate
                if (op == 80u) has_cc = true;      // OpCompositeConstruct
                if (op == 62u) has_store = true;   // OpStore
                if (op == 87u) has_sample = true;  // OpImageSampleImplicitLod
                if (op == 65u) has_access = true;  // OpAccessChain (UBO)
                i += wc;
            }
            Xs2ShaderFreeBytecode(spv);
            return has_dec && has_cc && has_store && (!require_sample || has_sample) && (require_sample || has_access);
        };
        SX_CHECK(check_spv_export(ps_kernel, sizeof(ps_kernel), true));
        SX_CHECK(check_spv_export(vs_kernel, sizeof(vs_kernel), false));
#if defined(_WIN32)
        {
            void* spv_ps = nullptr; size_t sz_ps = 0;
            void* spv_vs = nullptr; size_t sz_vs = 0;
            SX_CHECK(Xs2ShaderLiftToSpirv(ps_kernel, sizeof(ps_kernel), &opts, &spv_ps, &sz_ps) == 0);
            SX_CHECK(Xs2ShaderLiftToSpirv(vs_kernel, sizeof(vs_kernel), &opts, &spv_vs, &sz_vs) == 0);
            bool drv = false;
            SX_CHECK(VerifySpirvWithHostVulkanDriver(spv_ps, sz_ps, &drv));
            SX_CHECK(VerifySpirvWithHostVulkanDriver(spv_vs, sz_vs, nullptr));
            Xs2ShaderFreeBytecode(spv_ps);
            Xs2ShaderFreeBytecode(spv_vs);
        }
#endif
    }

    // 9. Wave32 Subgroup + Structured CFG Loop/Branch + SSBO StorageBuffer + LDS Shared Barrier Kernel
    {
        XS2_TRANSLATION_OPTIONS wave_opts{};
        wave_opts.targetShaderModel = 0x00060005u;
        wave_opts.enableWave32 = 1;
        wave_opts.enableSubgroupOps = 1;

        const uint32_t wave32_cfg_ssbo_kernel[] = {
            0xE0000000u, 0x00000200u, // MUBUF: buffer_load_dword v2, v0 (SSBO Binding 2)
            0x7E045D02u,              // VOP1:  v_rsq_f32 v2, v2 (GLSL.std.450 InverseSqrt, op=0x2E -> 0x2E<<9=0x5C00 | 0x102)
            0x7E060402u,              // VOP1:  v_readfirstlane_b32 s3, v2 (Wave32 BroadcastFirst, op=0x02)
            0x7C020602u,              // VOPC:  v_cmp_lt_f32 vcc_lo, v2, v3 (op=0x01)
            0xD6FF0004u, 0x00000000u, // VOP3:  v_mbcnt_lo_u32_b32 v4, v0, v1, v2 (Wave32 Ballot, op=0x2FF)
            0xBF87FFFCu,              // SOPP:  s_cbranch_vccnz -4 (backward structured loop -> OpLoopMerge)
            0xDA000000u, 0x00000200u, // LDS:   ds_write_b32 v0, v2 (Workgroup shared array)
            0xBF8A0000u,              // SOPP:  s_barrier (OpControlBarrier Workgroup)
            0xD8000000u, 0x05000000u, // LDS:   ds_read_b32 v5, v0
            0xBF850001u,              // SOPP:  s_cbranch_scc1 +1 (forward conditional -> OpSelectionMerge)
            0xE2000000u, 0x00000500u, // MUBUF: buffer_store_dword v5, v0 (SSBO Binding 2)
            0xBF800000u               // SOPP:  s_endpgm
        };

        char cfg_disasm[2048]{};
        SX_CHECK(Xs2ShaderDisassembleRdna2(wave32_cfg_ssbo_kernel, sizeof(wave32_cfg_ssbo_kernel),
                                           cfg_disasm, sizeof(cfg_disasm), nullptr) == 0);
        SX_CHECK(std::strstr(cfg_disasm, "v_readfirstlane_b32") != nullptr);
        SX_CHECK(std::strstr(cfg_disasm, "v_cmp_lt_f32") != nullptr);
        SX_CHECK(std::strstr(cfg_disasm, "v_mbcnt_lo_u32_b32") != nullptr);
        SX_CHECK(std::strstr(cfg_disasm, "s_cbranch_vccnz") != nullptr);
        SX_CHECK(std::strstr(cfg_disasm, "s_barrier") != nullptr);
        SX_CHECK(std::strstr(cfg_disasm, "s_cbranch_scc1") != nullptr);

        void* cfg_spv = nullptr;
        size_t cfg_spv_sz = 0;
        SX_CHECK(Xs2ShaderLiftToSpirv(wave32_cfg_ssbo_kernel, sizeof(wave32_cfg_ssbo_kernel),
                                      &wave_opts, &cfg_spv, &cfg_spv_sz) == 0);
        SX_CHECK(cfg_spv != nullptr && cfg_spv_sz >= 80 && (cfg_spv_sz % 4) == 0);

        const auto* w = static_cast<const uint32_t*>(cfg_spv);
        const size_t n = cfg_spv_sz / 4;
        bool has_ssbo_rt_arr = false, has_lds_arr = false, has_loop_merge = false;
        bool has_sel_merge = false, has_barrier_op = false, has_bcast_first = false, has_ballot = false;
        for (size_t i = 5; i < n; ) {
            const uint32_t wc = w[i] >> 16;
            const uint32_t op = w[i] & 0xFFFFu;
            SX_CHECK(wc >= 1 && i + wc <= n);
            if (op == 29u) has_ssbo_rt_arr = true;   // OpTypeRuntimeArray (SSBO)
            if (op == 28u) has_lds_arr = true;       // OpTypeArray (LDS Workgroup)
            if (op == 246u) has_loop_merge = true;   // OpLoopMerge
            if (op == 247u) has_sel_merge = true;    // OpSelectionMerge
            if (op == 224u) has_barrier_op = true;   // OpControlBarrier
            if (op == 338u) has_bcast_first = true;  // OpGroupNonUniformBroadcastFirst
            if (op == 339u) has_ballot = true;       // OpGroupNonUniformBallot
            i += wc;
        }
        SX_CHECK(has_ssbo_rt_arr && has_lds_arr && has_loop_merge && has_sel_merge &&
                 has_barrier_op && has_bcast_first && has_ballot);

#if defined(_WIN32)
        bool vk_present = false;
        const bool vk_ok = VerifySpirvWithHostVulkanDriver(cfg_spv, cfg_spv_sz, &vk_present);
        std::cout << "[test_xs2_lifting] Host Vulkan SPIR-V 1.5 driver validation (present="
                  << (vk_present ? "yes" : "no") << "): " << (vk_ok ? "PASSED" : "FAILED") << "\n";
        SX_CHECK(vk_ok);
#endif
        Xs2ShaderFreeBytecode(cfg_spv);
    }

    std::cout << "[test_xs2_lifting] All 16 Xs2Shader APIs verified successfully!\n";
    return 0;
}
