#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include "hardware_id.hpp"

namespace lic {
static const char* PRODUCT = "OctopusEAX";
static const char* ISSUER = "Octopus copyright";
static const char* FORMAT = "1";

static const unsigned char XOR_BYTE = 0x37;

inline std::string deobf(const unsigned char* d, size_t n) {
    std::string r(n, '\0');
    for (size_t i = 0; i < n; ++i) r[i] = (char)(d[i] ^ XOR_BYTE);
    return r;
}
inline std::wstring wdeobf(const unsigned char* d, size_t n) {
    std::wstring r(n, L'\0');
    for (size_t i = 0; i < n; ++i) r[i] = (wchar_t)(unsigned char)(d[i] ^ XOR_BYTE);
    return r;
}

static const unsigned char PATH_BIOS[] = {
    0x7F,0x76,0x65,0x73,0x60,0x76,0x65,0x72,0x6B,
    0x73,0x72,0x64,0x74,0x65,0x7E,0x67,0x63,0x7E,0x78,0x79,0x6B,
    0x64,0x4E,0x44,0x43,0x52,0x5A,0x6B,
    0x75,0x7E,0x78,0x64
};

static const unsigned char PATH_CRYPTOGRAPHY[] = {
    0x64,0x78,0x71,0x63,0x60,0x76,0x65,0x72,0x6B,
    0x7A,0x5E,0x54,0x45,0x58,0x44,0x58,0x51,0x43,0x6B,
    0x74,0x45,0x4E,0x47,0x43,0x58,0x50,0x45,0x56,0x47,0x5F,0x4E
};

static const unsigned char PATH_NT_VERSION[] = {
    0x64,0x78,0x71,0x63,0x60,0x76,0x65,0x72,0x6B,
    0x7A,0x5E,0x54,0x45,0x58,0x44,0x58,0x51,0x43,0x6B,
    0x60,0x5E,0x59,0x53,0x58,0x40,0x44,0x17,0x79,0x63,0x6B,
    0x74,0x42,0x45,0x45,0x52,0x59,0x43,0x61,0x52,0x45,0x44,0x5E,0x58,0x59
};

static const unsigned char PATH_STORE[] = {
    0x64,0x78,0x71,0x63,0x60,0x76,0x65,0x72,0x6B,
    0x7A,0x56,0x44,0x58,0x59,0x72,0x76,0x6F
};

static const unsigned char VALUE_MACHINE_GUID[] = {0x7A,0x56,0x54,0x5F,0x5E,0x59,0x52,0x70,0x42,0x5E,0x53};

static const unsigned char VALUE_PRODUCT_NAME[] = {0x64,0x4E,0x44,0x43,0x52,0x5A,0x67,0x45,0x58,0x53,0x42,0x54,0x43,0x79,0x56,0x5A,0x52};

static const unsigned char VALUE_MANUFACTURER[] = {0x64,0x4E,0x44,0x43,0x52,0x5A,0x7A,0x56,0x59,0x42,0x51,0x56,0x54,0x43,0x42,0x45,0x52,0x45};

static const unsigned char VALUE_BASEBOARD[] = {0x75,0x56,0x44,0x52,0x75,0x58,0x56,0x45,0x53,0x67,0x45,0x58,0x53,0x42,0x54,0x43};

static const unsigned char VALUE_PRODUCT_ID[] = {0x67,0x45,0x58,0x53,0x42,0x54,0x43,0x7E,0x53};

static const unsigned char VALUE_KEY[] = {0x7C,0x52,0x4E};
static const unsigned char VALUE_TS[]  = {0x63,0x44};
static const unsigned char VALUE_RM[]  = {0x65,0x5A};
static const unsigned char VALUE_TK[]  = {0x63,0x5C};
static const unsigned char VALUE_KH[]  = {0x7C,0x5F};
static const unsigned char VALUE_OUT[] = {0x64,0x78};

inline bool debuggerPresent() {
    const BOOL d1 = IsDebuggerPresent();

    BOOL d2 = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &d2);

#ifdef _WIN64
    volatile const BYTE* peb = (volatile const BYTE*)__readgsqword(0x60);
    volatile DWORD ntgf = *(volatile const DWORD*)(peb + 0xBC);
#else
    volatile const BYTE* peb = (volatile const BYTE*)__readfsdword(0x30);
    volatile DWORD ntgf = *(volatile const DWORD*)(peb + 0x68);
#endif
    const BOOL d3 = (ntgf & 0x70) ? TRUE : FALSE;

