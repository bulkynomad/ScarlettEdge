#include "scarlettx/gpu/shader_ir.h"
#include "scarlettx/gpu/xs2_abi.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
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
        case 6:
        case 7:   // RayGeneration
        case 8:   // Intersection
        case 9:   // AnyHit
        case 10:  // ClosestHit
        case 11:  // Miss
        case 12:  // Callable
            return ShaderStage::Library;
        case 13: return ShaderStage::Mesh;
        case 14: return ShaderStage::Amplification;
        default: return ShaderStage::Unknown;
    }
}

ShaderStage StageFromXs2Hint(uint32_t hint) {
    switch (hint) {
        case 1: return ShaderStage::Vertex;
        case 2: return ShaderStage::Pixel;
        case 3: return ShaderStage::Compute;
        case 4: return ShaderStage::Geometry;
        case 5: return ShaderStage::Hull;
        case 6: return ShaderStage::Domain;
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
            return ShaderStage::Library;
        case 13: return ShaderStage::Mesh;
        case 14: return ShaderStage::Amplification;
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

bool SignDxilContainerInPlace(uint8_t* dxbc_data, size_t total_size) {
    if (!dxbc_data || total_size < 32 || std::memcmp(dxbc_data, "DXBC", 4) != 0) {
        return false;
    }
#if defined(_WIN32)
    // Minimal COM definitions matching Windows SDK <dxcapi.h> ABI so dxil.dll can be
    // invoked dynamically regardless of SDK header version or dxguid linkage.
    static constexpr GUID kClsidDxcValidator = {
        0x8ca3e215u, 0xf728u, 0x4cf3u, {0x8cu, 0xddu, 0x88u, 0xafu, 0x91u, 0x75u, 0x87u, 0xa1u}};
    static constexpr GUID kIidDxcValidator = {
        0xa6e82bd2u, 0x1fd7u, 0x4826u, {0x98u, 0x11u, 0x28u, 0x57u, 0xe7u, 0x97u, 0xf4u, 0x9au}};
    static constexpr GUID kIidDxcBlob = {
        0x8ba5fb08u, 0x5195u, 0x40e2u, {0xacu, 0x58u, 0x0du, 0x98u, 0x9cu, 0x3au, 0x01u, 0x02u}};

    struct MinIDxcBlob : public IUnknown {
        virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
        virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
    };
    struct MinIDxcBlobEncoding;
    struct MinIDxcOperationResult : public IUnknown {
        virtual HRESULT STDMETHODCALLTYPE GetStatus(HRESULT* pStatus) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetResult(MinIDxcBlob** ppResult) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetErrorBuffer(MinIDxcBlobEncoding** ppErrors) = 0;
    };
    struct MinIDxcValidator : public IUnknown {
        virtual HRESULT STDMETHODCALLTYPE Validate(
            MinIDxcBlob* pShader, UINT32 Flags, MinIDxcOperationResult** ppResult) = 0;
    };

    class InPlaceDxcBlob final : public MinIDxcBlob {
    public:
        InPlaceDxcBlob(uint8_t* ptr, size_t sz) : ptr_(ptr), sz_(sz) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
            if (!ppv) return E_POINTER;
            if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, kIidDxcBlob)) {
                *ppv = static_cast<MinIDxcBlob*>(this);
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return ptr_; }
        SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return sz_; }

    private:
        uint8_t* ptr_;
        size_t sz_;
    };

    using FnDxcCreateInstance = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    struct DxilSignerRuntime {
        HMODULE mod = nullptr;
        FnDxcCreateInstance create_fn = nullptr;
        DxilSignerRuntime() {
            if (const char* env_dir = std::getenv("SCARLETTX_DXC_DIR"); env_dir && env_dir[0] != '\0') {
                std::error_code ec;
                const auto p = std::filesystem::path(env_dir) / "dxil.dll";
                if (std::filesystem::exists(p, ec)) {
                    mod = LoadLibraryExW(p.wstring().c_str(), nullptr,
                                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                }
            }
            if (!mod) {
                mod = LoadLibraryW(L"dxil.dll");
            }
            if (!mod) {
                const std::filesystem::path kits_root = "C:/Program Files (x86)/Windows Kits/10/bin";
                std::error_code ec;
                if (std::filesystem::exists(kits_root, ec)) {
                    std::filesystem::path best_dll;
                    for (const auto& entry : std::filesystem::directory_iterator(kits_root, ec)) {
                        if (!entry.is_directory(ec)) continue;
                        const auto cand = entry.path() / "x64" / "dxil.dll";
                        if (std::filesystem::exists(cand, ec) && cand > best_dll) {
                            best_dll = cand;
                        }
                    }
                    if (!best_dll.empty()) {
                        mod = LoadLibraryExW(best_dll.wstring().c_str(), nullptr,
                                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                    }
                }
            }
            if (mod) {
                create_fn = reinterpret_cast<FnDxcCreateInstance>(
                    reinterpret_cast<void*>(GetProcAddress(mod, "DxcCreateInstance")));
            }
        }
    };

    static DxilSignerRuntime signer;
    if (!signer.create_fn) {
        return false;
    }

    MinIDxcValidator* validator = nullptr;
    if (FAILED(signer.create_fn(kClsidDxcValidator, kIidDxcValidator, reinterpret_cast<void**>(&validator))) ||
        !validator) {
        return false;
    }

    uint8_t prev_digest[16]{};
    std::memcpy(prev_digest, dxbc_data + 4, 16);
    std::memset(dxbc_data + 4, 0, 16);

    InPlaceDxcBlob container_blob(dxbc_data, total_size);
    MinIDxcOperationResult* val_res = nullptr;
    constexpr UINT32 kValidatorFlagsInPlaceEdit = 1u;  // DxcValidatorFlags_InPlaceEdit
    const HRESULT hr = validator->Validate(&container_blob, kValidatorFlagsInPlaceEdit, &val_res);
    validator->Release();

    HRESULT val_status = E_FAIL;
    if (SUCCEEDED(hr) && val_res) {
        val_res->GetStatus(&val_status);
        if (SUCCEEDED(val_status)) {
            bool has_sig = false;
            for (size_t i = 4; i < 20; ++i) {
                if (dxbc_data[i] != 0) {
                    has_sig = true;
                    break;
                }
            }
            if (!has_sig) {
                MinIDxcBlob* out_blob = nullptr;
                if (SUCCEEDED(val_res->GetResult(&out_blob)) && out_blob) {
                    if (out_blob->GetBufferPointer() && out_blob->GetBufferSize() == total_size) {
                        std::memcpy(dxbc_data, out_blob->GetBufferPointer(), total_size);
                    }
                    out_blob->Release();
                }
                for (size_t i = 4; i < 20; ++i) {
                    if (dxbc_data[i] != 0) {
                        has_sig = true;
                        break;
                    }
                }
            }
            val_res->Release();
            if (has_sig) {
                return true;
            }
            std::memcpy(dxbc_data + 4, prev_digest, 16);
            return false;
        }
    }
    if (val_res) {
        val_res->Release();
    }
    std::memcpy(dxbc_data + 4, prev_digest, 16);
#endif
    return false;
}

bool ParseXs2PrecompiledHeader(const uint8_t* data, size_t byte_size, Xs2ParsedHeaderInfo& out_hdr) {
    out_hdr = Xs2ParsedHeaderInfo{};
    if (!data || byte_size == 0) {
        return false;
    }
    out_hdr.isa_ptr = data;
    out_hdr.isa_size = byte_size;

    // 1. Unwrap DXBC container carrying XS2M / XBIN microcode chunks if passed directly
    if (byte_size >= 32 && std::memcmp(data, "DXBC", 4) == 0) {
        const uint32_t chunk_count = ReadU32(data + 28);
        if (32ull + static_cast<size_t>(chunk_count) * 4ull <= byte_size) {
            for (uint32_t i = 0; i < chunk_count; ++i) {
                const uint32_t off = ReadU32(data + 32 + i * 4);
                if (static_cast<size_t>(off) + 8ull > byte_size) continue;
                std::string fcc(reinterpret_cast<const char*>(data + off), 4);
                const uint32_t sz = ReadU32(data + off + 4);
                if ((fcc == "XS2M" || fcc == "XBIN") &&
                    static_cast<size_t>(off) + 8ull + static_cast<size_t>(sz) <= byte_size) {
                    data = data + off + 8;
                    byte_size = sz;
                    out_hdr.has_header = true;
                    out_hdr.isa_ptr = data;
                    out_hdr.isa_size = byte_size;
                    break;
                }
            }
        }
    }

    // 2. Standalone "XS2\0" outer container header (16 bytes)
    if (byte_size >= 16 && std::memcmp(data, "XS2\0", 4) == 0) {
        out_hdr.has_header = true;
        const uint32_t ver = ReadU32(data + 4);
        const uint32_t stg = ReadU32(data + 8);
        const uint32_t explicit_len = ReadU32(data + 12);
        if (stg != 0) out_hdr.stage = stg;
        if (((ver >> 16) & XS2_HEADER_FLAG_BINDLESS) != 0) out_hdr.is_bindless = true;
        if (((ver >> 16) & XS2_HEADER_FLAG_IA_VERTEX_INPUTS) != 0) out_hdr.has_ia_vertex_inputs = true;
        const size_t avail = byte_size - 16u;
        const size_t sub_len = (explicit_len > 0 && explicit_len <= avail) ? explicit_len : avail;
        data += 16u;
        byte_size = sub_len;
        out_hdr.isa_ptr = data;
        out_hdr.isa_size = byte_size;
    }

    // 3. Rich Scarlett XS2_SHADER_HEADER ('XS2H' = 0x48325358u) with PGM_RSRC1/2 & root map
    if (byte_size >= sizeof(XS2_SHADER_HEADER) && ReadU32(data) == XS2_SHADER_HEADER_MAGIC) {
        XS2_SHADER_HEADER hdr{};
        std::memcpy(&hdr, data, sizeof(hdr));
        out_hdr.has_header = true;
        if (hdr.stage != 0) out_hdr.stage = hdr.stage;
        out_hdr.pgm_rsrc1 = hdr.pgmRsrc1;
        out_hdr.pgm_rsrc2 = hdr.pgmRsrc2;
        if (hdr.pgmRsrc1 != 0) {
            out_hdr.vgpr_count = ((hdr.pgmRsrc1 & 0x3Fu) + 1u) * 8u;
            out_hdr.sgpr_count = (((hdr.pgmRsrc1 >> 6) & 0xFu) + 1u) * 8u;
        }
        out_hdr.lds_bytes = ((hdr.pgmRsrc2 >> 7) & 0x1FFu) * 512u;
        out_hdr.user_sgpr_count = (hdr.userSgprCount != 0) ? hdr.userSgprCount : ((hdr.pgmRsrc2 >> 1) & 0x1Fu);
        if (((hdr.version >> 16) & XS2_HEADER_FLAG_BINDLESS) != 0 || (hdr.pgmRsrc2 & 0x40000000u) != 0) {
            out_hdr.is_bindless = true;
        }
        if (((hdr.version >> 16) & XS2_HEADER_FLAG_IA_VERTEX_INPUTS) != 0 || (hdr.pgmRsrc2 & 0x80000000u) != 0) {
            out_hdr.has_ia_vertex_inputs = true;
        }
        out_hdr.mrt_export_mask = hdr.mrtExportMask & 0xFFu;
        if (hdr.threadsX > 0) {
            out_hdr.has_workgroup_size = true;
            out_hdr.threads_x = hdr.threadsX;
            out_hdr.threads_y = std::max(1u, hdr.threadsY);
            out_hdr.threads_z = std::max(1u, hdr.threadsZ);
        }

        const size_t max_maps = (byte_size - sizeof(XS2_SHADER_HEADER)) / sizeof(XS2_ROOT_REGISTER_MAP);
        const size_t map_count = std::min<size_t>(hdr.rootMapCount, max_maps);
        out_hdr.root_map.reserve(map_count);
        const uint8_t* map_ptr = data + sizeof(XS2_SHADER_HEADER);
        for (size_t i = 0; i < map_count; ++i) {
            XS2_ROOT_REGISTER_MAP rm{};
            std::memcpy(&rm, map_ptr + i * sizeof(XS2_ROOT_REGISTER_MAP), sizeof(rm));
            out_hdr.root_map.push_back({rm.userSgprOffset, rm.registerIndex, rm.registerSpace});
        }
        const size_t hdr_total = sizeof(XS2_SHADER_HEADER) + map_count * sizeof(XS2_ROOT_REGISTER_MAP);
        const size_t avail = byte_size - hdr_total;
        const size_t isa_len = (hdr.codeByteSize > 0 && hdr.codeByteSize <= avail) ? hdr.codeByteSize : avail;
        out_hdr.isa_ptr = data + hdr_total;
        out_hdr.isa_size = isa_len;
        return true;
    }

    // 4. Compact PGM_RSRC1/2 hardware register prefix ('PGMR' = 0x524D4750u, 24 bytes)
    if (byte_size >= 24 && ReadU32(data) == 0x524D4750u) {
        out_hdr.has_header = true;
        out_hdr.pgm_rsrc1 = ReadU32(data + 4);
        out_hdr.pgm_rsrc2 = ReadU32(data + 8);
        if (out_hdr.pgm_rsrc1 != 0) {
            out_hdr.vgpr_count = ((out_hdr.pgm_rsrc1 & 0x3Fu) + 1u) * 8u;
            out_hdr.sgpr_count = (((out_hdr.pgm_rsrc1 >> 6) & 0xFu) + 1u) * 8u;
        }
        out_hdr.lds_bytes = ((out_hdr.pgm_rsrc2 >> 7) & 0x1FFu) * 512u;
        out_hdr.user_sgpr_count = (out_hdr.pgm_rsrc2 >> 1) & 0x1Fu;
        const uint32_t tx = ReadU32(data + 12);
        const uint32_t ty = ReadU32(data + 16);
        const uint32_t tz = ReadU32(data + 20);
        if (tx > 0) {
            out_hdr.has_workgroup_size = true;
            out_hdr.threads_x = tx;
            out_hdr.threads_y = std::max(1u, ty);
            out_hdr.threads_z = std::max(1u, tz);
        }
        out_hdr.isa_ptr = data + 24u;
        out_hdr.isa_size = byte_size - 24u;
        return true;
    }

    return out_hdr.has_header;
}

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info) {
    out_info = ShaderModuleInfo{};
    out_info.byte_size = static_cast<uint32_t>(bytecode.size());
    out_info.content_hash = HashShaderBytes(bytecode.data(), bytecode.size());

    if (bytecode.size() >= 4 &&
        (std::memcmp(bytecode.data(), "XS2\0", 4) == 0 || ReadU32(bytecode.data()) == XS2_SHADER_HEADER_MAGIC)) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
        Xs2ParsedHeaderInfo hdr{};
        ParseXs2PrecompiledHeader(bytecode.data(), bytecode.size(), hdr);
        out_info.stage = StageFromXs2Hint(hdr.stage);
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
                    if (prog_type >= 7u && prog_type <= 12u) {
                        out_info.has_raytracing_exports = true;
                    }
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

    return Status::Error(StatusCode::UnsupportedFormat, "Unrecognized shader container magic");
}

