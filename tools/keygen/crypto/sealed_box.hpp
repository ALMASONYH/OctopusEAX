#pragma once
#include <string>
#include <vector>
#include "aes_gcm.hpp"

namespace crypt {

inline bool randomBytes(unsigned char* p, size_t n) {
    return BCryptGenRandom(nullptr, p, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

inline bool derive(const std::string& secret, const unsigned char* salt,
                   unsigned char key[32]) {
    BCRYPT_ALG_HANDLE prf = nullptr;
    if (BCryptOpenAlgorithmProvider(&prf, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG))
        return false;
    NTSTATUS st = BCryptDeriveKeyPBKDF2(prf, (PUCHAR)secret.data(),
                                        (ULONG)secret.size(), (PUCHAR)salt,
                                        (ULONG)SALT_N, ROUNDS, key, 32, 0);
    BCryptCloseAlgorithmProvider(prf, 0);
    return st == 0;
}

inline bool seal(const unsigned char* plain, size_t n, const std::string& secret,
                 std::vector<unsigned char>& out) {
    out.assign(HDR_N + n, 0);
    unsigned char* salt  = &out[0];
    unsigned char* nonce = salt + SALT_N;
    unsigned char* tag   = nonce + NONCE_N;
    unsigned char* ct    = tag + TAG_N;
    if (!randomBytes(salt, SALT_N) || !randomBytes(nonce, NONCE_N)) return false;
    unsigned char k[32];
    if (!derive(secret, salt, k)) return false;
    Aes aes;
    bool ok = false;
    if (aes.open(k)) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        modeInfo(info, nonce, tag);
        ULONG w = 0;
        ok = BCryptEncrypt(aes.key, (PUCHAR)plain, (ULONG)n, &info, nullptr, 0,
                           ct, (ULONG)n, &w, 0) == 0 && w == n;
    }
    SecureZeroMemory(k, sizeof(k));
    if (!ok) out.clear();
    return ok;
}

inline bool unseal(const unsigned char* in, size_t n, const std::string& secret,
                   unsigned char* plain) {
    const unsigned char* salt  = in;
    const unsigned char* nonce = salt + SALT_N;
    const unsigned char* tag   = nonce + NONCE_N;
    const unsigned char* ct    = tag + TAG_N;
    unsigned char k[32];
    if (!derive(secret, salt, k)) return false;
    Aes aes;
    bool ok = false;
    if (aes.open(k)) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        modeInfo(info, (unsigned char*)nonce, (unsigned char*)tag);
        ULONG w = 0;
        ok = BCryptDecrypt(aes.key, (PUCHAR)ct, (ULONG)n, &info, nullptr, 0,
                           plain, (ULONG)n, &w, 0) == 0 && w == n;
    }
    SecureZeroMemory(k, sizeof(k));
    if (!ok) SecureZeroMemory(plain, n);
    return ok;
}

}
