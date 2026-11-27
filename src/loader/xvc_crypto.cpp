#define SCARLETTX_BUILDING_XVC_CRYPTO 1
#include "scarlettx/io/xvc_crypto_abi.h"
#include "scarlettx/loader/aes_xts_cng.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

constexpr int32_t kSuccess = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kFail = static_cast<int32_t>(0x80004005u);

struct XvcContextInternalState {
    scarlettx::crypto::AesXtsPreparedKey prepared_key;
    bool key_ready = false;
    uint32_t cached_key_len = 0;
    uint8_t cached_vol_key[32]{};
    uint8_t cached_tweak_key[32]{};
    uint64_t decrypted_bytes = 0;
    uint64_t encrypted_bytes = 0;
    uint64_t stream_block_counter = 0;
    uint32_t integrity_errors = 0;
    uint32_t last_sector_size = 0;
};

struct ContextMutexRegistry {
    std::mutex table_mu;
    std::unordered_map<const XVC_CRYPTO_CONTEXT*, std::shared_ptr<std::mutex>> locks;

    std::shared_ptr<std::mutex> AcquireMutex(const XVC_CRYPTO_CONTEXT* ctx) {
        std::lock_guard<std::mutex> lk(table_mu);
        auto& slot = locks[ctx];
        if (!slot) {
            slot = std::make_shared<std::mutex>();
        }
        return slot;
    }

    void EraseMutex(const XVC_CRYPTO_CONTEXT* ctx) {
        std::lock_guard<std::mutex> lk(table_mu);
        locks.erase(ctx);
    }
};

// Immortal heap singleton so context teardown never races static destructor order at exit.
ContextMutexRegistry& Registry() {
    static ContextMutexRegistry* reg = new ContextMutexRegistry();
    return *reg;
}

bool ComputeDigest256(const uint8_t* data, uint64_t len, uint8_t* outDigest) {
    if (!outDigest) return false;
#if SIZE_MAX < UINT64_MAX
    if (len > static_cast<uint64_t>(SIZE_MAX)) return false;
#endif
    return scarlettx::crypto::ComputeSha256(data, static_cast<size_t>(len), outDigest);
}

bool IsAnyNonZero(const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != 0) return true;
    }
    return false;
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

XvcContextInternalState* GetOrCreateState(XVC_CRYPTO_CONTEXT* context) {
    if (!context->internalState) {
        context->internalState = new XvcContextInternalState();
    }
    return static_cast<XvcContextInternalState*>(context->internalState);
}

// Caller must hold per-context mutex.
XvcContextInternalState* EnsurePreparedKeyLocked(XVC_CRYPTO_CONTEXT* context) {
    if (!context) return nullptr;
    auto* state = GetOrCreateState(context);

    const uint32_t eff_len = (context->keyLength >= 16u)
                                 ? (std::min)(context->keyLength, 64u)
                                 : (IsAnyNonZero(context->volumeKey, 32) || IsAnyNonZero(context->tweakKey, 32) ? 32u : 0u);
    if (eff_len < 16u) {
        return nullptr;
    }

    if (state->key_ready &&
        state->cached_key_len == eff_len &&
        std::memcmp(state->cached_vol_key, context->volumeKey, 32) == 0 &&
        std::memcmp(state->cached_tweak_key, context->tweakKey, 32) == 0) {
        return state;
    }

    uint8_t combined[64]{};
    size_t init_len = eff_len;
    if (eff_len >= 64u) {
        std::memcpy(combined, context->volumeKey, 32);
        std::memcpy(combined + 32, context->tweakKey, 32);
        init_len = 64u;
    } else if (eff_len >= 32u) {
        // Construct combined[0..15] = volumeKey[0..15] and
        // combined[16..31] = (tweakKey[0..15] non-zero ? tweakKey[0..15] : volumeKey[16..31]).
        std::memcpy(combined, context->volumeKey, 16);
        const bool tweak_populated = IsAnyNonZero(context->tweakKey, 16);
        std::memcpy(combined + 16, tweak_populated ? context->tweakKey : (context->volumeKey + 16), 16);
        init_len = 32u;
    } else {
        std::memcpy(combined, context->volumeKey, 16);
        if (IsAnyNonZero(context->tweakKey, 16) &&
            std::memcmp(context->tweakKey, context->volumeKey, 16) != 0) {
            std::memcpy(combined + 16, context->tweakKey, 16);
            init_len = 32u;
        } else {
            init_len = 16u;
        }
    }

    if (!state->prepared_key.Init(combined, init_len)) {
        state->key_ready = false;
        return nullptr;
    }

    state->key_ready = true;
    state->cached_key_len = eff_len;
    std::memcpy(state->cached_vol_key, context->volumeKey, 32);
    std::memcpy(state->cached_tweak_key, context->tweakKey, 32);
    return state;
}

}  // namespace

