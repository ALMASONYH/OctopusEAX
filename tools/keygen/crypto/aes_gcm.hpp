#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

namespace crypt {
static const size_t SALT_N  = 32;
static const size_t NONCE_N = 12;
static const size_t TAG_N   = 16;
static const size_t HDR_N   = SALT_N + NONCE_N + TAG_N;
static const ULONGLONG ROUNDS = 400000;

struct Aes {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    ~Aes() {
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    bool open(const unsigned char k[32]) {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))
            return false;
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                              (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0))
            return false;
        return BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, (PUCHAR)k, 32,
                                          0) == 0;
    }
};

inline void modeInfo(BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO& info,
                     unsigned char* nonce, unsigned char* tag) {
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = (ULONG)NONCE_N;
    info.pbTag = tag;
    info.cbTag = (ULONG)TAG_N;
}

}
