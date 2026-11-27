#pragma once

#include <stddef.h>
#include <cstdint>

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_XG_XS)
#define SCARLETTX_XG_API __declspec(dllexport)
#else
#define SCARLETTX_XG_API __declspec(dllimport)
#endif
#define SCARLETTX_XG_CALL __stdcall
#else
#define SCARLETTX_XG_API __attribute__((visibility("default")))
#define SCARLETTX_XG_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Verified Xbox Series X|S xg_xs.dll swizzle/tile mode constants.
typedef enum XG_SWIZZLE_MODE {
    XG_SWIZZLE_MODE_LINEAR = 0,
    XG_SWIZZLE_MODE_256B_S = 1,
    XG_SWIZZLE_MODE_256B_D = 2,
    XG_SWIZZLE_MODE_256B_R = 3,
    XG_SWIZZLE_MODE_4KB_Z = 4,
    XG_SWIZZLE_MODE_4KB_S = 5,
    XG_SWIZZLE_MODE_4KB_D = 6,
    XG_SWIZZLE_MODE_4KB_R = 7,
    XG_SWIZZLE_MODE_64KB_Z = 8,
    XG_SWIZZLE_MODE_64KB_S = 9,
    XG_SWIZZLE_MODE_64KB_D = 10,
    XG_SWIZZLE_MODE_64KB_R = 11,
    XG_SWIZZLE_MODE_64KB_Z_T = 16,
    XG_SWIZZLE_MODE_64KB_S_T = 17,
    XG_SWIZZLE_MODE_64KB_D_T = 18,
    XG_SWIZZLE_MODE_64KB_R_T = 19,
    XG_SWIZZLE_MODE_4KB_Z_X = 20,
    XG_SWIZZLE_MODE_4KB_S_X = 21,
    XG_SWIZZLE_MODE_4KB_D_X = 22,
    XG_SWIZZLE_MODE_4KB_R_X = 23,
    XG_SWIZZLE_MODE_64KB_Z_X = 24,
    XG_SWIZZLE_MODE_64KB_S_X = 25,
    XG_SWIZZLE_MODE_64KB_D_X = 26,
    XG_SWIZZLE_MODE_64KB_R_X = 27
} XG_SWIZZLE_MODE;

typedef struct XG_SAMPLE_DESC {
    uint32_t Count;
    uint32_t Quality;
} XG_SAMPLE_DESC;

// Layouts below follow what the title passes to XGCreateTexture2DComputer and
// reads back from the layout query. Fields whose meaning is not established
// are named Reserved*.
typedef struct XG_TEXTURE2D_DESC {
    uint32_t Width;
    uint32_t Height;
    uint32_t MipLevels;
    uint32_t ArraySize;
    uint32_t Format;
    XG_SAMPLE_DESC SampleDesc;
    uint32_t Usage;
    uint32_t BindFlags;
    uint32_t CPUAccessFlags;
    uint32_t MiscFlags;
    uint32_t SwizzleMode;  // +0x2C: result of XGComputeOptimalSwizzleMode
    uint32_t Pitch;
} XG_TEXTURE2D_DESC;

typedef struct XG_TEXTURE3D_DESC {
    uint32_t Width;
    uint32_t Height;
    uint32_t Depth;
    uint32_t MipLevels;
    uint32_t Format;
    uint32_t Usage;
    uint32_t BindFlags;
    uint32_t CPUAccessFlags;
    uint32_t MiscFlags;
    uint32_t SwizzleMode;
} XG_TEXTURE3D_DESC;

typedef struct XG_MIPLEVEL_LAYOUT {
    uint64_t SizeBytes;               // +0x00
    uint64_t OffsetBytes;             // +0x08
    uint64_t Slice2DSizeBytes;        // +0x10
    uint32_t PitchPixels;             // +0x18
    uint32_t PitchBytes;              // +0x1C
    uint32_t AlignmentBytes;          // +0x20
    uint32_t PaddedWidthElements;     // +0x24
    uint32_t PaddedHeightElements;    // +0x28
    uint32_t PaddedDepthOrArraySize;  // +0x2C
    uint32_t WidthElements;           // +0x30
    uint32_t HeightElements;          // +0x34
    uint32_t ReservedWidth;           // +0x38: the title reads this as the mip width
    uint32_t ReservedHeight;          // +0x3C: the title reads this as the mip height
    uint32_t DepthOrArraySize;        // +0x40
    uint32_t SampleCount;             // +0x44
    uint32_t SwizzleMode;             // +0x48
    uint32_t Reserved4C[5];           // +0x4C
    uint32_t Reserved60;              // +0x60: 1 in every layout observed
    uint32_t Reserved64;              // +0x64: per-mip flag tested by the title, 0 when observed
    uint64_t Reserved68;              // +0x68
} XG_MIPLEVEL_LAYOUT;

typedef struct XG_PLANE_LAYOUT {
    uint32_t Usage;               // +0x00
    uint64_t SizeBytes;           // +0x08
    uint64_t Reserved10;          // +0x10: equal to SizeBytes when observed
    uint64_t BaseOffsetBytes;     // +0x18
    uint64_t BaseAlignmentBytes;  // +0x20
    uint32_t BytesPerElement;     // +0x28
    uint32_t Reserved2C;          // +0x2C: mip index the title switches to when Reserved64 is set
    uint32_t Reserved30;          // +0x30
    XG_MIPLEVEL_LAYOUT MipLayout[15];  // +0x38
} XG_PLANE_LAYOUT;

