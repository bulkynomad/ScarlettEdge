#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_XVC_CRYPTO)
#    define SCARLETTX_XVC_CRYPTO_API __declspec(dllexport)
#  else
#    define SCARLETTX_XVC_CRYPTO_API __declspec(dllimport)
#  endif
#  define SCARLETTX_XVC_CRYPTO_CALL __stdcall
#else
#  define SCARLETTX_XVC_CRYPTO_API __attribute__((visibility("default")))
#  define SCARLETTX_XVC_CRYPTO_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XVC_CRYPTO_CONTEXT {
    uint8_t volumeKey[32];
    uint8_t tweakKey[32];
    uint32_t keyLength;
    uint32_t cipherMode; // 1 = AES-XTS, 2 = AES-CBC
    uint64_t sectorsProcessed;
    void* internalState;
} XVC_CRYPTO_CONTEXT;

typedef struct XVC_SIGNATURE_INFO {
    uint32_t signatureAlgorithm; // 1 = RSA-PSS-2048, 2 = ECDSA-P384
    uint8_t keyHash[32];
    uint32_t signatureLength;
    uint8_t signatureData[512];
    uint8_t isVerified;
} XVC_SIGNATURE_INFO;

typedef struct XVC_CRYPTO_STATUS {
    uint32_t isInitialized;
    uint32_t isKeyLoaded;
    uint64_t totalDecryptedBytes;
    uint64_t totalEncryptedBytes;
    uint32_t integrityErrorCount;
} XVC_CRYPTO_STATUS;

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoInitializeContext(
    XVC_CRYPTO_CONTEXT* context);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoReleaseContext(
    XVC_CRYPTO_CONTEXT* context);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoSetVolumeKey(
    XVC_CRYPTO_CONTEXT* context, const uint8_t* key, uint32_t keyLen);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDecryptSectorXts(
    XVC_CRYPTO_CONTEXT* context, uint64_t sectorIndex, const uint8_t* inCipher, uint8_t* outPlain, uint32_t sectorSize);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoEncryptSectorXts(
    XVC_CRYPTO_CONTEXT* context, uint64_t sectorIndex, const uint8_t* inPlain, uint8_t* outCipher, uint32_t sectorSize);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoVerifyContainerSignature(
    const uint8_t* headerData, uint32_t headerSize, const uint8_t* signature, uint32_t sigSize);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoComputeBlockSha256(
    const uint8_t* blockData, uint64_t blockSize, uint8_t* outDigest);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoVerifyIntegrityTable(
    const uint8_t* integrityTable, uint32_t tableSize, const uint8_t* blockData, uint64_t blockSize);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoParseManifestSignature(
    const uint8_t* manifestBytes, uint32_t manifestLen, XVC_SIGNATURE_INFO* outSigInfo);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDecryptPackageStream(
    XVC_CRYPTO_CONTEXT* context, const uint8_t* inData, uint8_t* outData, uint64_t length);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDeriveContentKey(
    const uint8_t* packageKey, const uint8_t* contentId, uint8_t* outKey);

SCARLETTX_XVC_CRYPTO_API int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoGetStatus(
    const XVC_CRYPTO_CONTEXT* context, XVC_CRYPTO_STATUS* outStatus);

#ifdef __cplusplus
}
#endif
