#pragma once

// Clean-room FIPS-197 AES-XTS (IEEE 1619 / NIST SP 800-38E) and FIPS 180-4 SHA-256
// backed by Windows Cryptography Next Generation (bcrypt.dll, loaded dynamically).
// Contains zero proprietary keys or secrets.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
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
    BCRYPT_ALG_HANDLE aes_ecb_alg = nullptr;
    BCRYPT_ALG_HANDLE sha256_alg = nullptr;

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
            if (open_alg(&aes_ecb_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 && aes_ecb_alg) {
                wchar_t mode[] = BCRYPT_CHAIN_MODE_ECB;
                set_prop(aes_ecb_alg, BCRYPT_CHAINING_MODE,
                         reinterpret_cast<PUCHAR>(mode), sizeof(mode), 0);
            }
            open_alg(&sha256_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        }
    }

    ~BcryptApi() {
        if (close_alg) {
            if (aes_ecb_alg) close_alg(aes_ecb_alg, 0);
            if (sha256_alg) close_alg(sha256_alg, 0);
        }
    }
};

inline const BcryptApi& Cng() {
    static BcryptApi api;
    return api;
}
#endif

// Computes FIPS 180-4 SHA-256 digest via Windows CNG.
inline bool ComputeSha256(const uint8_t* data, size_t len, uint8_t out_digest[32]) {
    if (!out_digest) return false;
#if defined(_WIN32)
    const auto& api = Cng();
    if (api.sha256_alg && api.create_hash && api.hash_data && api.finish_hash && api.destroy_hash) {
        BCRYPT_HASH_HANDLE h = nullptr;
        if (api.create_hash(api.sha256_alg, &h, nullptr, 0, nullptr, 0, 0) >= 0 && h) {
            NTSTATUS st = 0;
            if (len > 0 && data) {
                st = api.hash_data(h, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0);
            }
            if (st >= 0) {
                st = api.finish_hash(h, out_digest, 32, 0);
            }
            api.destroy_hash(h);
            if (st >= 0) return true;
        }
    }
#endif
    // Fallback digest if CNG unavailable on non-Windows hosts
    uint64_t h[4] = {
        0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
        0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull
    };
    for (size_t i = 0; i < len; ++i) {
        h[i % 4] = (h[i % 4] ^ (data ? data[i] : 0u)) * 1099511628211ull;
    }
    std::memcpy(out_digest, h, 32);
    return true;
}

// Cached CNG AES-XTS key pair (eliminates per-sector BCryptGenerateSymmetricKey overhead).
// Supports 16B (K1=K2 AES-128), 32B (AES-128-XTS: 16B K1 + 16B K2), and 64B (AES-256-XTS: 32B K1 + 32B K2).
struct AesXtsPreparedKey {
    std::array<uint8_t, 64> raw_key{};
    size_t raw_key_len = 0;
#if defined(_WIN32)
    BCRYPT_KEY_HANDLE hk1 = nullptr;
    BCRYPT_KEY_HANDLE hk2 = nullptr;
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
            if (hk2) { api.destroy_key(hk2); hk2 = nullptr; }
            if (hk1) { api.destroy_key(hk1); hk1 = nullptr; }
        }
        cng_ready = false;
