#include "scarlettx/gpu/shader_ir.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <d3dcompiler.h>
#if __has_include(<dxcapi.h>)
#include <dxcapi.h>
#define SCARLETTX_HAS_DXC_HEADER 1
#endif
#endif

namespace scarlettx {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void WriteU32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

bool IsXboxPrivateFourCC(const std::string& fourcc) {
    return fourcc == "XBOX" || fourcc == "XS2M" || fourcc == "XDBG" || fourcc == "XBIN";
}

void Md5TransformBlock(uint32_t state[4], const uint8_t block[64]) {
    static constexpr uint32_t kT[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };
    static constexpr uint8_t kS[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = ReadU32(block + i * 4);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f = 0, g = 0;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5u * i + 1u) & 15u;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) & 15u;
        } else {
            f = c ^ (b | (~d));
            g = (7u * i) & 15u;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        const uint32_t sum = a + f + kT[i] + m[g];
        b = b + ((sum << kS[i]) | (sum >> (32u - kS[i])));
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

ShaderStage DecodeDxbcProgramType(uint16_t program_type) {
    switch (program_type) {
        case 0: return ShaderStage::Pixel;
        case 1: return ShaderStage::Vertex;
        case 2: return ShaderStage::Geometry;
        case 3: return ShaderStage::Hull;
        case 4: return ShaderStage::Domain;
        case 5: return ShaderStage::Compute;
        case 6: return ShaderStage::Library;
        default: return ShaderStage::Unknown;
    }
}

}  // namespace

uint64_t HashShaderBytes(const uint8_t* data, size_t size) {
    uint64_t h = kFnvOffset;
    if (!data) {
        return h;
    }
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnvPrime;
    }
    return h;
}

bool UpdateDxbcContainerChecksum(uint8_t* dxbc_data, size_t total_size) {
    if (!dxbc_data || total_size < 32 || std::memcmp(dxbc_data, "DXBC", 4) != 0) {
        return false;
    }
    const uint8_t* p = dxbc_data + 20;
    const uint32_t msg_len = static_cast<uint32_t>(total_size - 20);
    const uint32_t num_bits = msg_len * 8u;
    const uint32_t last_word = (num_bits >> 2) | 1u;

    uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    const uint32_t full_bytes = msg_len & ~63u;
    for (uint32_t off = 0; off < full_bytes; off += 64u) {
        Md5TransformBlock(state, p + off);
    }

    const uint32_t rem = msg_len - full_bytes;
    const uint8_t* tail = p + full_bytes;
    if (rem >= 56u) {
        uint8_t blk1[64]{};
        std::memcpy(blk1, tail, rem);
        blk1[rem] = 0x80u;
        Md5TransformBlock(state, blk1);

        uint8_t blk2[64]{};
        WriteU32(blk2 + 0, num_bits);
        WriteU32(blk2 + 60, last_word);
        Md5TransformBlock(state, blk2);
    } else {
        uint8_t blk[64]{};
        WriteU32(blk + 0, num_bits);
        if (rem > 0) {
            std::memcpy(blk + 4, tail, rem);
        }
        blk[4 + rem] = 0x80u;
        WriteU32(blk + 60, last_word);
        Md5TransformBlock(state, blk);
    }

    for (int i = 0; i < 4; ++i) {
        WriteU32(dxbc_data + 4 + i * 4, state[i]);
    }
    return true;
}

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info) {
    out_info = ShaderModuleInfo{};
    out_info.byte_size = static_cast<uint32_t>(bytecode.size());
    out_info.content_hash = HashShaderBytes(bytecode.data(), bytecode.size());

    if (bytecode.size() >= 4 && std::memcmp(bytecode.data(), "XS2\0", 4) == 0) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
        if (bytecode.size() >= 12) {
            const uint32_t stage_hint = ReadU32(bytecode.data() + 8);
            if (stage_hint == 1) out_info.stage = ShaderStage::Vertex;
            else if (stage_hint == 2) out_info.stage = ShaderStage::Pixel;
            else if (stage_hint == 3) out_info.stage = ShaderStage::Compute;
        }
        return Status::Ok();
    }

    if (bytecode.size() < 32) {
        return Status::Error(StatusCode::ShaderError, "Shader container too small (< 32 bytes)");
    }

    if (std::memcmp(bytecode.data(), "DXBC", 4) == 0) {
        const uint32_t chunk_count = ReadU32(bytecode.data() + 28);
        out_info.chunk_count = chunk_count;
        out_info.kind = ShaderContainerKind::Dxbc;

        const size_t offsets_pos = 32;
        if (offsets_pos + static_cast<size_t>(chunk_count) * 4 > bytecode.size()) {
            return Status::Error(StatusCode::ShaderError, "DXBC chunk offset table truncated");
        }

        for (uint32_t i = 0; i < chunk_count; ++i) {
            const uint32_t chunk_off = ReadU32(bytecode.data() + offsets_pos + i * 4);
            if (static_cast<size_t>(chunk_off) + 8 > bytecode.size()) {
                continue;
            }
            std::string fourcc(reinterpret_cast<const char*>(bytecode.data() + chunk_off), 4);
            const uint32_t chunk_size = ReadU32(bytecode.data() + chunk_off + 4);
            const size_t data_off = static_cast<size_t>(chunk_off) + 8;

            if (fourcc == "DXIL" || fourcc == "ILDB") {
                out_info.kind = ShaderContainerKind::Dxil;
                if (data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                    const uint32_t ver_token = ReadU32(bytecode.data() + data_off);
                    const uint16_t prog_type = static_cast<uint16_t>((ver_token >> 16) & 0xFFFFu);
                    out_info.stage = DecodeDxbcProgramType(prog_type);
                    out_info.shader_model_major = (ver_token >> 4) & 0xFu;
                    out_info.shader_model_minor = ver_token & 0xFu;
                }
            } else if ((fourcc == "SHEX" || fourcc == "SHDR") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                const uint32_t ver_token = ReadU32(bytecode.data() + data_off);
                const uint16_t prog_type = static_cast<uint16_t>((ver_token >> 16) & 0xFFFFu);
                out_info.stage = DecodeDxbcProgramType(prog_type);
                out_info.shader_model_major = (ver_token >> 4) & 0xFu;
                out_info.shader_model_minor = ver_token & 0xFu;
            } else if ((fourcc == "ISGN" || fourcc == "ISG1") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                out_info.input_param_count = ReadU32(bytecode.data() + data_off);
            } else if ((fourcc == "OSGN" || fourcc == "OSG1") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                out_info.output_param_count = ReadU32(bytecode.data() + data_off);
            } else if (fourcc == "RDAT") {
                out_info.has_raytracing_exports = true;
                if (out_info.stage == ShaderStage::Unknown) {
                    out_info.stage = ShaderStage::Library;
                }
            } else if (fourcc == "RTS0") {
                out_info.has_embedded_root_signature = true;
            }

            if (IsXboxPrivateFourCC(fourcc)) {
                out_info.has_xbox_private_chunks = true;
            }

            out_info.chunk_fourccs.push_back(std::move(fourcc));
        }
        return Status::Ok();
    }

    if (std::memcmp(bytecode.data(), "XS2\0", 4) == 0) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
        return Status::Ok();
    }

    return Status::Error(StatusCode::UnsupportedFormat, "Unrecognized shader container magic");
}

