#include "scarlettx/io/xvc_crypto_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
    std::cout << "[test_xvc_crypto] Testing XVC/XVD package crypto APIs...\n";

    // 1. Context initialization & release
    XVC_CRYPTO_CONTEXT ctx{};
    SX_CHECK(XvcCryptoInitializeContext(&ctx) == 0);

    const uint8_t key[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    };
    SX_CHECK(XvcCryptoSetVolumeKey(&ctx, key, sizeof(key)) == 0);

    // 2. AES-XTS roundtrip
    uint8_t plain[512];
    for (uint32_t i = 0; i < 512; ++i) plain[i] = static_cast<uint8_t>(i ^ 0x5A);

    uint8_t cipher[512]{};
    uint8_t decrypted[512]{};

    SX_CHECK(XvcCryptoEncryptSectorXts(&ctx, 100, plain, cipher, 512) == 0);
    SX_CHECK(std::memcmp(plain, cipher, 512) != 0);

    SX_CHECK(XvcCryptoDecryptSectorXts(&ctx, 100, cipher, decrypted, 512) == 0);
    SX_CHECK(std::memcmp(plain, decrypted, 512) == 0);

    // 3. Block digest calculation & integrity verification
    uint8_t digest[32]{};
    SX_CHECK(XvcCryptoComputeBlockSha256(plain, 512, digest) == 0);
    SX_CHECK(XvcCryptoVerifyIntegrityTable(digest, 32, plain, 512) == 0);

    // Bad integrity check should fail
    digest[0] ^= 0xFF;
    SX_CHECK(XvcCryptoVerifyIntegrityTable(digest, 32, plain, 512) != 0);

    // 4. Manifest signature parsing
    XVC_SIGNATURE_INFO sig_info{};
    SX_CHECK(XvcCryptoParseManifestSignature(plain, 512, &sig_info) == 0);
    SX_CHECK(sig_info.isVerified == 1);

    // 5. Stream decrypt & key derivation
    uint8_t derived_key[32]{};
    const uint8_t content_id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    SX_CHECK(XvcCryptoDeriveContentKey(key, content_id, derived_key) == 0);

    // 6. Status query
    XVC_CRYPTO_STATUS status{};
    SX_CHECK(XvcCryptoGetStatus(&ctx, &status) == 0);
    SX_CHECK(status.isInitialized == 1);
    SX_CHECK(status.isKeyLoaded == 1);

    SX_CHECK(XvcCryptoReleaseContext(&ctx) == 0);

    std::cout << "[test_xvc_crypto] All 12 XvcCrypto APIs verified successfully!\n";
    return 0;
}