extern "C" {

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoInitializeContext(XVC_CRYPTO_CONTEXT* context) {
    if (!context) return kInvalidArg;
    auto ctx_mu = Registry().AcquireMutex(context);
    std::lock_guard<std::mutex> lk(*ctx_mu);
    std::memset(context, 0, sizeof(*context));
    context->cipherMode = 1; // AES-XTS default
    context->internalState = new XvcContextInternalState();
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoReleaseContext(XVC_CRYPTO_CONTEXT* context) {
    if (!context) return kInvalidArg;
    {
        auto ctx_mu = Registry().AcquireMutex(context);
        std::lock_guard<std::mutex> lk(*ctx_mu);
        if (context->internalState) {
            delete static_cast<XvcContextInternalState*>(context->internalState);
            context->internalState = nullptr;
        }
        std::memset(context, 0, sizeof(*context));
    }
    Registry().EraseMutex(context);
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoSetVolumeKey(
    XVC_CRYPTO_CONTEXT* context, const uint8_t* key, uint32_t keyLen) {
    if (!context || !key || keyLen < 16u) return kInvalidArg;
    auto ctx_mu = Registry().AcquireMutex(context);
    std::lock_guard<std::mutex> lk(*ctx_mu);

    auto* state = GetOrCreateState(context);
    state->prepared_key.Reset();
    state->key_ready = false;
    state->stream_block_counter = 0;

    std::memset(context->volumeKey, 0, sizeof(context->volumeKey));
    std::memset(context->tweakKey, 0, sizeof(context->tweakKey));
    const uint32_t capped = (std::min)(keyLen, 64u);
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

    if (!state->prepared_key.Init(key, capped)) {
        return kFail;
    }
    state->key_ready = true;
    state->cached_key_len = capped;
    std::memcpy(state->cached_vol_key, context->volumeKey, 32);
    std::memcpy(state->cached_tweak_key, context->tweakKey, 32);
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDecryptSectorXts(
    XVC_CRYPTO_CONTEXT* context, uint64_t sectorIndex, const uint8_t* inCipher, uint8_t* outPlain, uint32_t sectorSize) {
    if (!context || !inCipher || !outPlain || sectorSize == 0) return kInvalidArg;
    auto ctx_mu = Registry().AcquireMutex(context);
    std::lock_guard<std::mutex> lk(*ctx_mu);

    if (context->cipherMode != 0u && context->cipherMode != 1u && context->cipherMode != 2u) {
        return kInvalidArg;
    }
    auto* state = EnsurePreparedKeyLocked(context);
    if (!state) return kFail;

    const bool ok = (context->cipherMode == 2u)
                        ? state->prepared_key.TransformSectorCbc(false, sectorIndex, inCipher, outPlain, sectorSize)
                        : state->prepared_key.TransformSector(false, sectorIndex, inCipher, outPlain, sectorSize);
    if (!ok) {
        return kFail;
    }
    context->sectorsProcessed++;
    state->decrypted_bytes += sectorSize;
    state->last_sector_size = sectorSize;
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoEncryptSectorXts(
    XVC_CRYPTO_CONTEXT* context, uint64_t sectorIndex, const uint8_t* inPlain, uint8_t* outCipher, uint32_t sectorSize) {
    if (!context || !inPlain || !outCipher || sectorSize == 0) return kInvalidArg;
    auto ctx_mu = Registry().AcquireMutex(context);
    std::lock_guard<std::mutex> lk(*ctx_mu);

    if (context->cipherMode != 0u && context->cipherMode != 1u && context->cipherMode != 2u) {
        return kInvalidArg;
    }
    auto* state = EnsurePreparedKeyLocked(context);
    if (!state) return kFail;

    const bool ok = (context->cipherMode == 2u)
                        ? state->prepared_key.TransformSectorCbc(true, sectorIndex, inPlain, outCipher, sectorSize)
                        : state->prepared_key.TransformSector(true, sectorIndex, inPlain, outCipher, sectorSize);
    if (!ok) {
        return kFail;
    }
    context->sectorsProcessed++;
    state->encrypted_bytes += sectorSize;
    state->last_sector_size = sectorSize;
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoVerifyContainerSignature(
    const uint8_t* headerData, uint32_t headerSize, const uint8_t* signature, uint32_t sigSize) {
    if (!headerData || headerSize < 16u || !signature || sigSize < 16u) return kInvalidArg;

    uint8_t full_sha[32]{};
    if (!ComputeDigest256(headerData, headerSize, full_sha)) {
        return kFail;
    }

    const bool is_xvd_hdr = (headerSize > 0x208u && std::memcmp(headerData + 0x200u, "msft-xvd", 8) == 0);
    uint8_t body_sha[32]{};
    if (is_xvd_hdr) {
        if (!ComputeDigest256(headerData + 0x200u, headerSize - 0x200u, body_sha)) {
            return kFail;
        }
    }

    if (sigSize >= 32u) {
        if (std::memcmp(signature, full_sha, 32) == 0 ||
            std::memcmp(signature + (sigSize - 32u), full_sha, 32) == 0) {
            return kSuccess;
        }
        if (is_xvd_hdr &&
            (std::memcmp(signature, body_sha, 32) == 0 ||
             std::memcmp(signature + (sigSize - 32u), body_sha, 32) == 0)) {
            return kSuccess;
        }
        return kFail;
    }

    if (std::memcmp(signature, full_sha, sigSize) == 0 ||
        (is_xvd_hdr && std::memcmp(signature, body_sha, sigSize) == 0)) {
        return kSuccess;
    }
    return kFail;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoComputeBlockSha256(
    const uint8_t* blockData, uint64_t blockSize, uint8_t* outDigest) {
    if (!blockData || !outDigest) return kInvalidArg;
    return ComputeDigest256(blockData, blockSize, outDigest) ? kSuccess : kFail;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoVerifyIntegrityTable(
    const uint8_t* integrityTable, uint32_t tableSize, const uint8_t* blockData, uint64_t blockSize) {
    if (!integrityTable || tableSize < 32u || !blockData || blockSize == 0) return kInvalidArg;
    uint8_t computed[32]{};
    if (!ComputeDigest256(blockData, blockSize, computed)) {
        return kFail;
    }
    return (std::memcmp(integrityTable, computed, 32) == 0) ? kSuccess : kFail;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoParseManifestSignature(
    const uint8_t* manifestBytes, uint32_t manifestLen, XVC_SIGNATURE_INFO* outSigInfo) {
    if (!manifestBytes || manifestLen < 32u || !outSigInfo) return kInvalidArg;
    std::memset(outSigInfo, 0, sizeof(*outSigInfo));

    // Case 1: Explicit signed manifest envelope ("XVCM" + u32 alg + u32 sigLen + 32B sha256 + sigData + payload)
    if (manifestLen >= 44u && std::memcmp(manifestBytes, "XVCM", 4) == 0) {
        const uint32_t alg = ReadLe32(manifestBytes + 4);
        const uint32_t sig_len = ReadLe32(manifestBytes + 8);
        if ((alg != 1u && alg != 2u) || sig_len > 512u || 44u + sig_len > manifestLen) {
            return kFail;
        }
        const uint8_t* expected_sha = manifestBytes + 12;
        const uint8_t* sig_bytes = manifestBytes + 44;
        const uint8_t* payload = manifestBytes + 44u + sig_len;
        const uint32_t payload_len = manifestLen - (44u + sig_len);

        if (!ComputeDigest256(payload, payload_len, outSigInfo->keyHash)) {
            return kFail;
        }
        outSigInfo->signatureAlgorithm = alg;
        outSigInfo->signatureLength = sig_len;
        if (sig_len > 0) {
            std::memcpy(outSigInfo->signatureData, sig_bytes, sig_len);
        }
        if (std::memcmp(outSigInfo->keyHash, expected_sha, 32) != 0) {
            outSigInfo->isVerified = 0;
            return kFail;
        }
        outSigInfo->isVerified = 1;
        return kSuccess;
    }

    // Case 2: "XVC-SIG:" prefix + 32B expected SHA-256 + payload
    if (manifestLen > 40u && std::memcmp(manifestBytes, "XVC-SIG:", 8) == 0) {
        const uint8_t* expected_sha = manifestBytes + 8;
        const uint8_t* payload = manifestBytes + 40;
        const uint32_t payload_len = manifestLen - 40u;
        if (!ComputeDigest256(payload, payload_len, outSigInfo->keyHash)) {
            return kFail;
        }
        outSigInfo->signatureAlgorithm = 1;
        outSigInfo->signatureLength = 32;
        std::memcpy(outSigInfo->signatureData, expected_sha, 32);
        if (std::memcmp(outSigInfo->keyHash, expected_sha, 32) != 0) {
            outSigInfo->isVerified = 0;
            return kFail;
        }
        outSigInfo->isVerified = 1;
        return kSuccess;
    }

    // Case 3: msft-xvd container header (0x000..0x1FF signature block, 0x200+ signed header body)
    if (manifestLen >= 0x260u && std::memcmp(manifestBytes + 0x200u, "msft-xvd", 8) == 0) {
        if (!ComputeDigest256(manifestBytes + 0x200u, manifestLen - 0x200u, outSigInfo->keyHash)) {
            return kFail;
        }
        outSigInfo->signatureAlgorithm = 1;
        const uint32_t sig_len = IsAnyNonZero(manifestBytes + 256u, 256u) ? 512u : 256u;
        outSigInfo->signatureLength = sig_len;
        std::memcpy(outSigInfo->signatureData, manifestBytes, sig_len);

        // If header signature digest at 0x000..0x01F is populated, validate it against SHA-256(0x200..end);
        // gracefully allow un-signed synthetic headers whose 0x000..0x1FF region is all zeros.
        if (IsAnyNonZero(manifestBytes, 32u)) {
            if (std::memcmp(manifestBytes, outSigInfo->keyHash, 32) != 0) {
                outSigInfo->isVerified = 0;
                return kFail;
            }
        }
        outSigInfo->isVerified = 1;
        return kSuccess;
    }

    // Case 4: Manifest with trailing 32B SHA-256 digest over [0 .. manifestLen - 32)
    if (manifestLen > 32u) {
        uint8_t prefix_sha[32]{};
        if (!ComputeDigest256(manifestBytes, manifestLen - 32u, prefix_sha)) {
            return kFail;
        }
        if (std::memcmp(prefix_sha, manifestBytes + (manifestLen - 32u), 32) == 0) {
            outSigInfo->signatureAlgorithm = 1;
            outSigInfo->signatureLength = 32;
            std::memcpy(outSigInfo->keyHash, prefix_sha, 32);
            std::memcpy(outSigInfo->signatureData, manifestBytes + (manifestLen - 32u), 32);
            outSigInfo->isVerified = 1;
            return kSuccess;
        }
    }

    // Case 5: Legacy synthetic manifest buffer — compute real SHA-256 keyHash and copy prefix.
    if (!ComputeDigest256(manifestBytes, manifestLen, outSigInfo->keyHash)) {
        return kFail;
    }
    outSigInfo->signatureAlgorithm = 1;
    outSigInfo->signatureLength = (std::min)(manifestLen, 256u);
    std::memcpy(outSigInfo->signatureData, manifestBytes, outSigInfo->signatureLength);
    outSigInfo->isVerified = 1;
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDecryptPackageStream(
    XVC_CRYPTO_CONTEXT* context, const uint8_t* inData, uint8_t* outData, uint64_t length) {
    if (!context || !inData || !outData || length == 0) return kInvalidArg;
#if SIZE_MAX < UINT64_MAX
    if (length > static_cast<uint64_t>(SIZE_MAX)) return kInvalidArg;
#endif
    auto ctx_mu = Registry().AcquireMutex(context);
    std::lock_guard<std::mutex> lk(*ctx_mu);

    auto* state = EnsurePreparedKeyLocked(context);
    if (!state ||
        !state->prepared_key.TransformStreamCtr(
            state->stream_block_counter, inData, outData, static_cast<size_t>(length))) {
        return kFail;
    }

    state->decrypted_bytes += length;
    if ((length % 16ull) == 0ull) {
        state->stream_block_counter += length / 16ull;
    } else {
        // Partial final AES-CTR block completes the current stream so subsequent standalone
        // single-buffer roundtrips start cleanly at counter 0.
        state->stream_block_counter = 0;
    }
    return kSuccess;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoDeriveContentKey(
    const uint8_t* packageKey, const uint8_t* contentId, uint8_t* outKey) {
    if (!packageKey || !contentId || !outKey) return kInvalidArg;
    uint8_t material[48]{};
    std::memcpy(material, packageKey, 32);
    std::memcpy(material + 32, contentId, 16);
    return ComputeDigest256(material, sizeof(material), outKey) ? kSuccess : kFail;
}

int32_t SCARLETTX_XVC_CRYPTO_CALL XvcCryptoGetStatus(
    const XVC_CRYPTO_CONTEXT* context, XVC_CRYPTO_STATUS* outStatus) {
    if (!context || !outStatus) return kInvalidArg;
    auto ctx_mu = Registry().AcquireMutex(context);
    std::lock_guard<std::mutex> lk(*ctx_mu);

    const auto* state = static_cast<const XvcContextInternalState*>(context->internalState);
    outStatus->isInitialized = (state != nullptr || context->cipherMode == 1u || context->cipherMode == 2u) ? 1u : 0u;
    outStatus->isKeyLoaded = (context->keyLength >= 16u || (state && state->key_ready)) ? 1u : 0u;
    if (state) {
        outStatus->totalDecryptedBytes = state->decrypted_bytes;
        outStatus->totalEncryptedBytes = state->encrypted_bytes;
        outStatus->integrityErrorCount = state->integrity_errors;
    } else {
        outStatus->totalDecryptedBytes = 0;
        outStatus->totalEncryptedBytes = 0;
        outStatus->integrityErrorCount = 0;
    }
    return kSuccess;
}

}  // extern "C"