    return (d1 | d2 | d3) != 0;
}

inline void cpuLeaf(int out[4], unsigned int leaf) {
    volatile unsigned int l = leaf;
    __cpuid(out, (int)l);
}

static const int SIGNER_SLOTS = 4;
static const unsigned char SIGNERS[SIGNER_SLOTS][64] = {
#include "vendor_key.inc"
};

static const unsigned char OWNER_HASH[32] = {
    0x11,0x27,0x85,0xbc,0x66,0xde,0x7f,0x1f,0x29,0xf8,0x55,0xb1,0x2f,0x11,0x4e,0x99,
    0x5c,0x17,0x4e,0x1d,0xc5,0xef,0xb6,0xad,0xb5,0xb3,0x4c,0x9f,0xcb,0x0f,0x98,0xec
};

static const unsigned char OWNER_HASH_HEAD[16] = {
    0x11,0x27,0x85,0xbc,0x66,0xde,0x7f,0x1f,
    0x29,0xf8,0x55,0xb1,0x2f,0x11,0x4e,0x99
};
static const unsigned char OWNER_HASH_TAIL[16] = {
    0x5c,0x17,0x4e,0x1d,0xc5,0xef,0xb6,0xad,
    0xb5,0xb3,0x4c,0x9f,0xcb,0x0f,0x98,0xec
};

inline bool slotInUse(int slot) {
    for (int i = 0; i < 64; ++i)
        if (SIGNERS[slot][i]) return true;
    return false;
}

static const char* B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

inline std::string b32encode(const unsigned char* p, size_t n) {
    std::string out;
    unsigned int buf = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        buf = (buf << 8) | p[i];
        bits += 8;
        while (bits >= 5) { out += B32[(buf >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits) out += B32[(buf << (5 - bits)) & 31];
    return out;
}

inline std::string b32normalise(const std::string& s) {
    std::string t;
    for (char c : s) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == '0') c = 'O';
        if (c == '1') c = 'I';
        if (c == '8') c = 'B';
        if (c == '9') c = 'G';
        if ((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7')) t += c;
    }
    return t;
}

inline bool b32decode(const std::string& in, std::vector<unsigned char>& out,
                      size_t want) {
    std::string s = b32normalise(in);
    unsigned int buf = 0;
    int bits = 0;
    out.clear();
    for (char c : s) {
        const char* q = strchr(B32, c);
        if (!q) return false;
        buf = (buf << 5) | (unsigned int)(q - B32);
        bits += 5;
        if (bits >= 8) {
            out.push_back((unsigned char)((buf >> (bits - 8)) & 0xFF));
            bits -= 8;
        }
    }
    return out.size() == want;
}

inline std::string group(const std::string& s, size_t n) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && i % n == 0) out += '-';
        out += s[i];
    }
    return out;
}

inline bool sha256(const std::string& msg, unsigned char digest[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
        return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    if (!BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0)) {
        if (!BCryptHashData(h, (PUCHAR)msg.data(), (ULONG)msg.size(), 0))
            ok = !BCryptFinishHash(h, digest, 32, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

inline std::string regTrim(const std::string& in) {
    std::string t = in;
    while (!t.empty() && (unsigned char)t.back() <= ' ') t.pop_back();
    return t;
}

inline void addRegObf(std::string& s,
                      const unsigned char* kO, size_t kN,
                      const unsigned char* vO, size_t vN,
                      bool trim) {
    const std::wstring wk = wdeobf(kO, kN);
    const std::wstring wv = wdeobf(vO, vN);
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wk.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS)
        return;
    wchar_t v[256] = {0};
    DWORD cb = sizeof(v) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(hk, wv.c_str(), nullptr, &type, (LPBYTE)v, &cb)
            == ERROR_SUCCESS && type == REG_SZ) {
        if (trim) {
            char a[400] = {0};
            WideCharToMultiByte(CP_UTF8, 0, v, -1, a, sizeof(a) - 1, nullptr, nullptr);
            const std::string val = regTrim(a);
            if (!val.empty()) { s += '|'; s += val; }
        } else {
            char a[400];
            WideCharToMultiByte(CP_UTF8, 0, v, -1, a, sizeof(a), nullptr, nullptr);
            s += '|'; s += a;
        }
    }
    RegCloseKey(hk);
}

inline std::string machineFingerprintV1() {
    std::string s;
    HKEY hk;
    const std::wstring wkc = wdeobf(PATH_CRYPTOGRAPHY, sizeof(PATH_CRYPTOGRAPHY));
    const std::wstring wvm = wdeobf(VALUE_MACHINE_GUID, sizeof(VALUE_MACHINE_GUID));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wkc.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        wchar_t v[128] = {0};
        DWORD cb = sizeof(v) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(hk, wvm.c_str(), nullptr, &type, (LPBYTE)v, &cb)
                == ERROR_SUCCESS && type == REG_SZ) {
            char a[192];
            WideCharToMultiByte(CP_UTF8, 0, v, -1, a, sizeof(a), nullptr, nullptr);
            s += a;
        }
        RegCloseKey(hk);
    }
    wchar_t win[MAX_PATH];
    if (GetWindowsDirectoryW(win, MAX_PATH)) {
        wchar_t root[4] = {win[0], L':', L'\\', 0};
        DWORD serial = 0;
        if (GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr,
                                  nullptr, 0)) {
            char b[32];
            sprintf(b, "|%08lX", (unsigned long)serial);
            s += b;
        }
    }
    return s;
}

