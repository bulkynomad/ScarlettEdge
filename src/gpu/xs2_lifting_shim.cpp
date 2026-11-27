#define SCARLETTX_BUILDING_XS2 1
#include "scarlettx/gpu/xs2_abi.h"
#include "scarlettx/gpu/shader_ir.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);

uint32_t ReadLe32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void WriteLe32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

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
    uint32_t dword_pc = 0;
    uint32_t next_dword_pc = 1;
    uint32_t inst_bytes = 4;
    uint32_t raw_word0 = 0;
    uint32_t raw_word1 = 0;
    uint32_t literal_u32 = 0;
    uint32_t nsa_dwords = 0;
    uint32_t nsa_words[3]{};
    uint32_t mem_dwords = 1;
    bool is_64bit = false;
    bool has_literal = false;
    bool has_dpp = false;
    bool has_sdwa = false;
    bool is_branch = false;
    bool is_conditional_branch = false;
    bool is_backward_branch = false;
    bool is_subgroup_op = false;
    int32_t branch_simm16 = 0;
    Rdna2InstClass inst_class = Rdna2InstClass::UNKNOWN;
    std::string mnemonic;
    std::string operands;
};

std::string FormatValuSrc(uint32_t src9, const DecodedInstruction& inst) {
    if (src9 == 0x0FFu && inst.has_literal) {
        std::ostringstream os;
        os << "0x" << std::hex << inst.literal_u32;
        return os.str();
    }
    if (src9 == 0x0FAu && inst.has_dpp) {
        const uint32_t vreg = inst.raw_word1 & 0xFFu;
        const uint32_t dpp_ctrl = (inst.raw_word1 >> 8) & 0x1FFu;
        std::ostringstream os;
        os << "v" << vreg;
        if (dpp_ctrl < 0x100u) {
            os << " quad_perm:[" << (dpp_ctrl & 3u) << "," << ((dpp_ctrl >> 2) & 3u) << ","
               << ((dpp_ctrl >> 4) & 3u) << "," << ((dpp_ctrl >> 6) & 3u) << "]";
        } else {
            os << " dpp_ctrl:0x" << std::hex << dpp_ctrl;
        }
        return os.str();
    }
    if (src9 == 0x0F9u && inst.has_sdwa) {
        const uint32_t vreg = inst.raw_word1 & 0xFFu;
        const uint32_t src0_sel = (inst.raw_word1 >> 16) & 0x7u;
        return "v" + std::to_string(vreg) + " src0_sel:" + std::to_string(src0_sel);
    }
    if (src9 >= 256u) return "v" + std::to_string((src9 - 256u) & 0xFFu);
    if (src9 >= 128u && src9 <= 192u) return std::to_string(src9 - 128u);
    if (src9 >= 193u && src9 <= 208u) return std::to_string(-static_cast<int32_t>(src9 - 192u));
    if (src9 == 240u) return "0.5";
    if (src9 == 241u) return "-0.5";
    if (src9 == 242u) return "1.0";
    if (src9 == 243u) return "-1.0";
    if (src9 == 244u) return "2.0";
    if (src9 == 245u) return "-2.0";
    if (src9 == 246u) return "4.0";
    if (src9 == 247u) return "-4.0";
    return "s" + std::to_string(src9);
}

std::string FormatSaluSrc(uint32_t ssrc, const DecodedInstruction& inst) {
    if (ssrc == 0xFFu && inst.has_literal) {
        std::ostringstream os;
        os << "0x" << std::hex << inst.literal_u32;
        return os.str();
    }
    if (ssrc >= 128u && ssrc <= 192u) return std::to_string(ssrc - 128u);
    if (ssrc >= 193u && ssrc <= 208u) return std::to_string(-static_cast<int32_t>(ssrc - 192u));
    return "s" + std::to_string(ssrc);
}

