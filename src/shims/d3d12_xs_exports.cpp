#include <cstdint>

#include "scarlettx/gpu/d3d12_xs_abi.h"

extern "C" {

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12CreateDevice(
    void* pAdapter,
    uint32_t minimumFeatureLevel,
    const void* riid,
    void** ppDevice) {
    return D3D12XsCreateDevice(pAdapter, minimumFeatureLevel, riid, ppDevice);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12GetDebugInterface(
    const void* riid,
    void** ppvDebug) {
    return D3D12XsGetDebugInterface(riid, ppvDebug);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12SerializeRootSignature(
    const void* pRootSignature,
    uint32_t version,
    void** ppBlob,
    void** ppErrorBlob) {
    return D3D12XsSerializeRootSignature(pRootSignature, version, ppBlob, ppErrorBlob);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12SerializeVersionedRootSignature(
    const void* pRootSignature,
    void** ppBlob,
    void** ppErrorBlob) {
    return D3D12XsSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12CreateRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    return D3D12XsCreateRootSignatureDeserializer(
        pSrcData, srcDataSizeInBytes, pRootSignatureDeserializerInterface,
        ppRootSignatureDeserializer);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12CreateVersionedRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    return D3D12XsCreateVersionedRootSignatureDeserializer(
        pSrcData, srcDataSizeInBytes, pRootSignatureDeserializerInterface,
        ppRootSignatureDeserializer);
}

// The console reserves address space for its graphics driver here. The host
// driver manages its own, so a valid request succeeds with nothing reserved.
// 0x10000000 is what the console returns for a successful request.
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12ConfigureVirtualMemoryX(
    D3D12XS_VIRTUAL_MEMORY_CONFIG* pConfig) {
    if (!pConfig || (pConfig->Flags & ~1u) != 0 || pConfig->SizeIn4MbUnits > 33) {
        return static_cast<int32_t>(0x80070057u);
    }
    pConfig->Result0 = 0;
    pConfig->Result1 = 0;
    return 0x10000000;
}

// Data export: titles import the interface id itself. Same value as on PC.
struct ExportedGuid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};
SCARLETTX_D3D12_API extern const ExportedGuid IID_ID3D12Device;
const ExportedGuid IID_ID3D12Device = {0x189819f1u, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

}  // extern "C"