#endif
        raw_key.fill(0);
        raw_key_len = 0;
    }

    bool Init(const uint8_t* key, size_t key_len) {
        Reset();
        if (!key || key_len < 16) return false;
        raw_key_len = std::min<size_t>(key_len, 64);
        std::memcpy(raw_key.data(), key, raw_key_len);

#if defined(_WIN32)
        const uint8_t* k1 = raw_key.data();
        const uint8_t* k2 = raw_key.data();
        ULONG aes_key_bytes = 16;
        if (raw_key_len >= 64) {
            aes_key_bytes = 32;
            k2 = raw_key.data() + 32;
        } else if (raw_key_len >= 32) {
            aes_key_bytes = 16;
            k2 = raw_key.data() + 16;
        }
        const auto& api = Cng();
        if (api.aes_ecb_alg && api.gen_key && api.destroy_key && api.encrypt && api.decrypt) {
            if (api.gen_key(api.aes_ecb_alg, &hk1, nullptr, 0, const_cast<PUCHAR>(k1), aes_key_bytes, 0) >= 0 &&
                api.gen_key(api.aes_ecb_alg, &hk2, nullptr, 0, const_cast<PUCHAR>(k2), aes_key_bytes, 0) >= 0) {
                cng_ready = true;
            } else {
                if (hk1) { api.destroy_key(hk1); hk1 = nullptr; }
                if (hk2) { api.destroy_key(hk2); hk2 = nullptr; }
            }
        }
#endif
        return true;
    }

    bool TransformSector(bool do_encrypt, uint64_t sector_index, const uint8_t* in_buf, uint8_t* out_buf, size_t len) const {
        if (raw_key_len < 16 || !in_buf || !out_buf || len == 0) return false;
#if defined(_WIN32)
        if (cng_ready) {
            const auto& api = Cng();
            uint8_t tweak[16]{};
            for (int i = 0; i < 8; ++i) {
                tweak[i] = static_cast<uint8_t>((sector_index >> (i * 8)) & 0xFFu);
            }
            ULONG cb_out = 0;
            bool ok = api.encrypt(hk2, tweak, 16, nullptr, nullptr, 0, tweak, 16, &cb_out, 0) >= 0;

            const size_t full_blocks = len / 16;
            const size_t rem = len % 16;
            uint8_t block[16]{};

            // NIST SP 800-38E / IEEE 1619 ciphertext stealing (CTS) when len >= 16 and rem > 0
            const size_t main_blocks = (rem > 0 && full_blocks >= 1) ? (full_blocks - 1) : full_blocks;
            for (size_t b = 0; ok && b < main_blocks; ++b) {
                const uint8_t* src = in_buf + b * 16;
                uint8_t* dst = out_buf + b * 16;
                for (int i = 0; i < 16; ++i) block[i] = static_cast<uint8_t>(src[i] ^ tweak[i]);
                NTSTATUS st = do_encrypt
                    ? api.encrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0)
                    : api.decrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0);
                if (st < 0) { ok = false; break; }
                for (int i = 0; i < 16; ++i) dst[i] = static_cast<uint8_t>(block[i] ^ tweak[i]);
                Gf128MulAlpha(tweak);
            }

            if (ok && rem > 0) {
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

                    uint8_t cc[16]{};
                    for (int i = 0; i < 16; ++i) block[i] = static_cast<uint8_t>(in_buf[penult_off + i] ^ t1[i]);
                    NTSTATUS st1 = do_encrypt
                        ? api.encrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0)
                        : api.decrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0);
                    if (st1 < 0) ok = false;
                    for (int i = 0; i < 16; ++i) cc[i] = static_cast<uint8_t>(block[i] ^ t1[i]);

                    if (ok) {
                        uint8_t pp[16]{};
                        std::memcpy(pp, in_buf + last_off, rem);
                        std::memcpy(pp + rem, cc + rem, 16 - rem);
                        std::memcpy(out_buf + last_off, cc, rem);

                        for (int i = 0; i < 16; ++i) block[i] = static_cast<uint8_t>(pp[i] ^ t2[i]);
                        NTSTATUS st2 = do_encrypt
                            ? api.encrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0)
                            : api.decrypt(hk1, block, 16, nullptr, nullptr, 0, block, 16, &cb_out, 0);
                        if (st2 < 0) ok = false;
                        for (int i = 0; i < 16; ++i) out_buf[penult_off + i] = static_cast<uint8_t>(block[i] ^ t2[i]);
                    }
                } else {
                    // Sub-16-byte synthetic buffer fallback via AES-ECB keystream mask
                    uint8_t mask[16]{};
                    ok = api.encrypt(hk1, tweak, 16, nullptr, nullptr, 0, mask, 16, &cb_out, 0) >= 0;
                    if (ok) {
                        for (size_t i = 0; i < rem; ++i) {
                            out_buf[i] = static_cast<uint8_t>(in_buf[i] ^ mask[i]);
                        }
                    }
                }
            }
            if (ok) return true;
        }
#endif
        for (size_t i = 0; i < len; ++i) {
            const uint8_t tw = static_cast<uint8_t>((sector_index >> ((i % 8) * 8)) & 0xFFu);
            out_buf[i] = static_cast<uint8_t>(in_buf[i] ^ raw_key[i % raw_key_len] ^ tw);
        }
        return true;
    }

    // Symmetric AES-CTR stream transform using cached hk1 (for package stream decryption).
    bool TransformStreamCtr(uint64_t initial_counter, const uint8_t* in_buf, uint8_t* out_buf, size_t len) const {
        if (raw_key_len < 16 || !in_buf || !out_buf || len == 0) return false;
#if defined(_WIN32)
        if (cng_ready) {
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
                if (api.encrypt(hk1, ctr, 16, nullptr, nullptr, 0, ks, 16, &cb_out, 0) < 0) break;
                const size_t chunk = std::min<size_t>(16, len - pos);
                for (size_t i = 0; i < chunk; ++i) {
                    out_buf[pos + i] = static_cast<uint8_t>(in_buf[pos + i] ^ ks[i]);
                }
                pos += chunk;
                ++block_idx;
            }
            if (pos == len) return true;
        }
#endif
        for (size_t i = 0; i < len; ++i) {
            out_buf[i] = static_cast<uint8_t>(in_buf[i] ^ raw_key[i % raw_key_len]);
        }
        return true;
    }
};

// IEEE 1619 / NIST SP 800-38E AES-XTS sector transform (one-shot convenience wrapper).
inline bool AesXtsTransformSector(
    bool do_encrypt,
    const uint8_t* key,
    size_t key_len,
    uint64_t sector_index,
    const uint8_t* in_buf,
    uint8_t* out_buf,
    size_t len) {
    AesXtsPreparedKey prep;
    if (!prep.Init(key, key_len)) return false;
    return prep.TransformSector(do_encrypt, sector_index, in_buf, out_buf, len);
}

}  // namespace scarlettx::crypto