namespace {

struct MiniRdna2Inst {
    uint32_t dword_pc = 0;
    uint32_t next_dword_pc = 1;
    uint32_t inst_bytes = 4;
    uint32_t w0 = 0;
    uint32_t w1 = 0;
    uint32_t literal_u32 = 0;
    uint32_t nsa_dwords = 0;
    uint32_t nsa_words[3]{};
    bool is_64 = false;
    bool has_literal = false;
    bool has_dpp = false;
    bool has_sdwa = false;
    bool is_branch = false;
    bool is_cond_branch = false;
    int16_t branch_simm16 = 0;
    uint32_t mem_dwords = 1;
    std::string op;
};

// Full-width RDNA2 GFX10.3 instruction decoder (handles 32-bit, 64-bit, 96-bit literal,
// DPP/SDWA 64-bit modifiers, VOP3P, FLAT/GLOBAL/SCRATCH, MTBUF, NSA MIMG, and BVH64 raytracing).
MiniRdna2Inst DecodeMiniRdna2(const uint8_t* p, size_t rem, uint32_t byte_offset) {
    MiniRdna2Inst ins{};
    ins.dword_pc = byte_offset / 4u;
    if (rem < 4) {
        ins.inst_bytes = static_cast<uint32_t>(rem);
        ins.next_dword_pc = ins.dword_pc + 1u;
        ins.op = "s_nop";
        return ins;
    }
    ins.w0 = ReadU32(p);
    const uint32_t w0 = ins.w0;

    auto finish_size = [&]() {
        if (ins.inst_bytes > rem) {
            ins.inst_bytes = static_cast<uint32_t>(rem & ~size_t(3u));
            if (ins.inst_bytes == 0) ins.inst_bytes = 4u;
        }
        ins.is_64 = (ins.inst_bytes >= 8u);
        ins.next_dword_pc = ins.dword_pc + (ins.inst_bytes / 4u);
    };

    // 1. SOPP (0xBF800000)
    if ((w0 & 0xFF800000u) == 0xBF800000u) {
        const uint32_t sop = (w0 >> 16) & 0x7Fu;
        ins.branch_simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
        if (sop == 0x01 || w0 == 0xBF800000u) {
            ins.op = "s_endpgm";
        } else if (sop == 0x0A) {
            ins.op = "s_barrier";
        } else if (sop == 0x0C) {
            ins.op = "s_waitcnt";
        } else if (sop == 0x02 || (sop >= 0x04 && sop <= 0x09)) {
            ins.is_branch = true;
            ins.is_cond_branch = (sop != 0x02);
            if (sop == 0x02) ins.op = "s_branch";
            else if (sop == 0x04) ins.op = "s_cbranch_scc0";
            else if (sop == 0x05) ins.op = "s_cbranch_scc1";
            else if (sop == 0x06) ins.op = "s_cbranch_vccz";
            else if (sop == 0x07) ins.op = "s_cbranch_vccnz";
            else if (sop == 0x08) ins.op = "s_cbranch_execz";
            else ins.op = "s_cbranch_execnz";
        } else {
            ins.op = "s_nop";
        }
        finish_size();
        return ins;
    }

    // 2. SOPC (0xBF000000)
    if ((w0 & 0xFF800000u) == 0xBF000000u) {
        const uint32_t sop = (w0 >> 16) & 0x7Fu;
        const uint32_t ssrc0 = w0 & 0xFFu;
        const uint32_t ssrc1 = (w0 >> 8) & 0xFFu;
        if (ssrc0 == 0xFFu || ssrc1 == 0xFFu) {
            ins.has_literal = (rem >= 8);
            if (ins.has_literal) {
                ins.literal_u32 = ReadU32(p + 4);
                ins.w1 = ins.literal_u32;
                ins.inst_bytes = 8u;
            }
        }
        switch (sop) {
            case 0x00: ins.op = "s_cmp_eq_i32"; break;
            case 0x01: ins.op = "s_cmp_lg_i32"; break;
            case 0x02: ins.op = "s_cmp_gt_i32"; break;
            case 0x03: ins.op = "s_cmp_ge_i32"; break;
            case 0x04: ins.op = "s_cmp_lt_i32"; break;
            case 0x05: ins.op = "s_cmp_le_i32"; break;
            case 0x06: ins.op = "s_cmp_eq_u32"; break;
            case 0x07: ins.op = "s_cmp_lg_u32"; break;
            case 0x08: ins.op = "s_cmp_gt_u32"; break;
            case 0x09: ins.op = "s_cmp_ge_u32"; break;
            case 0x0A: ins.op = "s_cmp_lt_u32"; break;
            case 0x0B: ins.op = "s_cmp_le_u32"; break;
            default:   ins.op = "s_cmp_lt_i32"; break;
        }
        finish_size();
        return ins;
    }

    // 3. SOP1 (0xBE800000)
    if ((w0 & 0xFF800000u) == 0xBE800000u) {
        const uint32_t sop = (w0 >> 8) & 0xFFu;
        const uint32_t ssrc0 = w0 & 0xFFu;
        if (ssrc0 == 0xFFu) {
            ins.has_literal = (rem >= 8);
            if (ins.has_literal) {
                ins.literal_u32 = ReadU32(p + 4);
                ins.w1 = ins.literal_u32;
                ins.inst_bytes = 8u;
            }
        }
        if (sop == 0x00 || sop == 0x03) ins.op = "s_mov_b32";
        else if (sop == 0x04 || sop == 0x07) ins.op = "s_not_b32";
        else if (sop == 0x24 || sop == 0x3C) ins.op = "s_and_saveexec_b32";
        else ins.op = "s_mov_b32";
        finish_size();
        return ins;
    }

    // 4. SOPK (0xB0000000 .. 0xBE7FFFFF)
    if ((w0 & 0xF0000000u) == 0xB0000000u && (w0 & 0xFF800000u) < 0xBE800000u) {
        const uint32_t sopk = (w0 >> 23) & 0x1Fu;
        ins.branch_simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
        if (sopk == 0x14u) {  // s_setreg_imm32_b32
            ins.has_literal = (rem >= 8);
            if (ins.has_literal) {
                ins.literal_u32 = ReadU32(p + 4);
                ins.w1 = ins.literal_u32;
                ins.inst_bytes = 8u;
            }
            ins.op = "s_nop";
        } else if (sopk == 0x00u) ins.op = "s_movk_i32";
        else if (sopk == 0x03u) ins.op = "s_cmpk_eq_i32";
        else if (sopk == 0x04u) ins.op = "s_cmpk_lg_i32";
        else if (sopk == 0x0Fu) ins.op = "s_addk_i32";
        else if (sopk == 0x10u) ins.op = "s_mulk_i32";
        else ins.op = "s_nop";
        finish_size();
        return ins;
    }

    // 5. SOP2 (0x80000000 .. 0xAFFFFFFF)
    if ((w0 & 0xC0000000u) == 0x80000000u) {
        const uint32_t sop2 = (w0 >> 23) & 0x7Fu;
        const uint32_t ssrc0 = w0 & 0xFFu;
        const uint32_t ssrc1 = (w0 >> 8) & 0xFFu;
        if (ssrc0 == 0xFFu || ssrc1 == 0xFFu) {
            ins.has_literal = (rem >= 8);
            if (ins.has_literal) {
                ins.literal_u32 = ReadU32(p + 4);
                ins.w1 = ins.literal_u32;
                ins.inst_bytes = 8u;
            }
        }
        switch (sop2) {
            case 0x00: ins.op = "s_add_u32"; break;
            case 0x01: ins.op = "s_sub_u32"; break;
            case 0x02: ins.op = "s_add_i32"; break;
            case 0x03: ins.op = "s_sub_i32"; break;
            case 0x06: ins.op = "s_min_i32"; break;
            case 0x07: ins.op = "s_min_u32"; break;
            case 0x08: ins.op = "s_max_i32"; break;
            case 0x09: ins.op = "s_max_u32"; break;
            case 0x0A: ins.op = "s_cselect_b32"; break;
            case 0x0E: ins.op = "s_and_b32"; break;
            case 0x10: ins.op = "s_or_b32"; break;
            case 0x12: ins.op = "s_xor_b32"; break;
            case 0x1C: ins.op = "s_lshl_b32"; break;
            case 0x1E: ins.op = "s_lshr_b32"; break;
            case 0x20: ins.op = "s_ashr_i32"; break;
            case 0x24: ins.op = "s_mul_i32"; break;
            default:   ins.op = "s_add_u32"; break;
        }
        finish_size();
        return ins;
    }

    // 6. SMEM (0xF4000000)
    if ((w0 & 0xFC000000u) == 0xF4000000u) {
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
        }
        const uint32_t smem_op = (w0 >> 18) & 0xFFu;
        if (smem_op == 0x00u) { ins.op = "s_load_dword"; ins.mem_dwords = 1; }
        else if (smem_op == 0x01u) { ins.op = "s_load_dwordx2"; ins.mem_dwords = 2; }
        else { ins.op = "s_load_dwordx4"; ins.mem_dwords = 4; }
        if (w0 == 0xF4000000u) { ins.op = "s_load_dwordx4"; ins.mem_dwords = 4; }
        finish_size();
        return ins;
    }

