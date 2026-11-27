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
    if (context->internalState) {
        delete static_cast<scarlettx::crypto::AesXtsPreparedKey*>(context->internalState);
        context->internalState = nullptr;
    }
    std::memset(context, 0, sizeof(*context));
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoSetVolumeKey(
    XVC_CRYPTO_CONTEXT* context, const uint8_t* key, uint32_t keyLen) {
    if (!context || !key || keyLen < 16) return kInvalidArg;
    if (context->internalState) {
        delete static_cast<scarlettx::crypto::AesXtsPreparedKey*>(context->internalState);
        context->internalState = nullptr;
    }
    std::memset(context->volumeKey, 0, sizeof(context->volumeKey));
    std::memset(context->tweakKey, 0, sizeof(context->tweakKey));
    const uint32_t capped = std::min<uint32_t>(keyLen, 64u);
    if (capped >= 64u) {
        std::memcpy(context->volumeKey, key, 32);
        std::memcpy(context->tweakKey, key + 32, 32);
    } else if (capped >= 32u) {
        std::memcpy(context->volumeKey, key, 32);
        std::memcpy(context->tweakKey, key + 16, 16);
    } else {
        std::memcpy(context->volumeKey, key, capped);
        std::memcpy(context->tweakKey, key, capped);
    }
    context->keyLength = capped;
    auto* prep = new scarlettx::crypto::AesXtsPreparedKey();
    if (!prep->Init(key, capped)) {
        delete prep;
        return kFail;
    }
    context->internalState = prep;
    return kSuccess;
}

static const scarlettx::crypto::AesXtsPreparedKey* EnsurePreparedKey(XVC_CRYPTO_CONTEXT* context) {
    if (!context) return nullptr;
    if (context->internalState) {
        return static_cast<const scarlettx::crypto::AesXtsPreparedKey*>(context->internalState);
    }
    uint8_t combined[64]{};
    const size_t key_len = context->keyLength >= 16 ? std::min<uint32_t>(context->keyLength, 64u) : 32u;
    if (key_len >= 64u) {
        std::memcpy(combined, context->volumeKey, 32);
        std::memcpy(combined + 32, context->tweakKey, 32);
    } else {
        std::memcpy(combined, context->volumeKey, 32);
    }
    auto* prep = new scarlettx::crypto::AesXtsPreparedKey();
    if (!prep->Init(combined, key_len)) {
        delete prep;
        return nullptr;
    }
    context->internalState = prep;
    return prep;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDecryptSectorXts(
    XVC_CRYPTO_CONTEXT* context, uint64_t sectorIndex, const uint8_t* inCipher, uint8_t* outPlain, uint32_t sectorSize) {
    if (!context || !inCipher || !outPlain || sectorSize == 0) return kInvalidArg;
    const auto* prep = EnsurePreparedKey(context);
    if (!prep || !prep->TransformSector(false, sectorIndex, inCipher, outPlain, sectorSize)) {
        return kFail;
    }
    context->sectorsProcessed++;
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoEncryptSectorXts(
    XVC_CRYPTO_CONTEXT* context, uint64_t sectorIndex, const uint8_t* inPlain, uint8_t* outCipher, uint32_t sectorSize) {
    if (!context || !inPlain || !outCipher || sectorSize == 0) return kInvalidArg;
    const auto* prep = EnsurePreparedKey(context);
    if (!prep || !prep->TransformSector(true, sectorIndex, inPlain, outCipher, sectorSize)) {
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
    const auto* prep = EnsurePreparedKey(context);
    if (!prep || !prep->TransformStreamCtr(0, inData, outData, static_cast<size_t>(length))) {
        return kFail;
    }
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDeriveContentKey(
    const uint8_t* packageKey, const uint8_t* contentId, uint8_t* outKey) {
    if (!packageKey || !contentId || !outKey) return kInvalidArg;
    uint8_t material[48]{};
    std::memcpy(material, packageKey, 32);
    std::memcpy(material + 32, contentId, 16);
    ComputeDigest256(material, sizeof(material), outKey);
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