// Clean-room full-width RDNA2 ISA instruction decoder (GFX10.3 / Xbox Series X|S Scarlett)
DecodedInstruction DecodeRdna2Instruction(const uint8_t* code, size_t remaining, uint32_t offset) {
    DecodedInstruction inst{};
    inst.offset = offset;
    inst.dword_pc = offset / 4u;
    if (remaining < 4) {
        inst.inst_bytes = static_cast<uint32_t>(remaining);
        inst.next_dword_pc = inst.dword_pc + 1u;
        inst.mnemonic = "s_nop";
        return inst;
    }

    const uint32_t w0 = ReadLe32(code);
    inst.raw_word0 = w0;

    auto finalize = [&]() {
        if (inst.inst_bytes > remaining) {
            inst.inst_bytes = static_cast<uint32_t>(remaining & ~size_t(3u));
            if (inst.inst_bytes == 0) inst.inst_bytes = 4u;
        }
        inst.is_64bit = (inst.inst_bytes >= 8u);
        inst.next_dword_pc = inst.dword_pc + (inst.inst_bytes / 4u);
    };

    // 1. SOPP (Scalar Operation - Program Control): bits 31..23 == 0b101111111 (0xBF800000)
    if ((w0 & 0xFF800000u) == 0xBF800000u) {
        inst.inst_class = Rdna2InstClass::CONTROL;
        const uint32_t op = (w0 >> 16) & 0x7Fu;
        const uint16_t simm16 = static_cast<uint16_t>(w0 & 0xFFFFu);
        const int16_t rel = static_cast<int16_t>(simm16);
        inst.branch_simm16 = rel;
        if (op == 0x01 || w0 == 0xBF800000u) {
            inst.mnemonic = "s_endpgm";
        } else if (op == 0x00) {
            inst.mnemonic = "s_nop";
            inst.operands = std::to_string(simm16);
        } else if (op == 0x0A) {
            inst.mnemonic = "s_barrier";
        } else if (op == 0x0C) {
            inst.mnemonic = "s_waitcnt";
            std::ostringstream ss;
            ss << "vmcnt(" << (simm16 & 0x3Fu) << ") lgkmcnt(" << ((simm16 >> 8) & 0x3Fu) << ")";
            inst.operands = ss.str();
        } else if (op == 0x02 || (op >= 0x04 && op <= 0x09)) {
            inst.is_branch = true;
            inst.is_conditional_branch = (op != 0x02);
            inst.is_backward_branch = (rel <= -1);
            if (op == 0x02) inst.mnemonic = "s_branch";
            else if (op == 0x04) inst.mnemonic = "s_cbranch_scc0";
            else if (op == 0x05) inst.mnemonic = "s_cbranch_scc1";
            else if (op == 0x06) inst.mnemonic = "s_cbranch_vccz";
            else if (op == 0x07) inst.mnemonic = "s_cbranch_vccnz";
            else if (op == 0x08) inst.mnemonic = "s_cbranch_execz";
            else inst.mnemonic = "s_cbranch_execnz";
            const int64_t target_pc = static_cast<int64_t>(offset) + 4 + static_cast<int64_t>(rel) * 4;
            std::ostringstream ss;
            ss << "label_0x" << std::hex << std::max<int64_t>(0, target_pc);
            inst.operands = ss.str();
        } else {
            inst.mnemonic = "s_sopp_op" + std::to_string(op);
            inst.operands = "0x" + std::to_string(simm16);
        }
        finalize();
        return inst;
    }

    // 2. SOPC (Scalar Compare -> SCC): bits 31..23 == 0b101111110 (0xBF000000)
    if ((w0 & 0xFF800000u) == 0xBF000000u) {
        inst.inst_class = Rdna2InstClass::SALU;
        const uint32_t op = (w0 >> 16) & 0x7Fu;
        const uint32_t ssrc1 = (w0 >> 8) & 0xFFu;
        const uint32_t ssrc0 = w0 & 0xFFu;
        if ((ssrc0 == 0xFFu || ssrc1 == 0xFFu) && remaining >= 8) {
            inst.has_literal = true;
            inst.literal_u32 = ReadLe32(code + 4);
            inst.raw_word1 = inst.literal_u32;
            inst.inst_bytes = 8u;
        }
        switch (op) {
            case 0x00: inst.mnemonic = "s_cmp_eq_i32"; break;
            case 0x01: inst.mnemonic = "s_cmp_lg_i32"; break;
            case 0x02: inst.mnemonic = "s_cmp_gt_i32"; break;
            case 0x03: inst.mnemonic = "s_cmp_ge_i32"; break;
            case 0x04: inst.mnemonic = "s_cmp_lt_i32"; break;
            case 0x05: inst.mnemonic = "s_cmp_le_i32"; break;
            case 0x06: inst.mnemonic = "s_cmp_eq_u32"; break;
            case 0x07: inst.mnemonic = "s_cmp_lg_u32"; break;
            case 0x08: inst.mnemonic = "s_cmp_gt_u32"; break;
            case 0x09: inst.mnemonic = "s_cmp_ge_u32"; break;
            case 0x0A: inst.mnemonic = "s_cmp_lt_u32"; break;
            case 0x0B: inst.mnemonic = "s_cmp_le_u32"; break;
            default:   inst.mnemonic = "s_cmp_lt_i32"; break;
        }
        inst.operands = FormatSaluSrc(ssrc0, inst) + ", " + FormatSaluSrc(ssrc1, inst);
        finalize();
        return inst;
    }

    // 3. SOP1 (Scalar 1-Operand / Wave32 mask ops): bits 31..23 == 0b101111101 (0xBE800000)
    if ((w0 & 0xFF800000u) == 0xBE800000u) {
        inst.inst_class = Rdna2InstClass::SALU;
        const uint32_t sdst = (w0 >> 16) & 0x7Fu;
        const uint32_t op = (w0 >> 8) & 0xFFu;
        const uint32_t ssrc0 = w0 & 0xFFu;
        if (ssrc0 == 0xFFu && remaining >= 8) {
            inst.has_literal = true;
            inst.literal_u32 = ReadLe32(code + 4);
            inst.raw_word1 = inst.literal_u32;
            inst.inst_bytes = 8u;
        }
        if (op == 0x00 || op == 0x03) inst.mnemonic = "s_mov_b32";
        else if (op == 0x04 || op == 0x07) inst.mnemonic = "s_not_b32";
        else if (op == 0x24 || op == 0x3C) { inst.mnemonic = "s_and_saveexec_b32"; inst.is_subgroup_op = true; }
        else inst.mnemonic = "s_sop1_op" + std::to_string(op);
        inst.operands = "s" + std::to_string(sdst) + ", " + FormatSaluSrc(ssrc0, inst);
        finalize();
        return inst;
    }

    // 4. SOPK (Scalar 16-bit immediate ops): 0xB0000000 .. 0xBE7FFFFF
    if ((w0 & 0xF0000000u) == 0xB0000000u && (w0 & 0xFF800000u) < 0xBE800000u) {
        inst.inst_class = Rdna2InstClass::SALU;
        const uint32_t sopk = (w0 >> 23) & 0x1Fu;
        const uint32_t sdst = (w0 >> 16) & 0x7Fu;
        const int16_t imm = static_cast<int16_t>(w0 & 0xFFFFu);
        inst.branch_simm16 = imm;
        if (sopk == 0x14u && remaining >= 8) {
            inst.has_literal = true;
            inst.literal_u32 = ReadLe32(code + 4);
            inst.raw_word1 = inst.literal_u32;
            inst.inst_bytes = 8u;
            inst.mnemonic = "s_setreg_imm32_b32";
        } else if (sopk == 0x00u) inst.mnemonic = "s_movk_i32";
        else if (sopk == 0x03u) inst.mnemonic = "s_cmpk_eq_i32";
        else if (sopk == 0x04u) inst.mnemonic = "s_cmpk_lg_i32";
        else if (sopk == 0x0Fu) inst.mnemonic = "s_addk_i32";
        else if (sopk == 0x10u) inst.mnemonic = "s_mulk_i32";
        else inst.mnemonic = "s_sopk_op" + std::to_string(sopk);
        inst.operands = "s" + std::to_string(sdst) + ", " + std::to_string(imm);
        finalize();
        return inst;
    }

    // 5. SOP2 (Scalar 2-Operand ALU): 0x80000000 .. 0xAFFFFFFF
    if ((w0 & 0xC0000000u) == 0x80000000u) {
        inst.inst_class = Rdna2InstClass::SALU;
        const uint32_t sop2 = (w0 >> 23) & 0x7Fu;
        const uint32_t sdst = (w0 >> 16) & 0x7Fu;
        const uint32_t ssrc1 = (w0 >> 8) & 0xFFu;
        const uint32_t ssrc0 = w0 & 0xFFu;
        if ((ssrc0 == 0xFFu || ssrc1 == 0xFFu) && remaining >= 8) {
            inst.has_literal = true;
            inst.literal_u32 = ReadLe32(code + 4);
            inst.raw_word1 = inst.literal_u32;
            inst.inst_bytes = 8u;
        }
        switch (sop2) {
            case 0x00: inst.mnemonic = "s_add_u32"; break;
            case 0x01: inst.mnemonic = "s_sub_u32"; break;
            case 0x02: inst.mnemonic = "s_add_i32"; break;
            case 0x03: inst.mnemonic = "s_sub_i32"; break;
            case 0x06: inst.mnemonic = "s_min_i32"; break;
            case 0x07: inst.mnemonic = "s_min_u32"; break;
            case 0x08: inst.mnemonic = "s_max_i32"; break;
            case 0x09: inst.mnemonic = "s_max_u32"; break;
            case 0x0A: inst.mnemonic = "s_cselect_b32"; break;
            case 0x0E: inst.mnemonic = "s_and_b32"; break;
            case 0x10: inst.mnemonic = "s_or_b32"; break;
            case 0x12: inst.mnemonic = "s_xor_b32"; break;
            case 0x1C: inst.mnemonic = "s_lshl_b32"; break;
            case 0x1E: inst.mnemonic = "s_lshr_b32"; break;
            case 0x20: inst.mnemonic = "s_ashr_i32"; break;
            case 0x24: inst.mnemonic = "s_mul_i32"; break;
            default:   inst.mnemonic = "s_sop2_op" + std::to_string(sop2); break;
        }
        inst.operands = "s" + std::to_string(sdst) + ", " + FormatSaluSrc(ssrc0, inst) + ", " + FormatSaluSrc(ssrc1, inst);
        finalize();
        return inst;
    }

    // 6. SMEM (Scalar Memory / Uniform Buffers): bits 31..26 == 0b111101 (0xF4000000)
    if ((w0 & 0xFC000000u) == 0xF4000000u) {
        inst.inst_class = Rdna2InstClass::SALU;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
        }
        const uint32_t smem_op = (w0 >> 18) & 0xFFu;
        const uint32_t sdata = (w0 >> 6) & 0x7Fu;
        const uint32_t sbase = w0 & 0x3Fu;
        if (smem_op == 0x01u) {
            inst.mnemonic = "s_load_dwordx2";
            inst.mem_dwords = 2;
        } else if (smem_op == 0x08u) {
            inst.mnemonic = "s_buffer_load_dword";
            inst.mem_dwords = 1;
        } else {
            inst.mnemonic = "s_load_dwordx4";
            inst.mem_dwords = 4;
        }
        std::ostringstream ss;
        ss << "s[" << sdata << ":" << (sdata + inst.mem_dwords - 1u) << "], s["
           << (sbase * 2u) << ":" << (sbase * 2u + 1u) << "], 0x" << std::hex << (inst.raw_word1 & 0xFFFFFu);
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    auto decode_valu_ext = [&](uint32_t src0, bool implied_lit) {
        if (src0 == 0x0FFu || implied_lit) {
            if (remaining >= 8) {
                inst.has_literal = true;
                inst.literal_u32 = ReadLe32(code + 4);
                inst.raw_word1 = inst.literal_u32;
                inst.inst_bytes = 8u;
            }
        } else if (src0 == 0x0FAu || src0 == 0x0E9u || src0 == 0x0EAu) {
            if (remaining >= 8) {
                inst.has_dpp = true;
                inst.raw_word1 = ReadLe32(code + 4);
                inst.inst_bytes = 8u;
            }
        } else if (src0 == 0x0F9u) {
            if (remaining >= 8) {
                inst.has_sdwa = true;
                inst.raw_word1 = ReadLe32(code + 4);
                inst.inst_bytes = 8u;
            }
        }
    };

    // 7. VOP1 (Vector 1-Operand + Wave32 v_readfirstlane_b32): bits 31..25 == 0b0111111 (0x7E000000)
    if ((w0 & 0xFE000000u) == 0x7E000000u) {
        inst.inst_class = Rdna2InstClass::VALU;
        const uint32_t vdst = (w0 >> 17) & 0xFFu;
        const uint32_t op = (w0 >> 9) & 0xFFu;
        const uint32_t src0 = w0 & 0x1FFu;
        decode_valu_ext(src0, false);

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
        else if (op == 0x37) inst.mnemonic = "v_not_b32";
        else inst.mnemonic = "v_vop1_op" + std::to_string(op);

        std::ostringstream ss;
        if (op == 0x02) ss << "s" << vdst;
        else ss << "v" << vdst;
        ss << ", " << ((src0 < 256u && !inst.has_literal && !inst.has_dpp && !inst.has_sdwa)
                           ? ("v" + std::to_string(src0))
                           : FormatValuSrc(src0, inst));
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // 8. VOPC (Vector Compare -> Wave32 VCC): bits 31..25 == 0b0111110 (0x7C000000)
    if ((w0 & 0xFE000000u) == 0x7C000000u) {
        inst.inst_class = Rdna2InstClass::VALU;
        const uint32_t op = (w0 >> 17) & 0xFFu;
        const uint32_t vsrc1 = (w0 >> 9) & 0xFFu;
        const uint32_t src0 = w0 & 0x1FFu;
        decode_valu_ext(src0, false);

        const uint32_t cmp_kind = op & 0x07u;
        const char* suffix = (op >= 0xC0u) ? "_u32" : ((op >= 0x80u) ? "_i32" : "_f32");
        if (cmp_kind == 0x01) inst.mnemonic = std::string("v_cmp_lt") + suffix;
        else if (cmp_kind == 0x02) inst.mnemonic = std::string("v_cmp_eq") + suffix;
        else if (cmp_kind == 0x03) inst.mnemonic = std::string("v_cmp_le") + suffix;
        else if (cmp_kind == 0x04) inst.mnemonic = std::string("v_cmp_gt") + suffix;
        else if (cmp_kind == 0x05) inst.mnemonic = std::string("v_cmp_lg") + suffix;
        else inst.mnemonic = std::string("v_cmp_ge") + suffix;

        std::ostringstream ss;
        ss << "vcc_lo, " << FormatValuSrc(src0, inst) << ", v" << vsrc1;
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // 9. VOP2 (Vector Operation 2-operand): bit 31 == 0
    if ((w0 & 0x80000000u) == 0) {
        inst.inst_class = Rdna2InstClass::VALU;
        const uint32_t vop2_op = (w0 >> 25) & 0x3Fu;
        const uint32_t vdst = (w0 >> 17) & 0xFFu;
        const uint32_t vsrc1 = (w0 >> 9) & 0xFFu;
        const uint32_t src0 = w0 & 0x1FFu;
        const bool implied_lit = (vop2_op == 0x2Cu || vop2_op == 0x2Du || vop2_op == 0x37u || vop2_op == 0x38u);
        decode_valu_ext(src0, implied_lit);

        switch (vop2_op) {
            case 0x01:
            case 0x18: inst.mnemonic = "v_cndmask_b32"; break;
            case 0x03: inst.mnemonic = "v_add_f32"; break;
            case 0x04: inst.mnemonic = "v_sub_f32"; break;
            case 0x05: inst.mnemonic = "v_subrev_f32"; break;
            case 0x08: inst.mnemonic = "v_mul_f32"; break;
            case 0x09: inst.mnemonic = "v_mul_i32_i24"; break;
            case 0x0A: inst.mnemonic = "v_min_f32"; break;
            case 0x0B: inst.mnemonic = "v_max_f32"; break;
            case 0x0C: inst.mnemonic = "v_min_i32"; break;
            case 0x0D: inst.mnemonic = "v_max_i32"; break;
            case 0x0E: inst.mnemonic = "v_min_u32"; break;
            case 0x0F: inst.mnemonic = "v_max_u32"; break;
            case 0x10: inst.mnemonic = "v_lshrrev_b32"; break;
            case 0x11: inst.mnemonic = "v_ashrrev_i32"; break;
            case 0x12:
            case 0x1A: inst.mnemonic = "v_lshlrev_b32"; break;
            case 0x13:
            case 0x1B: inst.mnemonic = "v_and_b32"; break;
            case 0x14:
            case 0x1C: inst.mnemonic = "v_or_b32"; break;
            case 0x15:
            case 0x1D: inst.mnemonic = "v_xor_b32"; break;
            case 0x19:
            case 0x2B: inst.mnemonic = "v_fmac_f32"; break;
            case 0x25:
            case 0x28: inst.mnemonic = "v_add_nc_u32"; break;
            case 0x26:
            case 0x29: inst.mnemonic = "v_sub_nc_u32"; break;
            case 0x27: inst.mnemonic = "v_subrev_nc_u32"; break;
            case 0x2C: inst.mnemonic = "v_fmamk_f32"; break;
            case 0x2D: inst.mnemonic = "v_fmaak_f32"; break;
            default:   inst.mnemonic = "v_vop2_op" + std::to_string(vop2_op); break;
        }

        std::ostringstream ss;
        ss << "v" << vdst << ", " << FormatValuSrc(src0, inst) << ", v" << vsrc1;
        if (implied_lit && inst.has_literal && src0 != 0x0FFu) {
            ss << ", 0x" << std::hex << inst.literal_u32;
        }
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // 10. VOP3P (Packed FP16 / mad_mix 64-bit ops, +4 literal -> 12 bytes): 0xCC000000
    if ((w0 & 0xFC000000u) == 0xCC000000u) {
        inst.inst_class = Rdna2InstClass::VALU;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
            const uint32_t s0 = inst.raw_word1 & 0x1FFu;
            const uint32_t s1 = (inst.raw_word1 >> 9) & 0x1FFu;
            const uint32_t s2 = (inst.raw_word1 >> 18) & 0x1FFu;
            if ((s0 == 0x0FFu || s1 == 0x0FFu || s2 == 0x0FFu) && remaining >= 12) {
                inst.has_literal = true;
                inst.literal_u32 = ReadLe32(code + 8);
                inst.inst_bytes = 12u;
            }
        }
        const uint32_t vdst = w0 & 0xFFu;
        const uint32_t op = (w0 >> 16) & 0x7Fu;
        if (op == 0x12u) inst.mnemonic = "v_pk_mul_f16";
        else if (op == 0x13u) inst.mnemonic = "v_pk_add_f16";
        else if (op == 0x14u) inst.mnemonic = "v_pk_sub_f16";
        else if (op == 0x20u || op == 0x00u) inst.mnemonic = "v_fma_mix_f32";
        else inst.mnemonic = "v_pk_fma_f16";

        const uint32_t s0 = inst.raw_word1 & 0x1FFu;
        const uint32_t s1 = (inst.raw_word1 >> 9) & 0x1FFu;
        const uint32_t s2 = (inst.raw_word1 >> 18) & 0x1FFu;
        inst.operands = "v" + std::to_string(vdst) + ", " + FormatValuSrc(s0, inst) + ", " +
                        FormatValuSrc(s1, inst) + ", " + FormatValuSrc(s2, inst);
        finalize();
        return inst;
    }

    // 11. VOP3 (Vector Operation 3-operand, +4 literal -> 12 bytes): 0xD4000000
    if ((w0 & 0xFC000000u) == 0xD4000000u) {
        inst.inst_class = Rdna2InstClass::VALU;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
            const uint32_t s0 = inst.raw_word1 & 0x1FFu;
            const uint32_t s1 = (inst.raw_word1 >> 9) & 0x1FFu;
            const uint32_t s2 = (inst.raw_word1 >> 18) & 0x1FFu;
            if ((s0 == 0x0FFu || s1 == 0x0FFu || s2 == 0x0FFu) && remaining >= 12) {
                inst.has_literal = true;
                inst.literal_u32 = ReadLe32(code + 8);
                inst.inst_bytes = 12u;
            }
        }
        const uint32_t vdst = w0 & 0xFFu;
        const uint32_t op = (w0 >> 16) & 0x3FFu;
        if (op == 0x2FFu) {
            inst.mnemonic = "v_mbcnt_lo_u32_b32";
            inst.is_subgroup_op = true;
        } else if (op == 0x364u) {
            inst.mnemonic = "v_add3_u32";
        } else if (op == 0x36Fu) {
            inst.mnemonic = "v_lshl_or_b32";
        } else {
            inst.mnemonic = "v_fma_f32";
        }
        const uint32_t s0 = inst.raw_word1 & 0x1FFu;
        const uint32_t s1 = (inst.raw_word1 != 0) ? ((inst.raw_word1 >> 9) & 0x1FFu) : 1u;
        const uint32_t s2 = (inst.raw_word1 != 0) ? ((inst.raw_word1 >> 18) & 0x1FFu) : 2u;
        std::ostringstream ss;
        ss << "v" << vdst << ", " << FormatValuSrc(s0, inst) << ", "
           << FormatValuSrc(s1, inst) << ", " << FormatValuSrc(s2, inst);
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // 12. LDS (Local Data Share): bits 31..26 == 0b110110 (0xD8000000)
    if ((w0 & 0xFC000000u) == 0xD8000000u) {
        inst.inst_class = Rdna2InstClass::LDS;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
        }
        const uint32_t vdst = (inst.raw_word1 != 0) ? ((inst.raw_word1 >> 24) & 0xFFu) : ((w0 >> 18) & 0xFFu);
        const uint32_t vaddr = inst.raw_word1 & 0xFFu;
        inst.mnemonic = (w0 & (1u << 25)) ? "ds_write_b32" : "ds_read_b32";
        std::ostringstream ss;
        ss << "v" << vdst << ", v" << vaddr << " offset:" << (w0 & 0xFFFFu);
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // 13. FLAT / GLOBAL / SCRATCH (64-bit global/flat/scratch memory ops): 0xDC000000
    if ((w0 & 0xFC000000u) == 0xDC000000u) {
        inst.inst_class = Rdna2InstClass::VMEM;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
        }
        const uint32_t seg = (w0 >> 14) & 0x3u;
        const uint32_t fop = (w0 >> 18) & 0x7Fu;
        const uint32_t saddr = (inst.raw_word1 >> 16) & 0x7Fu;
        const bool is_store = (fop >= 0x18u && fop <= 0x1Fu) || ((w0 & (1u << 25)) != 0 && fop >= 0x18u);
        if (fop == 0x0Du || fop == 0x1Du) inst.mem_dwords = 2;
        else if (fop == 0x0Fu || fop == 0x1Fu) inst.mem_dwords = 3;
        else if (fop == 0x0Eu || fop == 0x1Eu) inst.mem_dwords = 4;
        else inst.mem_dwords = 1;
        const char* prefix = (seg == 1u) ? "scratch_" : ((seg == 0u && saddr == 0x7Fu) ? "flat_" : "global_");
        const char* suffix = (inst.mem_dwords == 4) ? "dwordx4" :
                             (inst.mem_dwords == 3) ? "dwordx3" :
                             (inst.mem_dwords == 2) ? "dwordx2" : "dword";
        inst.mnemonic = std::string(prefix) + (is_store ? "store_" : "load_") + suffix;
        const uint32_t vaddr = inst.raw_word1 & 0xFFu;
        const uint32_t vreg = is_store ? ((inst.raw_word1 >> 8) & 0xFFu) : ((inst.raw_word1 >> 24) & 0xFFu);
        std::ostringstream ss;
        ss << "v" << vreg << ", v" << vaddr << ", s[" << saddr << ":" << (saddr + 1u)
           << "] offset:" << (w0 & 0xFFFu);
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // 14. MUBUF / VMEM (Vector Memory / SSBO Buffer ops): bits 31..26 == 0b111000 (0xE0000000)
    if ((w0 & 0xFC000000u) == 0xE0000000u) {
        inst.inst_class = Rdna2InstClass::VMEM;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
        }
        const uint32_t mubuf_op = (w0 >> 18) & 0x7Fu;
        const bool is_store = ((w0 & (1u << 25)) != 0) || (mubuf_op >= 0x1Cu && mubuf_op <= 0x1Fu);
        if (mubuf_op == 0x0Du || mubuf_op == 0x1Du) inst.mem_dwords = 2;
        else if (mubuf_op == 0x0Eu || mubuf_op == 0x1Eu) inst.mem_dwords = 3;
        else if (mubuf_op == 0x0Fu || mubuf_op == 0x1Fu) inst.mem_dwords = 4;
        else inst.mem_dwords = 1;
        const uint32_t vdata = (inst.raw_word1 != 0) ? ((inst.raw_word1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
        const uint32_t vaddr = (inst.raw_word1 != 0) ? (inst.raw_word1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
        const uint32_t srsrc = ((inst.raw_word1 >> 16) & 0x1Fu) * 4u;
        inst.mnemonic = is_store ? "buffer_store_dword" : "buffer_load_dword";
        std::ostringstream ss;
        ss << "v" << vdata << ", v" << vaddr << ", s[" << srsrc << ":" << (srsrc + 3u) << "], 0 offen";
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // 15. MTBUF (Typed Buffer Format Loads/Stores): bits 31..26 == 0b111010 (0xE8000000)
    if ((w0 & 0xFC000000u) == 0xE8000000u) {
        inst.inst_class = Rdna2InstClass::VMEM;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
        }
        const uint32_t top = ((w0 >> 16) & 0x7u) | (((inst.raw_word1 >> 21) & 0x1u) << 3);
        const bool is_store = (top >= 4u && top <= 7u) || (top >= 12u);
        inst.mem_dwords = (top & 0x3u) + 1u;
        static const char* const kFmtComp[4] = {"x", "xy", "xyz", "xyzw"};
        inst.mnemonic = std::string(is_store ? "tbuffer_store_format_" : "tbuffer_load_format_") +
                        kFmtComp[(inst.mem_dwords - 1u) & 3u];
        const uint32_t vdata = (inst.raw_word1 >> 8) & 0xFFu;
        const uint32_t vaddr = inst.raw_word1 & 0xFFu;
        const uint32_t srsrc = ((inst.raw_word1 >> 16) & 0x1Fu) * 4u;
        std::ostringstream ss;
        ss << "v" << vdata << ", v" << vaddr << ", s[" << srsrc << ":" << (srsrc + 3u)
           << "], 0 format:" << ((w0 >> 19) & 0x7Fu);
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // 16. EXP (Export: pos0..3, mrt0..7, mrtz, param0..31): bits 31..26 == 0b111110 (0xF8000000)
    if ((w0 & 0xFC000000u) == 0xF8000000u) {
        inst.inst_class = Rdna2InstClass::EXPORT;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
        }
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
        finalize();
        return inst;
    }

    // 17. VINTRP (Pixel Shader Attribute Interpolation): bits 31..26 == 0b110010 (0xC8000000)
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
        finalize();
        return inst;
    }

    // 18. MIMG (Image Sample/Load/Store + NSA address words + BVH64 Raytracing): 0xF0000000
    if ((w0 & 0xFC000000u) == 0xF0000000u) {
        const uint32_t op = (w0 >> 18) & 0xFFu;
        const uint32_t nsa = (w0 >> 1) & 0x3u;
        const uint32_t dmask = (w0 >> 8) & 0xFu;
        if (remaining >= 8) {
            inst.raw_word1 = ReadLe32(code + 4);
            inst.inst_bytes = 8u;
            inst.nsa_dwords = nsa;
            for (uint32_t ni = 0; ni < nsa; ++ni) {
                if (remaining >= 12u + ni * 4u) {
                    inst.nsa_words[ni] = ReadLe32(code + 8u + ni * 4u);
                    inst.inst_bytes = 12u + ni * 4u;
                }
            }
        }
        const uint32_t vdata = (inst.raw_word1 >> 8) & 0xFFu;
        const uint32_t vaddr = inst.raw_word1 & 0xFFu;
        const uint32_t srsrc = ((inst.raw_word1 >> 16) & 0x1Fu) * 4u;
        if (op == 0xE6u || op == 0xE7u) {
            inst.inst_class = Rdna2InstClass::RAYTRACING;
            inst.mnemonic = (op == 0xE6u) ? "image_bvh64_intersect_ray" : "image_bvh_intersect_ray";
        } else {
            inst.inst_class = Rdna2InstClass::MIMG;
            if (op >= 0x20u) inst.mnemonic = "image_sample";
            else if (op >= 0x08u) inst.mnemonic = "image_store";
            else inst.mnemonic = "image_load";
        }
        std::ostringstream ss;
        ss << "v[" << vdata << ":" << (vdata + 3u) << "], v" << vaddr << ", s["
           << srsrc << ":" << (srsrc + 7u) << "] dmask:0x" << std::hex << (dmask ? dmask : 0xFu);
        if (nsa > 0) ss << std::dec << " nsa:" << nsa;
        inst.operands = ss.str();
        finalize();
        return inst;
    }

    // Fallback scalar / generic
    inst.inst_class = Rdna2InstClass::SALU;
    inst.mnemonic = "s_nop";
    inst.operands = "0";
    finalize();
    return inst;
}

struct SpirvCfgRegion {
    bool is_loop = false;
    size_t begin = 0;
    size_t end = 0;
    size_t inner_begin = 0;
    size_t inner_end = 0;
    size_t branch_idx = 0;
    uint32_t loop_id = 0;
    std::vector<SpirvCfgRegion> children;
};

bool InsertSpirvCfgRegion(
    SpirvCfgRegion candidate,
    size_t limit_begin,
    size_t limit_end,
    std::vector<SpirvCfgRegion>& list) {
    if (!candidate.is_loop && candidate.end > limit_end) {
        candidate.end = limit_end;
        candidate.inner_end = limit_end;
        if (candidate.inner_end <= candidate.inner_begin) {
            return false;
        }
    }
    if (candidate.begin < limit_begin || candidate.end > limit_end || candidate.begin >= candidate.end) {
        return false;
    }
    for (auto& child : list) {
        if (candidate.begin >= child.inner_begin && candidate.end <= child.inner_end) {
            return InsertSpirvCfgRegion(std::move(candidate), child.inner_begin, child.inner_end, child.children);
        }
        if (!(candidate.end <= child.begin || candidate.begin >= child.end)) {
            return false;
        }
    }
    auto pos = std::lower_bound(
        list.begin(), list.end(), candidate.begin,
        [](const SpirvCfgRegion& r, size_t b) { return r.begin < b; });
    list.insert(pos, std::move(candidate));
    return true;
}

std::vector<SpirvCfgRegion> BuildSpirvStructuredCfg(
    const std::vector<DecodedInstruction>& insts,
    uint32_t& out_num_loops) {
    out_num_loops = 0;
    const size_t n = insts.size();
    if (n == 0) return {};
    const uint32_t total_dwords = insts.back().next_dword_pc;

    std::vector<SpirvCfgRegion> candidates;
    for (size_t i = 0; i < n; ++i) {
        const auto& ins = insts[i];
        if (!ins.is_branch) continue;
        const int64_t raw_target = static_cast<int64_t>(ins.next_dword_pc) + static_cast<int64_t>(ins.branch_simm16);
        const uint32_t target_dw = static_cast<uint32_t>(
            std::clamp<int64_t>(raw_target, 0, static_cast<int64_t>(total_dwords)));

        if (raw_target <= static_cast<int64_t>(ins.dword_pc)) {
            size_t start_idx = i;
            for (size_t k = 0; k <= i; ++k) {
                if (insts[k].next_dword_pc > target_dw) {
                    start_idx = k;
                    break;
                }
            }
            SpirvCfgRegion r{};
            r.is_loop = true;
            r.begin = start_idx;
            r.end = i + 1u;
            r.inner_begin = start_idx;
            r.inner_end = i;
            r.branch_idx = i;
            candidates.push_back(std::move(r));
        } else {
            size_t end_idx = n;
            for (size_t k = i + 1u; k < n; ++k) {
                if (insts[k].dword_pc >= target_dw) {
                    end_idx = k;
                    break;
                }
            }
            if (end_idx > i + 1u) {
                SpirvCfgRegion r{};
                r.is_loop = false;
                r.begin = i;
                r.end = end_idx;
                r.inner_begin = i + 1u;
                r.inner_end = end_idx;
                r.branch_idx = i;
                candidates.push_back(std::move(r));
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const SpirvCfgRegion& a, const SpirvCfgRegion& b) {
        const size_t len_a = a.end - a.begin;
        const size_t len_b = b.end - b.begin;
        if (len_a != len_b) return len_a > len_b;
        return a.begin < b.begin;
    });

    std::vector<SpirvCfgRegion> roots;
    for (auto& c : candidates) {
        InsertSpirvCfgRegion(std::move(c), 0u, n, roots);
    }

    std::function<void(std::vector<SpirvCfgRegion>&)> number_loops = [&](std::vector<SpirvCfgRegion>& regs) {
        for (auto& r : regs) {
            if (r.is_loop) {
                r.loop_id = out_num_loops++;
            }
            number_loops(r.children);
        }
    };
    number_loops(roots);
    return roots;
}

struct PipelineState {
    uint32_t flags = 0;
    uint32_t max_entries = 1024;
    std::unordered_map<uint64_t, std::vector<uint8_t>> cache;
    std::deque<uint64_t> fifo_order;
    XS2_TRANSLATION_STATS stats{};
    std::mutex mu;
};

void InsertPipelineCacheBounded(PipelineState* p, uint64_t key, const std::vector<uint8_t>& blob) {
    if (!p || blob.empty() || p->max_entries == 0) return;
    auto existing = p->cache.find(key);
    if (existing != p->cache.end()) {
        existing->second = blob;
        return;
    }
    while (p->cache.size() >= p->max_entries) {
        if (!p->fifo_order.empty()) {
            const uint64_t oldest = p->fifo_order.front();
            p->fifo_order.pop_front();
            p->cache.erase(oldest);
        } else {
            p->cache.erase(p->cache.begin());
        }
    }
    p->cache[key] = blob;
    p->fifo_order.push_back(key);
}

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

    const auto* raw = static_cast<const uint8_t*>(binaryData);
    scarlettx::Xs2ParsedHeaderInfo hdr{};
    scarlettx::ParseXs2PrecompiledHeader(raw, byteSize, hdr);

    const uint8_t* code = hdr.isa_ptr;
    const size_t code_size = hdr.isa_size;
    size_t offset = 0;
    uint32_t inst_count = 0;
    uint32_t max_vgpr = hdr.vgpr_count;
    uint32_t max_sgpr = hdr.sgpr_count;
    uint32_t lds_bytes = hdr.lds_bytes;
    bool has_vmem = false;
    bool has_vs_export = false;
    bool has_ps_export_or_interp = (hdr.mrt_export_mask != 0);

    while (offset < code_size) {
        DecodedInstruction inst = DecodeRdna2Instruction(
            code + offset, code_size - offset, static_cast<uint32_t>(offset));
        inst_count++;
        if (inst.inst_class == Rdna2InstClass::VALU) max_vgpr = std::max(max_vgpr, 32u);
        if (inst.inst_class == Rdna2InstClass::SALU) max_sgpr = std::max(max_sgpr, 64u);
        if (inst.inst_class == Rdna2InstClass::LDS) lds_bytes = std::max(lds_bytes, 1024u);
        if (inst.inst_class == Rdna2InstClass::VMEM || inst.inst_class == Rdna2InstClass::RAYTRACING) has_vmem = true;
        if (inst.inst_class == Rdna2InstClass::VINTRP) has_ps_export_or_interp = true;
        if (inst.inst_class == Rdna2InstClass::EXPORT) {
            if (inst.operands.rfind("pos", 0) == 0 || inst.operands.rfind("param", 0) == 0) {
                has_vs_export = true;
            } else {
                has_ps_export_or_interp = true;
            }
        }
        offset += inst.inst_bytes;
    }

    if (hdr.stage != 0) {
        outInfo->stage = hdr.stage;
    } else if (has_vs_export) {
        outInfo->stage = 1;  // Vertex Shader
    } else if (has_ps_export_or_interp) {
        outInfo->stage = 2;  // Pixel Shader
    } else {
        outInfo->stage = has_vmem ? 3 : 2;  // 3 = Compute, 2 = Pixel
    }
    outInfo->instructionCount = inst_count;
    outInfo->vgprCount = std::max(32u, max_vgpr);
    outInfo->sgprCount = std::max(64u, max_sgpr);
    outInfo->ldsBytes = lds_bytes;
    outInfo->shaderHash = SimpleHash(raw, byteSize);
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderDisassembleRdna2(
    const void* microcode, size_t byteSize, char* outAsm, size_t maxAsmLen, size_t* actualLen) {
    if (!microcode || byteSize == 0 || !outAsm || maxAsmLen == 0) return kInvalidArg;

    const auto* raw = static_cast<const uint8_t*>(microcode);
    scarlettx::Xs2ParsedHeaderInfo hdr{};
    scarlettx::ParseXs2PrecompiledHeader(raw, byteSize, hdr);

    const uint8_t* code = hdr.isa_ptr;
    const size_t code_size = hdr.isa_size;
    std::ostringstream ss;
    size_t offset = 0;
    while (offset < code_size) {
        DecodedInstruction inst = DecodeRdna2Instruction(
            code + offset, code_size - offset, static_cast<uint32_t>(offset));
        ss << inst.mnemonic;
        if (!inst.operands.empty()) {
            ss << " " << inst.operands;
        }
        ss << "\n";
        offset += inst.inst_bytes;
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
        *outDxilBytecode = nullptr;
        *outDxilSize = 0;
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

    const auto* raw = static_cast<const uint8_t*>(microcode);
    scarlettx::Xs2ParsedHeaderInfo hdr{};
    scarlettx::ParseXs2PrecompiledHeader(raw, byteSize, hdr);
    if (!hdr.isa_ptr || hdr.isa_size < 4) {
        *outSpirvBytecode = nullptr;
        *outSpirvSize = 0;
        return kInvalidArg;
    }

    // Pass 1: Decode full-width RDNA2 GFX10.3 ISA instructions with exact byte/dword PCs
    const uint8_t* code = hdr.isa_ptr;
    const size_t code_size = hdr.isa_size;
    std::vector<DecodedInstruction> insts;
    size_t offset = 0;
    while (offset < code_size) {
        DecodedInstruction inst = DecodeRdna2Instruction(
            code + offset, code_size - offset, static_cast<uint32_t>(offset));
        offset += inst.inst_bytes;
        insts.push_back(std::move(inst));
    }

    XS2_MICROCODE_INFO info{};
    Xs2ShaderIdentifyMicrocode(microcode, byteSize, &info);
    // SPIR-V ExecutionModel: 0 = Vertex, 4 = Fragment, 5 = GLCompute
    const uint32_t exec_model = (info.stage == 1) ? 0u : ((info.stage == 2) ? 4u : 5u);

    bool has_smem = false, has_mimg = false, has_vinterp = false;
    bool has_ssbo = false, has_lds = false, has_barrier = false;
    bool has_subgroup = false, has_glsl_ext = false, has_mrtz = false;
    uint32_t mrt_mask = hdr.mrt_export_mask;

    for (const auto& inst : insts) {
        if (inst.inst_class == Rdna2InstClass::SALU && inst.mnemonic.rfind("s_load_", 0) == 0) has_smem = true;
        if (inst.inst_class == Rdna2InstClass::MIMG) has_mimg = true;
        if (inst.inst_class == Rdna2InstClass::VINTRP) has_vinterp = true;
        if (inst.inst_class == Rdna2InstClass::VMEM) has_ssbo = true;
        if (inst.inst_class == Rdna2InstClass::LDS) has_lds = true;
        if (inst.mnemonic == "s_barrier" && exec_model == 5u) { has_barrier = true; has_lds = true; }
        if (inst.is_subgroup_op && exec_model == 5u) has_subgroup = true;
        if (inst.mnemonic == "v_rsq_f32" || inst.mnemonic == "v_sqrt_f32" ||
            inst.mnemonic == "v_sin_f32" || inst.mnemonic == "v_cos_f32" ||
            inst.mnemonic == "v_min_f32" || inst.mnemonic == "v_max_f32") {
            has_glsl_ext = true;
        }
        if (inst.mnemonic == "exp" && exec_model == 4u) {
            const uint32_t target = (inst.raw_word0 >> 4) & 0x3Fu;
            if (target < 8u) mrt_mask |= (1u << target);
            else if (target == 8u) has_mrtz = true;
        }
    }
    if (options && options->enableSubgroupOps && exec_model == 5u) {
        has_subgroup = true;
    }

    uint32_t num_loops = 0;
    const auto cfg_roots = BuildSpirvStructuredCfg(insts, num_loops);

    const bool has_gid = (exec_model == 5u && has_ssbo);
    const bool has_vid = (exec_model == 0u && has_ssbo);

    std::vector<uint32_t> active_mrts;
    if (exec_model == 4u) {
        if (mrt_mask == 0u) mrt_mask = 1u;
        for (uint32_t m = 0; m < 8u; ++m) {
            if ((mrt_mask & (1u << m)) != 0) active_mrts.push_back(m);
        }
    }

    auto resolve_spv_binding = [&](uint32_t default_binding, uint32_t sgpr_hint) -> std::pair<uint32_t, uint32_t> {
        for (const auto& rm : hdr.root_map) {
            if (rm.user_sgpr_offset == sgpr_hint) {
                return {rm.register_index, rm.register_space};
            }
        }
        return {default_binding, 0u};
    };

    std::vector<uint32_t> spv;
    spv.reserve(320 + insts.size() * 14);
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

    const uint32_t kIdConstTrue = alloc_id();
    const uint32_t kIdConstFalse = alloc_id();

    // Deduplicated constant tables (emitted strictly in Section 8 before OpFunction)
    std::unordered_map<uint32_t, uint32_t> const_f32_map;
    std::unordered_map<uint32_t, uint32_t> const_u32_map;
    std::unordered_map<uint32_t, uint32_t> const_i32_map;
    std::vector<std::pair<uint32_t, uint32_t>> const_f32_order;
    std::vector<std::pair<uint32_t, uint32_t>> const_u32_order;
    std::vector<std::pair<uint32_t, uint32_t>> const_i32_order;

    auto get_f32_const = [&](uint32_t bits) -> uint32_t {
        auto it = const_f32_map.find(bits);
        if (it != const_f32_map.end()) return it->second;
        const uint32_t id = alloc_id();
        const_f32_map[bits] = id;
        const_f32_order.emplace_back(bits, id);
        return id;
    };
    auto get_u32_const = [&](uint32_t val) -> uint32_t {
        auto it = const_u32_map.find(val);
        if (it != const_u32_map.end()) return it->second;
        const uint32_t id = alloc_id();
        const_u32_map[val] = id;
        const_u32_order.emplace_back(val, id);
        return id;
    };
    auto get_i32_const = [&](uint32_t val) -> uint32_t {
        auto it = const_i32_map.find(val);
        if (it != const_i32_map.end()) return it->second;
        const uint32_t id = alloc_id();
        const_i32_map[val] = id;
        const_i32_order.emplace_back(val, id);
        return id;
    };

    // Pre-populate core constants + any literal/inline constants referenced by instructions
    const uint32_t kIdConst0 = get_f32_const(0x00000000u);
    const uint32_t kIdConst1 = get_f32_const(0x3F800000u);
    (void)get_f32_const(0xBF800000u);  // -1.0f
    (void)get_f32_const(0x3F000000u);  //  0.5f
    (void)get_f32_const(0xBF000000u);  // -0.5f
    (void)get_f32_const(0x40000000u);  //  2.0f
    (void)get_f32_const(0xC0000000u);  // -2.0f
    (void)get_f32_const(0x40800000u);  //  4.0f
    (void)get_f32_const(0xC0800000u);  // -4.0f
    (void)get_f32_const(0x3E800000u);  //  0.25f
    (void)get_f32_const(0x3727C5ACu);  //  1e-5f

    const uint32_t kIdConstI0 = get_i32_const(0u);
    (void)get_i32_const(1u);
    const uint32_t kIdConstI255 = get_i32_const(255u);

    const uint32_t kIdConstU0 = get_u32_const(0u);
    (void)get_u32_const(1u);
    (void)get_u32_const(8u);
    (void)get_u32_const(16u);
    (void)get_u32_const(24u);
    (void)get_u32_const(31u);
    (void)get_u32_const(0xFFu);
    (void)get_u32_const(0xFFFFu);
    (void)get_u32_const(0xFFFFFFFFu);
    if (num_loops > 0) (void)get_u32_const(1024u);
    const uint32_t kIdConstU256 = has_lds ? get_u32_const(256u) : 0u;
    const uint32_t kIdConstScopeWg = has_barrier ? get_u32_const(2u) : 0u;
    const uint32_t kIdConstSemWg = has_barrier ? get_u32_const(0x108u) : 0u;
    const uint32_t kIdConstScopeSub = has_subgroup ? get_u32_const(3u) : 0u;

    auto register_inline_src = [&](uint32_t src) {
        if (src >= 128u && src <= 192u) {
            const uint32_t v = src - 128u;
            (void)get_u32_const(v);
            (void)get_f32_const(v);
        } else if (src >= 193u && src <= 208u) {
            const uint32_t v = static_cast<uint32_t>(-static_cast<int32_t>(src - 192u));
            (void)get_u32_const(v);
            (void)get_f32_const(v);
        }
    };
    for (const auto& inst : insts) {
        if (inst.has_literal) {
            (void)get_u32_const(inst.literal_u32);
            (void)get_f32_const(inst.literal_u32);
        }
        register_inline_src(inst.raw_word0 & 0x1FFu);
        register_inline_src(inst.raw_word0 & 0xFFu);
        register_inline_src((inst.raw_word0 >> 8) & 0xFFu);
        if (inst.is_64bit) {
            register_inline_src(inst.raw_word1 & 0x1FFu);
            register_inline_src((inst.raw_word1 >> 9) & 0x1FFu);
            register_inline_src((inst.raw_word1 >> 18) & 0x1FFu);
        }
        if (inst.mnemonic.rfind("s_movk", 0) == 0 || inst.mnemonic.rfind("s_addk", 0) == 0 ||
            inst.mnemonic.rfind("s_mulk", 0) == 0 || inst.mnemonic.rfind("s_cmpk", 0) == 0) {
            const uint32_t imm_u = static_cast<uint32_t>(inst.branch_simm16);
            (void)get_u32_const(imm_u);
            (void)get_i32_const(imm_u);
        }
        if (inst.inst_class == Rdna2InstClass::VMEM) {
            const uint32_t dword_off = (inst.raw_word0 & 0xFFFu) / 4u;
            for (uint32_t c = 0; c < inst.mem_dwords; ++c) {
                (void)get_i32_const(dword_off + c);
            }
        }
    }

    const uint32_t kIdPtrFnF32 = alloc_id();
    const uint32_t kIdPtrFnU32 = alloc_id();
    const uint32_t kIdPtrFnBool = alloc_id();

    const bool has_vs_out = (exec_model == 0u);
    const uint32_t kIdPtrOutVec4 = (has_vs_out || !active_mrts.empty()) ? alloc_id() : 0u;
    const uint32_t kIdVsPosVar = has_vs_out ? alloc_id() : 0u;
    std::unordered_map<uint32_t, uint32_t> ps_mrt_vars;
    for (uint32_t m : active_mrts) {
        ps_mrt_vars[m] = alloc_id();
    }
    const uint32_t kIdPtrOutF32 = (exec_model == 4u && has_mrtz) ? alloc_id() : 0u;
    const uint32_t kIdMrtzVar = (exec_model == 4u && has_mrtz) ? alloc_id() : 0u;

    const uint32_t kIdPtrInVec4 = (has_vinterp && exec_model == 4u) ? alloc_id() : 0u;
    const uint32_t kIdInVar = (has_vinterp && exec_model == 4u) ? alloc_id() : 0u;
    const uint32_t kIdPtrInUVec3 = has_gid ? alloc_id() : 0u;
    const uint32_t kIdPtrInU32 = (has_gid || has_vid) ? alloc_id() : 0u;
    const uint32_t kIdGidVar = has_gid ? alloc_id() : 0u;
    const uint32_t kIdPtrInI32 = has_vid ? alloc_id() : 0u;
    const uint32_t kIdVidVar = has_vid ? alloc_id() : 0u;

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
        if (has_vs_out) ep.push_back(kIdVsPosVar);
        for (uint32_t m : active_mrts) ep.push_back(ps_mrt_vars[m]);
        if (exec_model == 4u && has_mrtz) ep.push_back(kIdMrtzVar);
        if (has_vinterp && exec_model == 4u) ep.push_back(kIdInVar);
        if (has_gid) ep.push_back(kIdGidVar);
        if (has_vid) ep.push_back(kIdVidVar);
        if (has_smem) ep.push_back(kIdUboVar);
        if (has_mimg) ep.push_back(kIdSamplerVar);
        if (has_ssbo) ep.push_back(kIdSsboVar);
        if (has_lds) ep.push_back(kIdLdsVar);
        spv.push_back((static_cast<uint32_t>(1 + ep.size()) << 16) | 15u);
        for (uint32_t w : ep) spv.push_back(w);
    }
    if (exec_model == 5u) {
        const uint32_t wg_x = hdr.has_workgroup_size ? hdr.threads_x : 8u;
        const uint32_t wg_y = hdr.has_workgroup_size ? hdr.threads_y : 1u;
        const uint32_t wg_z = hdr.has_workgroup_size ? hdr.threads_z : 1u;
        emit(16, {kIdMain, 17u, wg_x, wg_y, wg_z});       // LocalSize
    } else if (exec_model == 4u) {
        emit(16, {kIdMain, 7u});                          // OriginUpperLeft
        if (has_mrtz) emit(16, {kIdMain, 12u});           // DepthReplacing
    }

    // 4. Decorations (strictly before Types/Constants/Variables)
    if (has_vs_out) {
        emit(71, {kIdVsPosVar, 11u, 0u});                 // BuiltIn Position
    } else if (exec_model == 4u) {
        for (uint32_t m : active_mrts) {
            emit(71, {ps_mrt_vars[m], 30u, m});           // Location m
        }
        if (has_mrtz) emit(71, {kIdMrtzVar, 11u, 22u});   // BuiltIn FragDepth
        if (has_vinterp) emit(71, {kIdInVar, 30u, 0u});   // Location 0
    }
    if (has_gid) {
        emit(71, {kIdGidVar, 11u, 28u});                  // BuiltIn GlobalInvocationId (28)
    }
    if (has_vid) {
        emit(71, {kIdVidVar, 11u, 42u});                  // BuiltIn VertexIndex (42)
    }
    if (has_smem) {
        const auto [bind_idx, set_idx] = resolve_spv_binding(0u, 0u);
        emit(71, {kIdUboStruct, 2u});                     // Block
        emit(72, {kIdUboStruct, 0u, 35u, 0u});            // Member 0 Offset 0
        emit(71, {kIdUboVar, 34u, set_idx});              // DescriptorSet
        emit(71, {kIdUboVar, 33u, bind_idx});             // Binding
    }
    if (has_mimg) {
        const auto [bind_idx, set_idx] = resolve_spv_binding(1u, 4u);
        emit(71, {kIdSamplerVar, 34u, set_idx});          // DescriptorSet
        emit(71, {kIdSamplerVar, 33u, bind_idx});         // Binding
    }
    if (has_ssbo) {
        const auto [bind_idx, set_idx] = resolve_spv_binding(2u, 8u);
        emit(71, {kIdRtArrF32, 6u, 4u});                  // ArrayStride 4
        emit(71, {kIdSsboStruct, 2u});                    // Block
        emit(72, {kIdSsboStruct, 0u, 35u, 0u});           // Member 0 Offset 0
        emit(71, {kIdSsboVar, 34u, set_idx});             // DescriptorSet
        emit(71, {kIdSsboVar, 33u, bind_idx});            // Binding
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

    emit(41, {kIdBool, kIdConstTrue});                    // OpConstantTrue
    emit(42, {kIdBool, kIdConstFalse});                   // OpConstantFalse
    for (const auto& [bits, cid] : const_f32_order) {
        emit(43, {kIdF32, cid, bits});
    }
    for (const auto& [val, cid] : const_i32_order) {
        emit(43, {kIdI32, cid, val});
    }
    for (const auto& [val, cid] : const_u32_order) {
        emit(43, {kIdU32, cid, val});
    }

    emit(32, {kIdPtrFnF32, 7u, kIdF32});                  // Function* f32
    emit(32, {kIdPtrFnU32, 7u, kIdU32});                  // Function* u32
    emit(32, {kIdPtrFnBool, 7u, kIdBool});                // Function* bool

    if (kIdPtrOutVec4 != 0u) {
        emit(32, {kIdPtrOutVec4, 3u, kIdVec4});
    }
    if (has_vs_out) {
        emit(59, {kIdPtrOutVec4, kIdVsPosVar, 3u});
    }
    for (uint32_t m : active_mrts) {
        emit(59, {kIdPtrOutVec4, ps_mrt_vars[m], 3u});
    }
    if (exec_model == 4u && has_mrtz) {
        emit(32, {kIdPtrOutF32, 3u, kIdF32});
        emit(59, {kIdPtrOutF32, kIdMrtzVar, 3u});
    }
    if (has_vinterp && exec_model == 4u) {
        emit(32, {kIdPtrInVec4, 1u, kIdVec4});
        emit(59, {kIdPtrInVec4, kIdInVar, 1u});
    }
    if (kIdPtrInU32 != 0u) {
        emit(32, {kIdPtrInU32, 1u, kIdU32});
    }
    if (has_gid) {
        emit(32, {kIdPtrInUVec3, 1u, kIdUVec3});
        emit(59, {kIdPtrInUVec3, kIdGidVar, 1u});
    }
    if (has_vid) {
        emit(32, {kIdPtrInI32, 1u, kIdI32});
        emit(59, {kIdPtrInI32, kIdVidVar, 1u});
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

    // Collect touched VGPRs (0..255) and SGPRs (0..63) for Function-scope SSA dominance
    std::vector<uint32_t> touched_vgprs = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    std::vector<uint32_t> touched_sgprs = {0u, 1u, 2u, 3u};
    auto mark_vgpr = [&](uint32_t r) {
        r &= 0xFFu;
        if (std::find(touched_vgprs.begin(), touched_vgprs.end(), r) == touched_vgprs.end()) {
            touched_vgprs.push_back(r);
        }
    };
    auto mark_sgpr = [&](uint32_t s) {
        s &= 63u;
        if (std::find(touched_sgprs.begin(), touched_sgprs.end(), s) == touched_sgprs.end()) {
            touched_sgprs.push_back(s);
        }
    };

    for (const auto& inst : insts) {
        const uint32_t w0 = inst.raw_word0;
        const uint32_t w1 = inst.raw_word1;
        mark_vgpr(w0 & 0xFFu);
        mark_vgpr((w0 >> 9) & 0xFFu);
        mark_vgpr((w0 >> 17) & 0xFFu);
        mark_vgpr((w0 >> 18) & 0xFFu);
        mark_sgpr(w0 & 0x3Fu);
        mark_sgpr((w0 >> 8) & 0x3Fu);
        mark_sgpr((w0 >> 16) & 0x3Fu);
        mark_sgpr((w0 >> 17) & 0x3Fu);
        for (uint32_t c = 0; c < 4u; ++c) {
            mark_sgpr(((w0 >> 6) & 0x3Fu) + c);
        }
        if (inst.is_64bit) {
            for (uint32_t c = 0; c < 6u; ++c) {
                mark_vgpr((w1 & 0xFFu) + c);
                mark_vgpr(((w1 >> 8) & 0xFFu) + c);
                mark_vgpr(((w1 >> 24) & 0xFFu) + c);
            }
            mark_vgpr((w1 >> 9) & 0xFFu);
            mark_vgpr((w1 >> 16) & 0xFFu);
            mark_vgpr((w1 >> 18) & 0xFFu);
            mark_sgpr(w1 & 0x3Fu);
            mark_sgpr((w1 >> 9) & 0x3Fu);
            mark_sgpr((w1 >> 18) & 0x3Fu);
        }
        for (uint32_t ni = 0; ni < inst.nsa_dwords; ++ni) {
            for (uint32_t b = 0; b < 4u; ++b) {
                mark_vgpr((inst.nsa_words[ni] >> (b * 8u)) & 0xFFu);
            }
        }
    }

    // 6. Function Entry & Entry-Block OpVariable Declarations
    const uint32_t kIdEntryLabel = alloc_id();
    emit(54, {kIdVoid, kIdMain, 0u, kIdFnType});          // OpFunction
    emit(248, {kIdEntryLabel});                           // OpLabel

    std::unordered_map<uint32_t, uint32_t> vgpr_var;
    for (uint32_t r : touched_vgprs) {
        const uint32_t vid = alloc_id();
        vgpr_var[r] = vid;
        emit(59, {kIdPtrFnF32, vid, 7u});                 // OpVariable Function f32
    }
    std::unordered_map<uint32_t, uint32_t> sgpr_var;
    for (uint32_t s : touched_sgprs) {
        const uint32_t sid = alloc_id();
        sgpr_var[s] = sid;
        emit(59, {kIdPtrFnU32, sid, 7u});                 // OpVariable Function u32
    }
    const uint32_t kIdSccVar = alloc_id();
    const uint32_t kIdVccVar = alloc_id();
    const uint32_t kIdExecVar = alloc_id();
    emit(59, {kIdPtrFnBool, kIdSccVar, 7u});              // OpVariable Function bool (SCC)
    emit(59, {kIdPtrFnBool, kIdVccVar, 7u});              // OpVariable Function bool (VCC)
    emit(59, {kIdPtrFnBool, kIdExecVar, 7u});             // OpVariable Function bool (EXEC != 0)

    std::vector<uint32_t> loop_iter_vars(num_loops, 0u);
    for (uint32_t li = 0; li < num_loops; ++li) {
        loop_iter_vars[li] = alloc_id();
        emit(59, {kIdPtrFnU32, loop_iter_vars[li], 7u});  // OpVariable Function u32 (loop bound)
    }

    // Initialize Function variables (after ALL OpVariable instructions in entry block)
    for (uint32_t r : touched_vgprs) {
        emit(62, {vgpr_var[r], kIdConst1});               // Default VGPR = 1.0f
    }
    for (uint32_t s : touched_sgprs) {
        emit(62, {sgpr_var[s], kIdConstU0});              // Default SGPR = 0u
    }
    emit(62, {kIdSccVar, kIdConstTrue});
    emit(62, {kIdVccVar, kIdConstTrue});
    emit(62, {kIdExecVar, kIdConstTrue});
    for (uint32_t li = 0; li < num_loops; ++li) {
        emit(62, {loop_iter_vars[li], kIdConstU0});
    }

    // Initialize stage outputs with valid defaults so early exits or partial MRT shaders stay valid
    if (has_vs_out) {
        const uint32_t def_pos = alloc_id();
        emit(80, {kIdVec4, def_pos, kIdConst0, kIdConst0, kIdConst0, kIdConst1});
        emit(62, {kIdVsPosVar, def_pos});
    } else if (exec_model == 4u) {
        const uint32_t def_col = alloc_id();
        emit(80, {kIdVec4, def_col, kIdConst0, kIdConst0, kIdConst0, kIdConst1});
        for (uint32_t m : active_mrts) {
            emit(62, {ps_mrt_vars[m], def_col});
        }
        if (has_mrtz) emit(62, {kIdMrtzVar, kIdConst0});
    }

    // Seed v0 with float(GlobalInvocationId.x) in Compute shaders or float(VertexIndex) in Vertex shaders
    if (has_gid) {
        const uint32_t gid_ptr = alloc_id();
        const uint32_t gid_u32 = alloc_id();
        const uint32_t gid_f32 = alloc_id();
        emit(65, {kIdPtrInU32, gid_ptr, kIdGidVar, kIdConstU0}); // OpAccessChain %gid.x
        emit(61, {kIdU32, gid_u32, gid_ptr});                    // OpLoad u32
        emit(112, {kIdF32, gid_f32, gid_u32});                   // OpConvertUToF f32 <- u32
        emit(62, {vgpr_var[0], gid_f32});
    } else if (has_vid) {
        const uint32_t vid_i32 = alloc_id();
        const uint32_t vid_f32 = alloc_id();
        emit(61, {kIdI32, vid_i32, kIdVidVar});                  // OpLoad i32
        emit(111, {kIdF32, vid_f32, vid_i32});                   // OpConvertSToF f32 <- i32
        emit(62, {vgpr_var[0], vid_f32});
    }

    std::unordered_set<uint32_t> live_sgprs;
    for (uint32_t us = 0; us < std::min<uint32_t>(hdr.user_sgpr_count, 64u); ++us) {
        live_sgprs.insert(us);
    }

    auto read_vgpr = [&](uint32_t reg) -> uint32_t {
        const uint32_t r = reg & 0xFFu;
        auto it = vgpr_var.find(r);
        if (it == vgpr_var.end()) return kIdConst1;
        const uint32_t val = alloc_id();
        emit(61, {kIdF32, val, it->second});
        return val;
    };
    auto read_vgpr_u32 = [&](uint32_t reg) -> uint32_t {
        const uint32_t vf = read_vgpr(reg);
        const uint32_t vu = alloc_id();
        emit(124, {kIdU32, vu, vf});                             // OpBitcast u32 <- f32
        return vu;
    };
    auto write_vgpr = [&](uint32_t reg, uint32_t val_f32) {
        const uint32_t r = reg & 0xFFu;
        auto it = vgpr_var.find(r);
        if (it != vgpr_var.end()) {
            emit(62, {it->second, val_f32});
        }
    };
    auto write_vgpr_u32 = [&](uint32_t reg, uint32_t val_u32) {
        const uint32_t vf = alloc_id();
        emit(124, {kIdF32, vf, val_u32});                        // OpBitcast f32 <- u32
        write_vgpr(reg, vf);
    };
    auto read_sgpr = [&](uint32_t reg) -> uint32_t {
        const uint32_t s = reg & 63u;
        auto it = sgpr_var.find(s);
        if (it == sgpr_var.end()) return kIdConstU0;
        const uint32_t val = alloc_id();
        emit(61, {kIdU32, val, it->second});
        return val;
    };
    auto write_sgpr = [&](uint32_t reg, uint32_t val_u32) {
        const uint32_t s = reg & 63u;
        auto it = sgpr_var.find(s);
        if (it != sgpr_var.end()) {
            emit(62, {it->second, val_u32});
        }
        live_sgprs.insert(s);
    };

    auto eval_salu_u32 = [&](uint32_t ssrc, const DecodedInstruction& inst) -> uint32_t {
        if (ssrc == 0xFFu && inst.has_literal) return get_u32_const(inst.literal_u32);
        if (ssrc >= 128u && ssrc <= 192u) return get_u32_const(ssrc - 128u);
        if (ssrc >= 193u && ssrc <= 208u) {
            return get_u32_const(static_cast<uint32_t>(-static_cast<int32_t>(ssrc - 192u)));
        }
        return read_sgpr(ssrc);
    };

    auto eval_valu_f32 = [&](uint32_t src9, const DecodedInstruction& inst) -> uint32_t {
        if (src9 == 0x0FFu && inst.has_literal) return get_f32_const(inst.literal_u32);
        if ((src9 == 0x0FAu && inst.has_dpp) || (src9 == 0x0F9u && inst.has_sdwa)) {
            return read_vgpr(inst.raw_word1 & 0xFFu);
        }
        if (src9 >= 256u) return read_vgpr((src9 - 256u) & 0xFFu);
        if (src9 >= 128u && src9 <= 192u) return get_f32_const(src9 - 128u);
        if (src9 >= 193u && src9 <= 208u) {
            return get_f32_const(static_cast<uint32_t>(-static_cast<int32_t>(src9 - 192u)));
        }
        if (src9 == 240u) return get_f32_const(0x3F000000u);
        if (src9 == 241u) return get_f32_const(0xBF000000u);
        if (src9 == 242u) return get_f32_const(0x3F800000u);
        if (src9 == 243u) return get_f32_const(0xBF800000u);
        if (src9 == 244u) return get_f32_const(0x40000000u);
        if (src9 == 245u) return get_f32_const(0xC0000000u);
        if (src9 == 246u) return get_f32_const(0x40800000u);
        if (src9 == 247u) return get_f32_const(0xC0800000u);
        if (src9 < 106u && live_sgprs.count(src9 & 63u) > 0) {
            const uint32_t su = read_sgpr(src9);
            const uint32_t sf = alloc_id();
            emit(124, {kIdF32, sf, su});
            return sf;
        }
        return read_vgpr(src9 & 0xFFu);
    };

    auto eval_valu_u32 = [&](uint32_t src9, const DecodedInstruction& inst) -> uint32_t {
        if (src9 == 0x0FFu && inst.has_literal) return get_u32_const(inst.literal_u32);
        if (src9 == 0x0FAu && inst.has_dpp) return read_vgpr_u32(inst.raw_word1 & 0xFFu);
        if (src9 == 0x0F9u && inst.has_sdwa) {
            const uint32_t base = read_vgpr_u32(inst.raw_word1 & 0xFFu);
            const uint32_t sel = (inst.raw_word1 >> 16) & 0x7u;
            if (sel < 4u) {
                const uint32_t sh = alloc_id(), m = alloc_id();
                emit(194, {kIdU32, sh, base, get_u32_const(sel * 8u)});
                emit(199, {kIdU32, m, sh, get_u32_const(0xFFu)});
                return m;
            }
            if (sel < 6u) {
                const uint32_t sh = alloc_id(), m = alloc_id();
                emit(194, {kIdU32, sh, base, get_u32_const((sel - 4u) * 16u)});
                emit(199, {kIdU32, m, sh, get_u32_const(0xFFFFu)});
                return m;
            }
            return base;
        }
        if (src9 >= 256u) return read_vgpr_u32((src9 - 256u) & 0xFFu);
        if (src9 >= 128u && src9 <= 192u) return get_u32_const(src9 - 128u);
        if (src9 >= 193u && src9 <= 208u) {
            return get_u32_const(static_cast<uint32_t>(-static_cast<int32_t>(src9 - 192u)));
        }
        if (src9 < 106u && live_sgprs.count(src9 & 63u) > 0) return read_sgpr(src9);
        return read_vgpr_u32(src9 & 0xFFu);
    };

    auto eval_branch_cond = [&](const DecodedInstruction& br) -> uint32_t {
        if (!br.is_conditional_branch || br.mnemonic == "s_branch") {
            return kIdConstTrue;
        }
        uint32_t flag_var = kIdSccVar;
        bool invert = false;
        if (br.mnemonic == "s_cbranch_scc0") { flag_var = kIdSccVar; invert = true; }
        else if (br.mnemonic == "s_cbranch_scc1") { flag_var = kIdSccVar; invert = false; }
        else if (br.mnemonic == "s_cbranch_vccz") { flag_var = kIdVccVar; invert = true; }
        else if (br.mnemonic == "s_cbranch_vccnz") { flag_var = kIdVccVar; invert = false; }
        else if (br.mnemonic == "s_cbranch_execz") { flag_var = kIdExecVar; invert = true; }
        else if (br.mnemonic == "s_cbranch_execnz") { flag_var = kIdExecVar; invert = false; }
        const uint32_t cur = alloc_id();
        emit(61, {kIdBool, cur, flag_var});
        if (!invert) return cur;
        const uint32_t inv = alloc_id();
        emit(168, {kIdBool, inv, cur});                           // OpLogicalNot
        return inv;
    };

    // Pass 2: Emit structured SPIR-V SSA instructions
    auto emit_single_inst = [&](const DecodedInstruction& inst) {
        const uint32_t w0 = inst.raw_word0;
        const uint32_t w1 = inst.raw_word1;
        const uint32_t src0_9 = w0 & 0x1FFu;

        if (inst.mnemonic.rfind("s_load_", 0) == 0 && has_smem) {
            const uint32_t sdata = (w0 >> 6) & 0x7Fu;
            const uint32_t base_reg = (has_gid && sdata >= 4u) ? sdata : 0u;
            const uint32_t ptr_v4 = alloc_id();
            const uint32_t val_v4 = alloc_id();
            emit(65, {kIdPtrUboVec4, ptr_v4, kIdUboVar, kIdConstI0});
            emit(61, {kIdVec4, val_v4, ptr_v4});
            for (uint32_t c = 0; c < inst.mem_dwords && c < 4u; ++c) {
                const uint32_t comp = alloc_id();
                const uint32_t comp_u = alloc_id();
                emit(81, {kIdF32, comp, val_v4, c});
                emit(124, {kIdU32, comp_u, comp});
                write_sgpr(sdata + c, comp_u);
                write_vgpr(base_reg + c, comp);
            }
        } else if (inst.mnemonic == "s_mov_b32") {
            write_sgpr((w0 >> 16) & 0x7Fu, eval_salu_u32(w0 & 0xFFu, inst));
        } else if (inst.mnemonic == "s_not_b32") {
            const uint32_t sdst = (w0 >> 16) & 0x7Fu;
            const uint32_t inv = alloc_id(), nz = alloc_id();
            emit(200, {kIdU32, inv, eval_salu_u32(w0 & 0xFFu, inst)});
            write_sgpr(sdst, inv);
            emit(171, {kIdBool, nz, inv, kIdConstU0});
            emit(62, {kIdSccVar, nz});
        } else if (inst.mnemonic == "s_and_saveexec_b32") {
            const uint32_t sdst = (w0 >> 16) & 0x7Fu;
            const uint32_t src = eval_salu_u32(w0 & 0xFFu, inst);
            const uint32_t nz = alloc_id();
            write_sgpr(sdst, get_u32_const(0xFFFFFFFFu));
            emit(171, {kIdBool, nz, src, kIdConstU0});
            emit(62, {kIdExecVar, nz});
            emit(62, {kIdSccVar, nz});
        } else if (inst.mnemonic == "s_movk_i32") {
            write_sgpr((w0 >> 16) & 0x7Fu, get_u32_const(static_cast<uint32_t>(inst.branch_simm16)));
        } else if (inst.mnemonic == "s_addk_i32" || inst.mnemonic == "s_mulk_i32") {
            const uint32_t sdst = (w0 >> 16) & 0x7Fu;
            const uint32_t cur = read_sgpr(sdst);
            const uint32_t imm = get_u32_const(static_cast<uint32_t>(inst.branch_simm16));
            const uint32_t res = alloc_id();
            emit((inst.mnemonic == "s_addk_i32") ? 128u : 132u, {kIdU32, res, cur, imm});
            write_sgpr(sdst, res);
        } else if (inst.mnemonic == "s_cmpk_eq_i32" || inst.mnemonic == "s_cmpk_lg_i32") {
            const uint32_t cur = read_sgpr((w0 >> 16) & 0x7Fu);
            const uint32_t imm = get_u32_const(static_cast<uint32_t>(inst.branch_simm16));
            const uint32_t cmp = alloc_id();
            emit((inst.mnemonic == "s_cmpk_eq_i32") ? 170u : 171u, {kIdBool, cmp, cur, imm});
            emit(62, {kIdSccVar, cmp});
        } else if (inst.mnemonic == "s_add_u32" || inst.mnemonic == "s_add_i32" ||
                   inst.mnemonic == "s_sub_u32" || inst.mnemonic == "s_sub_i32" ||
                   inst.mnemonic == "s_mul_i32") {
            const uint32_t sdst = (w0 >> 16) & 0x7Fu;
            const uint32_t a = eval_salu_u32(w0 & 0xFFu, inst);
            const uint32_t b = eval_salu_u32((w0 >> 8) & 0xFFu, inst);
            const uint32_t res = alloc_id();
            const uint16_t spv_op = (inst.mnemonic == "s_mul_i32") ? 132u :
                                    (inst.mnemonic.find("sub") != std::string::npos) ? 130u : 128u;
            emit(spv_op, {kIdU32, res, a, b});
            write_sgpr(sdst, res);
            if (spv_op != 132u) {
                const uint32_t carry = alloc_id();
                emit(176, {kIdBool, carry, (spv_op == 128u) ? res : a, (spv_op == 128u) ? a : b});
                emit(62, {kIdSccVar, carry});
            }
        } else if (inst.mnemonic == "s_and_b32" || inst.mnemonic == "s_or_b32" || inst.mnemonic == "s_xor_b32") {
            const uint32_t sdst = (w0 >> 16) & 0x7Fu;
            const uint32_t a = eval_salu_u32(w0 & 0xFFu, inst);
            const uint32_t b = eval_salu_u32((w0 >> 8) & 0xFFu, inst);
            const uint32_t res = alloc_id(), nz = alloc_id();
            const uint16_t spv_op = (inst.mnemonic == "s_and_b32") ? 199u :
                                    (inst.mnemonic == "s_or_b32")  ? 197u : 198u;
            emit(spv_op, {kIdU32, res, a, b});
            write_sgpr(sdst, res);
            emit(171, {kIdBool, nz, res, kIdConstU0});
            emit(62, {kIdSccVar, nz});
        } else if (inst.mnemonic == "s_lshl_b32" || inst.mnemonic == "s_lshr_b32" || inst.mnemonic == "s_ashr_i32") {
            const uint32_t sdst = (w0 >> 16) & 0x7Fu;
            const uint32_t a = eval_salu_u32(w0 & 0xFFu, inst);
            const uint32_t b = eval_salu_u32((w0 >> 8) & 0xFFu, inst);
            const uint32_t sh = alloc_id();
            emit(199, {kIdU32, sh, b, get_u32_const(31u)});
            uint32_t res = 0;
            if (inst.mnemonic == "s_ashr_i32") {
                const uint32_t ai = alloc_id(), ri = alloc_id();
                res = alloc_id();
                emit(124, {kIdI32, ai, a});
                emit(195, {kIdI32, ri, ai, sh});
                emit(124, {kIdU32, res, ri});
            } else {
                res = alloc_id();
                emit((inst.mnemonic == "s_lshl_b32") ? 196u : 194u, {kIdU32, res, a, sh});
            }
            write_sgpr(sdst, res);
            const uint32_t nz = alloc_id();
            emit(171, {kIdBool, nz, res, kIdConstU0});
            emit(62, {kIdSccVar, nz});
        } else if (inst.mnemonic == "s_cselect_b32") {
            const uint32_t sdst = (w0 >> 16) & 0x7Fu;
            const uint32_t a = eval_salu_u32(w0 & 0xFFu, inst);
            const uint32_t b = eval_salu_u32((w0 >> 8) & 0xFFu, inst);
            const uint32_t c = alloc_id(), res = alloc_id();
            emit(61, {kIdBool, c, kIdSccVar});
            emit(169, {kIdU32, res, c, a, b});
            write_sgpr(sdst, res);
        } else if (inst.mnemonic.rfind("s_cmp_", 0) == 0) {
            const uint32_t s0 = w0 & 0xFFu;
            const uint32_t s1 = (w0 >> 8) & 0xFFu;
            const bool use_sgpr = (live_sgprs.count(s0 & 63u) > 0 || live_sgprs.count(s1 & 63u) > 0 ||
                                   inst.has_literal || s0 >= 128u || s1 >= 128u);
            const uint32_t a_u = use_sgpr ? eval_salu_u32(s0, inst) : read_vgpr_u32(s0);
            const uint32_t b_u = use_sgpr ? eval_salu_u32(s1, inst) : read_vgpr_u32(s1);
            const bool is_signed = (inst.mnemonic.find("_i32") != std::string::npos);
            const uint32_t cmp = alloc_id();
            if (inst.mnemonic.find("_eq_") != std::string::npos) {
                emit(170, {kIdBool, cmp, a_u, b_u});
            } else if (inst.mnemonic.find("_lg_") != std::string::npos) {
                emit(171, {kIdBool, cmp, a_u, b_u});
            } else if (is_signed) {
                const uint32_t ai = alloc_id(), bi = alloc_id();
                emit(124, {kIdI32, ai, a_u});
                emit(124, {kIdI32, bi, b_u});
                const uint16_t op = (inst.mnemonic.find("_gt_") != std::string::npos) ? 173u :
                                    (inst.mnemonic.find("_ge_") != std::string::npos) ? 175u :
                                    (inst.mnemonic.find("_le_") != std::string::npos) ? 179u : 177u;
                emit(op, {kIdBool, cmp, ai, bi});
            } else {
                const uint16_t op = (inst.mnemonic.find("_gt_") != std::string::npos) ? 172u :
                                    (inst.mnemonic.find("_ge_") != std::string::npos) ? 174u :
                                    (inst.mnemonic.find("_le_") != std::string::npos) ? 178u : 176u;
                emit(op, {kIdBool, cmp, a_u, b_u});
            }
            emit(62, {kIdSccVar, cmp});
        } else if ((inst.mnemonic == "image_sample" || inst.mnemonic == "image_load") && has_mimg) {
            const uint32_t vdata = (w1 >> 8) & 0xFFu;
            const uint32_t va0 = w1 & 0xFFu;
            const uint32_t va1 = (inst.nsa_dwords > 0) ? (inst.nsa_words[0] & 0xFFu) : ((va0 + 1u) & 0xFFu);
            const uint32_t u_val = read_vgpr(va0);
            const uint32_t v_val = read_vgpr(va1);
            const uint32_t uv_id = alloc_id();
            const uint32_t samp_id = alloc_id();
            const uint32_t texel_id = alloc_id();
            emit(80, {kIdVec2, uv_id, u_val, v_val});
            emit(61, {kIdSampledImage, samp_id, kIdSamplerVar});
            if (exec_model == 4u) {
                emit(87, {kIdVec4, texel_id, samp_id, uv_id});            // OpImageSampleImplicitLod
            } else {
                emit(88, {kIdVec4, texel_id, samp_id, uv_id, 2u, kIdConst0}); // OpImageSampleExplicitLod Lod 0
            }
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t comp = alloc_id();
                emit(81, {kIdF32, comp, texel_id, c});
                write_vgpr(vdata + c, comp);
            }
        } else if (inst.inst_class == Rdna2InstClass::RAYTRACING) {
            // Hardware BVH64 ray-box/slab intersection arithmetic in core SPIR-V 1.5
            const uint32_t vdata = (w1 >> 8) & 0xFFu;
            const uint32_t va = w1 & 0xFFu;
            const uint32_t orig_x = read_vgpr(va);
            const uint32_t dir_x = read_vgpr(va + 1u);
            const uint32_t eps = get_f32_const(0x3727C5ACu);
            const uint32_t quarter = get_f32_const(0x3E800000u);
            const uint32_t dist = alloc_id(), safe_den = alloc_id(), raw_t = alloc_id();
            const uint32_t pos_hit = alloc_id(), t_hit = alloc_id(), hit_f = alloc_id();
            emit(131, {kIdF32, dist, kIdConst1, orig_x});                 // 1.0f - orig.x
            emit(129, {kIdF32, safe_den, dir_x, eps});                    // dir.x + 1e-5f
            emit(135, {kIdF32, raw_t, dist, safe_den});                   // OpFDiv
            emit(186, {kIdBool, pos_hit, raw_t, kIdConst0});              // raw_t > 0.0f
            emit(169, {kIdF32, t_hit, pos_hit, raw_t, get_f32_const(0xBF800000u)});
            emit(169, {kIdF32, hit_f, pos_hit, kIdConst1, kIdConst0});
            write_vgpr(vdata + 0u, t_hit);
            write_vgpr(vdata + 1u, quarter);
            write_vgpr(vdata + 2u, quarter);
            write_vgpr(vdata + 3u, hit_f);
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
        } else if (inst.mnemonic == "v_mov_b32") {
            write_vgpr((w0 >> 17) & 0xFFu, eval_valu_f32(src0_9, inst));
        } else if (inst.mnemonic == "v_cvt_f32_i32") {
            const uint32_t su = eval_valu_u32(src0_9, inst);
            const uint32_t si = alloc_id(), rf = alloc_id();
            emit(124, {kIdI32, si, su});
            emit(111, {kIdF32, rf, si});
            write_vgpr((w0 >> 17) & 0xFFu, rf);
        } else if (inst.mnemonic == "v_cvt_f32_u32") {
            const uint32_t su = eval_valu_u32(src0_9, inst);
            const uint32_t rf = alloc_id();
            emit(112, {kIdF32, rf, su});
            write_vgpr((w0 >> 17) & 0xFFu, rf);
        } else if (inst.mnemonic == "v_cvt_u32_f32") {
            const uint32_t sf = eval_valu_f32(src0_9, inst);
            const uint32_t ru = alloc_id();
            emit(109, {kIdU32, ru, sf});
            write_vgpr_u32((w0 >> 17) & 0xFFu, ru);
        } else if (inst.mnemonic == "v_cvt_i32_f32") {
            const uint32_t sf = eval_valu_f32(src0_9, inst);
            const uint32_t ri = alloc_id(), ru = alloc_id();
            emit(110, {kIdI32, ri, sf});
            emit(124, {kIdU32, ru, ri});
            write_vgpr_u32((w0 >> 17) & 0xFFu, ru);
        } else if (inst.mnemonic == "v_not_b32") {
            const uint32_t su = eval_valu_u32(src0_9, inst);
            const uint32_t ru = alloc_id();
            emit(200, {kIdU32, ru, su});
            write_vgpr_u32((w0 >> 17) & 0xFFu, ru);
        } else if (inst.mnemonic == "v_rcp_f32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t s = eval_valu_f32(src0_9, inst);
            const uint32_t res = alloc_id();
            emit(135, {kIdF32, res, kIdConst1, s});
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_rsq_f32" || inst.mnemonic == "v_sqrt_f32" ||
                   inst.mnemonic == "v_sin_f32" || inst.mnemonic == "v_cos_f32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t s = eval_valu_f32(src0_9, inst);
            const uint32_t res = alloc_id();
            const uint32_t glsl_op = (inst.mnemonic == "v_sin_f32") ? 13u :
                                     (inst.mnemonic == "v_cos_f32") ? 14u :
                                     (inst.mnemonic == "v_sqrt_f32") ? 31u : 32u;
            emit(12, {kIdF32, res, kIdExtGlsl, glsl_op, s});
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_readfirstlane_b32") {
            const uint32_t sdst = (w0 >> 17) & 0xFFu;
            const uint32_t s = eval_valu_f32(src0_9, inst);
            uint32_t res = s;
            if (has_subgroup) {
                res = alloc_id();
                emit(338, {kIdF32, res, kIdConstScopeSub, s});            // OpGroupNonUniformBroadcastFirst
            }
            write_vgpr(sdst, res);
            const uint32_t res_u = alloc_id();
            emit(124, {kIdU32, res_u, res});
            write_sgpr(sdst, res_u);
        } else if (inst.mnemonic == "v_mbcnt_lo_u32_b32" && has_subgroup) {
            const uint32_t vdst = w0 & 0xFFu;
            const uint32_t c = alloc_id();
            emit(61, {kIdBool, c, kIdVccVar});
            const uint32_t bal = alloc_id();
            const uint32_t mask_u32 = alloc_id();
            const uint32_t mask_f32 = alloc_id();
            emit(339, {kIdUVec4, bal, kIdConstScopeSub, c});              // OpGroupNonUniformBallot
            emit(81, {kIdU32, mask_u32, bal, 0u});
            emit(112, {kIdF32, mask_f32, mask_u32});
            write_vgpr(vdst, mask_f32);
        } else if (inst.mnemonic.rfind("v_cmp_", 0) == 0) {
            const uint32_t vsrc1 = (w0 >> 9) & 0xFFu;
            const uint32_t cmp = alloc_id();
            if (inst.mnemonic.find("_u32") != std::string::npos) {
                const uint32_t a = eval_valu_u32(src0_9, inst);
                const uint32_t b = read_vgpr_u32(vsrc1);
                uint16_t spv_cmp = 174u;
                if (inst.mnemonic.find("_lt_") != std::string::npos) spv_cmp = 176u;
                else if (inst.mnemonic.find("_eq_") != std::string::npos) spv_cmp = 170u;
                else if (inst.mnemonic.find("_le_") != std::string::npos) spv_cmp = 178u;
                else if (inst.mnemonic.find("_gt_") != std::string::npos) spv_cmp = 172u;
                else if (inst.mnemonic.find("_lg_") != std::string::npos) spv_cmp = 171u;
                emit(spv_cmp, {kIdBool, cmp, a, b});
            } else if (inst.mnemonic.find("_i32") != std::string::npos) {
                const uint32_t au = eval_valu_u32(src0_9, inst);
                const uint32_t bu = read_vgpr_u32(vsrc1);
                if (inst.mnemonic.find("_eq_") != std::string::npos) {
                    emit(170, {kIdBool, cmp, au, bu});
                } else if (inst.mnemonic.find("_lg_") != std::string::npos) {
                    emit(171, {kIdBool, cmp, au, bu});
                } else {
                    const uint32_t ai = alloc_id(), bi = alloc_id();
                    emit(124, {kIdI32, ai, au});
                    emit(124, {kIdI32, bi, bu});
                    uint16_t spv_cmp = 175u;
                    if (inst.mnemonic.find("_lt_") != std::string::npos) spv_cmp = 177u;
                    else if (inst.mnemonic.find("_le_") != std::string::npos) spv_cmp = 179u;
                    else if (inst.mnemonic.find("_gt_") != std::string::npos) spv_cmp = 173u;
                    emit(spv_cmp, {kIdBool, cmp, ai, bi});
                }
            } else {
                const uint32_t a = eval_valu_f32(src0_9, inst);
                const uint32_t b = read_vgpr(vsrc1);
                uint16_t spv_cmp = 190u;
                if (inst.mnemonic.find("_lt_") != std::string::npos) spv_cmp = 184u;
                else if (inst.mnemonic.find("_eq_") != std::string::npos) spv_cmp = 180u;
                else if (inst.mnemonic.find("_le_") != std::string::npos) spv_cmp = 188u;
                else if (inst.mnemonic.find("_gt_") != std::string::npos) spv_cmp = 186u;
                else if (inst.mnemonic.find("_lg_") != std::string::npos) spv_cmp = 182u;
                emit(spv_cmp, {kIdBool, cmp, a, b});
            }
            emit(62, {kIdVccVar, cmp});
        } else if (inst.mnemonic == "v_add_f32" || inst.mnemonic == "v_sub_f32" ||
                   inst.mnemonic == "v_subrev_f32" || inst.mnemonic == "v_mul_f32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t s0 = eval_valu_f32(src0_9, inst);
            const uint32_t s1 = read_vgpr((w0 >> 9) & 0xFFu);
            const uint32_t lhs = (inst.mnemonic == "v_subrev_f32") ? s1 : s0;
            const uint32_t rhs = (inst.mnemonic == "v_subrev_f32") ? s0 : s1;
            const uint32_t res = alloc_id();
            const uint16_t op = (inst.mnemonic == "v_mul_f32") ? 133u :
                                (inst.mnemonic == "v_add_f32") ? 129u : 131u;
            emit(op, {kIdF32, res, lhs, rhs});
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_min_f32" || inst.mnemonic == "v_max_f32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t s0 = eval_valu_f32(src0_9, inst);
            const uint32_t s1 = read_vgpr((w0 >> 9) & 0xFFu);
            const uint32_t res = alloc_id();
            emit(12, {kIdF32, res, kIdExtGlsl, (inst.mnemonic == "v_min_f32") ? 40u : 43u, s0, s1});
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_add_nc_u32" || inst.mnemonic == "v_sub_nc_u32" ||
                   inst.mnemonic == "v_subrev_nc_u32" || inst.mnemonic == "v_mul_i32_i24") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t s0 = eval_valu_u32(src0_9, inst);
            const uint32_t s1 = read_vgpr_u32((w0 >> 9) & 0xFFu);
            const uint32_t lhs = (inst.mnemonic == "v_subrev_nc_u32") ? s1 : s0;
            const uint32_t rhs = (inst.mnemonic == "v_subrev_nc_u32") ? s0 : s1;
            const uint32_t res = alloc_id();
            const uint16_t op = (inst.mnemonic == "v_mul_i32_i24") ? 132u :
                                (inst.mnemonic == "v_add_nc_u32")  ? 128u : 130u;
            emit(op, {kIdU32, res, lhs, rhs});
            write_vgpr_u32(vdst, res);
        } else if (inst.mnemonic == "v_lshlrev_b32" || inst.mnemonic == "v_lshrrev_b32" || inst.mnemonic == "v_ashrrev_i32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t raw_sh = eval_valu_u32(src0_9, inst);
            const uint32_t sh = alloc_id();
            emit(199, {kIdU32, sh, raw_sh, get_u32_const(31u)});
            const uint32_t val = read_vgpr_u32((w0 >> 9) & 0xFFu);
            if (inst.mnemonic == "v_ashrrev_i32") {
                const uint32_t vi = alloc_id(), ri = alloc_id(), ru = alloc_id();
                emit(124, {kIdI32, vi, val});
                emit(195, {kIdI32, ri, vi, sh});
                emit(124, {kIdU32, ru, ri});
                write_vgpr_u32(vdst, ru);
            } else {
                const uint32_t ru = alloc_id();
                emit((inst.mnemonic == "v_lshlrev_b32") ? 196u : 194u, {kIdU32, ru, val, sh});
                write_vgpr_u32(vdst, ru);
            }
        } else if (inst.mnemonic == "v_and_b32" || inst.mnemonic == "v_or_b32" || inst.mnemonic == "v_xor_b32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t a = eval_valu_u32(src0_9, inst);
            const uint32_t b = read_vgpr_u32((w0 >> 9) & 0xFFu);
            const uint32_t res = alloc_id();
            const uint16_t op = (inst.mnemonic == "v_and_b32") ? 199u :
                                (inst.mnemonic == "v_or_b32")  ? 197u : 198u;
            emit(op, {kIdU32, res, a, b});
            write_vgpr_u32(vdst, res);
        } else if (inst.mnemonic == "v_cndmask_b32") {
            const uint32_t vdst = (w0 >> 17) & 0xFFu;
            const uint32_t vfalse = eval_valu_f32(src0_9, inst);
            const uint32_t vtrue = read_vgpr((w0 >> 9) & 0xFFu);
            const uint32_t c = alloc_id();
            const uint32_t res = alloc_id();
            emit(61, {kIdBool, c, kIdVccVar});
            emit(169, {kIdF32, res, c, vtrue, vfalse});
            write_vgpr(vdst, res);
        } else if (inst.mnemonic == "v_fmamk_f32" || inst.mnemonic == "v_fmaak_f32" ||
                   inst.mnemonic == "v_fmac_f32" || inst.mnemonic == "v_fma_f32" ||
                   inst.mnemonic == "v_fma_mix_f32" || inst.mnemonic.rfind("v_pk_", 0) == 0) {
            const bool is_vop2 = (inst.mnemonic == "v_fmamk_f32" || inst.mnemonic == "v_fmaak_f32" || inst.mnemonic == "v_fmac_f32");
            const uint32_t vdst = is_vop2 ? ((w0 >> 17) & 0xFFu) : (w0 & 0xFFu);
            uint32_t a = 0, b = 0, c = 0;
            if (inst.mnemonic == "v_fmamk_f32") {
                a = eval_valu_f32(src0_9, inst);
                b = get_f32_const(inst.literal_u32);
                c = read_vgpr((w0 >> 9) & 0xFFu);
            } else if (inst.mnemonic == "v_fmaak_f32") {
                a = eval_valu_f32(src0_9, inst);
                b = read_vgpr((w0 >> 9) & 0xFFu);
                c = get_f32_const(inst.literal_u32);
            } else if (inst.mnemonic == "v_fmac_f32") {
                a = eval_valu_f32(src0_9, inst);
                b = read_vgpr((w0 >> 9) & 0xFFu);
                c = read_vgpr(vdst);
            } else {
                a = (w1 != 0) ? eval_valu_f32(w1 & 0x1FFu, inst) : read_vgpr(0u);
                b = (w1 != 0) ? eval_valu_f32((w1 >> 9) & 0x1FFu, inst) : read_vgpr(1u);
                c = (w1 != 0) ? eval_valu_f32((w1 >> 18) & 0x1FFu, inst) : read_vgpr(vdst);
            }
            if (inst.mnemonic == "v_pk_mul_f16") {
                const uint32_t prod = alloc_id();
                emit(133, {kIdF32, prod, a, b});
                write_vgpr(vdst, prod);
            } else if (inst.mnemonic == "v_pk_add_f16" || inst.mnemonic == "v_pk_sub_f16") {
                const uint32_t sum = alloc_id();
                emit((inst.mnemonic == "v_pk_add_f16") ? 129u : 131u, {kIdF32, sum, a, b});
                write_vgpr(vdst, sum);
            } else {
                const uint32_t prod = alloc_id();
                const uint32_t sum = alloc_id();
                emit(133, {kIdF32, prod, a, b});
                emit(129, {kIdF32, sum, prod, c});
                write_vgpr(vdst, sum);
            }
        } else if (inst.mnemonic == "v_add3_u32" || inst.mnemonic == "v_lshl_or_b32") {
            const uint32_t vdst = w0 & 0xFFu;
            const uint32_t a = eval_valu_u32(w1 & 0x1FFu, inst);
            const uint32_t b = eval_valu_u32((w1 >> 9) & 0x1FFu, inst);
            const uint32_t c = eval_valu_u32((w1 >> 18) & 0x1FFu, inst);
            if (inst.mnemonic == "v_add3_u32") {
                const uint32_t s01 = alloc_id(), s012 = alloc_id();
                emit(128, {kIdU32, s01, a, b});
                emit(128, {kIdU32, s012, s01, c});
                write_vgpr_u32(vdst, s012);
            } else {
                const uint32_t sh = alloc_id(), shl = alloc_id(), res = alloc_id();
                emit(199, {kIdU32, sh, b, get_u32_const(31u)});
                emit(196, {kIdU32, shl, a, sh});
                emit(197, {kIdU32, res, shl, c});
                write_vgpr_u32(vdst, res);
            }
        } else if (inst.inst_class == Rdna2InstClass::VMEM &&
                   inst.mnemonic.find("load") != std::string::npos && has_ssbo) {
            const bool is_flat = (inst.mnemonic.rfind("global_", 0) == 0 ||
                                  inst.mnemonic.rfind("flat_", 0) == 0 ||
                                  inst.mnemonic.rfind("scratch_", 0) == 0);
            const uint32_t vdst = is_flat ? ((w1 >> 24) & 0xFFu)
                                          : ((w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu));
            const uint32_t vaddr = (w1 != 0) ? (w1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
            const uint32_t addr_f = read_vgpr(vaddr);
            const uint32_t base_i = alloc_id();
            emit(110, {kIdI32, base_i, addr_f});                          // OpConvertFToS
            const uint32_t dword_off = (w0 & 0xFFFu) / 4u;
            for (uint32_t c = 0; c < inst.mem_dwords; ++c) {
                uint32_t elem_idx = base_i;
                if (dword_off + c > 0u) {
                    elem_idx = alloc_id();
                    emit(128, {kIdI32, elem_idx, base_i, get_i32_const(dword_off + c)});
                }
                const uint32_t elem_ptr = alloc_id();
                const uint32_t loaded = alloc_id();
                emit(65, {kIdPtrSsboF32, elem_ptr, kIdSsboVar, kIdConstI0, elem_idx});
                emit(61, {kIdF32, loaded, elem_ptr});
                write_vgpr(vdst + c, loaded);
            }
        } else if (inst.inst_class == Rdna2InstClass::VMEM &&
                   inst.mnemonic.find("store") != std::string::npos && has_ssbo) {
            const uint32_t vdata = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
            const uint32_t vaddr = (w1 != 0) ? (w1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
            const uint32_t addr_f = read_vgpr(vaddr);
            const uint32_t base_i = alloc_id();
            emit(110, {kIdI32, base_i, addr_f});                          // OpConvertFToS
            const uint32_t dword_off = (w0 & 0xFFFu) / 4u;
            for (uint32_t c = 0; c < inst.mem_dwords; ++c) {
                uint32_t elem_idx = base_i;
                if (dword_off + c > 0u) {
                    elem_idx = alloc_id();
                    emit(128, {kIdI32, elem_idx, base_i, get_i32_const(dword_off + c)});
                }
                const uint32_t val_f = read_vgpr(vdata + c);
                const uint32_t elem_ptr = alloc_id();
                emit(65, {kIdPtrSsboF32, elem_ptr, kIdSsboVar, kIdConstI0, elem_idx});
                emit(62, {elem_ptr, val_f});
            }
        } else if (inst.mnemonic == "ds_read_b32" && has_lds) {
            const uint32_t vdst = (w1 != 0) ? ((w1 >> 24) & 0xFFu) : ((w0 >> 18) & 0xFFu);
            const uint32_t addr_f = read_vgpr(w1 & 0xFFu);
            const uint32_t addr_i = alloc_id();
            const uint32_t masked_i = alloc_id();
            const uint32_t lds_ptr = alloc_id();
            const uint32_t loaded = alloc_id();
            emit(110, {kIdI32, addr_i, addr_f});
            emit(199, {kIdI32, masked_i, addr_i, kIdConstI255});
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
            emit(199, {kIdI32, masked_i, addr_i, kIdConstI255});
            emit(65, {kIdPtrWgF32, lds_ptr, kIdLdsVar, masked_i});
            emit(62, {lds_ptr, val_f});
        } else if (inst.mnemonic == "s_barrier" && has_barrier) {
            emit(224, {kIdConstScopeWg, kIdConstScopeWg, kIdConstSemWg}); // OpControlBarrier
        } else if (inst.mnemonic == "exp") {
            const uint32_t target = (w0 >> 4) & 0x3Fu;
            const uint32_t c0 = read_vgpr(w1 & 0xFFu);
            const uint32_t c1 = read_vgpr((w1 >> 8) & 0xFFu);
            const uint32_t c2 = read_vgpr((w1 >> 16) & 0xFFu);
            const uint32_t c3 = read_vgpr((w1 >> 24) & 0xFFu);
            if (has_vs_out && target >= 12u && target <= 15u) {
                const uint32_t vec_id = alloc_id();
                emit(80, {kIdVec4, vec_id, c0, c1, c2, c3});
                emit(62, {kIdVsPosVar, vec_id});
            } else if (exec_model == 4u) {
                if (target < 8u && ps_mrt_vars.count(target) > 0) {
                    const uint32_t vec_id = alloc_id();
                    emit(80, {kIdVec4, vec_id, c0, c1, c2, c3});
                    emit(62, {ps_mrt_vars[target], vec_id});
                } else if (target == 8u && has_mrtz) {
                    emit(62, {kIdMrtzVar, c0});
                }
            }
        }
    };

    std::function<void(size_t, size_t, const std::vector<SpirvCfgRegion>&)> emit_cfg_range;
    emit_cfg_range = [&](size_t begin_idx, size_t end_idx, const std::vector<SpirvCfgRegion>& children) {
        size_t cur = begin_idx;
        for (const auto& reg : children) {
            for (size_t k = cur; k < reg.begin; ++k) {
                emit_single_inst(insts[k]);
            }
            if (reg.is_loop) {
                const uint32_t iter_var = loop_iter_vars[reg.loop_id];
                emit(62, {iter_var, kIdConstU0});                         // Reset loop bound = 0u
                const uint32_t lbl_hdr = alloc_id();
                const uint32_t lbl_body = alloc_id();
                const uint32_t lbl_cont = alloc_id();
                const uint32_t lbl_merge = alloc_id();
                emit(249, {lbl_hdr});                                     // OpBranch %lbl_hdr
                emit(248, {lbl_hdr});                                     // OpLabel %lbl_hdr
                emit(246, {lbl_merge, lbl_cont, 0u});                     // OpLoopMerge %lbl_merge %lbl_cont None
                emit(249, {lbl_body});                                    // OpBranch %lbl_body
                emit(248, {lbl_body});                                    // OpLabel %lbl_body
                emit_cfg_range(reg.inner_begin, reg.inner_end, reg.children);
                emit(249, {lbl_cont});                                    // OpBranch %lbl_cont
                emit(248, {lbl_cont});                                    // OpLabel %lbl_cont
                const uint32_t iter_cur = alloc_id();
                const uint32_t iter_next = alloc_id();
                const uint32_t lt_bound = alloc_id();
                const uint32_t br_cond = eval_branch_cond(insts[reg.branch_idx]);
                const uint32_t cont_cond = alloc_id();
                emit(61, {kIdU32, iter_cur, iter_var});
                emit(128, {kIdU32, iter_next, iter_cur, get_u32_const(1u)});
                emit(62, {iter_var, iter_next});
                emit(176, {kIdBool, lt_bound, iter_next, get_u32_const(1024u)}); // iter_next < 1024u
                emit(167, {kIdBool, cont_cond, lt_bound, br_cond});
                emit(250, {cont_cond, lbl_hdr, lbl_merge});               // OpBranchConditional
                emit(248, {lbl_merge});                                   // OpLabel %lbl_merge
            } else {
                const uint32_t skip_cond = eval_branch_cond(insts[reg.branch_idx]);
                const uint32_t lbl_body = alloc_id();
                const uint32_t lbl_merge = alloc_id();
                emit(247, {lbl_merge, 0u});                               // OpSelectionMerge %lbl_merge None
                emit(250, {skip_cond, lbl_merge, lbl_body});              // OpBranchConditional %skip_cond %lbl_merge %lbl_body
                emit(248, {lbl_body});                                    // OpLabel %lbl_body
                emit_cfg_range(reg.inner_begin, reg.inner_end, reg.children);
                emit(249, {lbl_merge});                                   // OpBranch %lbl_merge
                emit(248, {lbl_merge});                                   // OpLabel %lbl_merge
            }
            cur = reg.end;
        }
        for (size_t k = cur; k < end_idx; ++k) {
            emit_single_inst(insts[k]);
        }
    };

    emit_cfg_range(0u, insts.size(), cfg_roots);

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

    const auto* raw = static_cast<const uint8_t*>(microcode);
    scarlettx::Xs2ParsedHeaderInfo hdr{};
    scarlettx::ParseXs2PrecompiledHeader(raw, byteSize, hdr);

    const uint8_t* code = hdr.isa_ptr;
    const size_t code_size = hdr.isa_size;
    size_t offset = 0;
    uint32_t srvs = 0, uavs = 0, cbvs = 0, samplers = 0;

    while (offset < code_size) {
        DecodedInstruction inst = DecodeRdna2Instruction(
            code + offset, code_size - offset, static_cast<uint32_t>(offset));
        if (inst.mnemonic.find("store") != std::string::npos && inst.inst_class != Rdna2InstClass::LDS) {
            uavs++;
        } else if (inst.mnemonic.find("image_sample") != std::string::npos) {
            srvs++;
            samplers++;
        } else if (inst.inst_class == Rdna2InstClass::RAYTRACING ||
                   (inst.mnemonic.find("load") != std::string::npos &&
                    inst.inst_class != Rdna2InstClass::LDS &&
                    inst.mnemonic.rfind("s_load", 0) != 0)) {
            srvs++;
        } else if (inst.mnemonic.rfind("s_load", 0) == 0) {
            cbvs++;
        }
        offset += inst.inst_bytes;
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
    void* dxilBytecode, size_t byteSize, const XS2_REGISTER_REMAP* remapTable, uint32_t remapCount) {
    if (!dxilBytecode || byteSize == 0) return kInvalidArg;
    if (remapCount > 0 && !remapTable) return kInvalidArg;
    if (remapCount == 0) return kSuccess;

    auto* bytes = static_cast<uint8_t*>(dxilBytecode);

    // 1. SPIR-V 1.x module in-place OpDecorate Binding (33) & DescriptorSet (34) remapping
    if (byteSize >= 20 && (byteSize % 4u) == 0 && ReadLe32(bytes) == 0x07230203u) {
        auto* words = static_cast<uint32_t*>(dxilBytecode);
        const size_t word_count = byteSize / 4u;
        struct SpvVarDecorations {
            bool has_binding = false;
            bool has_set = false;
            size_t binding_word_idx = 0;
            size_t set_word_idx = 0;
            uint32_t orig_binding = 0;
        };
        std::unordered_map<uint32_t, SpvVarDecorations> decs;
        for (size_t wi = 5; wi < word_count;) {
            const uint32_t wc = words[wi] >> 16;
            const uint32_t op = words[wi] & 0xFFFFu;
            if (wc < 1 || wi + wc > word_count || op == 54u) break;  // Stop at OpFunction
            if (op == 71u && wc >= 4u) {                              // OpDecorate
                const uint32_t target_id = words[wi + 1];
                const uint32_t dec_kind = words[wi + 2];
                if (dec_kind == 33u) {  // Binding
                    decs[target_id].has_binding = true;
                    decs[target_id].binding_word_idx = wi + 3;
                    decs[target_id].orig_binding = words[wi + 3];
                } else if (dec_kind == 34u) {  // DescriptorSet
                    decs[target_id].has_set = true;
                    decs[target_id].set_word_idx = wi + 3;
                }
            }
            wi += wc;
        }
        for (const auto& [tid, info] : decs) {
            (void)tid;
            if (!info.has_binding) continue;
            for (uint32_t r = 0; r < remapCount; ++r) {
                if (remapTable[r].oldRegister == info.orig_binding) {
                    words[info.binding_word_idx] = remapTable[r].newRegister;
                    if (info.has_set) {
                        words[info.set_word_idx] = remapTable[r].registerSpace;
                    }
                    break;
                }
            }
        }
        return kSuccess;
    }

    // 2. DXBC / DXIL container in-place RDEF, RTS0, and SHEX/SHDR register remapping + re-signing
    if (byteSize >= 32 && std::memcmp(bytes, "DXBC", 4) == 0) {
        const uint32_t chunk_count = ReadLe32(bytes + 28);
        if (32ull + static_cast<size_t>(chunk_count) * 4ull > byteSize) {
            return kInvalidArg;
        }
        bool has_dxil = false;
        bool modified = false;

        auto find_remap = [&](uint32_t orig_reg) -> const XS2_REGISTER_REMAP* {
            for (uint32_t r = 0; r < remapCount; ++r) {
                if (remapTable[r].oldRegister == orig_reg) return &remapTable[r];
            }
            return nullptr;
        };

        for (uint32_t ci = 0; ci < chunk_count; ++ci) {
            const uint32_t chunk_off = ReadLe32(bytes + 32 + ci * 4);
            if (static_cast<size_t>(chunk_off) + 8ull > byteSize) continue;
            std::string fcc(reinterpret_cast<const char*>(bytes + chunk_off), 4);
            const uint32_t chunk_sz = ReadLe32(bytes + chunk_off + 4);
            if (static_cast<size_t>(chunk_off) + 8ull + static_cast<size_t>(chunk_sz) > byteSize) continue;
            uint8_t* cdata = bytes + chunk_off + 8;

            if (fcc == "DXIL" || fcc == "ILDB") {
                has_dxil = true;
            } else if (fcc == "RDEF" && chunk_sz >= 28u) {
                const uint32_t bind_cnt = ReadLe32(cdata + 8);
                const uint32_t bind_off = ReadLe32(cdata + 12);
                const uint8_t minor_v = cdata[16];
                const uint8_t major_v = cdata[17];
                const size_t stride = (major_v > 5u || (major_v == 5u && minor_v >= 1u)) ? 40u : 32u;
                for (uint32_t b = 0; b < bind_cnt; ++b) {
                    const size_t eoff = static_cast<size_t>(bind_off) + static_cast<size_t>(b) * stride;
                    if (eoff + stride > chunk_sz) break;
                    const uint32_t orig_bind = ReadLe32(cdata + eoff + 20u);
                    if (const auto* rm = find_remap(orig_bind)) {
                        WriteLe32(cdata + eoff + 20u, rm->newRegister);
                        if (stride == 40u) {
                            WriteLe32(cdata + eoff + 32u, rm->registerSpace);
                        }
                        modified = true;
                    }
                }
            } else if (fcc == "RTS0" && chunk_sz >= 24u) {
                const uint32_t rs_ver = ReadLe32(cdata + 0);
                const uint32_t num_params = ReadLe32(cdata + 4);
                const uint32_t params_off = ReadLe32(cdata + 8);
                for (uint32_t pi = 0; pi < num_params; ++pi) {
                    const size_t poff = static_cast<size_t>(params_off) + static_cast<size_t>(pi) * 12u;
                    if (poff + 12u > chunk_sz) break;
                    const uint32_t ptype = ReadLe32(cdata + poff + 0);
                    const uint32_t pay_off = ReadLe32(cdata + poff + 8);
                    if (ptype >= 1u && ptype <= 4u && static_cast<size_t>(pay_off) + 8u <= chunk_sz) {
                        const uint32_t orig_reg = ReadLe32(cdata + pay_off + 0);
                        if (const auto* rm = find_remap(orig_reg)) {
                            WriteLe32(cdata + pay_off + 0, rm->newRegister);
                            WriteLe32(cdata + pay_off + 4, rm->registerSpace);
                            modified = true;
                        }
                    } else if (ptype == 0u && static_cast<size_t>(pay_off) + 8u <= chunk_sz) {
                        const uint32_t nranges = ReadLe32(cdata + pay_off + 0);
                        const uint32_t roff = ReadLe32(cdata + pay_off + 4);
                        const size_t rstride = (rs_ver >= 2u) ? 24u : 20u;
                        for (uint32_t ri = 0; ri < nranges; ++ri) {
                            const size_t cur_r = static_cast<size_t>(roff) + static_cast<size_t>(ri) * rstride;
                            if (cur_r + rstride > chunk_sz) break;
                            const uint32_t orig_base = ReadLe32(cdata + cur_r + 8u);
                            if (const auto* rm = find_remap(orig_base)) {
                                WriteLe32(cdata + cur_r + 8u, rm->newRegister);
                                WriteLe32(cdata + cur_r + 12u, rm->registerSpace);
                                modified = true;
                            }
                        }
                    }
                }
            } else if ((fcc == "SHEX" || fcc == "SHDR") && chunk_sz >= 8u) {
                const size_t dw_cnt = chunk_sz / 4u;
                for (size_t pos = 2; pos < dw_cnt;) {
                    const uint32_t tok0 = ReadLe32(cdata + pos * 4u);
                    const uint32_t opcode = tok0 & 0x7FFu;
                    size_t ilen = 0;
                    if (opcode == 53u) {  // D3D10_SB_OPCODE_CUSTOMDATA
                        if (pos + 1u >= dw_cnt) break;
                        ilen = ReadLe32(cdata + (pos + 1u) * 4u);
                    } else {
                        ilen = (tok0 >> 24) & 0x7Fu;
                    }
                    if (ilen == 0 || pos + ilen > dw_cnt) break;

                    // Patch resource operand register indices in SM 5.0 declarations
                    if (opcode == 88u || opcode == 89u || opcode == 90u ||
                        opcode == 156u || opcode == 157u || opcode == 158u ||
                        opcode == 161u || opcode == 162u) {
                        size_t op_pos = pos + 1u;
                        if (opcode == 88u || opcode == 156u) {
                            // Resource return type token precedes or follows operand? In SM4/5 dcl_resource,
                            // operand is first at pos+1!
                        }
                        if (op_pos < pos + ilen) {
                            const uint32_t op_tok = ReadLe32(cdata + op_pos * 4u);
                            const uint32_t op_type = (op_tok >> 12) & 0xFFu;
                            const uint32_t idx_dim = (op_tok >> 20) & 0x3u;
                            const uint32_t idx0_rep = (op_tok >> 22) & 0x7u;
                            size_t ext = ((op_tok & 0x80000000u) != 0) ? 1u : 0u;
                            const size_t reg_pos = op_pos + 1u + ext;
                            if ((op_type == 6u || op_type == 7u || op_type == 8u || op_type == 30u) &&
                                idx_dim >= 1u && idx0_rep == 0u && reg_pos < pos + ilen) {
                                const uint32_t orig_reg = ReadLe32(cdata + reg_pos * 4u);
                                if (const auto* rm = find_remap(orig_reg)) {
                                    WriteLe32(cdata + reg_pos * 4u, rm->newRegister);
                                    modified = true;
                                }
                            }
                        }
                    }
                    pos += ilen;
                }
            }
        }

        if (modified) {
            if (!has_dxil || !scarlettx::SignDxilContainerInPlace(bytes, byteSize)) {
                scarlettx::UpdateDxbcContainerChecksum(bytes, byteSize);
            }
        }
        return kSuccess;
    }

    // 3. In-place HLSL source register(...) text remapping when ASCII shader text is passed
    std::string_view sv(reinterpret_cast<const char*>(bytes), byteSize);
    size_t scan = 0;
    while ((scan = sv.find("register(", scan)) != std::string_view::npos) {
        const size_t reg_char_pos = scan + 9u;
        if (reg_char_pos + 1u >= byteSize) break;
        const char kind = static_cast<char>(bytes[reg_char_pos]);
        if (kind == 'b' || kind == 't' || kind == 'u' || kind == 's') {
            size_t num_start = reg_char_pos + 1u;
            size_t num_end = num_start;
            uint32_t parsed = 0;
            while (num_end < byteSize && bytes[num_end] >= '0' && bytes[num_end] <= '9') {
                parsed = parsed * 10u + static_cast<uint32_t>(bytes[num_end] - '0');
                ++num_end;
            }
            if (num_end > num_start) {
                for (uint32_t r = 0; r < remapCount; ++r) {
                    if (remapTable[r].oldRegister == parsed) {
                        const std::string rep = std::to_string(remapTable[r].newRegister);
                        const size_t span = num_end - num_start;
                        if (rep.size() <= span) {
                            std::memcpy(bytes + num_start, rep.data(), rep.size());
                            for (size_t pad = rep.size(); pad < span; ++pad) {
                                bytes[num_start + pad] = ' ';
                            }
                        }
                        break;
                    }
                }
            }
            scan = num_end;
        } else {
            scan = reg_char_pos;
        }
    }
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
    scarlettx::Xs2ParsedHeaderInfo hdr{};
    scarlettx::ParseXs2PrecompiledHeader(static_cast<const uint8_t*>(microcode), byteSize, hdr);
    const bool wave64 = ((hdr.pgm_rsrc2 & (1u << 20)) != 0);
    outProps->waveSize = wave64 ? 64u : 32u;  // Scarlett Anaconda RDNA2 defaults to Wave32
    outProps->maxWavesPerComputeUnit = wave64 ? 16u : 32u;
    outProps->simdWidth = 32u;
    return kSuccess;
}

int32_t SCARLETTX_XS2_CALL Xs2ShaderTranslateComputeWorkgroupSize(
    const void* microcode, size_t byteSize, uint32_t* outX, uint32_t* outY, uint32_t* outZ) {
    if (!microcode || byteSize == 0 || !outX || !outY || !outZ) return kInvalidArg;
    scarlettx::Xs2ParsedHeaderInfo hdr{};
    scarlettx::ParseXs2PrecompiledHeader(static_cast<const uint8_t*>(microcode), byteSize, hdr);
    *outX = hdr.threads_x;
    *outY = hdr.threads_y;
    *outZ = hdr.threads_z;
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
    *outBytecode = nullptr;
    *outBytecodeSize = 0;

    auto* p = static_cast<PipelineState*>(pipeline);
    const uint64_t h = SimpleHash(static_cast<const uint8_t*>(microcode), byteSize);

    std::lock_guard<std::mutex> lk(p->mu);
    auto it = p->cache.find(h);
    if (it != p->cache.end()) {
        if (!it->second.empty()) {
            p->stats.cacheHits++;
            auto* buf = new uint8_t[it->second.size()];
            std::memcpy(buf, it->second.data(), it->second.size());
            *outBytecode = buf;
            *outBytecodeSize = it->second.size();
            return kSuccess;
        }
        p->cache.erase(it);
    }

    // Optional persistent on-disk shader cache when SCARLETTX_SHADER_CACHE_DIR is set
    std::filesystem::path disk_cache_path;
    if (const char* cache_dir = std::getenv("SCARLETTX_SHADER_CACHE_DIR"); cache_dir && cache_dir[0] != '\0') {
        std::ostringstream fname;
        fname << std::hex << std::setw(16) << std::setfill('0') << h << ".dxil";
        disk_cache_path = std::filesystem::path(cache_dir) / fname.str();
        std::error_code ec;
        if (std::filesystem::exists(disk_cache_path, ec)) {
            std::vector<uint8_t> cached;
            {
                std::ifstream ifs(disk_cache_path, std::ios::binary);
                if (ifs) {
                    cached.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
                }
            }
            if (cached.empty()) {
                // Evict corrupt 0-byte disk cache artifact and proceed to re-translate
                std::filesystem::remove(disk_cache_path, ec);
            } else {
                p->stats.cacheHits++;
                InsertPipelineCacheBounded(p, h, cached);
                auto* buf = new uint8_t[cached.size()];
                std::memcpy(buf, cached.data(), cached.size());
                *outBytecode = buf;
                *outBytecodeSize = cached.size();
                return kSuccess;
            }
        }
    }

    p->stats.cacheMisses++;

    // Lift to DXIL/DXBC, timing execution via steady_clock, and strictly verify success
    const auto t_start = std::chrono::steady_clock::now();
    void* lifted_code = nullptr;
    size_t lifted_size = 0;
    const int32_t rc = Xs2ShaderLiftToDxil(microcode, byteSize, nullptr, &lifted_code, &lifted_size);
    const auto t_end = std::chrono::steady_clock::now();
    const uint64_t elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count());
    p->stats.totalTranslationTimeUs += std::max<uint64_t>(1ull, elapsed_us);

    if (rc != kSuccess || !lifted_code || lifted_size == 0) {
        if (lifted_code) {
            delete[] static_cast<uint8_t*>(lifted_code);
        }
        return (rc != kSuccess) ? rc : kInvalidArg;
    }

    p->stats.totalShadersTranslated++;
    std::vector<uint8_t> lifted(
        static_cast<uint8_t*>(lifted_code),
        static_cast<uint8_t*>(lifted_code) + lifted_size);
    delete[] static_cast<uint8_t*>(lifted_code);

    InsertPipelineCacheBounded(p, h, lifted);
    if (!disk_cache_path.empty() && !lifted.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(disk_cache_path.parent_path(), ec);
        std::ofstream ofs(disk_cache_path, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(reinterpret_cast<const char*>(lifted.data()), static_cast<std::streamsize>(lifted.size()));
        }
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
    p->fifo_order.clear();
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