    // Helper for 32-bit VALU encodings (VOP1 / VOPC / VOP2) with 9-bit src0 modifiers
    auto decode_valu_src0_ext = [&](uint32_t src0, bool implied_lit) {
        if (src0 == 0x0FFu || implied_lit) {
            if (rem >= 8) {
                ins.has_literal = true;
                ins.literal_u32 = ReadU32(p + 4);
                ins.w1 = ins.literal_u32;
                ins.inst_bytes = 8u;
            }
        } else if (src0 == 0x0FAu || src0 == 0x0E9u || src0 == 0x0EAu) {
            if (rem >= 8) {
                ins.has_dpp = true;
                ins.w1 = ReadU32(p + 4);
                ins.inst_bytes = 8u;
            }
        } else if (src0 == 0x0F9u) {
            if (rem >= 8) {
                ins.has_sdwa = true;
                ins.w1 = ReadU32(p + 4);
                ins.inst_bytes = 8u;
            }
        }
    };

    // 7. VOP1 (0x7E000000)
    if ((w0 & 0xFE000000u) == 0x7E000000u) {
        const uint32_t vop = (w0 >> 9) & 0xFFu;
        const uint32_t src0 = w0 & 0x1FFu;
        decode_valu_src0_ext(src0, false);
        if (vop == 0x01) ins.op = "v_mov_b32";
        else if (vop == 0x02) ins.op = "v_readfirstlane_b32";
        else if (vop == 0x05) ins.op = "v_cvt_f32_i32";
        else if (vop == 0x06) ins.op = "v_cvt_f32_u32";
        else if (vop == 0x07) ins.op = "v_cvt_u32_f32";
        else if (vop == 0x08) ins.op = "v_cvt_i32_f32";
        else if (vop == 0x2A) ins.op = "v_rcp_f32";
        else if (vop == 0x2E) ins.op = "v_rsq_f32";
        else if (vop == 0x33) ins.op = "v_sqrt_f32";
        else if (vop == 0x35) ins.op = "v_sin_f32";
        else if (vop == 0x36) ins.op = "v_cos_f32";
        else if (vop == 0x37) ins.op = "v_not_b32";
        else ins.op = "v_mov_b32";
        finish_size();
        return ins;
    }

    // 8. VOPC (0x7C000000)
    if ((w0 & 0xFE000000u) == 0x7C000000u) {
        const uint32_t op = (w0 >> 17) & 0xFFu;
        const uint32_t src0 = w0 & 0x1FFu;
        decode_valu_src0_ext(src0, false);
        const uint32_t kind = op & 0x07u;
        if (op >= 0xC0u) {
            if (kind == 0x01) ins.op = "v_cmp_lt_u32";
            else if (kind == 0x02) ins.op = "v_cmp_eq_u32";
            else if (kind == 0x03) ins.op = "v_cmp_le_u32";
            else if (kind == 0x04) ins.op = "v_cmp_gt_u32";
            else if (kind == 0x05) ins.op = "v_cmp_lg_u32";
            else ins.op = "v_cmp_ge_u32";
        } else if (op >= 0x80u) {
            if (kind == 0x01) ins.op = "v_cmp_lt_i32";
            else if (kind == 0x02) ins.op = "v_cmp_eq_i32";
            else if (kind == 0x03) ins.op = "v_cmp_le_i32";
            else if (kind == 0x04) ins.op = "v_cmp_gt_i32";
            else if (kind == 0x05) ins.op = "v_cmp_lg_i32";
            else ins.op = "v_cmp_ge_i32";
        } else {
            if (kind == 0x01) ins.op = "v_cmp_lt_f32";
            else if (kind == 0x02) ins.op = "v_cmp_eq_f32";
            else if (kind == 0x03) ins.op = "v_cmp_le_f32";
            else if (kind == 0x04) ins.op = "v_cmp_gt_f32";
            else if (kind == 0x05) ins.op = "v_cmp_lg_f32";
            else ins.op = "v_cmp_ge_f32";
        }
        finish_size();
        return ins;
    }

    // 9. VOP2 (bit 31 == 0)
    if ((w0 & 0x80000000u) == 0) {
        const uint32_t vop2 = (w0 >> 25) & 0x3Fu;
        const uint32_t src0 = w0 & 0x1FFu;
        const bool implied_lit = (vop2 == 0x2Cu || vop2 == 0x2Du || vop2 == 0x37u || vop2 == 0x38u);
        decode_valu_src0_ext(src0, implied_lit);
        switch (vop2) {
            case 0x01:
            case 0x18: ins.op = "v_cndmask_b32"; break;
            case 0x03: ins.op = "v_add_f32"; break;
            case 0x04: ins.op = "v_sub_f32"; break;
            case 0x05: ins.op = "v_subrev_f32"; break;
            case 0x08: ins.op = "v_mul_f32"; break;
            case 0x09: ins.op = "v_mul_i32_i24"; break;
            case 0x0A: ins.op = "v_min_f32"; break;
            case 0x0B: ins.op = "v_max_f32"; break;
            case 0x0C: ins.op = "v_min_i32"; break;
            case 0x0D: ins.op = "v_max_i32"; break;
            case 0x0E: ins.op = "v_min_u32"; break;
            case 0x0F: ins.op = "v_max_u32"; break;
            case 0x10: ins.op = "v_lshrrev_b32"; break;
            case 0x11: ins.op = "v_ashrrev_i32"; break;
            case 0x12:
            case 0x1A: ins.op = "v_lshlrev_b32"; break;
            case 0x13:
            case 0x1B: ins.op = "v_and_b32"; break;
            case 0x14:
            case 0x1C: ins.op = "v_or_b32"; break;
            case 0x15:
            case 0x1D: ins.op = "v_xor_b32"; break;
            case 0x19:
            case 0x2B: ins.op = "v_fmac_f32"; break;
            case 0x25:
            case 0x28: ins.op = "v_add_nc_u32"; break;
            case 0x26:
            case 0x29: ins.op = "v_sub_nc_u32"; break;
            case 0x27: ins.op = "v_subrev_nc_u32"; break;
            case 0x2C: ins.op = "v_fmamk_f32"; break;
            case 0x2D: ins.op = "v_fmaak_f32"; break;
            default:   ins.op = "s_nop"; break;
        }
        finish_size();
        return ins;
    }

    // 10. VOP3P (0xCC000000: 64-bit packed FP16 / mad_mix ops, +4 literal -> 12 bytes)
    if ((w0 & 0xFC000000u) == 0xCC000000u) {
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
            const uint32_t s0 = ins.w1 & 0x1FFu;
            const uint32_t s1 = (ins.w1 >> 9) & 0x1FFu;
            const uint32_t s2 = (ins.w1 >> 18) & 0x1FFu;
            if ((s0 == 0x0FFu || s1 == 0x0FFu || s2 == 0x0FFu) && rem >= 12) {
                ins.has_literal = true;
                ins.literal_u32 = ReadU32(p + 8);
                ins.inst_bytes = 12u;
            }
        }
        const uint32_t op = (w0 >> 16) & 0x7Fu;
        if (op == 0x12u) ins.op = "v_pk_mul_f16";
        else if (op == 0x13u) ins.op = "v_pk_add_f16";
        else if (op == 0x14u) ins.op = "v_pk_sub_f16";
        else if (op == 0x20u || op == 0x00u) ins.op = "v_fma_mix_f32";
        else ins.op = "v_pk_fma_f16";
        finish_size();
        return ins;
    }

    // 11. VOP3A / VOP3B (0xD4000000: 64-bit, +4 literal -> 12 bytes)
    if ((w0 & 0xFC000000u) == 0xD4000000u) {
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
            const uint32_t s0 = ins.w1 & 0x1FFu;
            const uint32_t s1 = (ins.w1 >> 9) & 0x1FFu;
            const uint32_t s2 = (ins.w1 >> 18) & 0x1FFu;
            if ((s0 == 0x0FFu || s1 == 0x0FFu || s2 == 0x0FFu) && rem >= 12) {
                ins.has_literal = true;
                ins.literal_u32 = ReadU32(p + 8);
                ins.inst_bytes = 12u;
            }
        }
        const uint32_t op = (w0 >> 16) & 0x3FFu;
        if (op == 0x2FFu) ins.op = "v_mbcnt_lo_u32_b32";
        else if (op == 0x364u) ins.op = "v_add3_u32";
        else if (op == 0x36Fu) ins.op = "v_lshl_or_b32";
        else ins.op = "v_fma_f32";
        finish_size();
        return ins;
    }

    // 12. LDS / GDS (0xD8000000)
    if ((w0 & 0xFC000000u) == 0xD8000000u) {
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
        }
        ins.op = (w0 & (1u << 25)) ? "ds_write_b32" : "ds_read_b32";
        finish_size();
        return ins;
    }

    // 13. FLAT / GLOBAL / SCRATCH (0xDC000000)
    if ((w0 & 0xFC000000u) == 0xDC000000u) {
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
        }
        const uint32_t seg = (w0 >> 14) & 0x3u;
        const uint32_t fop = (w0 >> 18) & 0x7Fu;
        const uint32_t saddr = (ins.w1 >> 16) & 0x7Fu;
        const bool is_store = (fop >= 0x18u && fop <= 0x1Fu) || ((w0 & (1u << 25)) != 0 && fop >= 0x18u);
        if (fop == 0x0Du || fop == 0x1Du) ins.mem_dwords = 2;
        else if (fop == 0x0Fu || fop == 0x1Fu) ins.mem_dwords = 3;
        else if (fop == 0x0Eu || fop == 0x1Eu) ins.mem_dwords = 4;
        else ins.mem_dwords = 1;
        const char* prefix = (seg == 1u) ? "scratch_" : ((seg == 0u && saddr == 0x7Fu) ? "flat_" : "global_");
        const char* suffix = (ins.mem_dwords == 4) ? "dwordx4" :
                             (ins.mem_dwords == 3) ? "dwordx3" :
                             (ins.mem_dwords == 2) ? "dwordx2" : "dword";
        ins.op = std::string(prefix) + (is_store ? "store_" : "load_") + suffix;
        finish_size();
        return ins;
    }

    // 14. MUBUF (0xE0000000)
    if ((w0 & 0xFC000000u) == 0xE0000000u) {
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
        }
        const uint32_t mop = (w0 >> 18) & 0x7Fu;
        const bool store = ((w0 & (1u << 25)) != 0) || (mop >= 0x1Cu && mop <= 0x1Fu);
        if (mop == 0x0Du || mop == 0x1Du) ins.mem_dwords = 2;
        else if (mop == 0x0Eu || mop == 0x1Eu) ins.mem_dwords = 3;
        else if (mop == 0x0Fu || mop == 0x1Fu) ins.mem_dwords = 4;
        else ins.mem_dwords = 1;
        ins.op = store ? "buffer_store_dword" : "buffer_load_dword";
        finish_size();
        return ins;
    }

    // 15. MTBUF (0xE8000000: 64-bit tbuffer_load_format_* / tbuffer_store_format_*)
    if ((w0 & 0xFC000000u) == 0xE8000000u) {
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
        }
        const uint32_t top = ((w0 >> 16) & 0x7u) | (((ins.w1 >> 21) & 0x1u) << 3);
        const bool store = (top >= 4u && top <= 7u) || (top >= 12u);
        ins.mem_dwords = (top & 0x3u) + 1u;
        static const char* const kComp[4] = {"x", "xy", "xyz", "xyzw"};
        ins.op = std::string(store ? "tbuffer_store_format_" : "tbuffer_load_format_") +
                 kComp[(ins.mem_dwords - 1u) & 3u];
        finish_size();
        return ins;
    }

    // 16. MIMG (0xF0000000: 64-bit + NSA extra dwords [2:1], plus BVH64 raytracing 0xE6/0xE7)
    if ((w0 & 0xFC000000u) == 0xF0000000u) {
        const uint32_t nsa = (w0 >> 1) & 0x3u;
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
            ins.nsa_dwords = nsa;
            for (uint32_t ni = 0; ni < nsa; ++ni) {
                if (rem >= 12u + ni * 4u) {
                    ins.nsa_words[ni] = ReadU32(p + 8u + ni * 4u);
                    ins.inst_bytes = 12u + ni * 4u;
                }
            }
        }
        const uint32_t mop = (w0 >> 18) & 0xFFu;
        if (mop == 0xE6u || mop == 0xE7u) ins.op = "image_bvh64_intersect_ray";
        else if (mop >= 0x20u) ins.op = "image_sample";
        else if (mop >= 0x08u) ins.op = "image_store";
        else ins.op = "image_load";
        finish_size();
        return ins;
    }

    // 17. EXP (0xF8000000)
    if ((w0 & 0xFC000000u) == 0xF8000000u) {
        if (rem >= 8) {
            ins.w1 = ReadU32(p + 4);
            ins.inst_bytes = 8u;
        }
        ins.op = "exp";
        finish_size();
        return ins;
    }

    // 18. VINTRP (0xC8000000)
    if ((w0 & 0xFC000000u) == 0xC8000000u) {
        ins.op = "v_interp";
        finish_size();
        return ins;
    }

    ins.op = "s_nop";
    finish_size();
    return ins;
}

