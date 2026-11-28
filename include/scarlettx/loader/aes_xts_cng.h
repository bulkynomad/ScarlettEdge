#pragma once

// Clean-room FIPS-197 AES-XTS (IEEE 1619 / NIST SP 800-38E), AES-CBC,
// FIPS 180-4 SHA-256, and FIPS 198-1 HMAC-SHA-256 backed by Windows
// Cryptography Next Generation (bcrypt.dll, loaded dynamically).
// Contains zero proprietary keys or secrets.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#ifndef BCRYPT_XTS_AES_ALGORITHM
#define BCRYPT_XTS_AES_ALGORITHM L"XTS-AES"
#endif
#ifndef BCRYPT_MESSAGE_BLOCK_LENGTH
#define BCRYPT_MESSAGE_BLOCK_LENGTH L"MessageBlockLength"
#endif
#endif

namespace scarlettx::crypto {

inline void Gf128MulAlpha(uint8_t t[16]) {
    uint8_t carry = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t next_carry = static_cast<uint8_t>((t[i] >> 7) & 1u);
        t[i] = static_cast<uint8_t>((t[i] << 1) | carry);
        carry = next_carry;
    }
    if (carry) {
        t[0] ^= 0x87u;
    }
}

#if defined(_WIN32)
struct BcryptApi {
    using FnOpenAlg = NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
    using FnCloseAlg = NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);
    using FnSetProp = NTSTATUS (WINAPI*)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG);
    using FnGenKey = NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    using FnDestroyKey = NTSTATUS (WINAPI*)(BCRYPT_KEY_HANDLE);
    using FnEncrypt = NTSTATUS (WINAPI*)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
    using FnDecrypt = NTSTATUS (WINAPI*)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
    using FnCreateHash = NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    using FnHashData = NTSTATUS (WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    using FnFinishHash = NTSTATUS (WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    using FnDestroyHash = NTSTATUS (WINAPI*)(BCRYPT_HASH_HANDLE);

    HMODULE dll = nullptr;
    FnOpenAlg open_alg = nullptr;
    FnCloseAlg close_alg = nullptr;
    FnSetProp set_prop = nullptr;
    FnGenKey gen_key = nullptr;
    FnDestroyKey destroy_key = nullptr;
    FnEncrypt encrypt = nullptr;
    FnDecrypt decrypt = nullptr;
    FnCreateHash create_hash = nullptr;
    FnHashData hash_data = nullptr;
    FnFinishHash finish_hash = nullptr;
    FnDestroyHash destroy_hash = nullptr;
    BCRYPT_ALG_HANDLE aes_xts_alg = nullptr;
    BCRYPT_ALG_HANDLE aes_ecb_alg = nullptr;
    BCRYPT_ALG_HANDLE aes_cbc_alg = nullptr;
    BCRYPT_ALG_HANDLE sha256_alg = nullptr;
    BCRYPT_ALG_HANDLE hmac_sha256_alg = nullptr;
    bool xts_supports_16b_iv = false;

    BcryptApi() {
        dll = LoadLibraryW(L"bcrypt.dll");
        if (!dll) return;
        open_alg = reinterpret_cast<FnOpenAlg>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptOpenAlgorithmProvider")));
        close_alg = reinterpret_cast<FnCloseAlg>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptCloseAlgorithmProvider")));
        set_prop = reinterpret_cast<FnSetProp>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptSetProperty")));
        gen_key = reinterpret_cast<FnGenKey>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptGenerateSymmetricKey")));
        destroy_key = reinterpret_cast<FnDestroyKey>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptDestroyKey")));
        encrypt = reinterpret_cast<FnEncrypt>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptEncrypt")));
        decrypt = reinterpret_cast<FnDecrypt>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptDecrypt")));
        create_hash = reinterpret_cast<FnCreateHash>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptCreateHash")));
        hash_data = reinterpret_cast<FnHashData>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptHashData")));
        finish_hash = reinterpret_cast<FnFinishHash>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptFinishHash")));
        destroy_hash = reinterpret_cast<FnDestroyHash>(reinterpret_cast<void*>(GetProcAddress(dll, "BCryptDestroyHash")));

        if (open_alg && set_prop) {
            if (open_alg(&aes_xts_alg, BCRYPT_XTS_AES_ALGORITHM, nullptr, 0) < 0) {
                aes_xts_alg = nullptr;
            }
            if (open_alg(&aes_ecb_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 && aes_ecb_alg) {
                wchar_t ecb_mode[] = BCRYPT_CHAIN_MODE_ECB;
                if (set_prop(aes_ecb_alg, BCRYPT_CHAINING_MODE,
                             reinterpret_cast<PUCHAR>(ecb_mode), sizeof(ecb_mode), 0) < 0) {
                    if (close_alg) close_alg(aes_ecb_alg, 0);
                    aes_ecb_alg = nullptr;
                }
            }
            if (open_alg(&aes_cbc_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 && aes_cbc_alg) {
                wchar_t cbc_mode[] = BCRYPT_CHAIN_MODE_CBC;
                if (set_prop(aes_cbc_alg, BCRYPT_CHAINING_MODE,
                             reinterpret_cast<PUCHAR>(cbc_mode), sizeof(cbc_mode), 0) < 0) {
                    if (close_alg) close_alg(aes_cbc_alg, 0);
                    aes_cbc_alg = nullptr;
                }
            }
            open_alg(&sha256_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
            open_alg(&hmac_sha256_alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);

            // Probe whether host CNG BCRYPT_XTS_AES_ALGORITHM honors all 128 bits of a 16-byte IV
            // (needed when VDUID tweak_seed has non-zero upper 64 bits [8..15]).
            if (aes_xts_alg && aes_ecb_alg && gen_key && destroy_key && encrypt) {
                uint8_t test_key[32]{};
                for (int i = 0; i < 32; ++i) test_key[i] = static_cast<uint8_t>(i + 1);
                BCRYPT_KEY_HANDLE h_xts = nullptr;
                BCRYPT_KEY_HANDLE h_k1 = nullptr;
                BCRYPT_KEY_HANDLE h_k2 = nullptr;
                ULONG unit_sz = 16u;
                if (gen_key(aes_xts_alg, &h_xts, nullptr, 0, test_key, 32, 0) >= 0 && h_xts &&
                    set_prop(h_xts, BCRYPT_MESSAGE_BLOCK_LENGTH, reinterpret_cast<PUCHAR>(&unit_sz), sizeof(unit_sz), 0) >= 0 &&
                    gen_key(aes_ecb_alg, &h_k1, nullptr, 0, test_key, 16, 0) >= 0 && h_k1 &&
                    gen_key(aes_ecb_alg, &h_k2, nullptr, 0, test_key + 16, 16, 0) >= 0 && h_k2) {
                    uint8_t iv16[16] = {1, 2, 3, 4, 5, 6, 7, 8, 0xA5, 0x5A, 0xC3, 0x3C, 9, 10, 11, 12};
                    uint8_t iv16_copy[16]{};
                    std::memcpy(iv16_copy, iv16, 16);
                    uint8_t zero16[16]{};
                    uint8_t out_xts[16]{};
                    ULONG cb = 0;
                    if (encrypt(h_xts, zero16, 16, nullptr, iv16_copy, 16, out_xts, 16, &cb, 0) >= 0 && cb == 16) {
                        uint8_t ref_twk[16]{};
                        uint8_t ref_blk[16]{};
                        if (encrypt(h_k2, iv16, 16, nullptr, nullptr, 0, ref_twk, 16, &cb, 0) >= 0 &&
                            encrypt(h_k1, ref_twk, 16, nullptr, nullptr, 0, ref_blk, 16, &cb, 0) >= 0) {
                            for (int i = 0; i < 16; ++i) ref_blk[i] ^= ref_twk[i];
                            xts_supports_16b_iv = (std::memcmp(out_xts, ref_blk, 16) == 0);
                        }
                    }
                }
                if (h_k2) destroy_key(h_k2);
                if (h_k1) destroy_key(h_k1);
                if (h_xts) destroy_key(h_xts);
            }
        }
    }

    ~BcryptApi() {
        if (close_alg) {
            if (hmac_sha256_alg) close_alg(hmac_sha256_alg, 0);
            if (sha256_alg) close_alg(sha256_alg, 0);
            if (aes_cbc_alg) close_alg(aes_cbc_alg, 0);
            if (aes_ecb_alg) close_alg(aes_ecb_alg, 0);
            if (aes_xts_alg) close_alg(aes_xts_alg, 0);
        }
    }
};

// Immortal heap singleton: never destructs before static DStorageState or XvdContainer singletons at process exit.
inline const BcryptApi& Cng() {
    static const BcryptApi* api = new BcryptApi();
    return *api;
}
#endif

// Computes FIPS 180-4 SHA-256 digest via Windows CNG (chunked in <= 1 GiB blocks for buffers >= 4 GiB).
inline bool ComputeSha256(const uint8_t* data, size_t len, uint8_t out_digest[32]) {
    if (!out_digest || (len > 0 && !data)) return false;
#if defined(_WIN32)
    const auto& api = Cng();
    if (!api.sha256_alg || !api.create_hash || !api.hash_data || !api.finish_hash || !api.destroy_hash) {
        return false;
    }
    BCRYPT_HASH_HANDLE h = nullptr;
    if (api.create_hash(api.sha256_alg, &h, nullptr, 0, nullptr, 0, 0) < 0 || !h) {
        return false;
    }
    constexpr size_t kMaxHashChunk = 0x40000000ull; // 1 GiB
    NTSTATUS st = 0;
    const uint8_t* ptr = data;
    size_t remaining = len;
    while (remaining > 0 && st >= 0) {
        const ULONG chunk = static_cast<ULONG>((std::min)(remaining, kMaxHashChunk));
        st = api.hash_data(h, const_cast<PUCHAR>(ptr), chunk, 0);
        ptr += chunk;
        remaining -= chunk;
    }
    if (st >= 0) {
        st = api.finish_hash(h, out_digest, 32, 0);
    }
    api.destroy_hash(h);
    return st >= 0;
#else
    (void)data;
    (void)len;
    return false;
#endif
}

// Computes FIPS 198-1 HMAC-SHA-256 via Windows CNG (chunked in <= 1 GiB blocks for buffers >= 4 GiB).
inline bool ComputeHmacSha256(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t len,
    uint8_t out_digest[32]) {
    if (!out_digest || (key_len > 0 && !key) || (len > 0 && !data)) return false;
#if defined(_WIN32)
    if (key_len > 0x7FFFFFFFull) return false;
    const auto& api = Cng();
    if (!api.hmac_sha256_alg || !api.create_hash || !api.hash_data || !api.finish_hash || !api.destroy_hash) {
        return false;
    }
    uint8_t zero_key = 0;
    const uint8_t* eff_key = (key && key_len > 0) ? key : &zero_key;
    const ULONG eff_key_len = (key && key_len > 0) ? static_cast<ULONG>(key_len) : 1u;

    BCRYPT_HASH_HANDLE h = nullptr;
    if (api.create_hash(api.hmac_sha256_alg, &h, nullptr, 0,
                        const_cast<PUCHAR>(eff_key), eff_key_len, 0) < 0 || !h) {
        return false;
    }
    constexpr size_t kMaxHashChunk = 0x40000000ull; // 1 GiB
    NTSTATUS st = 0;
    const uint8_t* ptr = data;
    size_t remaining = len;
    while (remaining > 0 && st >= 0) {
        const ULONG chunk = static_cast<ULONG>((std::min)(remaining, kMaxHashChunk));
        st = api.hash_data(h, const_cast<PUCHAR>(ptr), chunk, 0);
        ptr += chunk;
        remaining -= chunk;
    }
    if (st >= 0) {
        st = api.finish_hash(h, out_digest, 32, 0);
    }
    api.destroy_hash(h);
    return st >= 0;
#else
    (void)key;
    (void)key_len;
    (void)data;
    (void)len;
    return false;
#endif
}

// Cached CNG AES-XTS / AES-CBC key set.
// Uses single-call 4 KiB Windows CNG BCRYPT_XTS_AES_ALGORITHM (BCRYPT_MESSAGE_BLOCK_LENGTH = 4096)
// for sector encryption/decryption (data_key || tweak_key: 32B AES-128-XTS or 64B AES-256-XTS).
struct AesXtsPreparedKey {
    static constexpr ULONG kSectorUnitBytes = 4096u;

    std::array<uint8_t, 64> raw_key{};
    size_t raw_key_len = 0;
#if defined(_WIN32)
    BCRYPT_KEY_HANDLE hk_xts_4k = nullptr;
    BCRYPT_KEY_HANDLE hk1 = nullptr;
    BCRYPT_KEY_HANDLE hk2 = nullptr;
    BCRYPT_KEY_HANDLE hk_cbc = nullptr;
    bool cng_ready = false;
#endif

    AesXtsPreparedKey() = default;
    AesXtsPreparedKey(const AesXtsPreparedKey&) = delete;
    AesXtsPreparedKey& operator=(const AesXtsPreparedKey&) = delete;

    ~AesXtsPreparedKey() { Reset(); }

    void Reset() {
#if defined(_WIN32)
        const auto& api = Cng();
        if (api.destroy_key) {
            if (hk_cbc) { api.destroy_key(hk_cbc); hk_cbc = nullptr; }
            if (hk2) { api.destroy_key(hk2); hk2 = nullptr; }
            if (hk1) { api.destroy_key(hk1); hk1 = nullptr; }
            if (hk_xts_4k) { api.destroy_key(hk_xts_4k); hk_xts_4k = nullptr; }
        }
        cng_ready = false;
#endif
        raw_key.fill(0);
        raw_key_len = 0;
    }

    bool Init(const uint8_t* key, size_t key_len, bool derive_distinct_tweak = false) {
        Reset();
        if (!key || key_len < 16) return false;
        raw_key_len = (std::min)(key_len, size_t{64});
        std::memcpy(raw_key.data(), key, raw_key_len);

#if defined(_WIN32)
        const uint8_t* k1 = raw_key.data();
        const uint8_t* k2 = raw_key.data();
        uint8_t derived_tweak_buf[32]{};
        ULONG aes_key_bytes = 16;
        if (raw_key_len >= 64) {
            aes_key_bytes = 32;
            k2 = raw_key.data() + 32;
        } else if (raw_key_len >= 32) {
            aes_key_bytes = 16;
            k2 = raw_key.data() + 16;
        } else if (derive_distinct_tweak) {
            static constexpr uint8_t kLabel[] = "SCARLETTX-XTS-TWEAK";
            if (!ComputeHmacSha256(raw_key.data(), 16, kLabel, sizeof(kLabel) - 1, derived_tweak_buf)) {
                Reset();
                return false;
            }
            std::memcpy(raw_key.data() + 16, derived_tweak_buf, 16);
            k2 = raw_key.data() + 16;
        }

        // Combined XTS key material: data_key (K1) || tweak_key (K2), 32B (AES-128-XTS) or 64B (AES-256-XTS).
        uint8_t xts_combined[64]{};
        const ULONG xts_combined_bytes = aes_key_bytes * 2u;
        std::memcpy(xts_combined, k1, aes_key_bytes);
        std::memcpy(xts_combined + aes_key_bytes, k2, aes_key_bytes);

        const auto& api = Cng();
        if (api.aes_xts_alg && api.gen_key && api.set_prop) {
            ULONG unit_bytes = kSectorUnitBytes;
            if (api.gen_key(api.aes_xts_alg, &hk_xts_4k, nullptr, 0,
                            xts_combined, xts_combined_bytes, 0) < 0 ||
                !hk_xts_4k ||
                api.set_prop(hk_xts_4k, BCRYPT_MESSAGE_BLOCK_LENGTH,
                             reinterpret_cast<PUCHAR>(&unit_bytes), sizeof(unit_bytes), 0) < 0) {
                if (hk_xts_4k && api.destroy_key) {
                    api.destroy_key(hk_xts_4k);
                }
                hk_xts_4k = nullptr;
            }
        }

        if (api.aes_ecb_alg && api.gen_key && api.destroy_key && api.encrypt && api.decrypt) {
            const bool ecb_ok =
                api.gen_key(api.aes_ecb_alg, &hk1, nullptr, 0, const_cast<PUCHAR>(k1), aes_key_bytes, 0) >= 0 &&
                api.gen_key(api.aes_ecb_alg, &hk2, nullptr, 0, const_cast<PUCHAR>(k2), aes_key_bytes, 0) >= 0;
            const bool cbc_ok = !api.aes_cbc_alg ||
                api.gen_key(api.aes_cbc_alg, &hk_cbc, nullptr, 0, const_cast<PUCHAR>(k1), aes_key_bytes, 0) >= 0;
            if (ecb_ok && cbc_ok && hk1 && hk2) {
                cng_ready = true;
            } else {
                Reset();
                return false;
            }
        }
        return cng_ready;
#else
        (void)derive_distinct_tweak;
        return false;
#endif
    }

    // IEEE 1619 / NIST SP 800-38E AES-XTS sector transform with optional 16-byte VDUID tweak seed.
    bool TransformSectorWithTweakSeed(
        bool do_encrypt,
        uint64_t sector_index,
        const uint8_t* tweak_seed,
        const uint8_t* in_buf,
        uint8_t* out_buf,
        size_t len) const {
        if (raw_key_len < 16 || !in_buf || !out_buf || len == 0) return false;
#if defined(_WIN32)
        if (!cng_ready) return false;
        const auto& api = Cng();

        // Single-call 4 KiB hardware-accelerated CNG BCRYPT_XTS_AES_ALGORITHM fast path:
        if (len == kSectorUnitBytes && hk_xts_4k != nullptr) {
            bool upper_seed_nonzero = false;
            uint64_t seed_low64 = 0;
            if (tweak_seed) {
                for (int i = 0; i < 8; ++i) {
                    seed_low64 |= static_cast<uint64_t>(tweak_seed[i]) << (i * 8);
                }
                for (int i = 8; i < 16; ++i) {
                    if (tweak_seed[i] != 0) {
                        upper_seed_nonzero = true;
                        break;
                    }
                }
            }
            if (!upper_seed_nonzero) {
                // Standard 64-bit little-endian sector tweak (cbIV = 8) in a single CNG call.
                uint64_t tweak_u64 = sector_index ^ seed_low64;
                uint8_t iv8[8]{};
                for (int i = 0; i < 8; ++i) {
                    iv8[i] = static_cast<uint8_t>((tweak_u64 >> (i * 8)) & 0xFFu);
                }
                ULONG cb_xts = 0;
                const NTSTATUS xts_st = do_encrypt
                    ? api.encrypt(hk_xts_4k, const_cast<PUCHAR>(in_buf), kSectorUnitBytes,
                                  nullptr, iv8, sizeof(iv8), out_buf, kSectorUnitBytes, &cb_xts, 0)
                    : api.decrypt(hk_xts_4k, const_cast<PUCHAR>(in_buf), kSectorUnitBytes,
                                  nullptr, iv8, sizeof(iv8), out_buf, kSectorUnitBytes, &cb_xts, 0);
                if (xts_st >= 0 && cb_xts == kSectorUnitBytes) {
                    return true;
                }
            } else if (api.xts_supports_16b_iv) {
                // Full 128-bit VDUID-seeded tweak in a single CNG call when verified on host OS.
                uint8_t iv16[16]{};
                std::memcpy(iv16, tweak_seed, 16);
                for (int i = 0; i < 8; ++i) {
                    iv16[i] ^= static_cast<uint8_t>((sector_index >> (i * 8)) & 0xFFu);
                }
                ULONG cb_xts = 0;
                const NTSTATUS xts_st = do_encrypt
                    ? api.encrypt(hk_xts_4k, const_cast<PUCHAR>(in_buf), kSectorUnitBytes,
                                  nullptr, iv16, sizeof(iv16), out_buf, kSectorUnitBytes, &cb_xts, 0)
                    : api.decrypt(hk_xts_4k, const_cast<PUCHAR>(in_buf), kSectorUnitBytes,
                                  nullptr, iv16, sizeof(iv16), out_buf, kSectorUnitBytes, &cb_xts, 0);
                if (xts_st >= 0 && cb_xts == kSectorUnitBytes) {
                    return true;
                }
            }
        }

        uint8_t tweak[16]{};
        if (tweak_seed) {
            std::memcpy(tweak, tweak_seed, 16);
        }
        for (int i = 0; i < 8; ++i) {
            tweak[i] ^= static_cast<uint8_t>((sector_index >> (i * 8)) & 0xFFu);
        }
        ULONG cb_out = 0;
        if (api.encrypt(hk2, tweak, 16, nullptr, nullptr, 0, tweak, 16, &cb_out, 0) < 0) {
            return false;
        }

        const size_t full_blocks = len / 16;
        const size_t rem = len % 16;
        uint8_t block[16]{};

        // NIST SP 800-38E / IEEE 1619 ciphertext stealing (CTS) when len >= 16 and rem > 0.
        // Batch all full 16B blocks in 4 KiB (256-block) single-call CNG dispatches (never 256 per-16B calls).
        const size_t main_blocks = (rem > 0 && full_blocks >= 1) ? (full_blocks - 1) : full_blocks;
        size_t done_blocks = 0;
        while (done_blocks < main_blocks) {
            const size_t batch_blocks = (std::min)(main_blocks - done_blocks, size_t{256});
            const ULONG batch_bytes = static_cast<ULONG>(batch_blocks * 16u);
            alignas(16) uint8_t tweaks[4096];
            alignas(16) uint8_t work[4096];
            const uint8_t* src_batch = in_buf + done_blocks * 16u;
            uint8_t* dst_batch = out_buf + done_blocks * 16u;
            for (size_t b = 0; b < batch_blocks; ++b) {
                std::memcpy(tweaks + b * 16u, tweak, 16u);
                Gf128MulAlpha(tweak);
            }
            for (size_t i = 0; i < batch_bytes; ++i) {
                work[i] = static_cast<uint8_t>(src_batch[i] ^ tweaks[i]);
            }
            const NTSTATUS st = do_encrypt
                ? api.encrypt(hk1, work, batch_bytes, nullptr, nullptr, 0, work, batch_bytes, &cb_out, 0)
                : api.decrypt(hk1, work, batch_bytes, nullptr, nullptr, 0, work, batch_bytes, &cb_out, 0);
            if (st < 0 || cb_out != batch_bytes) return false;
            for (size_t i = 0; i < batch_bytes; ++i) {
                dst_batch[i] = static_cast<uint8_t>(work[i] ^ tweaks[i]);
            }
            done_blocks += batch_blocks;
        }

        if (rem > 0) {
            if (full_blocks >= 1) {
                // IEEE 1619 / NIST SP 800-38E XTS Ciphertext Stealing on penultimate + partial block
                uint8_t t_penult[16]{};
                uint8_t t_last[16]{};
                std::memcpy(t_penult, tweak, 16);
                std::memcpy(t_last, tweak, 16);
                Gf128MulAlpha(t_last);

                const uint8_t* t1 = do_encrypt ? t_penult : t_last;
                const uint8_t* t2 = do_encrypt ? t_last : t_penult;
                const size_t penult_off = (full_blocks - 1) * 16;
                const size_t last_off = full_blocks * 16;

                uint8_t tail_copy[16]{};
                std::memcpy(tail_copy, in_buf + last_off, rem);

                uint8_t cc[16]{};
                for (int i = 0; i < 16; ++i) block[i] = static_cast<uint8_t>(in_buf[penult_off + i] ^ t1[i]);
                const NTSTATUS st1 = do_encrypt
                    ? api.encrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0)
                    : api.decrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0);
                if (st1 < 0) return false;
                for (int i = 0; i < 16; ++i) cc[i] = static_cast<uint8_t>(block[i] ^ t1[i]);

                uint8_t pp[16]{};
                std::memcpy(pp, tail_copy, rem);
                std::memcpy(pp + rem, cc + rem, 16 - rem);
                std::memcpy(out_buf + last_off, cc, rem);

                for (int i = 0; i < 16; ++i) block[i] = static_cast<uint8_t>(pp[i] ^ t2[i]);
                const NTSTATUS st2 = do_encrypt
                    ? api.encrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0)
                    : api.decrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0);
                if (st2 < 0) return false;
                for (int i = 0; i < 16; ++i) out_buf[penult_off + i] = static_cast<uint8_t>(block[i] ^ t2[i]);
            } else {
                // Sub-16-byte synthetic buffer via AES-ECB encrypted keystream mask (still real CNG AES)
                uint8_t mask[16]{};
                if (api.encrypt(hk1, tweak, 16, nullptr, nullptr, 0, mask, 16, &cb_out, 0) < 0) {
                    return false;
                }
                for (size_t i = 0; i < rem; ++i) {
                    out_buf[i] = static_cast<uint8_t>(in_buf[i] ^ mask[i]);
                }
            }
        }
        return true;
#else
        (void)do_encrypt;
        (void)sector_index;
        (void)tweak_seed;
        return false;
#endif
    }

    bool TransformSector(bool do_encrypt, uint64_t sector_index, const uint8_t* in_buf, uint8_t* out_buf, size_t len) const {
        return TransformSectorWithTweakSeed(do_encrypt, sector_index, nullptr, in_buf, out_buf, len);
    }

    // AES-CBC sector transform backed by CNG BCRYPT_CHAIN_MODE_CBC (for cipherMode == 2).
    bool TransformSectorCbc(
        bool do_encrypt,
        uint64_t sector_index,
        const uint8_t* in_buf,
        uint8_t* out_buf,
        size_t len) const {
        if (raw_key_len < 16 || !in_buf || !out_buf || len == 0) return false;
#if defined(_WIN32)
        if (!cng_ready || !hk_cbc) return false;
        const auto& api = Cng();
        uint8_t iv[16]{};
        for (int i = 0; i < 8; ++i) {
            iv[i] = static_cast<uint8_t>((sector_index >> (i * 8)) & 0xFFu);
        }

        const size_t full_bytes = (len / 16) * 16;
        const size_t rem = len % 16;
        uint8_t tail_in[16]{};
        if (rem > 0) {
            std::memcpy(tail_in, in_buf + full_bytes, rem);
        }

        uint8_t last_cipher_block[16]{};
        std::memcpy(last_cipher_block, iv, 16);
        if (!do_encrypt && full_bytes >= 16) {
            std::memcpy(last_cipher_block, in_buf + full_bytes - 16, 16);
        }

        constexpr size_t kMaxCbcChunk = 0x40000000ull; // 1 GiB (multiple of 16)
        size_t offset = 0;
        while (offset < full_bytes) {
            const ULONG chunk = static_cast<ULONG>((std::min)(full_bytes - offset, kMaxCbcChunk));
            ULONG cb_out = 0;
            const NTSTATUS st = do_encrypt
                ? api.encrypt(hk_cbc, const_cast<PUCHAR>(in_buf + offset), chunk,
                              nullptr, iv, 16, out_buf + offset, chunk, &cb_out, 0)
                : api.decrypt(hk_cbc, const_cast<PUCHAR>(in_buf + offset), chunk,
                              nullptr, iv, 16, out_buf + offset, chunk, &cb_out, 0);
            if (st < 0 || cb_out != chunk) return false;
            offset += chunk;
        }

        if (do_encrypt && full_bytes >= 16) {
            std::memcpy(last_cipher_block, out_buf + full_bytes - 16, 16);
        }

        if (rem > 0) {
            uint8_t mask[16]{};
            ULONG cb_out = 0;
            if (api.encrypt(hk1, last_cipher_block, 16, nullptr, nullptr, 0, mask, 16, &cb_out, 0) < 0) {
                return false;
            }
            for (size_t i = 0; i < rem; ++i) {
                out_buf[full_bytes + i] = static_cast<uint8_t>(tail_in[i] ^ mask[i]);
            }
        }
        return true;
#else
        (void)do_encrypt;
        (void)sector_index;
        return false;
#endif
    }

    // Symmetric AES-CTR stream transform using cached hk1 (for package stream decryption).
    bool TransformStreamCtr(uint64_t initial_counter, const uint8_t* in_buf, uint8_t* out_buf, size_t len) const {
        if (raw_key_len < 16 || !in_buf || !out_buf || len == 0) return false;
#if defined(_WIN32)
        if (!cng_ready) return false;
        const auto& api = Cng();
        uint8_t ctr[16]{};
        uint8_t ks[16]{};
        ULONG cb_out = 0;
        uint64_t block_idx = initial_counter;
        size_t pos = 0;
        while (pos < len) {
            for (int i = 0; i < 8; ++i) {
                ctr[i] = static_cast<uint8_t>((block_idx >> (i * 8)) & 0xFFu);
            }
            if (api.encrypt(hk1, ctr, 16, nullptr, nullptr, 0, ks, 16, &cb_out, 0) < 0) {
                return false;
            }
            const size_t chunk = (std::min)(size_t{16}, len - pos);
            for (size_t i = 0; i < chunk; ++i) {
                out_buf[pos + i] = static_cast<uint8_t>(in_buf[pos + i] ^ ks[i]);
            }
            pos += chunk;
            ++block_idx;
        }
        return true;
#else
        (void)initial_counter;
        return false;
#endif
    }
};

// IEEE 1619 / NIST SP 800-38E AES-XTS sector transform (one-shot convenience wrappers).
inline bool AesXtsTransformSectorWithTweakSeed(
    bool do_encrypt,
    const uint8_t* key,
    size_t key_len,
    uint64_t sector_index,
    const uint8_t* tweak_seed,
    const uint8_t* in_buf,
    uint8_t* out_buf,
    size_t len) {
    AesXtsPreparedKey prep;
    if (!prep.Init(key, key_len)) return false;
    return prep.TransformSectorWithTweakSeed(do_encrypt, sector_index, tweak_seed, in_buf, out_buf, len);
}

inline bool AesXtsTransformSector(
    bool do_encrypt,
    const uint8_t* key,
    size_t key_len,
    uint64_t sector_index,
    const uint8_t* in_buf,
    uint8_t* out_buf,
    size_t len) {
    return AesXtsTransformSectorWithTweakSeed(
        do_encrypt, key, key_len, sector_index, nullptr, in_buf, out_buf, len);
}

}  // namespace scarlettx::crypto
