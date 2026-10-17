#pragma once

#include <string>

#include "../crypto/sealed_box.hpp"

namespace embedded {
static const char* ANYWHERE = "OctopusEAX generator, unrestricted build";
static const char* ANYWHERE_LEGACY = "93bEAX generator, unrestricted build";
static const char* ANYWHERE_MASON = "MasonEAX generator, unrestricted build";

static const unsigned char BLOB[] = {
#include "../keygen_secret.inc"
};

static const size_t KEY_N  = 104;
static const size_t CEK_N  = 32;
static const size_t WRAP_N = crypt::HDR_N + CEK_N;
static const size_t BODY_N = crypt::HDR_N + KEY_N;

inline bool present() {
    if constexpr (sizeof(BLOB) > 6) return memcmp(BLOB, "MKS1", 4) == 0;
    else return false;
}

inline int wrapCount() { return present() ? BLOB[4] : 0; }

inline bool unrestricted() { return present() && BLOB[5] != 0; }

inline bool signingKey(const std::string& machineNumber,
                       unsigned char out[KEY_N]) {
    if (!present()) return false;
    const unsigned char* body  = BLOB + 6;
    const unsigned char* wraps = body + BODY_N;

    unsigned char cek[CEK_N];
    bool opened = false;
    auto trySecret = [&](const char* secret) {
        return crypt::unseal(wraps, CEK_N, secret, cek);
    };
    if (unrestricted()) {
        opened = trySecret(ANYWHERE) || trySecret(ANYWHERE_LEGACY) || trySecret(ANYWHERE_MASON);
    } else {
        for (int i = 0; i < wrapCount() && !opened; ++i)
            opened = crypt::unseal(wraps + (size_t)i * WRAP_N, CEK_N,
                                   machineNumber, cek);
        if (!opened)
            opened = trySecret(ANYWHERE) || trySecret(ANYWHERE_LEGACY) || trySecret(ANYWHERE_MASON);
    }
    if (!opened) return false;

    std::string cekAsSecret((const char*)cek, CEK_N);
    bool ok = crypt::unseal(body, KEY_N, cekAsSecret, out);
    SecureZeroMemory(cek, sizeof(cek));
    if (!ok) SecureZeroMemory(out, KEY_N);
    return ok;
}

}
