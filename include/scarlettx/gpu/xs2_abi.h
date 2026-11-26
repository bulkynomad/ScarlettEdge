#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_XS2)
#    define SCARLETTX_XS2_API __declspec(dllexport)
#  else
#    define SCARLETTX_XS2_API __declspec(dllimport)
#  endif
#  define SCARLETTX_XS2_CALL __stdcall
#else
#  define SCARLETTX_XS2_API __attribute__((visibility("default")))
#  define SCARLETTX_XS2_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* XS2_PIPELINE_HANDLE;

typedef struct XS2_MICROCODE_INFO {
    uint32_t stage;               // 1 = Vertex, 2 = Pixel, 3 = Compute, etc.
    uint32_t instructionCount;
    uint32_t vgprCount;
    uint32_t sgprCount;
    uint32_t ldsBytes;
    uint64_t shaderHash;
} XS2_MICROCODE_INFO;

typedef struct XS2_TRANSLATION_OPTIONS {
    uint32_t targetShaderModel;   // e.g. 0x00060005 for SM 6.5
    uint32_t optimizeLevel;       // 0 to 3
    uint32_t enableWave32;        // RDNA2 Wave32 mode
    uint32_t enableSubgroupOps;
} XS2_TRANSLATION_OPTIONS;

typedef struct XS2_RESOURCE_BINDINGS {
    uint32_t srvCount;
    uint32_t uavCount;
    uint32_t cbvCount;
    uint32_t samplerCount;
} XS2_RESOURCE_BINDINGS;

typedef struct XS2_REGISTER_REMAP {
    uint32_t oldRegister;
    uint32_t newRegister;
    uint32_t registerSpace;
} XS2_REGISTER_REMAP;

typedef struct XS2_WAVEFRONT_PROPS {
    uint32_t waveSize;            // 32 or 64
    uint32_t maxWavesPerComputeUnit;
    uint32_t simdWidth;
} XS2_WAVEFRONT_PROPS;

typedef struct XS2_PIPELINE_DESC {
    uint32_t flags;
    uint32_t maxCacheEntries;
} XS2_PIPELINE_DESC;

typedef struct XS2_TRANSLATION_STATS {
    uint64_t totalShadersTranslated;
    uint64_t cacheHits;
    uint64_t cacheMisses;
    uint64_t totalTranslationTimeUs;
} XS2_TRANSLATION_STATS;

// Microcode analysis & disassembly
SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderIdentifyMicrocode(
    const void* binaryData, size_t byteSize, XS2_MICROCODE_INFO* outInfo);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderDisassembleRdna2(
    const void* microcode, size_t byteSize, char* outAsm, size_t maxAsmLen, size_t* actualLen);

// Lifting to PC intermediate representations
SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderLiftToDxil(
    const void* microcode, size_t byteSize, const XS2_TRANSLATION_OPTIONS* options, void** outDxilBytecode, size_t* outDxilSize);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderLiftToSpirv(
    const void* microcode, size_t byteSize, const XS2_TRANSLATION_OPTIONS* options, void** outSpirvBytecode, size_t* outSpirvSize);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderFreeBytecode(void* bytecodeBuffer);

// Resource bindings & container transformations
SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderExtractResourceBindings(
    const void* microcode, size_t byteSize, XS2_RESOURCE_BINDINGS* outBindings);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderStripXboxVendorChunks(
    const void* inContainer, size_t inSize, void** outContainer, size_t* outSize);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderPatchRootSignatureRegisters(
    void* dxilBytecode, size_t byteSize, const XS2_REGISTER_REMAP* remapTable, uint32_t remapCount);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderValidateDxilContainer(
    const void* dxilData, size_t byteSize, int32_t* outIsValid);

// Wavefront & execution properties
SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderQueryWavefrontProperties(
    const void* microcode, size_t byteSize, XS2_WAVEFRONT_PROPS* outProps);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderTranslateComputeWorkgroupSize(
    const void* microcode, size_t byteSize, uint32_t* outX, uint32_t* outY, uint32_t* outZ);

// Translation pipeline & caching
SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderCreateTranslationPipeline(
    const XS2_PIPELINE_DESC* desc, XS2_PIPELINE_HANDLE* outPipeline);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderDestroyTranslationPipeline(
    XS2_PIPELINE_HANDLE pipeline);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderTranslateCached(
    XS2_PIPELINE_HANDLE pipeline, const void* microcode, size_t byteSize, void** outBytecode, size_t* outBytecodeSize);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderClearPipelineCache(
    XS2_PIPELINE_HANDLE pipeline);

SCARLETTX_XS2_API int32_t SCARLETTX_XS2_CALL Xs2ShaderGetTranslationStatistics(
    XS2_PIPELINE_HANDLE pipeline, XS2_TRANSLATION_STATS* outStats);

#ifdef __cplusplus
}
#endif
