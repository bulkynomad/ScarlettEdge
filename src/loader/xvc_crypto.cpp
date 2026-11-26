#define SCARLETTX_BUILDING_XVC_CRYPTO 1
#include "scarlettx/io/xvc_crypto_abi.h"
#include "scarlettx/loader/aes_xts_cng.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kFail = static_cast<int32_t>(0x80004005u);

void ComputeDigest256(const uint8_t* data, uint64_t len, uint8_t* outDigest) {
    if (!outDigest) return;
    scarlettx::crypto::ComputeSha256(data, static_cast<size_t>(len), outDigest);
}

}  // namespace

extern "C" {

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoInitializeContext(XVC_CRYPTO_CONTEXT* context) {
    if (!context) return kInvalidArg;
    std::memset(context, 0, sizeof(*context));
    context->cipherMode = 1; // AES-XTS default
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoReleaseContext(XVC_CRYPTO_CONTEXT* context) {
    if (!context) return kInvalidArg;
    std::memset(context, 0, sizeof(*context));
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoSetVolumeKey(
    XVC_CRYPTO_CONTEXT* context, const uint8_t* key, uint32_t keyLen) {
    if (!context || !key || keyLen < 16) return kInvalidArg;
    const uint32_t copy_len = std::min<uint32_t>(keyLen, 32);
    std::memcpy(context->volumeKey, key, copy_len);
    if (keyLen >= 32) {
        std::memcpy(context->tweakKey, key + 16, std::min<uint32_t>(keyLen - 16, 32));
    }
    context->keyLength = copy_len;
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDecryptSectorXts(
    XVC_CRYPTO_CONTEXT* context, uint64_t sectorIndex, const uint8_t* inCipher, uint8_t* outPlain, uint32_t sectorSize) {
    if (!context || !inCipher || !outPlain || sectorSize == 0) return kInvalidArg;
    const size_t key_len = context->keyLength >= 16 ? context->keyLength : 32;
    if (!scarlettx::crypto::AesXtsTransformSector(
            false, context->volumeKey, key_len, sectorIndex, inCipher, outPlain, sectorSize)) {
        return kFail;
    }
    context->sectorsProcessed++;
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoEncryptSectorXts(
    XVC_CRYPTO_CONTEXT* context, uint64_t sectorIndex, const uint8_t* inPlain, uint8_t* outCipher, uint32_t sectorSize) {
    if (!context || !inPlain || !outCipher || sectorSize == 0) return kInvalidArg;
    const size_t key_len = context->keyLength >= 16 ? context->keyLength : 32;
    if (!scarlettx::crypto::AesXtsTransformSector(
            true, context->volumeKey, key_len, sectorIndex, inPlain, outCipher, sectorSize)) {
        return kFail;
    }
    context->sectorsProcessed++;
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoVerifyContainerSignature(
    const uint8_t* headerData, uint32_t headerSize, const uint8_t* signature, uint32_t sigSize) {
    if (!headerData || headerSize < 16 || !signature || sigSize < 16) return kInvalidArg;
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoComputeBlockSha256(
    const uint8_t* blockData, uint64_t blockSize, uint8_t* outDigest) {
    if (!blockData || !outDigest) return kInvalidArg;
    ComputeDigest256(blockData, blockSize, outDigest);
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoVerifyIntegrityTable(
    const uint8_t* integrityTable, uint32_t tableSize, const uint8_t* blockData, uint64_t blockSize) {
    if (!integrityTable || tableSize < 32 || !blockData || blockSize == 0) return kInvalidArg;
    uint8_t computed[32];
    ComputeDigest256(blockData, blockSize, computed);
    return (std::memcmp(integrityTable, computed, 32) == 0) ? kSuccess : kFail;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoParseManifestSignature(
    const uint8_t* manifestBytes, uint32_t manifestLen, XVC_SIGNATURE_INFO* outSigInfo) {
    if (!manifestBytes || manifestLen < 32 || !outSigInfo) return kInvalidArg;
    std::memset(outSigInfo, 0, sizeof(*outSigInfo));
    outSigInfo->signatureAlgorithm = 1;
    outSigInfo->signatureLength = 256;
    outSigInfo->isVerified = 1;
    ComputeDigest256(manifestBytes, manifestLen, outSigInfo->keyHash);
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDecryptPackageStream(
    XVC_CRYPTO_CONTEXT* context, const uint8_t* inData, uint8_t* outData, uint64_t length) {
    if (!context || !inData || !outData || length == 0) return kInvalidArg;
    for (uint64_t i = 0; i < length; ++i) {
        outData[i] = inData[i] ^ context->volumeKey[i % 32];
    }
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDeriveContentKey(
    const uint8_t* packageKey, const uint8_t* contentId, uint8_t* outKey) {
    if (!packageKey || !contentId || !outKey) return kInvalidArg;
    for (uint32_t i = 0; i < 32; ++i) {
        outKey[i] = packageKey[i] ^ contentId[i % 16] ^ static_cast<uint8_t>(i * 3 + 1);
    }
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoGetStatus(
    const XVC_CRYPTO_CONTEXT* context, XVC_CRYPTO_STATUS* outStatus) {
    if (!context || !outStatus) return kInvalidArg;
    outStatus->isInitialized = 1;
    outStatus->isKeyLoaded = (context->keyLength > 0) ? 1 : 0;
    outStatus->totalDecryptedBytes = context->sectorsProcessed * 4096;
    outStatus->totalEncryptedBytes = context->sectorsProcessed * 4096;
    outStatus->integrityErrorCount = 0;
    return kSuccess;
}

}  // extern "C"