typedef struct XG_RESOURCE_LAYOUT {
    uint64_t SizeBytes;
    uint64_t BaseAlignmentBytes;
    uint32_t MipLevels;
    uint32_t Planes;
    XG_PLANE_LAYOUT Plane[4];  // +0x18
    uint32_t Dimension;
} XG_RESOURCE_LAYOUT;

#ifdef __cplusplus
static_assert(sizeof(XG_MIPLEVEL_LAYOUT) == 0x70, "XG_MIPLEVEL_LAYOUT is 0x70 bytes");
static_assert(offsetof(XG_MIPLEVEL_LAYOUT, ReservedWidth) == 0x38, "mip width offset");
static_assert(offsetof(XG_MIPLEVEL_LAYOUT, Reserved64) == 0x64, "mip flag offset");
static_assert(offsetof(XG_PLANE_LAYOUT, MipLayout) == 0x38, "plane header is 0x38 bytes");
static_assert(offsetof(XG_RESOURCE_LAYOUT, Plane) == 0x18, "resource header is 0x18 bytes");
static_assert(offsetof(XG_TEXTURE2D_DESC, SwizzleMode) == 0x2C, "swizzle mode offset");
#endif

// Verified exports from EmbeddedXvd/Windows/System32/xg_xs.dll
SCARLETTX_XG_API uint32_t SCARLETTX_XG_CALL XGComputeOptimalSwizzleMode(
    uint32_t resourceDimension,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t depthOrArraySize,
    uint32_t sampleCount,
    uint32_t bindFlags,
    uint32_t reserved);  // 8th argument, 0 at every call seen

SCARLETTX_XG_API int32_t SCARLETTX_XG_CALL XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* desc,
    void** computer);

SCARLETTX_XG_API int32_t SCARLETTX_XG_CALL XGCreateTexture3DComputer(
    const XG_TEXTURE3D_DESC* desc,
    void** computer);

// RDNA2 DCC (Delta Color Compression) 8x8 tile sector control codes.
typedef enum XG_DCC_KEY_CODE {
    XG_DCC_KEY_CLEAR_0000   = 0x00,  // Fast-clear RGBA8 = (0, 0, 0, 0)
    XG_DCC_KEY_CLEAR_0001   = 0x20,  // Fast-clear RGBA8 = (0, 0, 0, 255)
    XG_DCC_KEY_SOLID_COLOR  = 0x40,  // Uniform arbitrary RGBA8 color (4B payload)
    XG_DCC_KEY_CLEAR_1111   = 0x80,  // Fast-clear RGBA8 = (255, 255, 255, 255)
    XG_DCC_KEY_DELTA_4BIT   = 0xC0,  // Base RGBA8 (4B) + 64x4x4-bit unsigned delta (128B)
    XG_DCC_KEY_UNCOMPRESSED = 0xFF   // Raw 8x8 RGBA8 sector (256B payload)
} XG_DCC_KEY_CODE;

typedef struct XG_DCC_STATS {
    uint32_t TileCount;
    uint32_t FastClearTiles;
    uint32_t SolidColorTiles;
    uint32_t DeltaTiles;
    uint32_t UncompressedTiles;
    uint64_t CompressedPayloadBytes;
} XG_DCC_STATS;

// RDNA2 HTILE 8x8 hierarchical depth 32-bit word:
//   bits [31:18] = ZMax (14-bit UNORM), bits [17:4] = ZMin (14-bit UNORM), bits [3:0] = ZMask
#define XG_HTILE_ZMASK_CLEAR_ZMAX 0x0u
#define XG_HTILE_ZMASK_CONST_ZMIN 0x1u
#define XG_HTILE_ZMASK_EXPANDED   0xFu

SCARLETTX_XG_API int32_t SCARLETTX_XG_CALL XGCompressDccSurface(
    const uint8_t* srcRgba8,
    uint32_t width,
    uint32_t height,
    uint32_t srcRowPitch,
    uint8_t* outMetaKeys,
    uint32_t metaCapacity,
    uint8_t* outPayload,
    uint64_t payloadCapacity,
    uint64_t* outPayloadBytes,
    XG_DCC_STATS* outStats);

SCARLETTX_XG_API int32_t SCARLETTX_XG_CALL XGDecompressDccSurface(
    const uint8_t* metaKeys,
    uint32_t metaSize,
    const uint8_t* payload,
    uint64_t payloadSize,
    uint8_t* dstRgba8,
    uint32_t width,
    uint32_t height,
    uint32_t dstRowPitch);

SCARLETTX_XG_API int32_t SCARLETTX_XG_CALL XGBuildHtileMetadata(
    const float* srcDepth,
    uint32_t width,
    uint32_t height,
    uint32_t srcDepthStrideFloats,
    uint32_t* outHtileWords,
    uint32_t htileCapacityWords,
    float* outExpandedTiles,
    uint32_t expandedCapacityFloats,
    uint32_t* outExpandedFloatsWritten);

SCARLETTX_XG_API int32_t SCARLETTX_XG_CALL XGDecompressHtileDepth(
    const uint32_t* htileWords,
    uint32_t htileWordCount,
    const float* expandedTiles,
    uint32_t expandedFloatCount,
    float* dstDepth,
    uint32_t width,
    uint32_t height,
    uint32_t dstDepthStrideFloats);

#ifdef __cplusplus
}
#endif