struct HlslCfgRegion {
    bool is_loop = false;
    size_t begin = 0;        // Outer span [begin, end)
    size_t end = 0;
    size_t inner_begin = 0;  // Body subrange [inner_begin, inner_end)
    size_t inner_end = 0;
    size_t branch_idx = 0;
    uint32_t loop_id = 0;
    std::vector<HlslCfgRegion> children;
};

bool InsertHlslCfgRegion(
    HlslCfgRegion candidate,
    size_t limit_begin,
    size_t limit_end,
    std::vector<HlslCfgRegion>& list) {
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
            return InsertHlslCfgRegion(std::move(candidate), child.inner_begin, child.inner_end, child.children);
        }
        if (!(candidate.end <= child.begin || candidate.begin >= child.end)) {
            return false;  // Reject irreducible cross-jumping edge
        }
    }
    auto pos = std::lower_bound(
        list.begin(), list.end(), candidate.begin,
        [](const HlslCfgRegion& r, size_t b) { return r.begin < b; });
    list.insert(pos, std::move(candidate));
    return true;
}

std::vector<HlslCfgRegion> BuildHlslStructuredCfg(const std::vector<MiniRdna2Inst>& insts) {
    const size_t n = insts.size();
    if (n == 0) return {};
    const uint32_t total_dwords = insts.back().next_dword_pc;

    std::vector<HlslCfgRegion> candidates;
    uint32_t next_loop_id = 0;
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
            HlslCfgRegion r{};
            r.is_loop = true;
            r.begin = start_idx;
            r.end = i + 1u;
            r.inner_begin = start_idx;
            r.inner_end = i;
            r.branch_idx = i;
            r.loop_id = next_loop_id++;
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
                HlslCfgRegion r{};
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

    std::sort(candidates.begin(), candidates.end(), [](const HlslCfgRegion& a, const HlslCfgRegion& b) {
        const size_t len_a = a.end - a.begin;
        const size_t len_b = b.end - b.begin;
        if (len_a != len_b) return len_a > len_b;
        return a.begin < b.begin;
    });

    std::vector<HlslCfgRegion> roots;
    for (auto& c : candidates) {
        InsertHlslCfgRegion(std::move(c), 0u, n, roots);
    }
    return roots;
}

