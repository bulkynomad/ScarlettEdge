#include "scarlettx/gpu/shader_ir.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <d3dcompiler.h>
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

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info) {
    out_info = ShaderModuleInfo{};
    out_info.byte_size = static_cast<uint32_t>(bytecode.size());
    out_info.content_hash = HashShaderBytes(bytecode.data(), bytecode.size());

    if (bytecode.size() >= 4 && std::memcmp(bytecode.data(), "XS2\0", 4) == 0) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
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

Status TranslateXboxShaderToPc(
    const std::vector<uint8_t>& xbox_bytecode,
    ShaderTranslationResult& out_result) {
    out_result = ShaderTranslationResult{};
    Status st = InspectShaderContainer(xbox_bytecode, out_result.source_info);
    if (!st.ok()) {
        return st;
    }

    if (out_result.source_info.kind == ShaderContainerKind::ScarlettRdna2) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Precompiled Scarlett RDNA2 (XS2) shader lifting to DXIL is not yet implemented");
    }

    if (!out_result.source_info.has_xbox_private_chunks) {
        out_result.pc_bytecode = xbox_bytecode;
        out_result.translated_hash = out_result.source_info.content_hash;
        out_result.stripped_xbox_chunks = 0;
        return Status::Ok();
    }

    // Rebuild DXBC container without Xbox-private chunks while preserving all standard chunks.
    const uint32_t orig_count = ReadU32(xbox_bytecode.data() + 28);
    struct ChunkSlice {
        const uint8_t* ptr;
        size_t total_len;
    };
    std::vector<ChunkSlice> kept;
    kept.reserve(orig_count);

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
            ++out_result.stripped_xbox_chunks;
            continue;
        }
        kept.push_back({xbox_bytecode.data() + off, total_len});
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