inline std::string cpuIdentity(bool legacy) {
    int r[4];
    char vendor[13] = {0};
    cpuLeaf(r, 0);
    memcpy(vendor + 0, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    std::string s = vendor;
    cpuLeaf(r, 0x80000000u);
    if ((unsigned int)r[0] >= 0x80000004u) {
        char brand[49] = {0};
        for (int i = 0; i < 3; ++i) {
            cpuLeaf(r, 0x80000002u + i);
            memcpy(brand + i * 16, r, 16);
        }
        s += '|'; s += brand;
    }
    cpuLeaf(r, 1);
    char sig[24];
    if (legacy) sprintf(sig, "|%08X-%08X", (unsigned)r[0], (unsigned)r[3]);
    else sprintf(sig, "|%08X", (unsigned)r[0]);
    s += sig;
    return s;
}

inline std::string machineFingerprint() {
    std::string s = machineFingerprintV1();
    s += "||";
    s += cpuIdentity(false);
    addRegObf(s, PATH_BIOS, sizeof(PATH_BIOS), VALUE_PRODUCT_NAME, sizeof(VALUE_PRODUCT_NAME), true);
    addRegObf(s, PATH_BIOS, sizeof(PATH_BIOS), VALUE_MANUFACTURER, sizeof(VALUE_MANUFACTURER), true);
    addRegObf(s, PATH_BIOS, sizeof(PATH_BIOS), VALUE_BASEBOARD, sizeof(VALUE_BASEBOARD), true);
    addRegObf(s, PATH_NT_VERSION, sizeof(PATH_NT_VERSION), VALUE_PRODUCT_ID, sizeof(VALUE_PRODUCT_ID), true);
    return s;
}

inline std::string machineFingerprintV2Legacy() {
    std::string s = machineFingerprintV1();
    s += "||";
    s += cpuIdentity(true);
    addRegObf(s, PATH_BIOS, sizeof(PATH_BIOS), VALUE_PRODUCT_NAME, sizeof(VALUE_PRODUCT_NAME), false);
    addRegObf(s, PATH_BIOS, sizeof(PATH_BIOS), VALUE_MANUFACTURER, sizeof(VALUE_MANUFACTURER), false);
    addRegObf(s, PATH_BIOS, sizeof(PATH_BIOS), VALUE_BASEBOARD, sizeof(VALUE_BASEBOARD), false);
    addRegObf(s, PATH_NT_VERSION, sizeof(PATH_NT_VERSION), VALUE_PRODUCT_ID, sizeof(VALUE_PRODUCT_ID), false);
    return s;
}

inline std::string machineFingerprintV4() {
    std::string s = machineFingerprint();
    s += "||";
    s += hardwareSerials();
    return s;
}

inline std::string machineId() {
    unsigned char d[32];
    if (!sha256(machineFingerprintV4(), d)) return "";
    return group(b32encode(d, 10), 4);
}

inline std::string machineIdV3() {
    unsigned char d[32];
    if (!sha256(machineFingerprint(), d)) return "";
    return group(b32encode(d, 10), 4);
}

inline std::string machineIdV1() {
    unsigned char d[32];
    if (!sha256(machineFingerprintV1(), d)) return "";
    return group(b32encode(d, 10), 4);
}

inline std::string machineIdV2Legacy() {
    unsigned char d[32];
    if (!sha256(machineFingerprintV2Legacy(), d)) return "";
    return group(b32encode(d, 10), 4);
}

static const unsigned FLAG_PAUSED  = 1u;
static const unsigned FLAG_STOPPED = 2u;
static const unsigned CLOCK_LIFE   = 0xFFFFFFFFu;

inline bool clockExpired(const std::string& key);
inline void tickClock(const std::string& key);

enum LicState {
    LIC_NONE = 0,
    LIC_OK,
    LIC_EXPIRED,
    LIC_PAUSED,
    LIC_STOPPED,
    LIC_BAD
};

inline unsigned readU32(const unsigned char* p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
}
inline void writeU32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

inline std::string signedLine(const std::string& id, unsigned expiry = 0, unsigned flags = 0) {
    char b[48];
    sprintf(b, "%u|%u", expiry, flags);
    return std::string(PRODUCT) + "|" + ISSUER + "|" + FORMAT + "|" + id + "|" + b;
}

inline std::string signedLineLegacy(const std::string& id, unsigned expiry) {
    char b[24];
    sprintf(b, "%u", expiry);
    return std::string(PRODUCT) + "|" + ISSUER + "|" + FORMAT + "|" + id + "|" + b;
}

inline bool decodeKey(const std::string& key, std::vector<unsigned char>& sig,
                      unsigned& expiry, unsigned& flags, bool& modern) {
    expiry = 0;
    flags = 0;
    modern = false;
    std::vector<unsigned char> raw;
    if (b32decode(key, raw, 72) && raw.size() == 72) {
        expiry = readU32(raw.data());
        flags = readU32(raw.data() + 4);
        sig.assign(raw.begin() + 8, raw.end());
        modern = true;
        return true;
    }
    if (b32decode(key, raw, 68) && raw.size() == 68) {
        expiry = readU32(raw.data());
        sig.assign(raw.begin() + 4, raw.end());
        return true;
    }
    if (b32decode(key, raw, 64) && raw.size() == 64) {
        sig = std::move(raw);
        return true;
    }
    return false;
}

inline bool decodeKey(const std::string& key, std::vector<unsigned char>& sig, unsigned& expiry) {
    unsigned flags = 0;
    bool modern = false;
    return decodeKey(key, sig, expiry, flags, modern);
}

inline std::string encodeKey(unsigned expiry, unsigned flags, const unsigned char sig[64]) {
    unsigned char raw[72];
    writeU32(raw, expiry);
    writeU32(raw + 4, flags);
    memcpy(raw + 8, sig, 64);
    return group(b32encode(raw, 72), 8);
}

inline std::string encodeKey(unsigned expiry, const unsigned char sig[64]) {
    return encodeKey(expiry, 0, sig);
}

inline std::wstring membershipLeft(const std::string& key) {
    unsigned exp = 0, flags = 0;
    bool modern = false;
    std::vector<unsigned char> sig;
    if (!decodeKey(key, sig, exp, flags, modern)) return L"";
    if (flags & FLAG_STOPPED) return L"stopped";
    if (clockExpired(key)) return L"expired";
    if (flags & FLAG_PAUSED) return L"paused";
    if (exp == 0) return L"Lifetime";
    long long left = (long long)exp - (long long)time(nullptr);
    if (left <= 0) return L"expired";
    const int d = (int)(left / 86400);
    const int h = (int)((left % 86400) / 3600);
    const int m = (int)((left % 3600) / 60);
    wchar_t t[64];
    if (d > 0) swprintf_s(t, 64, L"%dd %dh left", d, h);
    else if (h > 0) swprintf_s(t, 64, L"%dh %dm left", h, m);
    else swprintf_s(t, 64, L"%dm left", m > 0 ? m : 1);
    return t;
}

inline bool verifyWith(const unsigned char pub[64], const unsigned char digest[32],
                       const unsigned char sig[64]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))
        return false;
    unsigned char blob[8 + 64] = {};
    *(ULONG*)(blob + 0) = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    *(ULONG*)(blob + 4) = 32;
    memcpy(blob + 8, pub, 64);

    BCRYPT_KEY_HANDLE k = nullptr;
    bool ok = false;
    if (!BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &k, blob,
                             sizeof(blob), 0)) {
        ok = !BCryptVerifySignature(k, nullptr, (PUCHAR)digest, 32,
                                    (PUCHAR)sig, 64, 0);
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

inline bool ownerIntact() {
    unsigned char d[32];
    if (!sha256(std::string((const char*)SIGNERS, sizeof(SIGNERS)), d)) return false;

    int diff1 = 0;
    for (int i = 0; i < 32; ++i) diff1 |= d[i] ^ OWNER_HASH[i];

    int diff2 = 0;
    for (int i = 0; i < 16; ++i) diff2 |= d[i]      ^ OWNER_HASH_HEAD[i];
    for (int i = 0; i < 16; ++i) diff2 |= d[16 + i] ^ OWNER_HASH_TAIL[i];

    return (diff1 | diff2) == 0;
}

inline int verifySlot(const std::string& id, const std::string& key) {
    if (!ownerIntact()) return -1;
    std::vector<unsigned char> sig;
    unsigned expiry = 0, flags = 0;
    bool modern = false;
    if (!decodeKey(key, sig, expiry, flags, modern) || sig.size() != 64) return -1;
    unsigned char digest[32];
    const std::string line = modern ? signedLine(id, expiry, flags) : signedLineLegacy(id, expiry);
    if (!sha256(line, digest)) return -1;
    for (int s = 0; s < SIGNER_SLOTS; ++s) {
        if (!slotInUse(s)) continue;
        if (verifyWith(SIGNERS[s], digest, sig.data())) return s;
    }
    return -1;
}

inline bool verify(const std::string& id, const std::string& key) {
    return verifySlot(id, key) >= 0;
}

struct MachineIds {
    std::string id[4];
};

inline MachineIds knownIds(bool fresh) {
    static std::mutex lock;
    static MachineIds cache;
    static bool have = false;
    std::lock_guard<std::mutex> hold(lock);
    if (fresh || !have) {
        cache = MachineIds{{machineId(), machineIdV3(), machineIdV2Legacy(), machineIdV1()}};
        have = true;
    }
    return cache;
}

inline bool matchesAny(const MachineIds& ids, const std::string& key) {
    for (const std::string& id : ids.id)
        if (!id.empty() && verify(id, key)) return true;
    return false;
}

inline bool belongsHere(const std::string& key) {
    return matchesAny(knownIds(false), key) || matchesAny(knownIds(true), key);
}

inline LicState inspectKey(const std::string& key) {
    if (key.empty()) return LIC_NONE;
    if (!belongsHere(key)) return LIC_BAD;
    unsigned exp = 0, flags = 0;
    bool modern = false;
    std::vector<unsigned char> sig;
    if (!decodeKey(key, sig, exp, flags, modern)) return LIC_BAD;
    if (flags & FLAG_STOPPED) return LIC_STOPPED;
    if (clockExpired(key)) return LIC_EXPIRED;
    if (exp != 0 && (unsigned)time(nullptr) > exp) return LIC_EXPIRED;
    if (flags & FLAG_PAUSED) return LIC_PAUSED;
    return LIC_OK;
}

static const char* const REVOKED[] = {
#include "revoked.inc"
    nullptr
};

inline bool revoked() {
    const MachineIds known = knownIds(false);
    for (const char* const* r = REVOKED; *r; ++r) {
        const std::string n = b32normalise(*r);
        if (n.size() != 16) continue;
        for (const std::string& id : known.id)
            if (b32normalise(id) == n) return true;
    }
    return false;
}

inline std::string readFrom(HKEY root) {
    HKEY hk;
    std::string out;
    const std::wstring wsto = wdeobf(PATH_STORE, sizeof(PATH_STORE));
    const std::string vkey = deobf(VALUE_KEY, sizeof(VALUE_KEY));
    if (RegOpenKeyExW(root, wsto.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        char v[256] = {0};
        DWORD cb = sizeof(v) - 1, type = 0;
        if (RegQueryValueExA(hk, vkey.c_str(), nullptr, &type, (LPBYTE)v, &cb)
                == ERROR_SUCCESS && type == REG_SZ)
            out = v;
        RegCloseKey(hk);
    }
    return out;
}

inline bool writeTo(HKEY root, const std::string& key) {
    HKEY hk;
    const std::wstring wsto = wdeobf(PATH_STORE, sizeof(PATH_STORE));
    const std::string vkey = deobf(VALUE_KEY, sizeof(VALUE_KEY));
    if (RegCreateKeyExW(root, wsto.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &hk, nullptr)
            != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExA(hk, vkey.c_str(), 0, REG_SZ,
                             (const BYTE*)key.c_str(), (DWORD)key.size() + 1);
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}

inline bool signedOutFrom(const std::string& key);

inline std::string loadKey() {
    std::string k = readFrom(HKEY_CURRENT_USER);
    if (k.empty()) k = readFrom(HKEY_LOCAL_MACHINE);
    return signedOutFrom(k) ? std::string() : k;
}

inline bool storeGetU32(HKEY root, const unsigned char* nm, size_t nlen, unsigned& out) {
    HKEY hk;
    const std::wstring wsto = wdeobf(PATH_STORE, sizeof(PATH_STORE));
    const std::string name = deobf(nm, nlen);
    if (RegOpenKeyExW(root, wsto.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS)
        return false;
    DWORD d = 0, sz = 4, type = 0;
    LONG rc = RegQueryValueExA(hk, name.c_str(), nullptr, &type, (BYTE*)&d, &sz);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = d;
    return true;
}
inline void storeSetU32(HKEY root, const unsigned char* nm, size_t nlen, unsigned v) {
    HKEY hk;
    const std::wstring wsto = wdeobf(PATH_STORE, sizeof(PATH_STORE));
    const std::string name = deobf(nm, nlen);
    if (RegCreateKeyExW(root, wsto.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;
    DWORD d = v;
    RegSetValueExA(hk, name.c_str(), 0, REG_DWORD, (const BYTE*)&d, 4);
    RegCloseKey(hk);
}
inline std::string storeGetSz(HKEY root, const unsigned char* nm, size_t nlen) {
    HKEY hk;
    const std::wstring wsto = wdeobf(PATH_STORE, sizeof(PATH_STORE));
    const std::string name = deobf(nm, nlen);
    std::string out;
    if (RegOpenKeyExW(root, wsto.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS)
        return out;
    char v[64] = {0};
    DWORD cb = sizeof(v) - 1, type = 0;
    if (RegQueryValueExA(hk, name.c_str(), nullptr, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS && type == REG_SZ)
        out = v;
    RegCloseKey(hk);
    return out;
}
inline void storeSetSz(HKEY root, const unsigned char* nm, size_t nlen, const std::string& v) {
    HKEY hk;
    const std::wstring wsto = wdeobf(PATH_STORE, sizeof(PATH_STORE));
    const std::string name = deobf(nm, nlen);
    if (RegCreateKeyExW(root, wsto.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExA(hk, name.c_str(), 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)v.size() + 1);
    RegCloseKey(hk);
}

inline std::string fingerOf(const std::string& data) {
    unsigned char d[32];
    if (!sha256(data, d)) return "";
    static const char* hx = "0123456789ABCDEF";
    std::string s(16, '0');
    for (int i = 0; i < 8; ++i) { s[(size_t)(2 * i)] = hx[d[i] >> 4]; s[(size_t)(2 * i + 1)] = hx[d[i] & 15]; }
    return s;
}

inline std::string keyFinger(const std::string& key) {
    return fingerOf(b32normalise(key));
}

inline std::string termsFinger(const std::string& key) {
    std::vector<unsigned char> sig;
    unsigned exp = 0, flags = 0;
    bool modern = false;
    if (!decodeKey(key, sig, exp, flags, modern) || sig.size() != 64) return "";
    char head[32];
    sprintf(head, "%u|%u|%d|", exp, flags, modern ? 1 : 0);
    std::string terms = head;
    terms.append((const char*)sig.data(), 32);
    return fingerOf(terms);
}

struct ClockRec { char hash[17]; unsigned remain, wall, tick; };

inline bool signedOutFrom(const std::string& key) {
    if (key.empty()) return false;
    const std::string mark = storeGetSz(HKEY_CURRENT_USER, VALUE_OUT, sizeof(VALUE_OUT));
    return !mark.empty() && mark == keyFinger(key);
}

inline void eraseFrom(HKEY root) {
    HKEY hk;
    const std::wstring wsto = wdeobf(PATH_STORE, sizeof(PATH_STORE));
    const std::string vkey = deobf(VALUE_KEY, sizeof(VALUE_KEY));
    if (RegOpenKeyExW(root, wsto.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) return;
    RegDeleteValueA(hk, vkey.c_str());
    RegCloseKey(hk);
}

inline void signOut() {
    std::string key = readFrom(HKEY_CURRENT_USER);
    if (key.empty()) key = readFrom(HKEY_LOCAL_MACHINE);
    if (!key.empty()) storeSetSz(HKEY_CURRENT_USER, VALUE_OUT, sizeof(VALUE_OUT), keyFinger(key));
    eraseFrom(HKEY_CURRENT_USER);
    eraseFrom(HKEY_LOCAL_MACHINE);
}

inline void signIn() {
    storeSetSz(HKEY_CURRENT_USER, VALUE_OUT, sizeof(VALUE_OUT), "");
}

inline std::wstring clockFile() {
    wchar_t pd[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? pd : L"C:\\ProgramData";
    dir += L"\\OctopusEAX";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\wt.dat";
}

inline bool readClockFile(ClockRec& c) {
    HANDLE h = CreateFileW(clockFile().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char raw[32] = {};
    DWORD n = 0;
    if (!ReadFile(h, raw, 28, &n, nullptr)) n = 0;
    CloseHandle(h);
    if (n < 28) return false;
    for (int i = 0; i < 28; ++i) raw[i] ^= 0xA5;
    memset(&c, 0, sizeof c);
    memcpy(c.hash, raw, 16);
    memcpy(&c.remain, raw + 16, 4);
    memcpy(&c.wall, raw + 20, 4);
    memcpy(&c.tick, raw + 24, 4);
    return c.hash[0] != 0;
}
inline void writeClockFile(const ClockRec& c) {
    unsigned char raw[28] = {};
    memcpy(raw, c.hash, 16);
    memcpy(raw + 16, &c.remain, 4);
    memcpy(raw + 20, &c.wall, 4);
    memcpy(raw + 24, &c.tick, 4);
    for (int i = 0; i < 28; ++i) raw[i] ^= 0xA5;
    HANDLE h = CreateFileW(clockFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, raw, 28, &w, nullptr);
    CloseHandle(h);
}

inline bool loadClock(ClockRec& c) {
    memset(&c, 0, sizeof c);
    ClockRec f{};
    bool haveF = readClockFile(f);
    std::string kh;
    unsigned rm = 0, wall = 0, tick = 0;
    bool haveR = false;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        kh = storeGetSz(root, VALUE_KH, sizeof(VALUE_KH));
        if (kh.empty()) continue;
        haveR = storeGetU32(root, VALUE_RM, sizeof(VALUE_RM), rm);
        storeGetU32(root, VALUE_TS, sizeof(VALUE_TS), wall);
        storeGetU32(root, VALUE_TK, sizeof(VALUE_TK), tick);
        break;
    }
    if (!kh.empty()) {
        memset(c.hash, 0, sizeof c.hash);
        memcpy(c.hash, kh.c_str(), kh.size() < 16 ? kh.size() : 16);
        c.remain = haveR ? rm : CLOCK_LIFE;
        c.wall = wall;
        c.tick = tick;
    }
    if (haveF && f.hash[0]) {
        if (!c.hash[0] || memcmp(c.hash, f.hash, 16) == 0) {
            if (!c.hash[0]) memcpy(c.hash, f.hash, 16);
            if (f.remain < c.remain) c.remain = f.remain;
            if (f.wall > c.wall) c.wall = f.wall;
        }
    }
    return c.hash[0] != 0;
}
inline void saveClock(const ClockRec& c) {
    storeSetSz(HKEY_LOCAL_MACHINE, VALUE_KH, sizeof(VALUE_KH), c.hash);
    storeSetSz(HKEY_CURRENT_USER, VALUE_KH, sizeof(VALUE_KH), c.hash);
    storeSetU32(HKEY_LOCAL_MACHINE, VALUE_RM, sizeof(VALUE_RM), c.remain);
    storeSetU32(HKEY_CURRENT_USER, VALUE_RM, sizeof(VALUE_RM), c.remain);
    storeSetU32(HKEY_LOCAL_MACHINE, VALUE_TS, sizeof(VALUE_TS), c.wall);
    storeSetU32(HKEY_CURRENT_USER, VALUE_TS, sizeof(VALUE_TS), c.wall);
    storeSetU32(HKEY_LOCAL_MACHINE, VALUE_TK, sizeof(VALUE_TK), c.tick);
    storeSetU32(HKEY_CURRENT_USER, VALUE_TK, sizeof(VALUE_TK), c.tick);
    writeClockFile(c);
}

inline void tickClock(const std::string& key) {
    if (key.empty()) return;
    unsigned exp = 0, flags = 0;
    bool modern = false;
    std::vector<unsigned char> sig;
    if (!decodeKey(key, sig, exp, flags, modern)) return;
    const std::string fp = termsFinger(key);
    if (fp.size() != 16) return;
    static std::mutex lock;
    std::lock_guard<std::mutex> hold(lock);
    const unsigned wall = (unsigned)time(nullptr);
    const unsigned tick = (unsigned)(GetTickCount64() / 1000ull);
    ClockRec c{};
    const bool had = loadClock(c);
    const bool same = had && memcmp(c.hash, fp.c_str(), 16) == 0;
    if (!same) {
        const unsigned seen = had ? c.wall : 0;
        const unsigned latest = wall > seen ? wall : seen;
        memset(&c, 0, sizeof c);
        memcpy(c.hash, fp.c_str(), 16);
        if (exp == 0) c.remain = CLOCK_LIFE;
        else c.remain = latest < exp ? exp - latest : 0;
        c.wall = latest;
        c.tick = tick;
        saveClock(c);
        return;
    }
    if (flags & (FLAG_PAUSED | FLAG_STOPPED)) {
        if (wall > c.wall) c.wall = wall;
        c.tick = tick;
        saveClock(c);
        return;
    }
    unsigned consume = 0;
    if (tick >= c.tick) consume = tick - c.tick;
    else if (c.wall && wall + 120 < c.wall) {
        if (c.remain != CLOCK_LIFE) c.remain = 0;
    }
    if (wall > c.wall) {
        const unsigned jump = wall - c.wall;
        if (jump > consume) consume = jump;
    }
    if (c.remain != CLOCK_LIFE && consume > 0) {
        if (consume >= c.remain) c.remain = 0;
        else c.remain -= consume;
    }
    if (wall > c.wall) c.wall = wall;
    c.tick = tick;
    saveClock(c);
}

inline bool clockExpired(const std::string& key) {
    unsigned exp = 0, flags = 0;
    bool modern = false;
    std::vector<unsigned char> sig;
    if (!decodeKey(key, sig, exp, flags, modern)) return true;
    if (exp == 0) return false;
    ClockRec c{};
    if (loadClock(c) && memcmp(c.hash, termsFinger(key).c_str(), 16) == 0)
        return c.remain == 0;
    return (unsigned)time(nullptr) > exp;
}

inline bool saveKey(const std::string& key) {
    const bool user = writeTo(HKEY_CURRENT_USER, key);
    const bool machine = writeTo(HKEY_LOCAL_MACHINE, key);
    if (user || machine) tickClock(key);
    return user || machine;
}

inline std::wstring gatePath(const std::string& machine) {
    const std::string id = b32normalise(machine);
    wchar_t pd[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? pd : L"C:\\ProgramData";
    dir += L"\\OctopusEAX";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\gate";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring p = dir + L"\\";
    for (char ch : id) p += (wchar_t)(unsigned char)ch;
    p += L".key";
    return p;
}
inline bool writeGate(const std::string& machine, const std::string& key) {
    HANDLE h = CreateFileW(gatePath(machine).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, key.data(), (DWORD)key.size(), &w, nullptr);
    CloseHandle(h);
    return ok != 0;
}
inline std::string readGateFile(const std::string& machine) {
    HANDLE h = CreateFileW(gatePath(machine).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[1200];
    DWORD n = 0;
    if (!ReadFile(h, buf, 1190, &n, nullptr)) n = 0;
    CloseHandle(h);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) --n;
    return std::string(buf, buf + n);
}
inline std::string readGateHere() {
    const MachineIds known = knownIds(false);
    for (const std::string& id : known.id) {
        if (id.empty()) continue;
        std::string k = readGateFile(id);
        if (!k.empty() && belongsHere(k)) return k;
    }
    return {};
}

inline void settleKey() {
    const std::string user = readFrom(HKEY_CURRENT_USER), machine = readFrom(HKEY_LOCAL_MACHINE);
    if (machine.empty() || b32normalise(user) == b32normalise(machine)) return;
    if (signedOutFrom(user) || signedOutFrom(machine)) return;
    const LicState mine = user.empty() ? LIC_NONE : inspectKey(user);
    if (mine == LIC_OK) {
        writeTo(HKEY_LOCAL_MACHINE, user);
        return;
    }
    if (mine == LIC_PAUSED || mine == LIC_STOPPED) return;
    if (user.empty() || inspectKey(machine) == LIC_OK) writeTo(HKEY_CURRENT_USER, machine);
}

inline LicState inspect() {
    if (revoked()) return LIC_STOPPED;
    if (debuggerPresent()) return LIC_BAD;
    const std::string k = loadKey();
    tickClock(k);
    return inspectKey(k);
}

inline bool activated() {
    return inspect() == LIC_OK;
}

}
