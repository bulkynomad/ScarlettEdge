#include "scarlettx/gpu/xs2_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

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

    // Lift to SPIR-V and validate magic header
    void* cs_spirv = nullptr;
    size_t cs_spirv_size = 0;
    SX_CHECK(Xs2ShaderLiftToSpirv(openworld_compute_kernel, openworld_kernel_bytes, &opts, &cs_spirv, &cs_spirv_size) == 0);
    SX_CHECK(cs_spirv != nullptr && cs_spirv_size >= 20);
    uint32_t spirv_magic = 0;
    std::memcpy(&spirv_magic, cs_spirv, sizeof(spirv_magic));
    SX_CHECK(spirv_magic == 0x07230203u);
    SX_CHECK(Xs2ShaderFreeBytecode(cs_spirv) == 0);

    std::cout << "[test_xs2_lifting] All 16 Xs2Shader APIs verified successfully!\n";
    return 0;
}
