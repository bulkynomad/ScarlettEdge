#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

enum class ShaderContainerKind : uint32_t {
    Unknown = 0,
    Dxbc = 1,
    Dxil = 2,
    ScarlettRdna2 = 3,
};

enum class ShaderStage : uint32_t {
    Unknown = 0,
    Vertex = 1,
    Pixel = 2,
    Compute = 3,
    Geometry = 4,
    Hull = 5,
    Domain = 6,
    Library = 7,  // DXR raytracing library (lib_6_3+)
};

struct ShaderModuleInfo {
    ShaderContainerKind kind = ShaderContainerKind::Unknown;
    ShaderStage stage = ShaderStage::Unknown;
    uint32_t byte_size = 0;
    uint32_t chunk_count = 0;
    uint32_t input_param_count = 0;
    uint32_t output_param_count = 0;
    uint32_t shader_model_major = 0;
    uint32_t shader_model_minor = 0;
    uint64_t content_hash = 0;
    bool has_raytracing_exports = false;
    bool has_embedded_root_signature = false;
    bool has_xbox_private_chunks = false;
    std::vector<std::string> chunk_fourccs;
};

struct ShaderTranslationResult {
    ShaderModuleInfo source_info;
    std::vector<uint8_t> pc_bytecode;
    uint64_t translated_hash = 0;
    uint32_t stripped_xbox_chunks = 0;
    bool lifted_from_rdna2 = false;
};

uint64_t HashShaderBytes(const uint8_t* data, size_t size);

// Computes and writes the retail 16-byte DXBC modified-MD5 checksum at bytes 4..19
// of a DXBC/DXIL container (hashing bytes 20..size). Returns false if size < 32.
bool UpdateDxbcContainerChecksum(uint8_t* dxbc_data, size_t total_size);

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info);

// Lifts raw Scarlett RDNA2 ISA microcode bytes into a genuine retail-signed PC DXIL (SM 6.x
// via dxcompiler.dll + dxil.dll when available) or DXBC (SM 5.0 via D3DCompile fallback)
// container accepted natively by Windows ID3D12Device::Create*PipelineState.
Status LiftRdna2MicrocodeToPcShader(
    const uint8_t* microcode,
    size_t byte_size,
    ShaderStage stage_override,
    uint32_t target_shader_model,
    bool enable_wave32,
    std::vector<uint8_t>& out_pc_dxbc,
    ShaderStage* out_detected_stage = nullptr);

// Translates a Scarlett/Xbox DXBC or DXIL container into a PC-compatible DXBC/DXIL
// container by stripping Xbox-private chunks (XBOX, XS2M, XDBG) and rebuilding the
// DXBC chunk offset table. For precompiled RDNA2 (XS2\0) binaries, returns
// UnsupportedFormat until an RDNA2-to-DXIL lifter is implemented.
Status TranslateXboxShaderToPc(
    const std::vector<uint8_t>& xbox_bytecode,
    ShaderTranslationResult& out_result);

// Compiles HLSL source to DXBC bytecode using d3dcompiler (Windows).
// target examples: "vs_5_0", "ps_5_0", "cs_5_0".
Status CompileHlslToDxbc(
    const std::string& hlsl_source,
    const std::string& entry_point,
    const std::string& target,
    std::vector<uint8_t>& out_bytecode);

// Thread-safe in-memory shader/PSO blob cache keyed by 64-bit hash.
class ShaderCache {
public:
    bool Lookup(uint64_t key, std::vector<uint8_t>& out_blob);
    void Store(uint64_t key, std::vector<uint8_t> blob);
    void Clear();

    uint64_t hits() const;
    uint64_t misses() const;
    size_t entry_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> entries_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};

}  // namespace scarlettx
