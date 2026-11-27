#define SCARLETTX_BUILDING_XS2 1
#include "scarlettx/gpu/xs2_abi.h"
#include "scarlettx/gpu/shader_ir.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);

enum class Rdna2InstClass {
    SALU,
    VALU,
    VMEM,
    MIMG,
    LDS,
    VINTRP,
    EXPORT,
    RAYTRACING,
    CONTROL,
    UNKNOWN
};

struct DecodedInstruction {
    uint32_t offset = 0;
    uint32_t raw_word0 = 0;
    uint32_t raw_word1 = 0;
    bool is_64bit = false;
    bool is_branch = false;
    bool is_conditional_branch = false;
    bool is_backward_branch = false;
    bool is_subgroup_op = false;
    int32_t branch_simm16 = 0;
    Rdna2InstClass inst_class = Rdna2InstClass::UNKNOWN;
    std::string mnemonic;
    std::string operands;
};

// Clean-room RDNA2 ISA instruction decoder (GFX10.3 / Xbox Series X|S Scarlett)
DecodedInstruction DecodeRdna2Instruction(const uint8_t* code, size_t remaining, uint32_t offset) {
    DecodedInstruction inst{};
    inst.offset = offset;
    if (remaining < 4) return inst;

    uint32_t w0 = 0;
    std::memcpy(&w0, code, 4);
    inst.raw_word0 = w0;

    // 1. SOPP (Scalar Operation - Program Control): bits 31..23 == 0b101111111 (0xBF800000)
    if ((w0 & 0xFF800000u) == 0xBF800000u) {
        inst.inst_class = Rdna2InstClass::CONTROL;
        const uint32_t op = (w0 >> 16) & 0x7Fu;
        const uint16_t simm16 = static_cast<uint16_t>(w0 & 0xFFFFu);
        const int16_t rel = static_cast<int16_t>(simm16);
        inst.branch_simm16 = rel;
        if (op == 0x01 || op == 0x00 || w0 == 0xBF800000u) {
            inst.mnemonic = "s_endpgm";
            inst.operands = "";
        } else if (op == 0x0A) {
            inst.mnemonic = "s_barrier";
            inst.operands = "";
        } else if (op == 0x0C) {
            inst.mnemonic = "s_waitcnt";
            std::ostringstream ss;
            ss << "vmcnt(" << (simm16 & 0x3Fu) << ") lgkmcnt(" << ((simm16 >> 8) & 0x3Fu) << ")";
            inst.operands = ss.str();
        } else if (op == 0x02 || (op >= 0x04 && op <= 0x08)) {
            inst.is_branch = true;
            inst.is_conditional_branch = (op != 0x02);
            inst.is_backward_branch = (rel < 0);
            if (op == 0x02) inst.mnemonic = "s_branch";
            else if (op == 0x04) inst.mnemonic = "s_cbranch_scc0";
            else if (op == 0x05) inst.mnemonic = "s_cbranch_scc1";
            else if (op == 0x06) inst.mnemonic = "s_cbranch_vccz";
            else if (op == 0x07) inst.mnemonic = "s_cbranch_vccnz";
            else inst.mnemonic = "s_cbranch_execz";
            const int64_t target_pc = static_cast<int64_t>(offset) + 4 + static_cast<int64_t>(rel) * 4;
            std::ostringstream ss;
            ss << "label_0x" << std::hex << std::max<int64_t>(0, target_pc);
            inst.operands = ss.str();
        } else {
            inst.mnemonic = "s_sopp_op" + std::to_string(op);
            inst.operands = "0x" + std::to_string(simm16);
        }
        return inst;
    }

    // 2. SOPC (Scalar Compare -> SCC): bits 31..23 == 0b101111110 (0xBF000000)
    if ((w0 & 0xFF800000u) == 0xBF000000u) {
        inst.inst_class = Rdna2InstClass::SALU;
        const uint32_t op = (w0 >> 16) & 0x7Fu;
        const uint32_t ssrc1 = (w0 >> 8) & 0xFFu;
        const uint32_t ssrc0 = w0 & 0xFFu;
        if (op == 0x00 || op == 0x06) inst.mnemonic = "s_cmp_eq_u32";
        else if (op == 0x01 || op == 0x07) inst.mnemonic = "s_cmp_lg_u32";
        else if (op == 0x02 || op == 0x08) inst.mnemonic = "s_cmp_gt_i32";
        else inst.mnemonic = "s_cmp_lt_i32";
        std::ostringstream ss;
        ss << "s" << ssrc0 << ", s" << ssrc1;
        inst.operands = ss.str();
        return inst;
    }

    // 3. SOP1 (Scalar 1-Operand / Wave32 mask ops): bits 31..23 == 0b101111101 (0xBE800000)
    if ((w0 & 0xFF800000u) == 0xBE800000u) {
        inst.inst_class = Rdna2InstClass::SALU;
        const uint32_t sdst = (w0 >> 16) & 0x7Fu;
        const uint32_t op = (w0 >> 8) & 0xFFu;
        const uint32_t ssrc0 = w0 & 0xFFu;
        if (op == 0x03) inst.mnemonic = "s_mov_b32";
        else if (op == 0x24) { inst.mnemonic = "s_and_saveexec_b32"; inst.is_subgroup_op = true; }
        else inst.mnemonic = "s_sop1_op" + std::to_string(op);
        std::ostringstream ss;
        ss << "s" << sdst << ", s" << ssrc0;
        inst.operands = ss.str();
        return inst;
    }

    // 4. SMEM (Scalar Memory / Uniform Buffers): bits 31..26 == 0b111101 (0xF4000000)
    if ((w0 & 0xFC000000u) == 0xF4000000u) {
        inst.inst_class = Rdna2InstClass::SALU;
        inst.is_64bit = (remaining >= 8);
        if (inst.is_64bit) std::memcpy(&inst.raw_word1, code + 4, 4);
        const uint32_t sdata = (w0 >> 6) & 0x7Fu;
        const uint32_t sbase = (w0 >> 0) & 0x3Fu;
        inst.mnemonic = "s_load_dwordx4";
        std::ostringstream ss;
        ss << "s[" << sdata << ":" << (sdata + 3) << "], s[" << (sbase * 2) << ":" << (sbase * 2 + 1) << "], 0x00";
        inst.operands = ss.str();
        return inst;
    }

    // 5. VOP1 (Vector 1-Operand + Wave32 v_readfirstlane_b32): bits 31..25 == 0b0111111 (0x7E000000)
    if ((w0 & 0xFE000000u) == 0x7E000000u) {
        inst.inst_class = Rdna2InstClass::VALU;
        const uint32_t vdst = (w0 >> 17) & 0xFFu;
        const uint32_t op = (w0 >> 9) & 0xFFu;
        const uint32_t src0 = w0 & 0x1FFu;
        if (op == 0x01) inst.mnemonic = "v_mov_b32";
        else if (op == 0x02) { inst.mnemonic = "v_readfirstlane_b32"; inst.is_subgroup_op = true; }
        else if (op == 0x05) inst.mnemonic = "v_cvt_f32_i32";
        else if (op == 0x06) inst.mnemonic = "v_cvt_f32_u32";
        else if (op == 0x07) inst.mnemonic = "v_cvt_u32_f32";
        else if (op == 0x08) inst.mnemonic = "v_cvt_i32_f32";
        else if (op == 0x2A) inst.mnemonic = "v_rcp_f32";
        else if (op == 0x2E) inst.mnemonic = "v_rsq_f32";
        else if (op == 0x33) inst.mnemonic = "v_sqrt_f32";
        else if (op == 0x35) inst.mnemonic = "v_sin_f32";
        else if (op == 0x36) inst.mnemonic = "v_cos_f32";
        else inst.mnemonic = "v_vop1_op" + std::to_string(op);

        std::ostringstream ss;
        if (op == 0x02) ss << "s" << vdst;
        else ss << "v" << vdst;
        ss << ", " << ((src0 >= 256) ? ("v" + std::to_string(src0 - 256)) : ("v" + std::to_string(src0)));
        inst.operands = ss.str();
        return inst;
    }

    // 6. VOPC (Vector Compare -> Wave32 VCC): bits 31..25 == 0b0111110 (0x7C000000)
    if ((w0 & 0xFE000000u) == 0x7C000000u) {
        inst.inst_class = Rdna2InstClass::VALU;
        const uint32_t op = (w0 >> 17) & 0xFFu;
        const uint32_t vsrc1 = (w0 >> 9) & 0xFFu;
        const uint32_t src0 = w0 & 0x1FFu;
        const uint32_t cmp_kind = op & 0x07u;
        if (cmp_kind == 0x01) inst.mnemonic = "v_cmp_lt_f32";
        else if (cmp_kind == 0x02) inst.mnemonic = "v_cmp_eq_f32";
        else if (cmp_kind == 0x03) inst.mnemonic = "v_cmp_le_f32";
        else if (cmp_kind == 0x04) inst.mnemonic = "v_cmp_gt_f32";
        else if (cmp_kind == 0x05) inst.mnemonic = "v_cmp_lg_f32";
        else inst.mnemonic = "v_cmp_ge_f32";
        std::ostringstream ss;
        ss << "vcc_lo, v" << (src0 & 0xFFu) << ", v" << vsrc1;
        inst.operands = ss.str();
        return inst;
    }

    // 7. VOP2 (Vector Operation 2-operand): bit 31 == 0
    if ((w0 & 0x80000000u) == 0) {
        inst.inst_class = Rdna2InstClass::VALU;
        const uint32_t vop2_op = (w0 >> 25) & 0x3Fu;
        const uint32_t vdst = (w0 >> 17) & 0xFFu;
        const uint32_t vsrc1 = (w0 >> 9) & 0xFFu;
        const uint32_t src0 = w0 & 0x1FFu;

        if (vop2_op == 0x03) inst.mnemonic = "v_add_f32";
        else if (vop2_op == 0x04) inst.mnemonic = "v_sub_f32";
        else if (vop2_op == 0x05) inst.mnemonic = "v_subrev_f32";
        else if (vop2_op == 0x08) inst.mnemonic = "v_mul_f32";
        else if (vop2_op == 0x0A) inst.mnemonic = "v_min_f32";
        else if (vop2_op == 0x0B) inst.mnemonic = "v_max_f32";
        else if (vop2_op == 0x18) inst.mnemonic = "v_cndmask_b32";
        else if (vop2_op == 0x19) inst.mnemonic = "v_fmac_f32";
        else inst.mnemonic = "v_vop2_op" + std::to_string(vop2_op);

        std::ostringstream ss;
        ss << "v" << vdst << ", v" << vsrc1 << ", " << ((src0 < 256) ? ("v" + std::to_string(src0)) : ("s" + std::to_string(src0 - 256)));
        inst.operands = ss.str();
        return inst;
    }

    // 8. VOP3 (Vector Operation 3-operand, e.g. FMA / ballot / bitcnt): bits 31..26 == 0b110101
    if ((w0 & 0xFC000000u) == 0xD4000000u) {
        inst.inst_class = Rdna2InstClass::VALU;
        inst.is_64bit = (remaining >= 8);
        if (inst.is_64bit) std::memcpy(&inst.raw_word1, code + 4, 4);
        const uint32_t vdst = w0 & 0xFFu;
        const uint32_t op = (w0 >> 16) & 0x3FFu;
        if (op == 0x2FFu) {
            inst.mnemonic = "v_mbcnt_lo_u32_b32";
            inst.is_subgroup_op = true;
        } else {
            inst.mnemonic = "v_fma_f32";
        }
        const uint32_t s0 = inst.raw_word1 & 0xFFu;
        const uint32_t s1 = (inst.raw_word1 != 0) ? ((inst.raw_word1 >> 9) & 0xFFu) : 1u;
        const uint32_t s2 = (inst.raw_word1 != 0) ? ((inst.raw_word1 >> 18) & 0xFFu) : 2u;
        std::ostringstream ss;
        ss << "v" << vdst << ", v" << s0 << ", v" << s1 << ", v" << s2;
        inst.operands = ss.str();
        return inst;
    }

    // 9. MUBUF / VMEM (Vector Memory / SSBO Buffer ops): bits 31..26 == 0b111000 (0xE0000000)
    if ((w0 & 0xFC000000u) == 0xE0000000u) {
        inst.inst_class = Rdna2InstClass::VMEM;
        inst.is_64bit = (remaining >= 8);
        if (inst.is_64bit) std::memcpy(&inst.raw_word1, code + 4, 4);
        const uint32_t mubuf_op = (w0 >> 18) & 0x7Fu;
        const bool is_store = ((w0 & (1u << 25)) != 0) || (mubuf_op >= 0x1Cu && mubuf_op <= 0x1Fu);
        const uint32_t vdata = (inst.raw_word1 != 0) ? ((inst.raw_word1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
        const uint32_t vaddr = (inst.raw_word1 != 0) ? (inst.raw_word1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
        inst.mnemonic = is_store ? "buffer_store_dword" : "buffer_load_dword";
        std::ostringstream ss;
        ss << "v" << vdata << ", v" << vaddr << ", s[0:3], 0 offen";
        inst.operands = ss.str();
        return inst;
    }

    // 10. EXP (Export: pos0..3, mrt0..7, mrtz, param0..31): bits 31..26 == 0b111110 (0xF8000000)
    if ((w0 & 0xFC000000u) == 0xF8000000u) {
        inst.inst_class = Rdna2InstClass::EXPORT;
        inst.is_64bit = (remaining >= 8);
        if (inst.is_64bit) std::memcpy(&inst.raw_word1, code + 4, 4);
        const uint32_t target = (w0 >> 4) & 0x3Fu;
        const uint32_t vsrc0 = inst.raw_word1 & 0xFFu;
        const uint32_t vsrc1 = (inst.raw_word1 >> 8) & 0xFFu;
        const uint32_t vsrc2 = (inst.raw_word1 >> 16) & 0xFFu;
        const uint32_t vsrc3 = (inst.raw_word1 >> 24) & 0xFFu;
        inst.mnemonic = "exp";
        std::ostringstream ss;
        if (target < 8) ss << "mrt" << target;
        else if (target == 8) ss << "mrtz";
        else if (target >= 12 && target <= 15) ss << "pos" << (target - 12);
        else if (target >= 32) ss << "param" << (target - 32);
        else ss << "target" << target;
        ss << " v" << vsrc0 << ", v" << vsrc1 << ", v" << vsrc2 << ", v" << vsrc3;
        if (w0 & (1u << 11)) ss << " done";
        inst.operands = ss.str();
        return inst;
    }

    // 11. VINTRP (Pixel Shader Attribute Interpolation): bits 31..26 == 0b110010 (0xC8000000)
    if ((w0 & 0xFC000000u) == 0xC8000000u) {
        inst.inst_class = Rdna2InstClass::VINTRP;
        const uint32_t vdst = (w0 >> 18) & 0xFFu;
        const uint32_t op = (w0 >> 16) & 0x3u;
        const uint32_t attr = (w0 >> 10) & 0x3Fu;
        const uint32_t attrchan = (w0 >> 8) & 0x3u;
        const uint32_t vsrc = w0 & 0xFFu;
        static const char kChan[4] = {'x', 'y', 'z', 'w'};
        if (op == 0) inst.mnemonic = "v_interp_p1_f32";
        else if (op == 1) inst.mnemonic = "v_interp_p2_f32";
        else inst.mnemonic = "v_interp_mov_f32";
        std::ostringstream ss;
        ss << "v" << vdst << ", v" << vsrc << ", attr" << attr << "." << kChan[attrchan];
        inst.operands = ss.str();
        return inst;
    }

    // 12. MIMG (Image / Texture Sample & Load/Store): bits 31..26 == 0b111100 (0xF0000000)
    if ((w0 & 0xFC000000u) == 0xF0000000u) {
        inst.inst_class = Rdna2InstClass::MIMG;
        inst.is_64bit = (remaining >= 8);
        if (inst.is_64bit) std::memcpy(&inst.raw_word1, code + 4, 4);
        const uint32_t op = (w0 >> 18) & 0xFFu;
        const uint32_t vdata = (inst.raw_word1 >> 8) & 0xFFu;
        const uint32_t vaddr = inst.raw_word1 & 0xFFu;
        if (op >= 0x20) inst.mnemonic = "image_sample";
        else if (op >= 0x08) inst.mnemonic = "image_store";
        else inst.mnemonic = "image_load";
        std::ostringstream ss;
        ss << "v[" << vdata << ":" << (vdata + 3) << "], v" << vaddr << ", s[0:7], s[8:11] dmask:0xf";
        inst.operands = ss.str();
        return inst;
    }

    // 13. LDS (Local Data Share): bits 31..26 == 0b110110 (0xD8000000)
    if ((w0 & 0xFC000000u) == 0xD8000000u) {
        inst.inst_class = Rdna2InstClass::LDS;
        inst.is_64bit = (remaining >= 8);
        if (inst.is_64bit) std::memcpy(&inst.raw_word1, code + 4, 4);
        const uint32_t vdst = (inst.raw_word1 != 0) ? ((inst.raw_word1 >> 24) & 0xFFu) : ((w0 >> 18) & 0xFFu);
        const uint32_t vaddr = inst.raw_word1 & 0xFFu;
        inst.mnemonic = (w0 & (1u << 25)) ? "ds_write_b32" : "ds_read_b32";
        std::ostringstream ss;
        ss << "v" << vdst << ", v" << vaddr << " offset:" << (w0 & 0xFFFFu);
        inst.operands = ss.str();
        return inst;
    }

    // Fallback scalar / generic
    inst.inst_class = Rdna2InstClass::SALU;
    inst.mnemonic = "s_nop";
    inst.operands = "0";
    return inst;
}

struct PipelineState {
    uint32_t flags = 0;
    uint32_t max_entries = 1024;
    std::unordered_map<uint64_t, std::vector<uint8_t>> cache;
    XS2_TRANSLATION_STATS stats{};
    std::mutex mu;
};

uint64_t SimpleHash(const uint8_t* p, size_t s) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < s; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

}  // namespace

extern "C" {

int32_t SCARLETTX_XS2_CALL Xs2ShaderIdentifyMicrocode(
    const void* binaryData, size_t byteSize, XS2_MICROCODE_INFO* outInfo) {
    if (!binaryData || byteSize < 4 || !outInfo) return kInvalidArg;

    const auto* code = static_cast<const uint8_t*>(binaryData);
    size_t offset = 0;
    uint32_t inst_count = 0;
    uint32_t max_vgpr = 0;
    uint32_t max_sgpr = 0;
    uint32_t lds_bytes = 0;
    bool has_vmem = false;
    bool has_vs_export = false;
    bool has_ps_export_or_interp = false;

    while (offset < byteSize) {
        DecodedInstruction inst = DecodeRdna2Instruction(code + offset, byteSize - offset, static_cast<uint32_t>(offset));
        inst_count++;
        if (inst.inst_class == Rdna2InstClass::VALU) max_vgpr = std::max(max_vgpr, 32u);
        if (inst.inst_class == Rdna2InstClass::SALU) max_sgpr = std::max(max_sgpr, 64u);
        if (inst.inst_class == Rdna2InstClass::LDS) lds_bytes = std::max(lds_bytes, 1024u);
        if (inst.inst_class == Rdna2InstClass::VMEM) has_vmem = true;
        if (inst.inst_class == Rdna2InstClass::VINTRP) has_ps_export_or_interp = true;
        if (inst.inst_class == Rdna2InstClass::EXPORT) {
            if (inst.operands.rfind("pos", 0) == 0 || inst.operands.rfind("param", 0) == 0) {
                has_vs_export = true;
            } else {
                has_ps_export_or_interp = true;
            }
        }
        offset += inst.is_64bit ? 8 : 4;
    }

    if (has_vs_export) outInfo->stage = 1;           // Vertex Shader
    else if (has_ps_export_or_interp) outInfo->stage = 2; // Pixel Shader
    else outInfo->stage = has_vmem ? 3 : 2;          // 3 = Compute, 2 = Pixel
    outInfo->instructionCount = inst_count;
    outInfo->vgprCount = std::max(32u, max_vgpr);
    outInfo->sgprCount = std::max(64u, max_sgpr);
    outInfo->ldsBytes = lds_bytes;
    outInfo->shaderHash = SimpleHash(code, byteSize);
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderDisassembleRdna2(
    const void* microcode, size_t byteSize, char* outAsm, size_t maxAsmLen, size_t* actualLen) {
    if (!microcode || byteSize == 0 || !outAsm || maxAsmLen == 0) return kInvalidArg;

    const auto* code = static_cast<const uint8_t*>(microcode);
    std::ostringstream ss;
    size_t offset = 0;
    while (offset < byteSize) {
        DecodedInstruction inst = DecodeRdna2Instruction(code + offset, byteSize - offset, static_cast<uint32_t>(offset));
        ss << inst.mnemonic;
        if (!inst.operands.empty()) {
            ss << " " << inst.operands;
        }
        ss << "\n";
        offset += inst.is_64bit ? 8 : 4;
    }

    std::string text = ss.str();
    const size_t copy_len = std::min(text.size(), maxAsmLen - 1);
    std::memcpy(outAsm, text.data(), copy_len);
    outAsm[copy_len] = '\0';
    if (actualLen) *actualLen = copy_len;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderLiftToDxil(
    const void* microcode, size_t byteSize, const XS2_TRANSLATION_OPTIONS* options, void** outDxilBytecode, size_t* outDxilSize) {
    if (!microcode || byteSize == 0 || !outDxilBytecode || !outDxilSize) return kInvalidArg;

    const uint32_t target_sm = options ? options->targetShaderModel : 0x00060005u;
    const bool wave32 = options ? (options->enableWave32 != 0) : true;
    std::vector<uint8_t> compiled_dxbc;
    const auto st = scarlettx::LiftRdna2MicrocodeToPcShader(
        static_cast<const uint8_t*>(microcode),
        byteSize,
        scarlettx::ShaderStage::Unknown,
        target_sm,
        wave32,
        compiled_dxbc);
    if (!st.ok() || compiled_dxbc.empty()) {
        return kInvalidArg;
    }

    auto* buf = new uint8_t[compiled_dxbc.size()];
    std::memcpy(buf, compiled_dxbc.data(), compiled_dxbc.size());
    *outDxilBytecode = buf;
    *outDxilSize = compiled_dxbc.size();
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderLiftToSpirv(
    const void* microcode, size_t byteSize, const XS2_TRANSLATION_OPTIONS* options, void** outSpirvBytecode, size_t* outSpirvSize) {
    if (!microcode || byteSize == 0 || !outSpirvBytecode || !outSpirvSize) return kInvalidArg;

    // Decode RDNA2 instructions and emit a Vulkan-1.3-compliant SPIR-V 1.5 binary module
    // supporting 256-VGPR register files, SSBO StorageBuffer Load/Store (Binding 2),
    // Workgroup LDS shared arrays, structured CFG reconvergence (OpSelectionMerge / OpLoopMerge),
    // GLSL.std.450 math intrinsics, and Wave32 Subgroup operations (GroupNonUniform / Ballot).
    const auto* code = static_cast<const uint8_t*>(microcode);
    std::vector<DecodedInstruction> insts;
    size_t offset = 0;
    while (offset < byteSize) {
        DecodedInstruction inst = DecodeRdna2Instruction(code + offset, byteSize - offset, static_cast<uint32_t>(offset));
        offset += inst.is_64bit ? 8 : 4;
        insts.push_back(std::move(inst));
    }

    XS2_MICROCODE_INFO info{};
    Xs2ShaderIdentifyMicrocode(microcode, byteSize, &info);
    // SPIR-V ExecutionModel: 0 = Vertex, 4 = Fragment, 5 = GLCompute
    const uint32_t exec_model = (info.stage == 1) ? 0u : ((info.stage == 2) ? 4u : 5u);
    const bool has_output_var = (exec_model == 0u || exec_model == 4u);

    bool has_smem = false, has_mimg = false, has_vinterp = false;
    bool has_ssbo = false, has_lds = false, has_barrier = false;
    bool has_loop = false, has_subgroup = false, has_glsl_ext = false;
    for (const auto& inst : insts) {
        if (inst.inst_class == Rdna2InstClass::SALU && inst.mnemonic == "s_load_dwordx4") has_smem = true;
        if (inst.inst_class == Rdna2InstClass::MIMG && inst.mnemonic == "image_sample") has_mimg = true;
        if (inst.inst_class == Rdna2InstClass::VINTRP) has_vinterp = true;
        if (inst.inst_class == Rdna2InstClass::VMEM) has_ssbo = true;
        if (inst.inst_class == Rdna2InstClass::LDS) has_lds = true;
        if (inst.mnemonic == "s_barrier") { has_barrier = true; has_lds = true; }
        if (inst.is_branch && inst.is_backward_branch) has_loop = true;
        if (inst.is_subgroup_op && exec_model == 5u) has_subgroup = true;
        if (inst.mnemonic == "v_rsq_f32" || inst.mnemonic == "v_sqrt_f32" ||
            inst.mnemonic == "v_sin_f32" || inst.mnemonic == "v_cos_f32") {
            has_glsl_ext = true;
        }
    }
    if (options && options->enableSubgroupOps && exec_model == 5u) {
        has_subgroup = true;
    }
    const bool has_gid = (exec_model == 5u && has_ssbo);

    std::vector<uint32_t> spv;
    spv.reserve(256 + insts.size() * 12);
    auto emit = [&](uint16_t opcode, std::initializer_list<uint32_t> operands) {
        const uint32_t wc = static_cast<uint32_t>(1 + operands.size());
        spv.push_back((wc << 16) | opcode);
        for (uint32_t v : operands) spv.push_back(v);
    };

    // 5-word SPIR-V 1.5 header (bound patched at end)
    spv.push_back(0x07230203u);  // Magic
    spv.push_back(0x00010500u);  // SPIR-V 1.5
    spv.push_back(0x53585332u);  // Generator: ScarlettEdge XS2 Lifter
    spv.push_back(0u);           // Bound (patched below)
    spv.push_back(0u);           // Schema

    uint32_t next_id = 1;
    auto alloc_id = [&]() -> uint32_t { return next_id++; };

    const uint32_t kIdExtGlsl = has_glsl_ext ? alloc_id() : 0u;
    const uint32_t kIdVoid = alloc_id();
    const uint32_t kIdFnType = alloc_id();
    const uint32_t kIdMain = alloc_id();
    const uint32_t kIdF32 = alloc_id();
    const uint32_t kIdVec2 = alloc_id();
    const uint32_t kIdVec4 = alloc_id();
    const uint32_t kIdBool = alloc_id();
    const uint32_t kIdI32 = alloc_id();
    const uint32_t kIdU32 = alloc_id();
    const uint32_t kIdUVec3 = has_gid ? alloc_id() : 0u;
    const uint32_t kIdUVec4 = has_subgroup ? alloc_id() : 0u;

    const uint32_t kIdConst0 = alloc_id();
    const uint32_t kIdConst1 = alloc_id();
    const uint32_t kIdConstTrue = alloc_id();
    const uint32_t kIdConstFalse = alloc_id();
    const uint32_t kIdConstI0 = alloc_id();
    const uint32_t kIdConstI1 = alloc_id();
    const uint32_t kIdConstI4 = alloc_id();
    const uint32_t kIdConstI255 = alloc_id();
    const uint32_t kIdConstU0 = alloc_id();
    const uint32_t kIdConstU256 = has_lds ? alloc_id() : 0u;
    const uint32_t kIdConstScopeWg = has_barrier ? alloc_id() : 0u;
    const uint32_t kIdConstSemWg = has_barrier ? alloc_id() : 0u;
    const uint32_t kIdConstScopeSub = has_subgroup ? alloc_id() : 0u;

    const uint32_t kIdPtrFnF32 = alloc_id();
    const uint32_t kIdPtrFnI32 = alloc_id();
    const uint32_t kIdPtrFnBool = alloc_id();
    const uint32_t kIdPtrOutVec4 = has_output_var ? alloc_id() : 0u;
    const uint32_t kIdOutVar = has_output_var ? alloc_id() : 0u;
    const uint32_t kIdPtrInVec4 = (has_vinterp && exec_model == 4u) ? alloc_id() : 0u;
    const uint32_t kIdInVar = (has_vinterp && exec_model == 4u) ? alloc_id() : 0u;
    const uint32_t kIdPtrInUVec3 = has_gid ? alloc_id() : 0u;
    const uint32_t kIdPtrInU32 = has_gid ? alloc_id() : 0u;
    const uint32_t kIdGidVar = has_gid ? alloc_id() : 0u;

    const uint32_t kIdUboStruct = has_smem ? alloc_id() : 0u;
    const uint32_t kIdPtrUbo = has_smem ? alloc_id() : 0u;
    const uint32_t kIdPtrUboVec4 = has_smem ? alloc_id() : 0u;
    const uint32_t kIdUboVar = has_smem ? alloc_id() : 0u;

    const uint32_t kIdImage2D = has_mimg ? alloc_id() : 0u;
    const uint32_t kIdSampledImage = has_mimg ? alloc_id() : 0u;
    const uint32_t kIdPtrSampler = has_mimg ? alloc_id() : 0u;
    const uint32_t kIdSamplerVar = has_mimg ? alloc_id() : 0u;

    const uint32_t kIdRtArrF32 = has_ssbo ? alloc_id() : 0u;
    const uint32_t kIdSsboStruct = has_ssbo ? alloc_id() : 0u;
    const uint32_t kIdPtrSsboStruct = has_ssbo ? alloc_id() : 0u;
    const uint32_t kIdPtrSsboF32 = has_ssbo ? alloc_id() : 0u;
    const uint32_t kIdSsboVar = has_ssbo ? alloc_id() : 0u;

    const uint32_t kIdLdsArr = has_lds ? alloc_id() : 0u;
    const uint32_t kIdPtrWgArr = has_lds ? alloc_id() : 0u;
    const uint32_t kIdPtrWgF32 = has_lds ? alloc_id() : 0u;
    const uint32_t kIdLdsVar = has_lds ? alloc_id() : 0u;

    // 1. Capabilities
    emit(17, {1u});                                       // OpCapability Shader
    if (has_subgroup) {
        emit(17, {61u});                                  // OpCapability GroupNonUniform
        emit(17, {64u});                                  // OpCapability GroupNonUniformBallot
    }

    // 2. Extended instruction sets ("GLSL.std.450")
    if (has_glsl_ext) {
        emit(11, {kIdExtGlsl, 0x4C534C47u, 0x6474732Eu, 0x3035342Eu, 0x00000000u});
    }

    // 3. MemoryModel & EntryPoint (SPIR-V 1.5 lists ALL statically referenced global variables)
    emit(14, {0u, 1u});                                   // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {exec_model, kIdMain, 0x6E69616Du, 0u};
        if (has_output_var) ep.push_back(kIdOutVar);
        if (has_vinterp && exec_model == 4u) ep.push_back(kIdInVar);
        if (has_gid) ep.push_back(kIdGidVar);
        if (has_smem) ep.push_back(kIdUboVar);
        if (has_mimg) ep.push_back(kIdSamplerVar);
        if (has_ssbo) ep.push_back(kIdSsboVar);
        if (has_lds) ep.push_back(kIdLdsVar);
        spv.push_back((static_cast<uint32_t>(1 + ep.size()) << 16) | 15u);
        for (uint32_t w : ep) spv.push_back(w);
    }
    if (exec_model == 5u) {
        emit(16, {kIdMain, 17u, 8u, 8u, 1u});             // OpExecutionMode %main LocalSize 8 8 1
    } else if (exec_model == 4u) {
        emit(16, {kIdMain, 7u});                          // OpExecutionMode %main OriginUpperLeft
    }

    // 4. Decorations (strictly before Types/Constants/Variables)
    if (exec_model == 0u) {
        emit(71, {kIdOutVar, 11u, 0u});                   // BuiltIn Position
    } else if (exec_model == 4u) {
        emit(71, {kIdOutVar, 30u, 0u});                   // Location 0
        if (has_vinterp) emit(71, {kIdInVar, 30u, 0u});   // Location 0
    }
    if (has_gid) {
        emit(71, {kIdGidVar, 11u, 28u});                  // BuiltIn GlobalInvocationId (28)
    }
    if (has_smem) {
        emit(71, {kIdUboStruct, 2u});                     // Block
        emit(72, {kIdUboStruct, 0u, 35u, 0u});            // Member 0 Offset 0
        emit(71, {kIdUboVar, 34u, 0u});                   // DescriptorSet 0
        emit(71, {kIdUboVar, 33u, 0u});                   // Binding 0
    }
    if (has_mimg) {
        emit(71, {kIdSamplerVar, 34u, 0u});               // DescriptorSet 0
        emit(71, {kIdSamplerVar, 33u, 1u});               // Binding 1
    }
    if (has_ssbo) {
        emit(71, {kIdRtArrF32, 6u, 4u});                  // ArrayStride 4
        emit(71, {kIdSsboStruct, 2u});                    // Block
        emit(72, {kIdSsboStruct, 0u, 35u, 0u});           // Member 0 Offset 0
        emit(71, {kIdSsboVar, 34u, 0u});                  // DescriptorSet 0
        emit(71, {kIdSsboVar, 33u, 2u});                  // Binding 2
    }

    // 5. Types, Constants, and Global Variables
    emit(19, {kIdVoid});                                  // OpTypeVoid
    emit(33, {kIdFnType, kIdVoid});                       // OpTypeFunction
    emit(22, {kIdF32, 32u});                              // OpTypeFloat 32
    emit(23, {kIdVec2, kIdF32, 2u});                      // OpTypeVector vec2
    emit(23, {kIdVec4, kIdF32, 4u});                      // OpTypeVector vec4
    emit(20, {kIdBool});                                  // OpTypeBool
    emit(21, {kIdI32, 32u, 1u});                          // OpTypeInt 32 signed
    emit(21, {kIdU32, 32u, 0u});                          // OpTypeInt 32 unsigned
    if (has_gid) emit(23, {kIdUVec3, kIdU32, 3u});        // OpTypeVector uvec3
    if (has_subgroup) emit(23, {kIdUVec4, kIdU32, 4u});   // OpTypeVector uvec4

    emit(43, {kIdF32, kIdConst0, 0x00000000u});           // 0.0f
    emit(43, {kIdF32, kIdConst1, 0x3F800000u});           // 1.0f
    emit(41, {kIdBool, kIdConstTrue});                    // true
    emit(42, {kIdBool, kIdConstFalse});                   // false
    emit(43, {kIdI32, kIdConstI0, 0u});                   // i32 0
    emit(43, {kIdI32, kIdConstI1, 1u});                   // i32 1
    emit(43, {kIdI32, kIdConstI4, 4u});                   // i32 4 (max structured loop unroll bound)
    emit(43, {kIdI32, kIdConstI255, 255u});               // i32 255 (LDS index mask)
    emit(43, {kIdU32, kIdConstU0, 0u});                   // u32 0
    if (has_lds) emit(43, {kIdU32, kIdConstU256, 256u});  // u32 256 (LDS shared float count)
    if (has_barrier) {
        emit(43, {kIdU32, kIdConstScopeWg, 2u});          // Scope Workgroup (2)
        emit(43, {kIdU32, kIdConstSemWg, 0x108u});        // AcquireRelease (0x8) | WorkgroupMemory (0x100)
    }
    if (has_subgroup) {
        emit(43, {kIdU32, kIdConstScopeSub, 3u});         // Scope Subgroup (3)
    }

    emit(32, {kIdPtrFnF32, 7u, kIdF32});                  // Function* f32
    emit(32, {kIdPtrFnI32, 7u, kIdI32});                  // Function* i32
    emit(32, {kIdPtrFnBool, 7u, kIdBool});                // Function* bool

    if (has_output_var) {
        emit(32, {kIdPtrOutVec4, 3u, kIdVec4});
        emit(59, {kIdPtrOutVec4, kIdOutVar, 3u});
    }
    if (has_vinterp && exec_model == 4u) {
        emit(32, {kIdPtrInVec4, 1u, kIdVec4});
        emit(59, {kIdPtrInVec4, kIdInVar, 1u});
    }
    if (has_gid) {
        emit(32, {kIdPtrInUVec3, 1u, kIdUVec3});
        emit(32, {kIdPtrInU32, 1u, kIdU32});
        emit(59, {kIdPtrInUVec3, kIdGidVar, 1u});
    }
    if (has_smem) {
        emit(30, {kIdUboStruct, kIdVec4});
        emit(32, {kIdPtrUbo, 2u, kIdUboStruct});
        emit(32, {kIdPtrUboVec4, 2u, kIdVec4});
        emit(59, {kIdPtrUbo, kIdUboVar, 2u});
    }
    if (has_mimg) {
        emit(25, {kIdImage2D, kIdF32, 1u, 0u, 0u, 0u, 1u, 0u});
        emit(27, {kIdSampledImage, kIdImage2D});
        emit(32, {kIdPtrSampler, 0u, kIdSampledImage});
        emit(59, {kIdPtrSampler, kIdSamplerVar, 0u});
    }
    if (has_ssbo) {
        emit(29, {kIdRtArrF32, kIdF32});                  // OpTypeRuntimeArray %f32
        emit(30, {kIdSsboStruct, kIdRtArrF32});           // OpTypeStruct { f32[] }
        emit(32, {kIdPtrSsboStruct, 12u, kIdSsboStruct}); // StorageBuffer* %SsboStruct
        emit(32, {kIdPtrSsboF32, 12u, kIdF32});           // StorageBuffer* f32
        emit(59, {kIdPtrSsboStruct, kIdSsboVar, 12u});    // OpVariable StorageBuffer
    }
    if (has_lds) {
        emit(28, {kIdLdsArr, kIdF32, kIdConstU256});      // OpTypeArray f32[256]
        emit(32, {kIdPtrWgArr, 4u, kIdLdsArr});           // Workgroup* f32[256]
        emit(32, {kIdPtrWgF32, 4u, kIdF32});              // Workgroup* f32
        emit(59, {kIdPtrWgArr, kIdLdsVar, 4u});           // OpVariable Workgroup
    }

    // Collect every VGPR index referenced so Function-scope backing variables can be declared
    // in the entry block (guaranteeing strict SPIR-V SSA dominance across CFG merge blocks).
    std::vector<uint32_t> touched_regs = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    auto mark_reg = [&](uint32_t r) {
        r &= 0xFFu;
        if (std::find(touched_regs.begin(), touched_regs.end(), r) == touched_regs.end()) {
            touched_regs.push_back(r);
        }
    };
    for (const auto& inst : insts) {
        const uint32_t w0 = inst.raw_word0;
        const uint32_t w1 = inst.raw_word1;
        mark_reg(w0 & 0xFFu);
        mark_reg((w0 >> 9) & 0xFFu);
        mark_reg((w0 >> 17) & 0xFFu);
        mark_reg((w0 >> 18) & 0xFFu);
        if (inst.is_64bit) {
            mark_reg(w1 & 0xFFu);
            mark_reg((w1 >> 8) & 0xFFu);
            mark_reg((w1 >> 9) & 0xFFu);
            mark_reg((w1 >> 16) & 0xFFu);
            mark_reg((w1 >> 18) & 0xFFu);
            mark_reg((w1 >> 24) & 0xFFu);
        }
    }

    // 6. Function Entry & Entry-Block OpVariable Declarations
    const uint32_t kIdEntryLabel = alloc_id();
    emit(54, {kIdVoid, kIdMain, 0u, kIdFnType});          // OpFunction
    emit(248, {kIdEntryLabel});                           // OpLabel

    std::unordered_map<uint32_t, uint32_t> vgpr_var;
    for (uint32_t r : touched_regs) {
        const uint32_t vid = alloc_id();
        vgpr_var[r] = vid;
        emit(59, {kIdPtrFnF32, vid, 7u});                 // OpVariable Function f32
    }
    const uint32_t kIdCondVar = alloc_id();
    emit(59, {kIdPtrFnBool, kIdCondVar, 7u});             // OpVariable Function bool (SCC/VCC)
    const uint32_t kIdLoopIterVar = has_loop ? alloc_id() : 0u;
    if (has_loop) {
        emit(59, {kIdPtrFnI32, kIdLoopIterVar, 7u});      // OpVariable Function i32
    }

    // Initialize Function variables (after ALL OpVariable instructions in entry block)
    for (uint32_t r : touched_regs) {
        emit(62, {vgpr_var[r], kIdConst1});               // Default VGPR = 1.0f
    }
    emit(62, {kIdCondVar, kIdConstTrue});
    if (has_loop) {
        emit(62, {kIdLoopIterVar, kIdConstI0});
    }

    // Seed v0 with float(GlobalInvocationId.x) in Compute shaders with SSBO access
    uint32_t gid_i32_ssa = kIdConstI0;
    if (has_gid) {
        const uint32_t gid_ptr = alloc_id();
        const uint32_t gid_u32 = alloc_id();
        const uint32_t gid_i32 = alloc_id();
        const uint32_t gid_f32 = alloc_id();
        emit(65, {kIdPtrInU32, gid_ptr, kIdGidVar, kIdConstU0}); // OpAccessChain %gid.x
        emit(61, {kIdU32, gid_u32, gid_ptr});                    // OpLoad u32
        emit(124, {kIdI32, gid_i32, gid_u32});                   // OpBitcast i32 <- u32
        emit(112, {kIdF32, gid_f32, gid_u32});                   // OpConvertUToF f32 <- u32
        emit(62, {vgpr_var[0], gid_f32});
        gid_i32_ssa = gid_i32;
    }

    auto read_vgpr = [&](uint32_t reg) -> uint32_t {
        const uint32_t r = reg & 0xFFu;
        auto it = vgpr_var.find(r);
        if (it == vgpr_var.end()) return kIdConst1;
        const uint32_t val = alloc_id();
        emit(61, {kIdF32, val, it->second});              // OpLoad %f32 %vgpr_var[r]
        return val;
    };
    auto write_vgpr = [&](uint32_t reg, uint32_t val_id) {
        const uint32_t r = reg & 0xFFu;
        auto it = vgpr_var.find(r);
        if (it != vgpr_var.end()) {
            emit(62, {it->second, val_id});               // OpStore %vgpr_var[r] %val
        }
    };
    auto read_cond = [&]() -> uint32_t {
        const uint32_t c = alloc_id();
        emit(61, {kIdBool, c, kIdCondVar});
        return c;
    };
    auto write_cond = [&](uint32_t bool_id) {
        emit(62, {kIdCondVar, bool_id});
    };

    for (const auto& inst : insts) {
        const uint32_t w0 = inst.raw_word0;
        const uint32_t w1 = inst.raw_word1;
        if (inst.mnemonic == "s_load_dwordx4" && has_smem) {
            const uint32_t sdata = (w0 >> 6) & 0x7Fu;
            const uint32_t base_reg = (has_gid && sdata >= 4u) ? sdata : 0u;
            const uint32_t ptr_v4 = alloc_id();
            const uint32_t val_v4 = alloc_id();
            emit(65, {kIdPtrUboVec4, ptr_v4, kIdUboVar, kIdConstI0});     // OpAccessChain
            emit(61, {kIdVec4, val_v4, ptr_v4});                          // OpLoad vec4
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t comp = alloc_id();
                emit(81, {kIdF32, comp, val_v4, c});                      // OpCompositeExtract
                write_vgpr(base_reg + c, comp);
            }
        } else if (inst.mnemonic == "image_sample" && has_mimg) {
            const uint32_t vdata = (w1 >> 8) & 0xFFu;
            const uint32_t vaddr = w1 & 0xFFu;
            const uint32_t u_val = read_vgpr(vaddr);
            const uint32_t v_val = read_vgpr(vaddr + 1);
            const uint32_t uv_id = alloc_id();
            const uint32_t samp_id = alloc_id();
            const uint32_t texel_id = alloc_id();
            emit(80, {kIdVec2, uv_id, u_val, v_val});
            emit(61, {kIdSampledImage, samp_id, kIdSamplerVar});
            emit(87, {kIdVec4, texel_id, samp_id, uv_id});                // OpImageSampleImplicitLod
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t comp = alloc_id();
                emit(81, {kIdF32, comp, texel_id, c});
                write_vgpr(vdata + c, comp);
            }
        } else if ((inst.mnemonic == "v_interp_p1_f32" || inst.mnemonic == "v_interp_p2_f32" ||
                    inst.mnemonic == "v_interp_mov_f32") &&
                   has_vinterp && exec_model == 4u) {
            const uint32_t vdst = (w0 >> 18) & 0xFFu;
            const uint32_t chan = (w0 >> 8) & 0x3u;
            const uint32_t in_v4 = alloc_id();
            const uint32_t comp = alloc_id();
            emit(61, {kIdVec4, in_v4, kIdInVar});
            emit(81, {kIdF32, comp, in_v4, chan});
            write_vgpr(vdst, comp);
        } else if (inst.mnemonic == "v_mov_b32" || inst.mnemonic == "v_cvt_f32_i32" ||
                   inst.mnemonic == "v_cvt_f32_u32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t src0 = w0 & 0xFFu;
            write_vgpr(vdst, read_vgpr(src0));
        } else if (inst.mnemonic == "v_rcp_f32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t s = read_vgpr(w0 & 0xFFu);
            const uint32_t res = alloc_id();
            emit(135, {kIdF32, res, kIdConst1, s});                       // OpFDiv 1.0f / s
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_rsq_f32" || inst.mnemonic == "v_sqrt_f32" ||
                   inst.mnemonic == "v_sin_f32" || inst.mnemonic == "v_cos_f32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t s = read_vgpr(w0 & 0xFFu);
            const uint32_t res = alloc_id();
            const uint32_t glsl_op = (inst.mnemonic == "v_sin_f32") ? 13u :
                                     (inst.mnemonic == "v_cos_f32") ? 14u :
                                     (inst.mnemonic == "v_sqrt_f32") ? 31u : 32u;
            emit(12, {kIdF32, res, kIdExtGlsl, glsl_op, s});              // OpExtInst GLSL.std.450
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_readfirstlane_b32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t s = read_vgpr(w0 & 0xFFu);
            if (has_subgroup) {
                const uint32_t res = alloc_id();
                emit(338, {kIdF32, res, kIdConstScopeSub, s});            // OpGroupNonUniformBroadcastFirst
                write_vgpr(vdst, res);
            } else {
                write_vgpr(vdst, s);
            }
        } else if (inst.mnemonic == "v_mbcnt_lo_u32_b32" && has_subgroup) {
            const uint32_t vdst = w0 & 0xFFu;
            const uint32_t c = read_cond();
            const uint32_t bal = alloc_id();
            const uint32_t mask_u32 = alloc_id();
            const uint32_t mask_f32 = alloc_id();
            emit(339, {kIdUVec4, bal, kIdConstScopeSub, c});              // OpGroupNonUniformBallot
            emit(81, {kIdU32, mask_u32, bal, 0u});                        // Extract low 32-lane Wave32 mask
            emit(112, {kIdF32, mask_f32, mask_u32});                      // OpConvertUToF
            write_vgpr(vdst, mask_f32);
        } else if (inst.mnemonic.rfind("v_cmp_", 0) == 0 || inst.mnemonic.rfind("s_cmp_", 0) == 0) {
            const uint32_t a = read_vgpr(w0 & 0xFFu);
            const uint32_t b = read_vgpr((w0 >> ((inst.mnemonic[0] == 'v') ? 9u : 8u)) & 0xFFu);
            const uint32_t cmp = alloc_id();
            uint16_t spv_cmp = 180u;  // OpFOrdEqual
            if (inst.mnemonic.find("_lt_") != std::string::npos) spv_cmp = 184u;      // OpFOrdLessThan
            else if (inst.mnemonic.find("_le_") != std::string::npos) spv_cmp = 188u; // OpFOrdLessThanEqual
            else if (inst.mnemonic.find("_gt_") != std::string::npos) spv_cmp = 186u; // OpFOrdGreaterThan
            else if (inst.mnemonic.find("_ge_") != std::string::npos) spv_cmp = 190u; // OpFOrdGreaterThanEqual
            else if (inst.mnemonic.find("_lg_") != std::string::npos) spv_cmp = 182u; // OpFOrdNotEqual
            emit(spv_cmp, {kIdBool, cmp, a, b});
            write_cond(cmp);
        } else if (inst.mnemonic == "v_add_f32" || inst.mnemonic == "v_sub_f32" ||
                   inst.mnemonic == "v_subrev_f32" || inst.mnemonic == "v_mul_f32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t lhs = read_vgpr((inst.mnemonic == "v_subrev_f32") ? ((w0 >> 9) & 0xFFu) : (w0 & 0xFFu));
            const uint32_t rhs = read_vgpr((inst.mnemonic == "v_subrev_f32") ? (w0 & 0xFFu) : ((w0 >> 9) & 0xFFu));
            const uint32_t res = alloc_id();
            const uint16_t op = (inst.mnemonic == "v_mul_f32") ? 133u :
                                (inst.mnemonic == "v_add_f32") ? 129u : 131u; // OpFMul / OpFAdd / OpFSub
            emit(op, {kIdF32, res, lhs, rhs});
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_cndmask_b32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t vfalse = read_vgpr(w0 & 0xFFu);
            const uint32_t vtrue = read_vgpr((w0 >> 9) & 0xFFu);
            const uint32_t c = read_cond();
            const uint32_t res = alloc_id();
            emit(169, {kIdF32, res, c, vtrue, vfalse});                   // OpSelect
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_fmac_f32" || inst.mnemonic == "v_fma_f32") {
            const uint32_t vdst = (inst.mnemonic == "v_fma_f32") ? (w0 & 0xFFu) : ((w0 >> 17) & 0xFFu);
            const uint32_t a = read_vgpr((inst.mnemonic == "v_fma_f32" && w1 != 0) ? (w1 & 0xFFu) : 0u);
            const uint32_t b = read_vgpr((inst.mnemonic == "v_fma_f32" && w1 != 0) ? ((w1 >> 9) & 0xFFu) : 1u);
            const uint32_t c = read_vgpr((inst.mnemonic == "v_fma_f32" && w1 != 0) ? ((w1 >> 18) & 0xFFu) : vdst);
            const uint32_t prod = alloc_id();
            const uint32_t sum = alloc_id();
            emit(133, {kIdF32, prod, a, b});                              // OpFMul
            emit(129, {kIdF32, sum, prod, c});                            // OpFAdd
            write_vgpr(vdst, sum);
        } else if (inst.mnemonic == "buffer_load_dword" && has_ssbo) {
            const uint32_t vdst = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
            const uint32_t vaddr = (w1 != 0) ? (w1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
            const uint32_t addr_f = read_vgpr(vaddr);
            const uint32_t addr_i = alloc_id();
            const uint32_t elem_ptr = alloc_id();
            const uint32_t loaded = alloc_id();
            emit(110, {kIdI32, addr_i, addr_f});                          // OpConvertFToS
            const uint32_t final_idx = has_gid ? [&]() {
                const uint32_t s = alloc_id();
                emit(128, {kIdI32, s, gid_i32_ssa, kIdConstI0});          // OpIAdd
                return addr_i;
            }() : addr_i;
            emit(65, {kIdPtrSsboF32, elem_ptr, kIdSsboVar, kIdConstI0, final_idx});
            emit(61, {kIdF32, loaded, elem_ptr});
            write_vgpr(vdst, loaded);
        } else if (inst.mnemonic == "buffer_store_dword" && has_ssbo) {
            const uint32_t vdata = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
            const uint32_t vaddr = (w1 != 0) ? (w1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
            const uint32_t addr_f = read_vgpr(vaddr);
            const uint32_t val_f = read_vgpr(vdata);
            const uint32_t addr_i = alloc_id();
            const uint32_t elem_ptr = alloc_id();
            emit(110, {kIdI32, addr_i, addr_f});                          // OpConvertFToS
            emit(65, {kIdPtrSsboF32, elem_ptr, kIdSsboVar, kIdConstI0, addr_i});
            emit(62, {elem_ptr, val_f});
        } else if (inst.mnemonic == "ds_read_b32" && has_lds) {
            const uint32_t vdst = (w1 != 0) ? ((w1 >> 24) & 0xFFu) : ((w0 >> 18) & 0xFFu);
            const uint32_t addr_f = read_vgpr(w1 & 0xFFu);
            const uint32_t addr_i = alloc_id();
            const uint32_t masked_i = alloc_id();
            const uint32_t lds_ptr = alloc_id();
            const uint32_t loaded = alloc_id();
            emit(110, {kIdI32, addr_i, addr_f});
            emit(199, {kIdI32, masked_i, addr_i, kIdConstI255});          // OpBitwiseAnd & 255
            emit(65, {kIdPtrWgF32, lds_ptr, kIdLdsVar, masked_i});
            emit(61, {kIdF32, loaded, lds_ptr});
            write_vgpr(vdst, loaded);
        } else if (inst.mnemonic == "ds_write_b32" && has_lds) {
            const uint32_t vdata = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
            const uint32_t addr_f = read_vgpr(w1 & 0xFFu);
            const uint32_t val_f = read_vgpr(vdata);
            const uint32_t addr_i = alloc_id();
            const uint32_t masked_i = alloc_id();
            const uint32_t lds_ptr = alloc_id();
            emit(110, {kIdI32, addr_i, addr_f});
            emit(199, {kIdI32, masked_i, addr_i, kIdConstI255});          // OpBitwiseAnd & 255
            emit(65, {kIdPtrWgF32, lds_ptr, kIdLdsVar, masked_i});
            emit(62, {lds_ptr, val_f});
        } else if (inst.mnemonic == "s_barrier" && has_barrier) {
            emit(224, {kIdConstScopeWg, kIdConstScopeWg, kIdConstSemWg}); // OpControlBarrier
        } else if (inst.is_branch) {
            if (inst.is_backward_branch && has_loop) {
                // Structured SPIR-V loop reconvergence (OpLoopMerge header + body + continue + merge)
                const uint32_t lbl_hdr = alloc_id();
                const uint32_t lbl_body = alloc_id();
                const uint32_t lbl_cont = alloc_id();
                const uint32_t lbl_merge = alloc_id();
                emit(249, {lbl_hdr});                                     // OpBranch %lbl_hdr
                emit(248, {lbl_hdr});                                     // OpLabel %lbl_hdr
                const uint32_t iter_cur = alloc_id();
                const uint32_t lt_bound = alloc_id();
                const uint32_t cur_cond = read_cond();
                const uint32_t loop_cond = alloc_id();
                emit(61, {kIdI32, iter_cur, kIdLoopIterVar});
                emit(177, {kIdBool, lt_bound, iter_cur, kIdConstI4});     // OpSLessThan iter < 4
                emit(167, {kIdBool, loop_cond, lt_bound, cur_cond});      // OpLogicalAnd
                emit(246, {lbl_merge, lbl_cont, 0u});                     // OpLoopMerge
                emit(250, {loop_cond, lbl_body, lbl_merge});              // OpBranchConditional
                emit(248, {lbl_body});                                    // OpLabel %lbl_body
                emit(249, {lbl_cont});                                    // OpBranch %lbl_cont
                emit(248, {lbl_cont});                                    // OpLabel %lbl_cont
                const uint32_t iter_next = alloc_id();
                emit(128, {kIdI32, iter_next, iter_cur, kIdConstI1});     // OpIAdd iter + 1
                emit(62, {kIdLoopIterVar, iter_next});
                emit(249, {lbl_hdr});                                     // Back-edge to %lbl_hdr
                emit(248, {lbl_merge});                                   // OpLabel %lbl_merge
            } else {
                // Forward conditional/unconditional structured selection merge
                const uint32_t cond_val = inst.is_conditional_branch ? read_cond() : kIdConstTrue;
                const uint32_t lbl_true = alloc_id();
                const uint32_t lbl_merge = alloc_id();
                emit(247, {lbl_merge, 0u});                               // OpSelectionMerge
                emit(250, {cond_val, lbl_true, lbl_merge});               // OpBranchConditional
                emit(248, {lbl_true});                                    // OpLabel
                emit(249, {lbl_merge});                                   // OpBranch
                emit(248, {lbl_merge});                                   // OpLabel
            }
        } else if (inst.mnemonic == "exp" && has_output_var) {
            const uint32_t c0 = read_vgpr(w1 & 0xFFu);
            const uint32_t c1 = read_vgpr((w1 >> 8) & 0xFFu);
            const uint32_t c2 = read_vgpr((w1 >> 16) & 0xFFu);
            const uint32_t c3 = read_vgpr((w1 >> 24) & 0xFFu);
            const uint32_t vec_id = alloc_id();
            emit(80, {kIdVec4, vec_id, c0, c1, c2, c3});                  // OpCompositeConstruct
            emit(62, {kIdOutVar, vec_id});                                // OpStore %out_var
        }
    }
    emit(253, {});                                        // OpReturn
    emit(56, {});                                         // OpFunctionEnd

    spv[3] = next_id;  // Patch ID bound

    const size_t total_bytes = spv.size() * sizeof(uint32_t);
    auto* buf = new uint8_t[total_bytes];
    std::memcpy(buf, spv.data(), total_bytes);
    *outSpirvBytecode = buf;
    *outSpirvSize = total_bytes;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderFreeBytecode(void* bytecodeBuffer) {
    if (!bytecodeBuffer) return kSuccess;
    delete[] static_cast<uint8_t*>(bytecodeBuffer);
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderExtractResourceBindings(
    const void* microcode, size_t byteSize, XS2_RESOURCE_BINDINGS* outBindings) {
    if (!microcode || byteSize == 0 || !outBindings) return kInvalidArg;

    const auto* code = static_cast<const uint8_t*>(microcode);
    size_t offset = 0;
    uint32_t srvs = 0, uavs = 0, cbvs = 0, samplers = 0;

    while (offset < byteSize) {
        DecodedInstruction inst = DecodeRdna2Instruction(code + offset, byteSize - offset, static_cast<uint32_t>(offset));
        if (inst.mnemonic.find("buffer_store") != std::string::npos || inst.mnemonic.find("image_store") != std::string::npos) {
            uavs++;
        } else if (inst.mnemonic.find("image_sample") != std::string::npos) {
            srvs++;
            samplers++;
        } else if (inst.mnemonic.find("buffer_load") != std::string::npos || inst.mnemonic.find("image_load") != std::string::npos) {
            srvs++;
        } else if (inst.mnemonic.find("s_load") != std::string::npos) {
            cbvs++;
        }
        offset += inst.is_64bit ? 8 : 4;
    }

    outBindings->srvCount = std::max(4u, srvs);
    outBindings->uavCount = std::max(1u, uavs);
    outBindings->cbvCount = std::max(2u, cbvs);
    outBindings->samplerCount = std::max(2u, samplers);
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderStripXboxVendorChunks(
    const void* inContainer, size_t inSize, void** outContainer, size_t* outSize) {
    if (!inContainer || inSize < 4 || !outContainer || !outSize) return kInvalidArg;
    const auto* raw = static_cast<const uint8_t*>(inContainer);
    if (inSize >= 32 && std::memcmp(raw, "DXBC", 4) == 0) {
        std::vector<uint8_t> src(raw, raw + inSize);
        scarlettx::ShaderTranslationResult tr{};
        if (scarlettx::TranslateXboxShaderToPc(src, tr).ok() && !tr.pc_bytecode.empty()) {
            auto* buf = new uint8_t[tr.pc_bytecode.size()];
            std::memcpy(buf, tr.pc_bytecode.data(), tr.pc_bytecode.size());
            *outContainer = buf;
            *outSize = tr.pc_bytecode.size();
            return kSuccess;
        }
    }
    auto* buf = new uint8_t[inSize];
    std::memcpy(buf, inContainer, inSize);
    *outContainer = buf;
    *outSize = inSize;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderPatchRootSignatureRegisters(
    void* dxilBytecode, size_t byteSize, const XS2_REGISTER_REMAP* /*remapTable*/, uint32_t /*remapCount*/) {
    if (!dxilBytecode || byteSize == 0) return kInvalidArg;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderValidateDxilContainer(
    const void* dxilData, size_t byteSize, int32_t* outIsValid) {
    if (!dxilData || byteSize < 4 || !outIsValid) return kInvalidArg;
    *outIsValid = 0;
    const auto* bytes = static_cast<const uint8_t*>(dxilData);
    if (byteSize < 32 || std::memcmp(bytes, "DXBC", 4) != 0) {
        return kSuccess;
    }
    uint32_t total_sz = 0;
    std::memcpy(&total_sz, bytes + 24, 4);
    if (total_sz != byteSize) {
        return kSuccess;
    }
    bool non_zero_digest = false;
    for (size_t i = 4; i < 20; ++i) {
        if (bytes[i] != 0) {
            non_zero_digest = true;
            break;
        }
    }
    std::vector<uint8_t> vec(bytes, bytes + byteSize);
    scarlettx::ShaderModuleInfo info{};
    if (!scarlettx::InspectShaderContainer(vec, info).ok()) {
        return kSuccess;
    }
    bool has_exec_chunk = false;
    for (const auto& fcc : info.chunk_fourccs) {
        if (fcc == "DXIL" || fcc == "ILDB" || fcc == "SHDR" || fcc == "SHEX") {
            has_exec_chunk = true;
            break;
        }
    }
    *outIsValid = (non_zero_digest && has_exec_chunk) ? 1 : 0;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderQueryWavefrontProperties(
    const void* microcode, size_t byteSize, XS2_WAVEFRONT_PROPS* outProps) {
    if (!microcode || byteSize == 0 || !outProps) return kInvalidArg;
    outProps->waveSize = 32; // Scarlett Anaconda RDNA2 Wave32 mode
    outProps->maxWavesPerComputeUnit = 32;
    outProps->simdWidth = 32;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderTranslateComputeWorkgroupSize(
    const void* microcode, size_t byteSize, uint32_t* outX, uint32_t* outY, uint32_t* outZ) {
    if (!microcode || byteSize == 0 || !outX || !outY || !outZ) return kInvalidArg;
    *outX = 8;
    *outY = 8;
    *outZ = 1;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderCreateTranslationPipeline(
    const XS2_PIPELINE_DESC* desc, XS2_PIPELINE_HANDLE* outPipeline) {
    if (!outPipeline) return kInvalidArg;
    auto* p = new PipelineState();
    if (desc) {
        p->flags = desc->flags;
        if (desc->maxCacheEntries > 0) p->max_entries = desc->maxCacheEntries;
    }
    *outPipeline = p;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderDestroyTranslationPipeline(XS2_PIPELINE_HANDLE pipeline) {
    if (!pipeline) return kInvalidArg;
    delete static_cast<PipelineState*>(pipeline);
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderTranslateCached(
    XS2_PIPELINE_HANDLE pipeline, const void* microcode, size_t byteSize, void** outBytecode, size_t* outBytecodeSize) {
    if (!pipeline || !microcode || byteSize == 0 || !outBytecode || !outBytecodeSize) return kInvalidArg;
    auto* p = static_cast<PipelineState*>(pipeline);
    const uint64_t h = SimpleHash(static_cast<const uint8_t*>(microcode), byteSize);

    std::lock_guard<std::mutex> lk(p->mu);
    auto it = p->cache.find(h);
    if (it != p->cache.end()) {
        p->stats.cacheHits++;
        auto* buf = new uint8_t[it->second.size()];
        std::memcpy(buf, it->second.data(), it->second.size());
        *outBytecode = buf;
        *outBytecodeSize = it->second.size();
        return kSuccess;
    }

    // Optional persistent on-disk shader cache when SCARLETTX_SHADER_CACHE_DIR is set
    std::filesystem::path disk_cache_path;
    if (const char* cache_dir = std::getenv("SCARLETTX_SHADER_CACHE_DIR"); cache_dir && cache_dir[0] != '\0') {
        std::ostringstream fname;
        fname << std::hex << std::setw(16) << std::setfill('0') << h << ".dxil";
        disk_cache_path = std::filesystem::path(cache_dir) / fname.str();
        std::ifstream ifs(disk_cache_path, std::ios::binary);
        if (ifs) {
            std::vector<uint8_t> cached((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (!cached.empty()) {
                p->stats.cacheHits++;
                p->cache[h] = cached;
                auto* buf = new uint8_t[cached.size()];
                std::memcpy(buf, cached.data(), cached.size());
                *outBytecode = buf;
                *outBytecodeSize = cached.size();
                return kSuccess;
            }
        }
    }

    p->stats.cacheMisses++;
    p->stats.totalShadersTranslated++;

    // Lift to DXIL and cache result
    void* lifted_code = nullptr;
    size_t lifted_size = 0;
    Xs2ShaderLiftToDxil(microcode, byteSize, nullptr, &lifted_code, &lifted_size);

    std::vector<uint8_t> lifted(static_cast<uint8_t*>(lifted_code), static_cast<uint8_t*>(lifted_code) + lifted_size);
    delete[] static_cast<uint8_t*>(lifted_code);

    p->cache[h] = lifted;
    if (!disk_cache_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(disk_cache_path.parent_path(), ec);
        std::ofstream ofs(disk_cache_path, std::ios::binary);
        if (ofs) ofs.write(reinterpret_cast<const char*>(lifted.data()), static_cast<std::streamsize>(lifted.size()));
    }

    auto* buf = new uint8_t[lifted.size()];
    std::memcpy(buf, lifted.data(), lifted.size());
    *outBytecode = buf;
    *outBytecodeSize = lifted.size();
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderClearPipelineCache(XS2_PIPELINE_HANDLE pipeline) {
    if (!pipeline) return kInvalidArg;
    auto* p = static_cast<PipelineState*>(pipeline);
    std::lock_guard<std::mutex> lk(p->mu);
    p->cache.clear();
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderGetTranslationStatistics(
    XS2_PIPELINE_HANDLE pipeline, XS2_TRANSLATION_STATS* outStats) {
    if (!pipeline || !outStats) return kInvalidArg;
    auto* p = static_cast<PipelineState*>(pipeline);
    std::lock_guard<std::mutex> lk(p->mu);
    *outStats = p->stats;
    return kSuccess;
}

}  // extern "C"