std::string FormatHexU32(uint32_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << v << "u";
    return os.str();
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
                        if (std::filesystem::exists(cand, ec)) {
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
    std::vector<uint8_t> candidate;
    if (ptr && sz >= 32 && std::memcmp(ptr, "DXBC", 4) == 0) {
        candidate.assign(ptr, ptr + sz);
        for (size_t i = 4; i < 20; ++i) {
            if (candidate[i] != 0) {
                signed_ok = true;
                break;
            }
        }
        if (!signed_ok) {
            signed_ok = SignDxilContainerInPlace(candidate.data(), candidate.size());
        }
    }
    if (signed_ok) {
        out_dxil = std::move(candidate);
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

    Xs2ParsedHeaderInfo hdr{};
    ParseXs2PrecompiledHeader(microcode, byte_size, hdr);
    if (!hdr.isa_ptr || hdr.isa_size < 4) {
        return Status::Error(StatusCode::UnsupportedFormat, "XS2 header contains no RDNA2 ISA microcode");
    }

    // Pass 1: Full-width RDNA2 GFX10.3 instruction stream decode & operand/CFG discovery
    std::vector<MiniRdna2Inst> insts;
    bool has_smem = false, has_mimg = false, has_ssbo = false, has_ssbo_write = false;
    bool has_lds = false, has_bvh_ray = false, has_dynamic_sgpr_rsrc = false;
    bool has_vs_exp = false, has_ps_exp = false, has_mrtz = false;
    uint32_t mrt_mask = hdr.mrt_export_mask;
    std::unordered_set<uint32_t> regs = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    std::unordered_set<uint32_t> defined_sgprs;
    for (uint32_t us = 0; us < std::min<uint32_t>(hdr.user_sgpr_count, 64u); ++us) {
        defined_sgprs.insert(us);
    }

    size_t off = 0;
    while (off < hdr.isa_size) {
        MiniRdna2Inst ins = DecodeMiniRdna2(hdr.isa_ptr + off, hdr.isa_size - off, static_cast<uint32_t>(off));
        off += ins.inst_bytes;

        if (ins.op.rfind("s_load_", 0) == 0) {
            has_smem = true;
            const uint32_t sdata = (ins.w0 >> 6) & 0x7Fu;
            for (uint32_t k = 0; k < ins.mem_dwords; ++k) defined_sgprs.insert((sdata + k) & 63u);
        } else if (ins.op.rfind("s_mov", 0) == 0 || ins.op == "s_not_b32" || ins.op == "s_and_saveexec_b32" ||
                   ins.op == "s_addk_i32" || ins.op == "s_mulk_i32") {
            defined_sgprs.insert(((ins.w0 >> 16) & 0x7Fu) & 63u);
        } else if (ins.op.rfind("s_", 0) == 0 && ins.op.rfind("s_cmp", 0) != 0 &&
                   ins.op.rfind("s_cbranch", 0) != 0 && ins.op != "s_branch" &&
                   ins.op != "s_endpgm" && ins.op != "s_barrier" && ins.op != "s_waitcnt" && ins.op != "s_nop") {
            defined_sgprs.insert(((ins.w0 >> 16) & 0x7Fu) & 63u);
        } else if (ins.op == "v_readfirstlane_b32") {
            defined_sgprs.insert(((ins.w0 >> 17) & 0xFFu) & 63u);
        } else if (ins.op == "image_sample" || ins.op == "image_load" || ins.op == "image_store") {
            has_mimg = true;
            const uint32_t srsrc = ((ins.w1 >> 16) & 0x1Fu) * 4u;
            if (srsrc >= 4u && defined_sgprs.count(srsrc & 63u) > 0) has_dynamic_sgpr_rsrc = true;
        } else if (ins.op == "image_bvh64_intersect_ray") {
            has_bvh_ray = true;
        } else if (ins.op.rfind("buffer_", 0) == 0 || ins.op.rfind("tbuffer_", 0) == 0 ||
                   ins.op.rfind("global_", 0) == 0 || ins.op.rfind("flat_", 0) == 0 ||
                   ins.op.rfind("scratch_", 0) == 0) {
            has_ssbo = true;
            if (ins.op.find("store") != std::string::npos) has_ssbo_write = true;
            const uint32_t srsrc = ((ins.w1 >> 16) & 0x1Fu) * 4u;
            if (srsrc >= 4u && defined_sgprs.count(srsrc & 63u) > 0) has_dynamic_sgpr_rsrc = true;
        } else if (ins.op == "ds_read_b32" || ins.op == "ds_write_b32" || ins.op == "s_barrier") {
            has_lds = true;
        } else if (ins.op == "v_interp") {
            has_ps_exp = true;
        } else if (ins.op == "exp") {
            const uint32_t target = (ins.w0 >> 4) & 0x3Fu;
            if ((target >= 12 && target <= 15) || target >= 32) {
                has_vs_exp = true;
            } else {
                has_ps_exp = true;
                if (target < 8u) mrt_mask |= (1u << target);
                else if (target == 8u) has_mrtz = true;
            }
        }

        regs.insert(ins.w0 & 0xFFu);
        regs.insert((ins.w0 >> 9) & 0xFFu);
        regs.insert((ins.w0 >> 17) & 0xFFu);
        regs.insert((ins.w0 >> 18) & 0xFFu);
        if (ins.is_64) {
            for (uint32_t c = 0; c < 4u; ++c) {
                regs.insert(((ins.w1 & 0xFFu) + c) & 0xFFu);
                regs.insert((((ins.w1 >> 8) & 0xFFu) + c) & 0xFFu);
                regs.insert((((ins.w1 >> 24) & 0xFFu) + c) & 0xFFu);
            }
            regs.insert((ins.w1 >> 9) & 0xFFu);
            regs.insert((ins.w1 >> 16) & 0xFFu);
            regs.insert((ins.w1 >> 18) & 0xFFu);
        }
        for (uint32_t ni = 0; ni < ins.nsa_dwords; ++ni) {
            for (uint32_t b = 0; b < 4u; ++b) {
                regs.insert((ins.nsa_words[ni] >> (b * 8u)) & 0xFFu);
            }
        }
        insts.push_back(std::move(ins));
    }

    ShaderStage stage = stage_override;
    if (stage == ShaderStage::Unknown && hdr.stage != 0) {
        stage = StageFromXs2Hint(hdr.stage);
    }
    if (stage == ShaderStage::Unknown) {
        if (has_vs_exp) stage = ShaderStage::Vertex;
        else if (has_ps_exp) stage = ShaderStage::Pixel;
        else stage = (has_ssbo || has_bvh_ray || has_lds) ? ShaderStage::Compute : ShaderStage::Pixel;
    }
    if (out_detected_stage) *out_detected_stage = stage;

    std::vector<uint32_t> active_mrts;
    if (stage == ShaderStage::Pixel) {
        if (mrt_mask == 0u) mrt_mask = 1u;
        for (uint32_t m = 0; m < 8u; ++m) {
            if ((mrt_mask & (1u << m)) != 0) active_mrts.push_back(m);
        }
    }
    const bool multi_mrt = (active_mrts.size() > 1u || has_mrtz || (!active_mrts.empty() && active_mrts[0] != 0u));

    const auto cfg_roots = BuildHlslStructuredCfg(insts);

    auto resolve_root_binding = [&](uint32_t default_reg, uint32_t sgpr_hint) -> std::pair<uint32_t, uint32_t> {
        for (const auto& rm : hdr.root_map) {
            if (rm.user_sgpr_offset == sgpr_hint) {
                return {rm.register_index, rm.register_space};
            }
        }
        return {default_reg, 0u};
    };

    auto fmt_reg_space = [](const char* prefix, std::pair<uint32_t, uint32_t> rs) -> std::string {
        std::ostringstream os;
        os << "register(" << prefix << rs.first;
        if (rs.second != 0u) os << ", space" << rs.second;
        os << ")";
        return os.str();
    };

    const bool want_sm66_bindless =
        (target_shader_model >= 0x00060006u) && (hdr.is_bindless || has_dynamic_sgpr_rsrc);

    auto build_hlsl = [&](bool sm6_wave_ops, bool sm66_bindless) -> std::string {
        std::ostringstream h;
        const bool use_rayquery = sm6_wave_ops && has_bvh_ray;

        if (sm66_bindless) {
            h << "cbuffer Xs2UserSgprRoot : " << fmt_reg_space("b", resolve_root_binding(0u, 0u))
              << " { uint4 xs2_root_sgprs[4]; };\n";
        } else {
            if (has_smem) {
                h << "cbuffer Xs2Ubo : " << fmt_reg_space("b", resolve_root_binding(0u, 0u))
                  << " { float4 ubo0; };\n";
            }
            if (has_mimg && stage != ShaderStage::Vertex) {
                h << "Texture2D<float4> Xs2Tex0 : " << fmt_reg_space("t", resolve_root_binding(0u, 4u)) << ";\n";
                h << "SamplerState Xs2Samp0 : " << fmt_reg_space("s", resolve_root_binding(0u, 8u)) << ";\n";
            }
            if (has_ssbo) {
                if (stage == ShaderStage::Vertex && !has_ssbo_write) {
                    h << "ByteAddressBuffer Xs2VtxBuf0 : "
                      << fmt_reg_space("t", resolve_root_binding(0u, 0u)) << ";\n";
                } else {
                    h << "RWByteAddressBuffer Xs2Ssbo0 : "
                      << fmt_reg_space("u", resolve_root_binding(0u, 0u)) << ";\n";
                }
            }
        }
        if (use_rayquery) {
            const uint32_t bvh_t = (has_mimg && !sm66_bindless) ? 1u : 0u;
            h << "RaytracingAccelerationStructure Xs2Bvh0 : "
              << fmt_reg_space("t", resolve_root_binding(bvh_t, 12u)) << ";\n";
        }
        if (has_lds && (stage == ShaderStage::Compute || stage == ShaderStage::Mesh || stage == ShaderStage::Amplification)) {
            h << "groupshared float lds_mem[256];\n";
        }

        const uint32_t wx = hdr.threads_x;
        const uint32_t wy = hdr.threads_y;
        const uint32_t wz = hdr.threads_z;

        if (stage == ShaderStage::Vertex) {
            h << "struct VSOut { float4 pos : SV_POSITION; float4 attr0 : TEXCOORD0; };\n";
            if (hdr.has_ia_vertex_inputs) {
                h << "struct VSIn { float4 ia_pos : POSITION0; float4 ia_uv : TEXCOORD0; "
                  << "uint vid : SV_VertexID; uint iid : SV_InstanceID; };\n";
                h << "VSOut main(VSIn vin) {\n    uint vid = vin.vid;\n";
            } else {
                h << "VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n";
            }
        } else if (stage == ShaderStage::Pixel) {
            h << "struct PSIn { float4 pos : SV_POSITION; float4 attr0 : TEXCOORD0; };\n";
            if (multi_mrt) {
                h << "struct PSOut {\n";
                for (uint32_t m : active_mrts) {
                    h << "    float4 mrt" << m << " : SV_TARGET" << m << ";\n";
                }
                if (has_mrtz) {
                    h << "    float mrtz : SV_Depth;\n";
                }
                h << "};\nPSOut main(PSIn pin) {\n";
            } else {
                h << "float4 main(PSIn pin) : SV_TARGET0 {\n";
            }
        } else if (stage == ShaderStage::Mesh && sm6_wave_ops) {
            h << "struct MSOut { float4 pos : SV_POSITION; };\n";
            h << "[outputtopology(\"triangle\")]\n";
            h << "[numthreads(" << wx << ", " << wy << ", " << wz << ")]\n";
            h << "void main(uint gidx : SV_GroupIndex, uint3 gid : SV_GroupID, "
              << "out vertices MSOut verts[3], out indices uint3 tris[1]) {\n"
              << "    SetMeshOutputCounts(3, 1);\n"
              << "    if (gidx < 3u) verts[gidx].pos = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
              << "    if (gidx == 0u) tris[0] = uint3(0u, 1u, 2u);\n"
              << "    uint3 tid = uint3(gid.x * " << wx << "u + (gidx % " << wx << "u), gidx / " << wx << "u, 0u);\n";
        } else {
            if (sm6_wave_ops && enable_wave32) {
                h << "[WaveSize(32)]\n";
            }
            h << "[numthreads(" << wx << ", " << wy << ", " << wz << ")]\n";
            h << "void main(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gidx : SV_GroupIndex) {\n";
        }

        // 64-entry Scalar GPR file s[0..63] + touched Vector GPRs v0..v255 + SCC/VCC/EXEC flags
        h << "    uint s[64] = {\n"
          << "        0u,0u,0u,0u,0u,0u,0u,0u, 0u,0u,0u,0u,0u,0u,0u,0u,\n"
          << "        0u,0u,0u,0u,0u,0u,0u,0u, 0u,0u,0u,0u,0u,0u,0u,0u,\n"
          << "        0u,0u,0u,0u,0u,0u,0u,0u, 0u,0u,0u,0u,0u,0u,0u,0u,\n"
          << "        0u,0u,0u,0u,0u,0u,0u,0u, 0u,0u,0u,0u,0u,0u,0u,0u };\n";
        if (sm66_bindless) {
            h << "    [unroll] for (uint _ui = 0u; _ui < 4u; ++_ui) {\n"
              << "        s[_ui * 4u + 0u] = xs2_root_sgprs[_ui].x;\n"
              << "        s[_ui * 4u + 1u] = xs2_root_sgprs[_ui].y;\n"
              << "        s[_ui * 4u + 2u] = xs2_root_sgprs[_ui].z;\n"
              << "        s[_ui * 4u + 3u] = xs2_root_sgprs[_ui].w;\n"
              << "    }\n";
        }

        std::vector<uint32_t> sorted_regs(regs.begin(), regs.end());
        std::sort(sorted_regs.begin(), sorted_regs.end());
        for (uint32_t r : sorted_regs) {
            h << "    float v" << r << " = 1.0f;\n";
        }
        h << "    bool scc = true;\n    bool vcc = true;\n    bool exec_nz = true;\n";

        if (stage == ShaderStage::Vertex) {
            if (hdr.has_ia_vertex_inputs) {
                h << "    v0 = vin.ia_pos.x; v1 = vin.ia_pos.y; v2 = vin.ia_pos.z; v3 = vin.ia_pos.w;\n";
                h << "    float4 out_pos = vin.ia_pos;\n    float4 out_attr = vin.ia_uv;\n";
            } else {
                h << "    float2 tri[3] = { float2(0.0f, 0.7f), float2(0.7f, -0.7f), float2(-0.7f, -0.7f) };\n";
                h << "    v0 = tri[vid % 3u].x; v1 = tri[vid % 3u].y; v2 = 0.0f; v3 = (float)iid + 1.0f;\n";
                h << "    float4 out_pos = float4(v0, v1, 0.0f, 1.0f);\n";
                h << "    float4 out_attr = float4(1.0f, 1.0f, 1.0f, 1.0f);\n";
            }
        } else if (stage == ShaderStage::Pixel) {
            for (uint32_t m : active_mrts) {
                if (m == 0u) h << "    float4 out_mrt0 = pin.attr0;\n";
                else h << "    float4 out_mrt" << m << " = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
            }
            if (has_mrtz) h << "    float out_mrtz = pin.pos.z;\n";
        } else {
            const uint32_t wg_total = wx * wy * wz;
            if (wy == 1u && wz == 1u) {
                h << "    v0 = (float)tid.x;\n";
            } else {
                h << "    v0 = (float)(gid.x * " << wg_total << "u + gidx);\n";
            }
        }

        // Track runtime-defined SGPRs during emission so VALU src0 < 106 reads SGPRs when written
        std::unordered_set<uint32_t> live_sgprs;
        for (uint32_t us = 0; us < std::min<uint32_t>(hdr.user_sgpr_count, 64u); ++us) {
            live_sgprs.insert(us);
        }
        if (sm66_bindless) {
            for (uint32_t us = 0; us < 16u; ++us) live_sgprs.insert(us);
        }

        auto salu_src_u32 = [&](uint32_t ssrc, const MiniRdna2Inst& ins) -> std::string {
            if (ssrc == 0xFFu && ins.has_literal) return FormatHexU32(ins.literal_u32);
            if (ssrc >= 128u && ssrc <= 192u) return std::to_string(ssrc - 128u) + "u";
            if (ssrc >= 193u && ssrc <= 208u) {
                const int32_t neg = -static_cast<int32_t>(ssrc - 192u);
                return FormatHexU32(static_cast<uint32_t>(neg));
            }
            if (ssrc == 240u) return "0x3F000000u";
            if (ssrc == 241u) return "0xBF000000u";
            if (ssrc == 242u) return "0x3F800000u";
            if (ssrc == 243u) return "0xBF800000u";
            if (ssrc == 244u) return "0x40000000u";
            if (ssrc == 245u) return "0xC0000000u";
            if (ssrc == 246u) return "0x40800000u";
            if (ssrc == 247u) return "0xC0800000u";
            return "s[" + std::to_string(ssrc & 63u) + "]";
        };

        auto valu_src_f32 = [&](uint32_t src9, const MiniRdna2Inst& ins) -> std::string {
            if (src9 == 0x0FFu && ins.has_literal) {
                return "asfloat(" + FormatHexU32(ins.literal_u32) + ")";
            }
            if (src9 == 0x0FAu && ins.has_dpp) {
                const uint32_t vreg = ins.w1 & 0xFFu;
                const uint32_t dpp_ctrl = (ins.w1 >> 8) & 0x1FFu;
                if (sm6_wave_ops && (stage == ShaderStage::Compute || stage == ShaderStage::Pixel) && dpp_ctrl < 0x100u) {
                    return "QuadReadLaneAt(v" + std::to_string(vreg) + ", " + std::to_string(dpp_ctrl & 3u) + "u)";
                }
                return "v" + std::to_string(vreg);
            }
            if (src9 == 0x0F9u && ins.has_sdwa) {
                return "v" + std::to_string(ins.w1 & 0xFFu);
            }
            if (src9 >= 256u) {
                return "v" + std::to_string((src9 - 256u) & 0xFFu);
            }
            if (src9 >= 128u && src9 <= 192u) {
                return "asfloat(" + std::to_string(src9 - 128u) + "u)";
            }
            if (src9 >= 193u && src9 <= 208u) {
                const uint32_t bits = static_cast<uint32_t>(-static_cast<int32_t>(src9 - 192u));
                return "asfloat(" + FormatHexU32(bits) + ")";
            }
            if (src9 == 240u) return "0.5f";
            if (src9 == 241u) return "-0.5f";
            if (src9 == 242u) return "1.0f";
            if (src9 == 243u) return "-1.0f";
            if (src9 == 244u) return "2.0f";
            if (src9 == 245u) return "-2.0f";
            if (src9 == 246u) return "4.0f";
            if (src9 == 247u) return "-4.0f";
            if (src9 < 106u && live_sgprs.count(src9 & 63u) > 0) {
                return "asfloat(s[" + std::to_string(src9 & 63u) + "])";
            }
            return "v" + std::to_string(src9 & 0xFFu);
        };

        auto valu_src_u32 = [&](uint32_t src9, const MiniRdna2Inst& ins) -> std::string {
            if (src9 == 0x0FFu && ins.has_literal) {
                return FormatHexU32(ins.literal_u32);
            }
            if (src9 == 0x0FAu && ins.has_dpp) {
                const uint32_t vreg = ins.w1 & 0xFFu;
                const uint32_t dpp_ctrl = (ins.w1 >> 8) & 0x1FFu;
                if (sm6_wave_ops && (stage == ShaderStage::Compute || stage == ShaderStage::Pixel) && dpp_ctrl < 0x100u) {
                    return "QuadReadLaneAt(asuint(v" + std::to_string(vreg) + "), " + std::to_string(dpp_ctrl & 3u) + "u)";
                }
                return "asuint(v" + std::to_string(vreg) + ")";
            }
            if (src9 == 0x0F9u && ins.has_sdwa) {
                const uint32_t vreg = ins.w1 & 0xFFu;
                const uint32_t sel = (ins.w1 >> 16) & 0x7u;
                const std::string base = "asuint(v" + std::to_string(vreg) + ")";
                if (sel < 4u) return "((" + base + " >> " + std::to_string(sel * 8u) + "u) & 0xFFu)";
                if (sel < 6u) return "((" + base + " >> " + std::to_string((sel - 4u) * 16u) + "u) & 0xFFFFu)";
                return base;
            }
            if (src9 >= 256u) {
                return "asuint(v" + std::to_string((src9 - 256u) & 0xFFu) + ")";
            }
            if (src9 >= 128u && src9 <= 192u) {
                return std::to_string(src9 - 128u) + "u";
            }
            if (src9 >= 193u && src9 <= 208u) {
                const uint32_t bits = static_cast<uint32_t>(-static_cast<int32_t>(src9 - 192u));
                return FormatHexU32(bits);
            }
            if (src9 < 106u && live_sgprs.count(src9 & 63u) > 0) {
                return "s[" + std::to_string(src9 & 63u) + "]";
            }
            return "asuint(v" + std::to_string(src9 & 0xFFu) + ")";
        };

        auto branch_cond_expr = [](const MiniRdna2Inst& br) -> std::string {
            if (br.op == "s_cbranch_scc0") return "(!scc)";
            if (br.op == "s_cbranch_scc1") return "scc";
            if (br.op == "s_cbranch_vccz") return "(!vcc)";
            if (br.op == "s_cbranch_vccnz") return "vcc";
            if (br.op == "s_cbranch_execz") return "(!exec_nz)";
            if (br.op == "s_cbranch_execnz") return "exec_nz";
            return "true";
        };

        auto emit_single = [&](const MiniRdna2Inst& ins, const std::string& ind) {
            const uint32_t w0 = ins.w0;
            const uint32_t w1 = ins.w1;
            const uint32_t src0_9 = w0 & 0x1FFu;

            if (ins.op.rfind("s_load_", 0) == 0) {
                const uint32_t sdata = ((w0 >> 6) & 0x7Fu) & 63u;
                for (uint32_t k = 0; k < ins.mem_dwords; ++k) live_sgprs.insert((sdata + k) & 63u);
                if (sm66_bindless) {
                    const uint32_t sbase = ((w0 & 0x3Fu) * 2u) & 63u;
                    h << ind << "{ ByteAddressBuffer cb = ResourceDescriptorHeap[NonUniformResourceIndex(s[" << sbase << "])];\n"
                      << ind << "  uint4 ld = cb.Load4(0u);\n"
                      << ind << "  s[" << sdata << "] = ld.x; s[" << ((sdata + 1u) & 63u) << "] = ld.y; "
                      << "s[" << ((sdata + 2u) & 63u) << "] = ld.z; s[" << ((sdata + 3u) & 63u) << "] = ld.w;\n"
                      << ind << "  v0 = asfloat(ld.x); v1 = asfloat(ld.y); v2 = asfloat(ld.z); v3 = asfloat(ld.w); }\n";
                } else if (has_smem) {
                    h << ind << "s[" << sdata << "] = asuint(ubo0.x); s[" << ((sdata + 1u) & 63u) << "] = asuint(ubo0.y); "
                      << "s[" << ((sdata + 2u) & 63u) << "] = asuint(ubo0.z); s[" << ((sdata + 3u) & 63u) << "] = asuint(ubo0.w);\n";
                    if (sdata == 0u || stage != ShaderStage::Compute) {
                        h << ind << "v0 = ubo0.x; v1 = ubo0.y; v2 = ubo0.z; v3 = ubo0.w;\n";
                    }
                }
            } else if (ins.op == "s_mov_b32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] = " << salu_src_u32(w0 & 0xFFu, ins) << ";\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_not_b32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] = ~(" << salu_src_u32(w0 & 0xFFu, ins) << "); scc = (s[" << sdst << "] != 0u);\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_and_saveexec_b32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] = 0xFFFFFFFFu; exec_nz = (" << salu_src_u32(w0 & 0xFFu, ins) << " != 0u); scc = exec_nz;\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_movk_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] = " << FormatHexU32(static_cast<uint32_t>(static_cast<int32_t>(ins.branch_simm16))) << ";\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_addk_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] += " << FormatHexU32(static_cast<uint32_t>(static_cast<int32_t>(ins.branch_simm16))) << ";\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_mulk_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] *= " << FormatHexU32(static_cast<uint32_t>(static_cast<int32_t>(ins.branch_simm16))) << ";\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_cmpk_eq_i32" || ins.op == "s_cmpk_lg_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                const char* cmp = (ins.op == "s_cmpk_eq_i32") ? " == " : " != ";
                h << ind << "scc = (asint(s[" << sdst << "])" << cmp << static_cast<int32_t>(ins.branch_simm16) << ");\n";
            } else if (ins.op == "s_add_u32" || ins.op == "s_add_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                const std::string a = salu_src_u32(w0 & 0xFFu, ins);
                const std::string b = salu_src_u32((w0 >> 8) & 0xFFu, ins);
                h << ind << "{ uint _sa = " << a << "; uint _sb = " << b << "; s[" << sdst << "] = _sa + _sb; scc = (s[" << sdst << "] < _sa); }\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_sub_u32" || ins.op == "s_sub_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                const std::string a = salu_src_u32(w0 & 0xFFu, ins);
                const std::string b = salu_src_u32((w0 >> 8) & 0xFFu, ins);
                h << ind << "{ uint _sa = " << a << "; uint _sb = " << b << "; s[" << sdst << "] = _sa - _sb; scc = (_sa < _sb); }\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_and_b32" || ins.op == "s_or_b32" || ins.op == "s_xor_b32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                const char* bop = (ins.op == "s_and_b32") ? " & " : ((ins.op == "s_or_b32") ? " | " : " ^ ");
                h << ind << "s[" << sdst << "] = (" << salu_src_u32(w0 & 0xFFu, ins) << bop
                  << salu_src_u32((w0 >> 8) & 0xFFu, ins) << "); scc = (s[" << sdst << "] != 0u);\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_lshl_b32" || ins.op == "s_lshr_b32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                const char* sop = (ins.op == "s_lshl_b32") ? " << " : " >> ";
                h << ind << "s[" << sdst << "] = (" << salu_src_u32(w0 & 0xFFu, ins) << sop
                  << "(" << salu_src_u32((w0 >> 8) & 0xFFu, ins) << " & 31u)); scc = (s[" << sdst << "] != 0u);\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_ashr_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] = (uint)(asint(" << salu_src_u32(w0 & 0xFFu, ins) << ") >> ("
                  << salu_src_u32((w0 >> 8) & 0xFFu, ins) << " & 31u)); scc = (s[" << sdst << "] != 0u);\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_mul_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] = (" << salu_src_u32(w0 & 0xFFu, ins) << " * "
                  << salu_src_u32((w0 >> 8) & 0xFFu, ins) << ");\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_cselect_b32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                h << ind << "s[" << sdst << "] = scc ? " << salu_src_u32(w0 & 0xFFu, ins) << " : "
                  << salu_src_u32((w0 >> 8) & 0xFFu, ins) << ";\n";
                live_sgprs.insert(sdst);
            } else if (ins.op == "s_min_u32" || ins.op == "s_max_u32" || ins.op == "s_min_i32" || ins.op == "s_max_i32") {
                const uint32_t sdst = ((w0 >> 16) & 0x7Fu) & 63u;
                const char* fn = (ins.op.find("min") != std::string::npos) ? "min" : "max";
                if (ins.op.find("_i32") != std::string::npos) {
                    h << ind << "s[" << sdst << "] = (uint)" << fn << "(asint(" << salu_src_u32(w0 & 0xFFu, ins)
                      << "), asint(" << salu_src_u32((w0 >> 8) & 0xFFu, ins) << "));\n";
                } else {
                    h << ind << "s[" << sdst << "] = (uint)" << fn << "(" << salu_src_u32(w0 & 0xFFu, ins) << ", "
                      << salu_src_u32((w0 >> 8) & 0xFFu, ins) << ");\n";
                }
                live_sgprs.insert(sdst);
            } else if (ins.op.rfind("s_cmp_", 0) == 0) {
                const uint32_t s0 = w0 & 0xFFu;
                const uint32_t s1 = (w0 >> 8) & 0xFFu;
                const bool use_sgpr = (live_sgprs.count(s0 & 63u) > 0 || live_sgprs.count(s1 & 63u) > 0 || ins.has_literal || s0 >= 128u || s1 >= 128u);
                const std::string lhs = use_sgpr ? salu_src_u32(s0, ins) : ("asuint(v" + std::to_string(s0) + ")");
                const std::string rhs = use_sgpr ? salu_src_u32(s1, ins) : ("asuint(v" + std::to_string(s1) + ")");
                const bool signed_cmp = (ins.op.find("_i32") != std::string::npos);
                const std::string a = signed_cmp ? ("asint(" + lhs + ")") : lhs;
                const std::string b = signed_cmp ? ("asint(" + rhs + ")") : rhs;
                const char* cop = " == ";
                if (ins.op.find("_lg_") != std::string::npos) cop = " != ";
                else if (ins.op.find("_gt_") != std::string::npos) cop = " > ";
                else if (ins.op.find("_ge_") != std::string::npos) cop = " >= ";
                else if (ins.op.find("_lt_") != std::string::npos) cop = " < ";
                else if (ins.op.find("_le_") != std::string::npos) cop = " <= ";
                h << ind << "scc = (" << a << cop << b << ");\n";
            } else if (ins.op == "v_interp" && stage == ShaderStage::Pixel) {
                const uint32_t vd = (w0 >> 18) & 0xFFu;
                const char ch = "xyzw"[(w0 >> 8) & 0x3u];
                h << ind << "v" << vd << " = pin.attr0." << ch << ";\n";
            } else if ((ins.op == "image_sample" || ins.op == "image_load") && stage != ShaderStage::Vertex) {
                const uint32_t vd = (w1 >> 8) & 0xFFu;
                const uint32_t va0 = w1 & 0xFFu;
                const uint32_t va1 = (ins.nsa_dwords > 0) ? (ins.nsa_words[0] & 0xFFu) : ((va0 + 1u) & 0xFFu);
                if (sm66_bindless) {
                    const uint32_t srsrc = (((w1 >> 16) & 0x1Fu) * 4u) & 63u;
                    const uint32_t ssamp = (((w1 >> 21) & 0x1Fu) * 4u) & 63u;
                    h << ind << "{ Texture2D<float4> _tex = ResourceDescriptorHeap[NonUniformResourceIndex(s[" << srsrc << "])];\n"
                      << ind << "  SamplerState _smp = SamplerDescriptorHeap[NonUniformResourceIndex(s[" << ssamp << "])];\n"
                      << ind << "  float4 smp = _tex.SampleLevel(_smp, float2(v" << va0 << ", v" << va1 << "), 0.0f);\n"
                      << ind << "  v" << vd << " = smp.x; v" << ((vd + 1) & 0xFFu) << " = smp.y; v"
                      << ((vd + 2) & 0xFFu) << " = smp.z; v" << ((vd + 3) & 0xFFu) << " = smp.w; }\n";
                } else if (has_mimg) {
                    h << ind << "{ float4 smp = Xs2Tex0.SampleLevel(Xs2Samp0, float2(v" << va0 << ", v" << va1 << "), 0.0f);\n"
                      << ind << "  v" << vd << " = smp.x; v" << ((vd + 1) & 0xFFu) << " = smp.y; v"
                      << ((vd + 2) & 0xFFu) << " = smp.z; v" << ((vd + 3) & 0xFFu) << " = smp.w; }\n";
                }
            } else if (ins.op == "image_bvh64_intersect_ray") {
                const uint32_t vd = (w1 >> 8) & 0xFFu;
                const uint32_t va = w1 & 0xFFu;
                if (use_rayquery) {
                    h << ind << "{ RayQuery<RAY_FLAG_FORCE_OPAQUE> _rq;\n"
                      << ind << "  RayDesc _rd; _rd.Origin = float3(v" << va << ", v" << ((va + 1) & 0xFFu) << ", v" << ((va + 2) & 0xFFu) << ");\n"
                      << ind << "  float3 _dir = float3(v" << ((va + 3) & 0xFFu) << ", v" << ((va + 4) & 0xFFu) << ", 1.0f);\n"
                      << ind << "  _rd.Direction = normalize(length(_dir) > 1e-5f ? _dir : float3(0.0f, 0.0f, 1.0f));\n"
                      << ind << "  _rd.TMin = 0.001f; _rd.TMax = 10000.0f;\n"
                      << ind << "  _rq.TraceRayInline(Xs2Bvh0, RAY_FLAG_FORCE_OPAQUE, 0xFFu, _rd); _rq.Proceed();\n"
                      << ind << "  bool _hit = (_rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT);\n"
                      << ind << "  float2 _bary = _hit ? _rq.CommittedTriangleBarycentrics() : float2(0.0f, 0.0f);\n"
                      << ind << "  v" << vd << " = _hit ? _rq.CommittedRayT() : -1.0f;\n"
                      << ind << "  v" << ((vd + 1) & 0xFFu) << " = _bary.x; v" << ((vd + 2) & 0xFFu) << " = _bary.y;\n"
                      << ind << "  v" << ((vd + 3) & 0xFFu) << " = _hit ? 1.0f : 0.0f; }\n";
                } else {
                    h << ind << "{ float _t = max(0.0f, (1.0f - v" << va << ") * rcp(max(abs(v" << ((va + 1) & 0xFFu) << "), 1e-5f)));\n"
                      << ind << "  v" << vd << " = _t; v" << ((vd + 1) & 0xFFu) << " = 0.25f; v"
                      << ((vd + 2) & 0xFFu) << " = 0.25f; v" << ((vd + 3) & 0xFFu) << " = (_t > 0.0f) ? 1.0f : 0.0f; }\n";
                }
            } else if (ins.op == "v_mov_b32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = " << valu_src_f32(src0_9, ins) << ";\n";
            } else if (ins.op == "v_cvt_f32_i32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = (float)asint(" << valu_src_u32(src0_9, ins) << ");\n";
            } else if (ins.op == "v_cvt_f32_u32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = (float)(" << valu_src_u32(src0_9, ins) << ");\n";
            } else if (ins.op == "v_cvt_u32_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = asfloat((uint)max(" << valu_src_f32(src0_9, ins) << ", 0.0f));\n";
            } else if (ins.op == "v_cvt_i32_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = asfloat((uint)(int)(" << valu_src_f32(src0_9, ins) << "));\n";
            } else if (ins.op == "v_not_b32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = asfloat(~(" << valu_src_u32(src0_9, ins) << "));\n";
            } else if (ins.op == "v_readfirstlane_b32") {
                const uint32_t sdst = (w0 >> 17) & 0xFFu;
                const std::string vs_f = valu_src_f32(src0_9, ins);
                if (sm6_wave_ops && stage == ShaderStage::Compute) {
                    h << ind << "{ float _wfl = WaveReadLaneFirst(" << vs_f << "); v" << sdst << " = _wfl; s[" << (sdst & 63u) << "] = asuint(_wfl); }\n";
                } else {
                    h << ind << "{ float _wfl = " << vs_f << "; v" << sdst << " = _wfl; s[" << (sdst & 63u) << "] = asuint(_wfl); }\n";
                }
                live_sgprs.insert(sdst & 63u);
            } else if (ins.op == "v_mbcnt_lo_u32_b32") {
                const uint32_t vd = w0 & 0xFFu;
                if (sm6_wave_ops && stage == ShaderStage::Compute) {
                    h << ind << "v" << vd << " = (float)WaveActiveBallot(vcc).x;\n";
                } else {
                    h << ind << "v" << vd << " = vcc ? 1.0f : 0.0f;\n";
                }
            } else if (ins.op == "v_rcp_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = 1.0f / max(abs(" << valu_src_f32(src0_9, ins) << "), 1e-6f);\n";
            } else if (ins.op == "v_rsq_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = rsqrt(max(abs(" << valu_src_f32(src0_9, ins) << "), 1e-6f));\n";
            } else if (ins.op == "v_sqrt_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = sqrt(max(" << valu_src_f32(src0_9, ins) << ", 0.0f));\n";
            } else if (ins.op == "v_sin_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = sin(" << valu_src_f32(src0_9, ins) << ");\n";
            } else if (ins.op == "v_cos_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = cos(" << valu_src_f32(src0_9, ins) << ");\n";
            } else if (ins.op.rfind("v_cmp_", 0) == 0) {
                const uint32_t vsrc1 = (w0 >> 9) & 0xFFu;
                const char* cop = " >= ";
                if (ins.op.find("_lt_") != std::string::npos) cop = " < ";
                else if (ins.op.find("_eq_") != std::string::npos) cop = " == ";
                else if (ins.op.find("_le_") != std::string::npos) cop = " <= ";
                else if (ins.op.find("_gt_") != std::string::npos) cop = " > ";
                else if (ins.op.find("_lg_") != std::string::npos) cop = " != ";
                if (ins.op.find("_u32") != std::string::npos) {
                    h << ind << "vcc = (" << valu_src_u32(src0_9, ins) << cop << "asuint(v" << vsrc1 << "));\n";
                } else if (ins.op.find("_i32") != std::string::npos) {
                    h << ind << "vcc = (asint(" << valu_src_u32(src0_9, ins) << ")" << cop << "asint(v" << vsrc1 << "));\n";
                } else {
                    h << ind << "vcc = (" << valu_src_f32(src0_9, ins) << cop << "v" << vsrc1 << ");\n";
                }
            } else if (ins.op == "v_add_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = " << valu_src_f32(src0_9, ins) << " + v" << ((w0 >> 9) & 0xFFu) << ";\n";
            } else if (ins.op == "v_sub_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = " << valu_src_f32(src0_9, ins) << " - v" << ((w0 >> 9) & 0xFFu) << ";\n";
            } else if (ins.op == "v_subrev_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = v" << ((w0 >> 9) & 0xFFu) << " - " << valu_src_f32(src0_9, ins) << ";\n";
            } else if (ins.op == "v_mul_f32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = " << valu_src_f32(src0_9, ins) << " * v" << ((w0 >> 9) & 0xFFu) << ";\n";
            } else if (ins.op == "v_min_f32" || ins.op == "v_max_f32") {
                const char* fn = (ins.op == "v_min_f32") ? "min" : "max";
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = " << fn << "(" << valu_src_f32(src0_9, ins) << ", v" << ((w0 >> 9) & 0xFFu) << ");\n";
            } else if (ins.op == "v_add_nc_u32" || ins.op == "v_sub_nc_u32" || ins.op == "v_subrev_nc_u32" || ins.op == "v_mul_i32_i24") {
                const uint32_t vd = (w0 >> 17) & 0xFFu;
                const std::string a = valu_src_u32(src0_9, ins);
                const std::string b = "asuint(v" + std::to_string((w0 >> 9) & 0xFFu) + ")";
                if (ins.op == "v_add_nc_u32") h << ind << "v" << vd << " = asfloat(" << a << " + " << b << ");\n";
                else if (ins.op == "v_sub_nc_u32") h << ind << "v" << vd << " = asfloat(" << a << " - " << b << ");\n";
                else if (ins.op == "v_subrev_nc_u32") h << ind << "v" << vd << " = asfloat(" << b << " - " << a << ");\n";
                else h << ind << "v" << vd << " = asfloat((uint)(asint(" << a << ") * asint(" << b << ")));\n";
            } else if (ins.op == "v_lshlrev_b32" || ins.op == "v_lshrrev_b32" || ins.op == "v_ashrrev_i32") {
                const uint32_t vd = (w0 >> 17) & 0xFFu;
                const std::string sh = "(" + valu_src_u32(src0_9, ins) + " & 31u)";
                const std::string val = "asuint(v" + std::to_string((w0 >> 9) & 0xFFu) + ")";
                if (ins.op == "v_lshlrev_b32") h << ind << "v" << vd << " = asfloat(" << val << " << " << sh << ");\n";
                else if (ins.op == "v_lshrrev_b32") h << ind << "v" << vd << " = asfloat(" << val << " >> " << sh << ");\n";
                else h << ind << "v" << vd << " = asfloat((uint)(asint(" << val << ") >> " << sh << "));\n";
            } else if (ins.op == "v_and_b32" || ins.op == "v_or_b32" || ins.op == "v_xor_b32") {
                const uint32_t vd = (w0 >> 17) & 0xFFu;
                const char* bop = (ins.op == "v_and_b32") ? " & " : ((ins.op == "v_or_b32") ? " | " : " ^ ");
                h << ind << "v" << vd << " = asfloat(" << valu_src_u32(src0_9, ins) << bop
                  << "asuint(v" << ((w0 >> 9) & 0xFFu) << "));\n";
            } else if (ins.op == "v_min_u32" || ins.op == "v_max_u32" || ins.op == "v_min_i32" || ins.op == "v_max_i32") {
                const uint32_t vd = (w0 >> 17) & 0xFFu;
                const char* fn = (ins.op.find("min") != std::string::npos) ? "min" : "max";
                if (ins.op.find("_i32") != std::string::npos) {
                    h << ind << "v" << vd << " = asfloat((uint)" << fn << "(asint(" << valu_src_u32(src0_9, ins)
                      << "), asint(v" << ((w0 >> 9) & 0xFFu) << ")));\n";
                } else {
                    h << ind << "v" << vd << " = asfloat(" << fn << "(" << valu_src_u32(src0_9, ins)
                      << ", asuint(v" << ((w0 >> 9) & 0xFFu) << ")));\n";
                }
            } else if (ins.op == "v_cndmask_b32") {
                h << ind << "v" << ((w0 >> 17) & 0xFFu) << " = vcc ? v" << ((w0 >> 9) & 0xFFu)
                  << " : " << valu_src_f32(src0_9, ins) << ";\n";
            } else if (ins.op == "v_fmamk_f32") {
                const uint32_t vd = (w0 >> 17) & 0xFFu;
                h << ind << "v" << vd << " = mad(" << valu_src_f32(src0_9, ins) << ", asfloat("
                  << FormatHexU32(ins.literal_u32) << "), v" << ((w0 >> 9) & 0xFFu) << ");\n";
            } else if (ins.op == "v_fmaak_f32") {
                const uint32_t vd = (w0 >> 17) & 0xFFu;
                h << ind << "v" << vd << " = mad(" << valu_src_f32(src0_9, ins) << ", v"
                  << ((w0 >> 9) & 0xFFu) << ", asfloat(" << FormatHexU32(ins.literal_u32) << "));\n";
            } else if (ins.op == "v_fmac_f32") {
                const uint32_t vd = (w0 >> 17) & 0xFFu;
                h << ind << "v" << vd << " = mad(" << valu_src_f32(src0_9, ins) << ", v"
                  << ((w0 >> 9) & 0xFFu) << ", v" << vd << ");\n";
            } else if (ins.op == "v_fma_f32" || ins.op == "v_fma_mix_f32") {
                const uint32_t vd = w0 & 0xFFu;
                const std::string a = (w1 != 0) ? valu_src_f32(w1 & 0x1FFu, ins) : "v0";
                const std::string b = (w1 != 0) ? valu_src_f32((w1 >> 9) & 0x1FFu, ins) : "v1";
                const std::string c = (w1 != 0) ? valu_src_f32((w1 >> 18) & 0x1FFu, ins) : ("v" + std::to_string(vd));
                h << ind << "v" << vd << " = mad(" << a << ", " << b << ", " << c << ");\n";
            } else if (ins.op.rfind("v_pk_", 0) == 0) {
                const uint32_t vd = w0 & 0xFFu;
                const std::string a = valu_src_u32(w1 & 0x1FFu, ins);
                const std::string b = valu_src_u32((w1 >> 9) & 0x1FFu, ins);
                const std::string c = valu_src_u32((w1 >> 18) & 0x1FFu, ins);
                h << ind << "{ uint _pa = " << a << "; uint _pb = " << b << "; uint _pc = " << c << ";\n"
                  << ind << "  float2 _fa = float2(f16tof32(_pa), f16tof32(_pa >> 16u));\n"
                  << ind << "  float2 _fb = float2(f16tof32(_pb), f16tof32(_pb >> 16u));\n"
                  << ind << "  float2 _fc = float2(f16tof32(_pc), f16tof32(_pc >> 16u));\n";
                if (ins.op == "v_pk_mul_f16") h << ind << "  float2 _fr = _fa * _fb;\n";
                else if (ins.op == "v_pk_add_f16") h << ind << "  float2 _fr = _fa + _fb;\n";
                else if (ins.op == "v_pk_sub_f16") h << ind << "  float2 _fr = _fa - _fb;\n";
                else h << ind << "  float2 _fr = mad(_fa, _fb, _fc);\n";
                h << ind << "  v" << vd << " = asfloat((f32tof16(_fr.x) & 0xFFFFu) | (f32tof16(_fr.y) << 16u)); }\n";
            } else if (ins.op == "v_add3_u32" || ins.op == "v_lshl_or_b32") {
                const uint32_t vd = w0 & 0xFFu;
                const std::string a = valu_src_u32(w1 & 0x1FFu, ins);
                const std::string b = valu_src_u32((w1 >> 9) & 0x1FFu, ins);
                const std::string c = valu_src_u32((w1 >> 18) & 0x1FFu, ins);
                if (ins.op == "v_add3_u32") h << ind << "v" << vd << " = asfloat(" << a << " + " << b << " + " << c << ");\n";
                else h << ind << "v" << vd << " = asfloat((" << a << " << (" << b << " & 31u)) | " << c << ");\n";
            } else if (ins.op.find("load") != std::string::npos && has_ssbo) {
                const bool is_flat = (ins.op.rfind("global_", 0) == 0 || ins.op.rfind("flat_", 0) == 0 || ins.op.rfind("scratch_", 0) == 0);
                const uint32_t vd = is_flat ? ((w1 >> 24) & 0xFFu) : ((w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu));
                const uint32_t va = (w1 != 0) ? (w1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
                const uint32_t imm_off = (w0 & 0xFFFu);
                const std::string buf_name = (stage == ShaderStage::Vertex && !has_ssbo_write && !sm66_bindless) ? "Xs2VtxBuf0" : "Xs2Ssbo0";
                for (uint32_t c = 0; c < ins.mem_dwords; ++c) {
                    const uint32_t dst_r = (vd + c) & 0xFFu;
                    const uint32_t byte_delta = imm_off + c * 4u;
                    if (sm66_bindless) {
                        const uint32_t srsrc = (((w1 >> 16) & 0x1Fu) * 4u) & 63u;
                        h << ind << "{ RWByteAddressBuffer _bb = ResourceDescriptorHeap[NonUniformResourceIndex(s[" << srsrc << "])];\n"
                          << ind << "  v" << dst_r << " = asfloat(_bb.Load(((uint)max(v" << va << ", 0.0f)) * 4u + " << byte_delta << "u)); }\n";
                    } else {
                        h << ind << "v" << dst_r << " = asfloat(" << buf_name << ".Load(((uint)max(v" << va
                          << ", 0.0f)) * 4u + " << byte_delta << "u));\n";
                    }
                }
            } else if (ins.op.find("store") != std::string::npos && has_ssbo) {
                const uint32_t vd = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
                const uint32_t va = (w1 != 0) ? (w1 & 0xFFu) : ((w0 >> 10) & 0xFFu);
                const uint32_t imm_off = (w0 & 0xFFFu);
                for (uint32_t c = 0; c < ins.mem_dwords; ++c) {
                    const uint32_t src_r = (vd + c) & 0xFFu;
                    const uint32_t byte_delta = imm_off + c * 4u;
                    if (sm66_bindless) {
                        const uint32_t srsrc = (((w1 >> 16) & 0x1Fu) * 4u) & 63u;
                        h << ind << "{ RWByteAddressBuffer _bb = ResourceDescriptorHeap[NonUniformResourceIndex(s[" << srsrc << "])];\n"
                          << ind << "  _bb.Store(((uint)max(v" << va << ", 0.0f)) * 4u + " << byte_delta << "u, asuint(v" << src_r << ")); }\n";
                    } else {
                        h << ind << "Xs2Ssbo0.Store(((uint)max(v" << va << ", 0.0f)) * 4u + "
                          << byte_delta << "u, asuint(v" << src_r << "));\n";
                    }
                }
            } else if (ins.op == "ds_read_b32" && has_lds &&
                       (stage == ShaderStage::Compute || stage == ShaderStage::Mesh || stage == ShaderStage::Amplification)) {
                const uint32_t vd = (w1 != 0) ? ((w1 >> 24) & 0xFFu) : ((w0 >> 18) & 0xFFu);
                const uint32_t va = w1 & 0xFFu;
                h << ind << "v" << vd << " = lds_mem[((uint)max(v" << va << ", 0.0f)) & 255u];\n";
            } else if (ins.op == "ds_write_b32" && has_lds &&
                       (stage == ShaderStage::Compute || stage == ShaderStage::Mesh || stage == ShaderStage::Amplification)) {
                const uint32_t vd = (w1 != 0) ? ((w1 >> 8) & 0xFFu) : ((w0 >> 18) & 0xFFu);
                const uint32_t va = w1 & 0xFFu;
                h << ind << "lds_mem[((uint)max(v" << va << ", 0.0f)) & 255u] = v" << vd << ";\n";
            } else if (ins.op == "s_barrier" && has_lds &&
                       (stage == ShaderStage::Compute || stage == ShaderStage::Mesh || stage == ShaderStage::Amplification)) {
                h << ind << "GroupMemoryBarrierWithGroupSync();\n";
            } else if (ins.op == "exp") {
                const uint32_t target = (w0 >> 4) & 0x3Fu;
                const uint32_t c0 = w1 & 0xFFu, c1 = (w1 >> 8) & 0xFFu;
                const uint32_t c2 = (w1 >> 16) & 0xFFu, c3 = (w1 >> 24) & 0xFFu;
                if (stage == ShaderStage::Vertex) {
                    if (target >= 12 && target <= 15) {
                        h << ind << "out_pos = float4(v" << c0 << ", v" << c1 << ", v" << c2 << ", v" << c3 << ");\n";
                    } else {
                        h << ind << "out_attr = float4(v" << c0 << ", v" << c1 << ", v" << c2 << ", v" << c3 << ");\n";
                    }
                } else if (stage == ShaderStage::Pixel) {
                    if (target < 8u) {
                        h << ind << "out_mrt" << target << " = float4(v" << c0 << ", v" << c1 << ", v" << c2 << ", v" << c3 << ");\n";
                    } else if (target == 8u && has_mrtz) {
                        h << ind << "out_mrtz = saturate(v" << c0 << ");\n";
                    }
                }
            }
        };

        std::function<void(size_t, size_t, const std::vector<HlslCfgRegion>&, const std::string&)> emit_range;
        emit_range = [&](size_t begin_idx, size_t end_idx, const std::vector<HlslCfgRegion>& children, const std::string& ind) {
            size_t cur = begin_idx;
            for (const auto& reg : children) {
                for (size_t k = cur; k < reg.begin; ++k) {
                    emit_single(insts[k], ind);
                }
                if (reg.is_loop) {
                    const std::string iv = "_sx_iter" + std::to_string(reg.loop_id);
                    h << ind << "[loop] for (uint " << iv << " = 0u; " << iv << " < 1024u; ++" << iv << ") {\n";
                    emit_range(reg.inner_begin, reg.inner_end, reg.children, ind + "    ");
                    h << ind << "    if (!" << branch_cond_expr(insts[reg.branch_idx]) << ") break;\n";
                    h << ind << "}\n";
                } else {
                    h << ind << "if (!" << branch_cond_expr(insts[reg.branch_idx]) << ") {\n";
                    emit_range(reg.inner_begin, reg.inner_end, reg.children, ind + "    ");
                    h << ind << "}\n";
                }
                cur = reg.end;
            }
            for (size_t k = cur; k < end_idx; ++k) {
                emit_single(insts[k], ind);
            }
        };

        emit_range(0u, insts.size(), cfg_roots, "    ");

        if (stage == ShaderStage::Vertex) {
            h << "    VSOut o; o.pos = out_pos; o.attr0 = out_attr; return o;\n}\n";
        } else if (stage == ShaderStage::Pixel) {
            if (multi_mrt) {
                h << "    PSOut o;\n";
                for (uint32_t m : active_mrts) {
                    h << "    o.mrt" << m << " = out_mrt" << m << ";\n";
                }
                if (has_mrtz) {
                    h << "    o.mrtz = out_mrtz;\n";
                }
                h << "    return o;\n}\n";
            } else {
                h << "    return out_mrt0;\n}\n";
            }
        } else {
            h << "}\n";
        }
        return h.str();
    };

#if defined(_WIN32) && defined(SCARLETTX_HAS_DXC_HEADER)
    if (target_shader_model >= 0x00060000u || stage == ShaderStage::Mesh || stage == ShaderStage::Amplification) {
        const uint32_t req_minor = std::min<uint32_t>(target_shader_model & 0xFFu, 6u);
        const uint32_t minor = (want_sm66_bindless) ? 6u :
                               (has_bvh_ray || stage == ShaderStage::Mesh || stage == ShaderStage::Amplification)
                                   ? std::max<uint32_t>(req_minor, 5u)
                                   : req_minor;
        const std::wstring prefix = (stage == ShaderStage::Vertex)        ? L"vs_6_" :
                                    (stage == ShaderStage::Pixel)         ? L"ps_6_" :
                                    (stage == ShaderStage::Mesh)          ? L"ms_6_" :
                                    (stage == ShaderStage::Amplification) ? L"as_6_" : L"cs_6_";
        const std::wstring profile = prefix + std::to_wstring(minor);
        if (want_sm66_bindless) {
            const std::string hlsl_bindless = build_hlsl(true, true);
            if (TryCompileHlslWithSignedDxc(hlsl_bindless, L"main", profile, out_pc_dxbc)) {
                return Status::Ok();
            }
        }
        const std::string hlsl_sm6 = build_hlsl(true, false);
        if (TryCompileHlslWithSignedDxc(hlsl_sm6, L"main", profile, out_pc_dxbc)) {
            return Status::Ok();
        }
    }
#else
    (void)target_shader_model;
    (void)enable_wave32;
    (void)want_sm66_bindless;
#endif

    // Universal retail-signed SM 5.0 DXBC fallback via D3DCompile (d3dcompiler_47.dll)
    const char* target_sm5 = (stage == ShaderStage::Vertex) ? "vs_5_0" :
                             (stage == ShaderStage::Pixel)  ? "ps_5_0" : "cs_5_0";
    return CompileHlslToDxbc(build_hlsl(false, false), "main", target_sm5, out_pc_dxbc);
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
        Xs2ParsedHeaderInfo parsed{};
        ParseXs2PrecompiledHeader(xbox_bytecode.data(), xbox_bytecode.size(), parsed);
        if (!parsed.isa_ptr || parsed.isa_size < 4) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "XS2 container has header only without RDNA2 microcode payload");
        }
        const uint32_t target_sm = parsed.is_bindless ? 0x00060006u : 0x00060005u;
        ShaderStage detected = ShaderStage::Unknown;
        Status lift_st = LiftRdna2MicrocodeToPcShader(
            xbox_bytecode.data(),
            xbox_bytecode.size(),
            out_result.source_info.stage,
            target_sm,
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

    // Rebuild DXBC/DXIL container without Xbox-private chunks while preserving all standard chunks.
    const uint32_t orig_count = ReadU32(xbox_bytecode.data() + 28);
    struct ChunkSlice {
        const uint8_t* ptr;
        size_t total_len;
    };
    std::vector<ChunkSlice> kept;
    kept.reserve(orig_count);
    bool has_pc_exec_chunk = false;
    bool has_dxil_chunk = false;
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
        if (fourcc == "DXIL" || fourcc == "ILDB") {
            has_pc_exec_chunk = true;
            has_dxil_chunk = true;
        } else if (fourcc == "SHDR" || fourcc == "SHEX") {
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

    // Re-sign DXIL containers via retail dxil.dll (IDxcValidator::Validate InPlaceEdit),
    // falling back to modified-MD5 container checksum for SM 5.0 DXBC or when dxil.dll is unavailable.
    if (!has_dxil_chunk ||
        !SignDxilContainerInPlace(out_result.pc_bytecode.data(), out_result.pc_bytecode.size())) {
        UpdateDxbcContainerChecksum(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
    }
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