namespace {

struct MiniRdna2Inst {
    uint32_t w0 = 0;
    uint32_t w1 = 0;
    bool is_64 = false;
    std::string op;
};

MiniRdna2Inst DecodeMiniRdna2(const uint8_t* p, size_t rem) {
    MiniRdna2Inst ins{};
    if (rem < 4) return ins;
    ins.w0 = ReadU32(p);
    const uint32_t w0 = ins.w0;

    if ((w0 & 0xFF800000u) == 0xBF800000u) {
        const uint32_t sop = (w0 >> 16) & 0x7Fu;
        if (sop == 0x00 || sop == 0x01 || w0 == 0xBF800000u) ins.op = "s_endpgm";
        else if (sop == 0x0A) ins.op = "s_barrier";
        else if (sop == 0x02 || (sop >= 0x04 && sop <= 0x08)) ins.op = "s_branch";
        else ins.op = "s_nop";
        return ins;
    }
    if ((w0 & 0xFF800000u) == 0xBF000000u) {
        const uint32_t sop = (w0 >> 16) & 0x7Fu;
        ins.op = (sop == 0x00 || sop == 0x06) ? "s_cmp_eq" : "s_cmp_lt";
        return ins;
    }
    if ((w0 & 0xFC000000u) == 0xF4000000u) {
        ins.is_64 = (rem >= 8);
        if (ins.is_64) ins.w1 = ReadU32(p + 4);
        ins.op = "s_load_dwordx4";
        return ins;
    }
    if ((w0 & 0xFE000000u) == 0x7E000000u) {
        const uint32_t vop = (w0 >> 9) & 0xFFu;
        if (vop == 0x01 || vop == 0x05 || vop == 0x06) ins.op = "v_mov_b32";
        else if (vop == 0x02) ins.op = "v_readfirstlane_b32";
        else if (vop == 0x2A) ins.op = "v_rcp_f32";
        else if (vop == 0x2E) ins.op = "v_rsq_f32";
        else if (vop == 0x33) ins.op = "v_sqrt_f32";
        else if (vop == 0x35) ins.op = "v_sin_f32";
        else if (vop == 0x36) ins.op = "v_cos_f32";
        else ins.op = "v_mov_b32";
        return ins;
    }
    if ((w0 & 0xFE000000u) == 0x7C000000u) {
        const uint32_t kind = ((w0 >> 17) & 0xFFu) & 0x07u;
        if (kind == 0x01) ins.op = "v_cmp_lt_f32";
        else if (kind == 0x02) ins.op = "v_cmp_eq_f32";
        else if (kind == 0x04) ins.op = "v_cmp_gt_f32";
        else ins.op = "v_cmp_ge_f32";
        return ins;
    }
    if ((w0 & 0x80000000u) == 0) {
        const uint32_t vop2 = (w0 >> 25) & 0x3Fu;
        if (vop2 == 0x03) ins.op = "v_add_f32";
        else if (vop2 == 0x04) ins.op = "v_sub_f32";
        else if (vop2 == 0x05) ins.op = "v_subrev_f32";
        else if (vop2 == 0x08) ins.op = "v_mul_f32";
        else if (vop2 == 0x0A) ins.op = "v_min_f32";
        else if (vop2 == 0x0B) ins.op = "v_max_f32";
        else if (vop2 == 0x18) ins.op = "v_cndmask_b32";
        else if (vop2 == 0x19) ins.op = "v_fmac_f32";
        else ins.op = "s_nop";
        return ins;
    }
    if ((w0 & 0xFC000000u) == 0xD4000000u) {
        ins.is_64 = (rem >= 8);
        if (ins.is_64) ins.w1 = ReadU32(p + 4);
        ins.op = (((w0 >> 16) & 0x3FFu) == 0x2FFu) ? "v_mbcnt_lo_u32_b32" : "v_fma_f32";
        return ins;
    }
    if ((w0 & 0xFC000000u) == 0xE0000000u) {
        ins.is_64 = (rem >= 8);
        if (ins.is_64) ins.w1 = ReadU32(p + 4);
        const uint32_t mop = (w0 >> 18) & 0x7Fu;
        const bool store = ((w0 & (1u << 25)) != 0) || (mop >= 0x1Cu && mop <= 0x1Fu);
        ins.op = store ? "buffer_store_dword" : "buffer_load_dword";
        return ins;
    }
    if ((w0 & 0xFC000000u) == 0xF8000000u) {
        ins.is_64 = (rem >= 8);
        if (ins.is_64) ins.w1 = ReadU32(p + 4);
        ins.op = "exp";
        return ins;
    }
    if ((w0 & 0xFC000000u) == 0xC8000000u) {
        ins.op = "v_interp";
        return ins;
    }
    if ((w0 & 0xFC000000u) == 0xF0000000u) {
        ins.is_64 = (rem >= 8);
        if (ins.is_64) ins.w1 = ReadU32(p + 4);
        ins.op = "image_sample";
        return ins;
    }
    if ((w0 & 0xFC000000u) == 0xD8000000u) {
        ins.is_64 = (rem >= 8);
        if (ins.is_64) ins.w1 = ReadU32(p + 4);
        ins.op = (w0 & (1u << 25)) ? "ds_write_b32" : "ds_read_b32";
        return ins;
    }
    ins.op = "s_nop";
    return ins;
}

#if defined(_WIN32) && defined(SCARLETTX_HAS_DXC_HEADER)
bool TryCompileHlslWithSignedDxc(
    const std::string& hlsl_source,
    const std::wstring& entry_w,
    const std::wstring& profile_w,
    std::vector<uint8_t>& out_dxil) {
    struct DxcRuntime {
        HMODULE mod = nullptr;
        DxcCreateInstanceProc create_fn = nullptr;
        DxcRuntime() {
            if (const char* env_dir = std::getenv("SCARLETTX_DXC_DIR"); env_dir && env_dir[0] != '\0') {
                const auto p = std::filesystem::path(env_dir) / "dxcompiler.dll";
                if (std::filesystem::exists(p)) {
                    mod = LoadLibraryExW(p.wstring().c_str(), nullptr,
                                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                }
            }
            if (!mod) {
                mod = LoadLibraryW(L"dxcompiler.dll");
            }
            if (!mod) {
                const std::filesystem::path kits_root = "C:/Program Files (x86)/Windows Kits/10/bin";
                std::error_code ec;
                if (std::filesystem::exists(kits_root, ec)) {
                    std::filesystem::path best_dll;
                    for (const auto& entry : std::filesystem::directory_iterator(kits_root, ec)) {
                        if (!entry.is_directory(ec)) continue;
                        const auto cand = entry.path() / "x64" / "dxcompiler.dll";
                        const auto signer = entry.path() / "x64" / "dxil.dll";
                        if (std::filesystem::exists(cand, ec) && std::filesystem::exists(signer, ec)) {
                            if (cand > best_dll) best_dll = cand;
                        }
                    }
                    if (!best_dll.empty()) {
                        mod = LoadLibraryExW(best_dll.wstring().c_str(), nullptr,
                                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                    }
                }
            }
            if (mod) {
                create_fn = reinterpret_cast<DxcCreateInstanceProc>(
                    reinterpret_cast<void*>(GetProcAddress(mod, "DxcCreateInstance")));
            }
        }
    };
    static DxcRuntime dxc;
    if (!dxc.create_fn) return false;

    IDxcCompiler3* compiler = nullptr;
    if (FAILED(dxc.create_fn(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler) {
        return false;
    }

    DxcBuffer src_buf{};
    src_buf.Ptr = hlsl_source.data();
    src_buf.Size = hlsl_source.size();
    src_buf.Encoding = DXC_CP_UTF8;

    const wchar_t* args[] = {
        L"-E", entry_w.c_str(),
        L"-T", profile_w.c_str(),
        L"-O1",
        L"-HV", L"2021",
    };

    IDxcResult* result = nullptr;
    const HRESULT hr = compiler->Compile(&src_buf, args, 7, nullptr, IID_PPV_ARGS(&result));
    compiler->Release();
    if (FAILED(hr) || !result) {
        if (result) result->Release();
        return false;
    }

    HRESULT compile_status = E_FAIL;
    result->GetStatus(&compile_status);
    if (FAILED(compile_status)) {
        result->Release();
        return false;
    }

    IDxcBlob* obj_blob = nullptr;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj_blob), nullptr)) || !obj_blob) {
        result->Release();
        return false;
    }

    const auto* ptr = static_cast<const uint8_t*>(obj_blob->GetBufferPointer());
    const size_t sz = obj_blob->GetBufferSize();
    bool signed_ok = false;
    if (ptr && sz >= 32 && std::memcmp(ptr, "DXBC", 4) == 0) {
        for (size_t i = 4; i < 20; ++i) {
            if (ptr[i] != 0) {
                signed_ok = true;
                break;
            }
        }
    }
    if (signed_ok) {
        out_dxil.assign(ptr, ptr + sz);
    }
    obj_blob->Release();
    result->Release();
    return signed_ok;
}
#endif

}  // namespace

