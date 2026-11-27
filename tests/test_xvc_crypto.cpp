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

    // 7. NIST FIPS 180-4 SHA-256 known-answer vector: SHA256("abc")
    {
        const uint8_t abc[3] = {'a', 'b', 'c'};
        uint8_t sha_out[32]{};
        SX_CHECK(XvcCryptoComputeBlockSha256(abc, 3, sha_out) == 0);
        const uint8_t expected[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };
        SX_CHECK(std::memcmp(sha_out, expected, 32) == 0);
    }

    // 8. IEEE 1619 / NIST SP 800-38E AES-128-XTS Vector 1 (all-zero 32B key, sector 0, 32B zero plaintext)
    {
        XVC_CRYPTO_CONTEXT vctx{};
        SX_CHECK(XvcCryptoInitializeContext(&vctx) == 0);
        const uint8_t zkey[32]{};
        const uint8_t zplain[32]{};
        uint8_t xts_cipher[32]{};
        uint8_t xts_plain[32]{};
        SX_CHECK(XvcCryptoSetVolumeKey(&vctx, zkey, sizeof(zkey)) == 0);
        SX_CHECK(XvcCryptoEncryptSectorXts(&vctx, 0, zplain, xts_cipher, 32) == 0);
        const uint8_t expected_ieee1619_v1[32] = {
            0x91, 0x7c, 0xf6, 0x9e, 0xbd, 0x68, 0xb2, 0xec,
            0x9b, 0x9f, 0xe9, 0xa3, 0xea, 0xdd, 0xa6, 0x92,
            0xcd, 0x43, 0xd2, 0xf5, 0x95, 0x98, 0xed, 0x85,
            0x8c, 0x02, 0xc2, 0x65, 0x2f, 0xbf, 0x92, 0x2e
        };
        SX_CHECK(std::memcmp(xts_cipher, expected_ieee1619_v1, 32) == 0);
        SX_CHECK(XvcCryptoDecryptSectorXts(&vctx, 0, xts_cipher, xts_plain, 32) == 0);
        SX_CHECK(std::memcmp(xts_plain, zplain, 32) == 0);
        SX_CHECK(XvcCryptoReleaseContext(&vctx) == 0);
    }

    // 9. Full 64-byte AES-256-XTS + IEEE 1619 CTS non-aligned tail (37 bytes) + AES-CTR stream roundtrip
    {
        XVC_CRYPTO_CONTEXT ctx256{};
        SX_CHECK(XvcCryptoInitializeContext(&ctx256) == 0);
        uint8_t key64_a[64]{};
        uint8_t key64_b[64]{};
        for (int i = 0; i < 64; ++i) {
            key64_a[i] = static_cast<uint8_t>(i + 1);
            key64_b[i] = static_cast<uint8_t>(i + 1);
        }
        key64_b[48] ^= 0xFFu; // Differs only in second 32B half (tweak key K2), proving 64B key is not truncated!

        uint8_t cts_plain[37]{};
        for (int i = 0; i < 37; ++i) cts_plain[i] = static_cast<uint8_t>(0x30 + i);
        uint8_t cts_cipher_a[37]{}, cts_cipher_b[37]{}, cts_dec[37]{};

        SX_CHECK(XvcCryptoSetVolumeKey(&ctx256, key64_a, 64) == 0);
        SX_CHECK(XvcCryptoEncryptSectorXts(&ctx256, 7, cts_plain, cts_cipher_a, 37) == 0);
        SX_CHECK(XvcCryptoDecryptSectorXts(&ctx256, 7, cts_cipher_a, cts_dec, 37) == 0);
        SX_CHECK(std::memcmp(cts_plain, cts_dec, 37) == 0);

        SX_CHECK(XvcCryptoSetVolumeKey(&ctx256, key64_b, 64) == 0);
        SX_CHECK(XvcCryptoEncryptSectorXts(&ctx256, 7, cts_plain, cts_cipher_b, 37) == 0);
        SX_CHECK(std::memcmp(cts_cipher_a, cts_cipher_b, 37) != 0);

        uint8_t stream_cipher[37]{}, stream_dec[37]{};
        SX_CHECK(XvcCryptoDecryptPackageStream(&ctx256, cts_plain, stream_cipher, 37) == 0);
        SX_CHECK(std::memcmp(cts_plain, stream_cipher, 37) != 0);
        SX_CHECK(XvcCryptoDecryptPackageStream(&ctx256, stream_cipher, stream_dec, 37) == 0);
        SX_CHECK(std::memcmp(cts_plain, stream_dec, 37) == 0);
        SX_CHECK(XvcCryptoReleaseContext(&ctx256) == 0);
    }

    std::cout << "[test_xvc_crypto] All 12 XvcCrypto APIs + NIST SHA-256 & IEEE 1619 AES-XTS vectors verified!\n";
    return 0;
}