Status LiftRdna2MicrocodeToPcShader(
    const uint8_t* microcode,
    size_t byte_size,
    ShaderStage stage_override,
    uint32_t target_shader_model,
    bool enable_wave32,
    std::vector<uint8_t>& out_pc_dxbc,
    ShaderStage* out_detected_stage) {
    out_pc_dxbc.clear();
    if (!microcode || byte_size < 4) {
        return Status::Error(StatusCode::InvalidArgument, "Empty RDNA2 microcode buffer");
    }

    std::vector<MiniRdna2Inst> insts;
    bool has_smem = false, has_mimg = false, has_ssbo = false, has_lds = false;
    bool has_vs_exp = false, has_ps_exp = false;
    std::unordered_set<uint32_t> regs = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};

    size_t off = 0;
    while (off < byte_size) {
        MiniRdna2Inst ins = DecodeMiniRdna2(microcode + off, byte_size - off);
        off += ins.is_64 ? 8u : 4u;
        if (ins.op == "s_load_dwordx4") has_smem = true;
        else if (ins.op == "image_sample") has_mimg = true;
        else if (ins.op == "buffer_load_dword" || ins.op == "buffer_store_dword") has_ssbo = true;
        else if (ins.op == "ds_read_b32" || ins.op == "ds_write_b32" || ins.op == "s_barrier") has_lds = true;
        else if (ins.op == "v_interp") has_ps_exp = true;
        else if (ins.op == "exp") {
            const uint32_t target = (ins.w0 >> 4) & 0x3Fu;
            if (target >= 12 && target <= 15) has_vs_exp = true;
            else has_ps_exp = true;
        }
        regs.insert(ins.w0 & 0xFFu);
        regs.insert((ins.w0 >> 9) & 0xFFu);
        regs.insert((ins.w0 >> 17) & 0xFFu);
        regs.insert((ins.w0 >> 18) & 0xFFu);
        if (ins.is_64) {
            regs.insert(ins.w1 & 0xFFu);
            regs.insert((ins.w1 >> 8) & 0xFFu);
            regs.insert((ins.w1 >> 9) & 0xFFu);
            regs.insert((ins.w1 >> 16) & 0xFFu);
            regs.insert((ins.w1 >> 18) & 0xFFu);
            regs.insert((ins.w1 >> 24) & 0xFFu);
        }
        insts.push_back(std::move(ins));
    }

    ShaderStage stage = stage_override;
    if (stage == ShaderStage::Unknown) {
        if (has_vs_exp) stage = ShaderStage::Vertex;
        else if (has_ps_exp) stage = ShaderStage::Pixel;
        else stage = has_ssbo ? ShaderStage::Compute : ShaderStage::Pixel;
    }
    if (out_detected_stage) *out_detected_stage = stage;

    auto build_hlsl = [&](bool sm6_wave_ops) -> std::string {
        std::ostringstream h;
        if (has_smem) {
            h << "cbuffer Xs2Ubo : register(b0) { float4 ubo0; };\n";
        }
        if (has_mimg && stage != ShaderStage::Vertex) {
            h << "Texture2D<float4> Xs2Tex0 : register(t0);\n";
            h << "SamplerState Xs2Samp0 : register(s0);\n";
        }
        if (has_ssbo && stage == ShaderStage::Compute) {
            h << "RWByteAddressBuffer Xs2Ssbo0 : register(u0);\n";
        }
        if (has_lds && stage == ShaderStage::Compute) {
            h << "groupshared float lds_mem[256];\n";
        }

        if (stage == ShaderStage::Vertex) {
            h << "struct VSOut { float4 pos : SV_POSITION; float4 attr0 : TEXCOORD0; };\n";
            h << "VSOut main(uint vid : SV_VertexID) {\n";
        } else if (stage == ShaderStage::Pixel) {
            h << "struct PSIn { float4 pos : SV_POSITION; float4 attr0 : TEXCOORD0; };\n";
            h << "float4 main(PSIn pin) : SV_TARGET0 {\n";
        } else {
            if (sm6_wave_ops && enable_wave32) {
                h << "[WaveSize(32)]\n";
            }
            h << "[numthreads(16, 1, 1)]\n";
            h << "void main(uint3 tid : SV_DispatchThreadID) {\n";
        }

        std::vector<uint32_t> sorted_regs(regs.begin(), regs.end());
        std::sort(sorted_regs.begin(), sorted_regs.end());
        for (uint32_t r : sorted_regs) {
            h << "    float v" << r << " = 1.0f;\n";
        }
        h << "    bool cond = true;\n";
        if (stage == ShaderStage::Vertex) {
            h << "    float2 tri[3] = { float2(0.0f, 0.7f), float2(0.7f, -0.7f), float2(-0.7f, -0.7f) };\n";
            h << "    v0 = tri[vid % 3u].x; v1 = tri[vid % 3u].y; v2 = 0.0f; v3 = 1.0f;\n";
            h << "    float4 out_pos = float4(v0, v1, 0.0f, 1.0f);\n";
            h << "    float4 out_attr = float4(1.0f, 1.0f, 1.0f, 1.0f);\n";
        } else if (stage == ShaderStage::Pixel) {
            h << "    float4 out_mrt0 = pin.attr0;\n";
        } else {
            h << "    v0 = (float)tid.x;\n";
        }

        for (const auto& ins : insts) {
            const uint32_t w0 = ins.w0;
            const uint32_t w1 = ins.w1;
            if (ins.op == "s_load_dwordx4" && has_smem) {
                h << "    v0 = ubo0.x; v1 = ubo0.y; v2 = ubo0.z; v3 = ubo0.w;\n";
            } else if (ins.op == "v_interp" && stage == ShaderStage::Pixel) {
                const uint32_t vd = (w0 >> 18) & 0xFFu;
                const char ch = "xyzw"[(w0 >> 8) & 0x3u];
                h << "    v" << vd << " = pin.attr0." << ch << ";\n";
            } else if (ins.op == "image_sample" && has_mimg && stage != ShaderStage::Vertex) {
                const uint32_t vd = (w1 >> 8) & 0xFFu;
                const uint32_t va = w1 & 0xFFu;
                h << "    { float4 smp = Xs2Tex0.SampleLevel(Xs2Samp0, float2(v" << va << ", v" << ((va + 1) & 0xFFu) << "), 0.0f);\n";
                h << "      v" << vd << " = smp.x; v" << ((vd + 1) & 0xFFu) << " = smp.y; v"
                  << ((vd + 2) & 0xFFu) << " = smp.z; v" << ((vd + 3) & 0xFFu) << " = smp.w; }\n";
            } else if (ins.op == "v_mov_b32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = v" << (w0 & 0xFFu) << ";\n";
            } else if (ins.op == "v_readfirstlane_b32") {
                const uint32_t vd = (w0 >> 17) & 0xFFu;
                const uint32_t vs = w0 & 0xFFu;
                if (sm6_wave_ops && stage == ShaderStage::Compute) {
                    h << "    v" << vd << " = WaveReadLaneFirst(v" << vs << ");\n";
                } else {
                    h << "    v" << vd << " = v" << vs << ";\n";
                }
            } else if (ins.op == "v_mbcnt_lo_u32_b32") {
                const uint32_t vd = w0 & 0xFFu;
                if (sm6_wave_ops && stage == ShaderStage::Compute) {
                    h << "    v" << vd << " = (float)WaveActiveBallot(cond).x;\n";
                } else {
                    h << "    v" << vd << " = cond ? 1.0f : 0.0f;\n";
                }
            } else if (ins.op == "v_rcp_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = 1.0f / max(abs(v" << (w0 & 0xFFu) << "), 1e-6f);\n";
            } else if (ins.op == "v_rsq_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = rsqrt(max(abs(v" << (w0 & 0xFFu) << "), 1e-6f));\n";
            } else if (ins.op == "v_sqrt_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = sqrt(max(v" << (w0 & 0xFFu) << ", 0.0f));\n";
            } else if (ins.op == "v_sin_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = sin(v" << (w0 & 0xFFu) << ");\n";
            } else if (ins.op == "v_cos_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = cos(v" << (w0 & 0xFFu) << ");\n";
            } else if (ins.op == "v_cmp_lt_f32" || ins.op == "s_cmp_lt") {
                h << "    cond = (v" << (w0 & 0xFFu) << " < v" << ((w0 >> 9) & 0xFFu) << ");\n";
            } else if (ins.op == "v_cmp_eq_f32" || ins.op == "s_cmp_eq") {
                h << "    cond = (v" << (w0 & 0xFFu) << " == v" << ((w0 >> 9) & 0xFFu) << ");\n";
            } else if (ins.op == "v_cmp_gt_f32") {
                h << "    cond = (v" << (w0 & 0xFFu) << " > v" << ((w0 >> 9) & 0xFFu) << ");\n";
            } else if (ins.op == "v_cmp_ge_f32") {
                h << "    cond = (v" << (w0 & 0xFFu) << " >= v" << ((w0 >> 9) & 0xFFu) << ");\n";
            } else if (ins.op == "v_add_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = v" << ((w0 >> 9) & 0xFFu) << " + v" << (w0 & 0xFFu) << ";\n";
            } else if (ins.op == "v_sub_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = v" << (w0 & 0xFFu) << " - v" << ((w0 >> 9) & 0xFFu) << ";\n";
            } else if (ins.op == "v_subrev_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = v" << ((w0 >> 9) & 0xFFu) << " - v" << (w0 & 0xFFu) << ";\n";
            } else if (ins.op == "v_mul_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = v" << ((w0 >> 9) & 0xFFu) << " * v" << (w0 & 0xFFu) << ";\n";
            } else if (ins.op == "v_min_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = min(v" << ((w0 >> 9) & 0xFFu) << ", v" << (w0 & 0xFFu) << ");\n";
            } else if (ins.op == "v_max_f32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = max(v" << ((w0 >> 9) & 0xFFu) << ", v" << (w0 & 0xFFu) << ");\n";
            } else if (ins.op == "v_cndmask_b32") {
                h << "    v" << ((w0 >> 17) & 0xFFu) << " = cond ? v" << ((w0 >> 9) & 0xFFu) << " : v" << (w0 & 0xFFu) << ";\n";
            } else if (ins.op == "v_fmac_f32" || ins.op == "v_fma_f32") {
                const uint32_t vd = (ins.op == "v_fma_f32") ? (w0 & 0xFFu) : ((w0 >> 17) & 0xFFu);
                const uint32_t a = (ins.op == "v_fma_f32" && w1 != 0) ? (w1 & 0xFFu) : 0u;
                const uint32_t b = (ins.op == "v_fma_f32" && w1 != 0) ? ((w1 >> 9) & 0xFFu) : 1u;
                const uint32_t c = (ins.op == "v_fma_f32" && w1 != 0) ? ((w1 >> 18) & 0xFFu) : vd;
                h << "    v" << vd << " = mad(v" << a << ", v" << b << ", v" << c << ");\n";
            } else if (ins.op == "buffer_load_dword" && has_ssbo && stage == ShaderStage::Compute) {
                const uint32_t vd = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
                const uint32_t va = (w1 != 0) ? (w1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
                h << "    v" << vd << " = asfloat(Xs2Ssbo0.Load(((uint)max(v" << va << ", 0.0f)) * 4u));\n";
            } else if (ins.op == "buffer_store_dword" && has_ssbo && stage == ShaderStage::Compute) {
                const uint32_t vd = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
                const uint32_t va = (w1 != 0) ? (w1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
                h << "    Xs2Ssbo0.Store(((uint)max(v" << va << ", 0.0f)) * 4u, asuint(v" << vd << "));\n";
            } else if (ins.op == "ds_read_b32" && has_lds && stage == ShaderStage::Compute) {
                const uint32_t vd = (w1 != 0) ? ((w1 >> 24) & 0xFFu) : ((w0 >> 18) & 0xFFu);
                const uint32_t va = w1 & 0xFFu;
                h << "    v" << vd << " = lds_mem[((uint)max(v" << va << ", 0.0f)) & 255u];\n";
            } else if (ins.op == "ds_write_b32" && has_lds && stage == ShaderStage::Compute) {
                const uint32_t vd = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
                const uint32_t va = w1 & 0xFFu;
                h << "    lds_mem[((uint)max(v" << va << ", 0.0f)) & 255u] = v" << vd << ";\n";
            } else if (ins.op == "s_barrier" && has_lds && stage == ShaderStage::Compute) {
                h << "    GroupMemoryBarrierWithGroupSync();\n";
            } else if (ins.op == "exp") {
                const uint32_t target = (w0 >> 4) & 0x3Fu;
                const uint32_t c0 = w1 & 0xFFu, c1 = (w1 >> 8) & 0xFFu;
                const uint32_t c2 = (w1 >> 16) & 0xFFu, c3 = (w1 >> 24) & 0xFFu;
                if (stage == ShaderStage::Vertex) {
                    if (target >= 12 && target <= 15) {
                        h << "    out_pos = float4(v" << c0 << ", v" << c1 << ", v" << c2 << ", v" << c3 << ");\n";
                    } else {
                        h << "    out_attr = float4(v" << c0 << ", v" << c1 << ", v" << c2 << ", v" << c3 << ");\n";
                    }
                } else if (stage == ShaderStage::Pixel) {
                    h << "    out_mrt0 = float4(v" << c0 << ", v" << c1 << ", v" << c2 << ", v" << c3 << ");\n";
                }
            }
        }

        if (stage == ShaderStage::Vertex) {
            h << "    VSOut o; o.pos = out_pos; o.attr0 = out_attr; return o;\n}\n";
        } else if (stage == ShaderStage::Pixel) {
            h << "    return out_mrt0;\n}\n";
        } else {
            h << "}\n";
        }
        return h.str();
    };

#if defined(_WIN32) && defined(SCARLETTX_HAS_DXC_HEADER)
    if (target_shader_model >= 0x00060000u) {
        const uint32_t minor = std::min<uint32_t>(target_shader_model & 0xFFu, 6u);
        const std::wstring prefix = (stage == ShaderStage::Vertex) ? L"vs_6_" :
                                    (stage == ShaderStage::Pixel)  ? L"ps_6_" : L"cs_6_";
        const std::wstring profile = prefix + std::to_wstring(minor);
        const std::string hlsl_sm6 = build_hlsl(true);
        if (TryCompileHlslWithSignedDxc(hlsl_sm6, L"main", profile, out_pc_dxbc)) {
            return Status::Ok();
        }
    }
#else
    (void)target_shader_model;
    (void)enable_wave32;
#endif

    // Universal retail-signed SM 5.0 DXBC fallback via D3DCompile (d3dcompiler_47.dll)
    const char* target_sm5 = (stage == ShaderStage::Vertex) ? "vs_5_0" :
                             (stage == ShaderStage::Pixel)  ? "ps_5_0" : "cs_5_0";
    return CompileHlslToDxbc(build_hlsl(false), "main", target_sm5, out_pc_dxbc);
}

Status TranslateXboxShaderToPc(
    const std::vector<uint8_t>& xbox_bytecode,
    ShaderTranslationResult& out_result) {
    out_result = ShaderTranslationResult{};
    Status st = InspectShaderContainer(xbox_bytecode, out_result.source_info);
    if (!st.ok()) {
        return st;
    }

    if (out_result.source_info.kind == ShaderContainerKind::ScarlettRdna2) {
        // Standalone XS2\0 container layout:
        //   [0..3]  = "XS2\0" magic
        //   [4..7]  = container version (uint32)
        //   [8..11] = optional stage override (0=auto, 1=VS, 2=PS, 3=CS)
        //   [12..15]= optional code byte size (0 = remainder of buffer)
        //   [16..]  = Scarlett RDNA2 ISA microcode words
        if (xbox_bytecode.size() < 20) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "XS2 container has header only without RDNA2 microcode payload");
        }
        const uint32_t explicit_len = ReadU32(xbox_bytecode.data() + 12);
        const size_t avail = xbox_bytecode.size() - 16u;
        const size_t code_len = (explicit_len > 0 && explicit_len <= avail) ? explicit_len : avail;
        ShaderStage detected = ShaderStage::Unknown;
        Status lift_st = LiftRdna2MicrocodeToPcShader(
            xbox_bytecode.data() + 16,
            code_len,
            out_result.source_info.stage,
            0x00060005u,
            true,
            out_result.pc_bytecode,
            &detected);
        if (!lift_st.ok()) {
            return lift_st;
        }
        out_result.source_info.stage = detected;
        out_result.lifted_from_rdna2 = true;
        out_result.translated_hash =
            HashShaderBytes(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
        return Status::Ok();
    }

    if (!out_result.source_info.has_xbox_private_chunks) {
        out_result.pc_bytecode = xbox_bytecode;
        out_result.translated_hash = out_result.source_info.content_hash;
        out_result.stripped_xbox_chunks = 0;
        return Status::Ok();
    }

    // Rebuild DXBC container without Xbox-private chunks while preserving all standard chunks.
    // Also capture XS2M/XBIN microcode in case the retail container stripped PC DXIL/SHDR!
    const uint32_t orig_count = ReadU32(xbox_bytecode.data() + 28);
    struct ChunkSlice {
        const uint8_t* ptr;
        size_t total_len;
    };
    std::vector<ChunkSlice> kept;
    kept.reserve(orig_count);
    bool has_pc_exec_chunk = false;
    const uint8_t* embedded_xs2_ptr = nullptr;
    size_t embedded_xs2_len = 0;

    for (uint32_t i = 0; i < orig_count; ++i) {
        const uint32_t off = ReadU32(xbox_bytecode.data() + 32 + i * 4);
        if (static_cast<size_t>(off) + 8 > xbox_bytecode.size()) {
            continue;
        }
        std::string fourcc(reinterpret_cast<const char*>(xbox_bytecode.data() + off), 4);
        const uint32_t sz = ReadU32(xbox_bytecode.data() + off + 4);
        const size_t total_len = 8ull + static_cast<size_t>(sz);
        if (static_cast<size_t>(off) + total_len > xbox_bytecode.size()) {
            return Status::Error(StatusCode::ShaderError, "DXBC chunk extends past end of buffer");
        }
        if (IsXboxPrivateFourCC(fourcc)) {
            if ((fourcc == "XS2M" || fourcc == "XBIN") && sz >= 4 && !embedded_xs2_ptr) {
                embedded_xs2_ptr = xbox_bytecode.data() + off + 8;
                embedded_xs2_len = sz;
            }
            ++out_result.stripped_xbox_chunks;
            continue;
        }
        if (fourcc == "DXIL" || fourcc == "ILDB" || fourcc == "SHDR" || fourcc == "SHEX") {
            has_pc_exec_chunk = true;
        }
        kept.push_back({xbox_bytecode.data() + off, total_len});
    }

    // Retail Xbox DXBC container with stripped DXIL/SHDR and only XS2M/XBIN microcode: lift directly!
    if (!has_pc_exec_chunk && embedded_xs2_ptr && embedded_xs2_len >= 4) {
        ShaderStage detected = ShaderStage::Unknown;
        Status lift_st = LiftRdna2MicrocodeToPcShader(
            embedded_xs2_ptr,
            embedded_xs2_len,
            out_result.source_info.stage,
            0x00060005u,
            true,
            out_result.pc_bytecode,
            &detected);
        if (!lift_st.ok()) {
            return lift_st;
        }
        out_result.source_info.stage = detected;
        out_result.lifted_from_rdna2 = true;
        out_result.translated_hash =
            HashShaderBytes(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
        return Status::Ok();
    }

    const size_t header_and_table = 32ull + kept.size() * 4ull;
    size_t total_size = header_and_table;
    for (const auto& c : kept) {
        total_size += c.total_len;
    }

    out_result.pc_bytecode.resize(total_size, 0);
    // Copy DXBC magic + version fields (bytes 0..23), then write updated size + chunk count.
    std::memcpy(out_result.pc_bytecode.data(), xbox_bytecode.data(), 24);
    WriteU32(out_result.pc_bytecode.data() + 24, static_cast<uint32_t>(total_size));
    WriteU32(out_result.pc_bytecode.data() + 28, static_cast<uint32_t>(kept.size()));

    size_t cursor = header_and_table;
    for (size_t i = 0; i < kept.size(); ++i) {
        WriteU32(out_result.pc_bytecode.data() + 32 + i * 4, static_cast<uint32_t>(cursor));
        std::memcpy(out_result.pc_bytecode.data() + cursor, kept[i].ptr, kept[i].total_len);
        cursor += kept[i].total_len;
    }

    UpdateDxbcContainerChecksum(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
    out_result.translated_hash =
        HashShaderBytes(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
    return Status::Ok();
}

Status CompileHlslToDxbc(
    const std::string& hlsl_source,
    const std::string& entry_point,
    const std::string& target,
    std::vector<uint8_t>& out_bytecode) {
    out_bytecode.clear();
#if defined(_WIN32)
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        hlsl_source.data(),
        hlsl_source.size(),
        "scarlettx_inline.hlsl",
        nullptr,
        nullptr,
        entry_point.c_str(),
        target.c_str(),
        D3DCOMPILE_OPTIMIZATION_LEVEL1,
        0,
        &code,
        &errors);
    if (FAILED(hr) || !code) {
        std::string msg = "D3DCompile failed";
        if (errors && errors->GetBufferPointer() && errors->GetBufferSize() > 0) {
            msg += ": ";
            msg.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        if (errors) errors->Release();
        if (code) code->Release();
        return Status::Error(StatusCode::ShaderError, msg);
    }
    const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
    out_bytecode.assign(bytes, bytes + code->GetBufferSize());
    code->Release();
    if (errors) errors->Release();
    return Status::Ok();
#else
    (void)hlsl_source;
    (void)entry_point;
    (void)target;
    return Status::Error(StatusCode::NotImplemented, "D3DCompile requires Windows");
#endif
}

bool ShaderCache::Lookup(uint64_t key, std::vector<uint8_t>& out_blob) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++misses_;
        return false;
    }
    ++hits_;
    out_blob = it->second;
    return true;
}

void ShaderCache::Store(uint64_t key, std::vector<uint8_t> blob) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = std::move(blob);
}

void ShaderCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    hits_ = 0;
    misses_ = 0;
}

uint64_t ShaderCache::hits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

uint64_t ShaderCache::misses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
}

size_t ShaderCache::entry_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace scarlettx
