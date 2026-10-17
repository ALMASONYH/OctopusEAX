#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/licensing/license.hpp"
#include "crypto/sealed_box.hpp"
#include "embedded/embedded_key.hpp"

static bool writeInc(const std::vector<unsigned char>& b, const char* path) {
    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) { printf("cannot write %s\n", path); return false; }
    for (size_t i = 0; i < b.size(); ++i)
        fprintf(f, "0x%02X%s", b[i],
                i + 1 == b.size() ? "\n" : ((i % 12 == 11) ? ",\n" : ","));
    fclose(f);
    return true;
}

static void printHash(const unsigned char table[lic::SIGNER_SLOTS][64]) {
    unsigned char d[32];
    if (!lic::sha256(std::string((const char*)table, (size_t)lic::SIGNER_SLOTS * 64), d)) return;
    auto row = [&](const char* name, int from, int n) {
        printf("static const unsigned char %s[%d] = {\n    ", name, n);
        for (int i = 0; i < n; ++i) printf("0x%02x%s", d[from + i], i + 1 == n ? "\n" : (i % 16 == 15 ? ",\n    " : ","));
        printf("};\n");
    };
    row("OWNER_HASH", 0, 32);
    row("OWNER_HASH_HEAD", 0, 16);
    row("OWNER_HASH_TAIL", 16, 16);
}

static int slotOf(const unsigned char priv[embedded::KEY_N]) {
    for (int s = 0; s < lic::SIGNER_SLOTS; ++s) {
        if (!lic::slotInUse(s) || memcmp(lic::SIGNERS[s], priv + 8, 64) != 0) continue;
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_KEY_HANDLE k = nullptr;
        bool ok = false;
        if (!BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) {
            if (!BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &k, (PUCHAR)priv, (ULONG)embedded::KEY_N, 0)) {
                unsigned char digest[32] = {0x4F, 0x43, 0x54}, sig[64];
                ULONG n = 0;
                ok = !BCryptSignHash(k, nullptr, digest, 32, sig, 64, &n, 0) && n == 64 && lic::verifyWith(lic::SIGNERS[s], digest, sig);
                BCryptDestroyKey(k);
            }
            BCryptCloseAlgorithmProvider(alg, 0);
        }
        if (ok) return s;
    }
    return -1;
}

static std::string cleanCode(const std::string& in) {
    return lic::b32normalise(in);
}

static bool openCek(const std::vector<std::string>& secrets, unsigned char cek[embedded::CEK_N]) {
    if (!embedded::present()) return false;
    const unsigned char* wraps = embedded::BLOB + 6 + embedded::BODY_N;
    for (const std::string& s : secrets)
        for (int i = 0; i < embedded::wrapCount(); ++i)
            if (crypt::unseal(wraps + (size_t)i * embedded::WRAP_N, embedded::CEK_N, s, cek)) return true;
    return false;
}

static int reseal(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: MakeSecret --reseal <file for the recovery code> [--code <recovery code>] [machine numbers...]\n");
        return 1;
    }
    const std::string codeFile = argv[2];
    std::vector<std::string> secrets = {embedded::ANYWHERE, embedded::ANYWHERE_LEGACY, embedded::ANYWHERE_MASON};
    const lic::MachineIds here = lic::knownIds(true);
    for (const std::string& id : here.id) if (!id.empty()) secrets.push_back(id);
    std::vector<std::string> machines;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--code" && i + 1 < argc) { secrets.push_back(cleanCode(argv[++i])); continue; }
        const std::string clean = lic::b32normalise(argv[i]);
        if (clean.size() != 16) { printf("not a machine number: %s\n", argv[i]); return 1; }
        machines.push_back(lic::group(clean, 4));
    }
    if (machines.empty())
        for (const std::string& id : here.id)
            if (!id.empty() && std::find(machines.begin(), machines.end(), id) == machines.end()) machines.push_back(id);
    if (machines.empty() || machines.size() > 200) { printf("no machine numbers\n"); return 1; }

    unsigned char cek[embedded::CEK_N];
    if (!openCek(secrets, cek)) { printf("the current generator secret does not open here\n"); return 2; }
    const unsigned char* body = embedded::BLOB + 6;
    unsigned char priv[embedded::KEY_N];
    const std::string cekAsSecret((const char*)cek, sizeof(cek));
    if (!crypt::unseal(body, embedded::KEY_N, cekAsSecret, priv)) { SecureZeroMemory(cek, sizeof(cek)); printf("the signing key does not open\n"); return 2; }
    const int slot = slotOf(priv);
    SecureZeroMemory(priv, sizeof(priv));
    if (slot < 0) { SecureZeroMemory(cek, sizeof(cek)); printf("the signing key matches no slot in vendor_key.inc; nothing written\n"); return 2; }

    unsigned char rnd[16];
    if (!crypt::randomBytes(rnd, sizeof(rnd))) { SecureZeroMemory(cek, sizeof(cek)); printf("no randomness\n"); return 2; }
    const std::string code = lic::b32encode(rnd, 16).substr(0, 25);
    SecureZeroMemory(rnd, sizeof(rnd));

    std::vector<unsigned char> blob = {'M', 'K', 'S', '1', (unsigned char)(machines.size() + 1), 0};
    blob.insert(blob.end(), body, body + embedded::BODY_N);
    std::vector<std::string> wrapSecrets = machines;
    wrapSecrets.push_back(code);
    for (const std::string& s : wrapSecrets) {
        std::vector<unsigned char> w;
        if (!crypt::seal(cek, sizeof(cek), s, w)) { SecureZeroMemory(cek, sizeof(cek)); printf("sealing failed\n"); return 2; }
        blob.insert(blob.end(), w.begin(), w.end());
    }

    bool allOpen = true, phraseOpens = false;
    const unsigned char* newWraps = blob.data() + 6 + embedded::BODY_N;
    for (size_t i = 0; i < wrapSecrets.size(); ++i) {
        unsigned char back[embedded::CEK_N];
        allOpen = allOpen && crypt::unseal(newWraps + i * embedded::WRAP_N, embedded::CEK_N, wrapSecrets[i], back) && memcmp(back, cek, sizeof(cek)) == 0;
        SecureZeroMemory(back, sizeof(back));
    }
    for (const char* p : {embedded::ANYWHERE, embedded::ANYWHERE_LEGACY, embedded::ANYWHERE_MASON})
        for (size_t i = 0; i < wrapSecrets.size(); ++i) {
            unsigned char back[embedded::CEK_N];
            if (crypt::unseal(newWraps + i * embedded::WRAP_N, embedded::CEK_N, p, back)) phraseOpens = true;
            SecureZeroMemory(back, sizeof(back));
        }
    SecureZeroMemory(cek, sizeof(cek));
    if (!allOpen || phraseOpens) { printf("self-check failed; nothing written\n"); return 2; }

    FILE* f = nullptr;
    fopen_s(&f, codeFile.c_str(), "w");
    if (!f) { printf("cannot write %s; nothing written\n", codeFile.c_str()); return 2; }
    fprintf(f, "%s\n", lic::group(code, 5).c_str());
    fclose(f);
    if (!writeInc(blob, "keygen_secret.new.inc")) return 2;

    printf("\nkeygen_secret.new.inc written: the same signing key (slot %d), now locked to %d machine number%s and the recovery code.\n",
           slot, (int)machines.size(), machines.size() == 1 ? "" : "s");
    for (const std::string& m : machines) printf("    %s\n", m.c_str());
    printf("The recovery code is in %s. Keep it off this PC.\n", codeFile.c_str());
    printf("On a new PC: MakeSecret --reseal <file> --code <recovery code>, then rebuild the generator.\n");
    printf("Customer keys do not change.\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--reseal") return reseal(argc, argv);

    std::vector<std::string> machines;
    for (int i = 1; i < argc; ++i) {
        std::string clean = lic::b32normalise(argv[i]);
        if (clean.size() != 16) {
            printf("not a machine number: %s\n", argv[i]);
            return 1;
        }
        machines.push_back(lic::group(clean, 4));
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)
        || BCryptGenerateKeyPair(alg, &k, 256, 0) || BCryptFinalizeKeyPair(k, 0)) {
        printf("key generation failed\n");
        return 2;
    }
    unsigned char priv[8 + 96], pub[8 + 64];
    ULONG n = 0;
    if (BCryptExportKey(k, nullptr, BCRYPT_ECCPRIVATE_BLOB, priv, sizeof(priv), &n, 0)
        || n != sizeof(priv)
        || BCryptExportKey(k, nullptr, BCRYPT_ECCPUBLIC_BLOB, pub, sizeof(pub), &n, 0)
        || n != sizeof(pub)) {
        printf("export failed\n");
        return 2;
    }
    BCryptDestroyKey(k);
    BCryptCloseAlgorithmProvider(alg, 0);

    unsigned char cek[embedded::CEK_N];
    if (!crypt::randomBytes(cek, sizeof(cek))) { printf("no randomness\n"); return 2; }
    std::string cekAsSecret((const char*)cek, sizeof(cek));

    std::vector<unsigned char> body;
    if (!crypt::seal(priv, embedded::KEY_N, cekAsSecret, body)) {
        printf("sealing the key failed\n");
        return 2;
    }

    std::vector<unsigned char> blob;
    blob.push_back('M'); blob.push_back('K'); blob.push_back('S'); blob.push_back('1');
    blob.push_back((unsigned char)(machines.empty() ? 1 : machines.size()));
    blob.push_back((unsigned char)(machines.empty() ? 1 : 0));
    blob.insert(blob.end(), body.begin(), body.end());

    if (machines.empty()) {
        std::vector<unsigned char> w;
        if (!crypt::seal(cek, sizeof(cek), embedded::ANYWHERE, w)) return 2;
        blob.insert(blob.end(), w.begin(), w.end());
    } else {
        for (const std::string& m : machines) {
            std::vector<unsigned char> w;
            if (!crypt::seal(cek, sizeof(cek), m, w)) return 2;
            blob.insert(blob.end(), w.begin(), w.end());
        }
    }
    SecureZeroMemory(cek, sizeof(cek));
    SecureZeroMemory(priv, sizeof(priv));
    if (!writeInc(blob, "keygen_secret.inc")) return 2;

    printf("\nkeygen_secret.inc written  (%d bytes)\n", (int)blob.size());
    if (machines.empty()) {
        printf("\n  THIS BUILD RUNS ON ANY MACHINE.\n"
               "  Whoever gets the generator can issue keys. Rebuild with\n"
               "  machine numbers listed if that is not what you want.\n");
    } else {
        printf("\n  locked to %d machine%s:\n", (int)machines.size(),
               machines.size() == 1 ? "" : "s");
        for (const std::string& m : machines) printf("    %s\n", m.c_str());
        printf("  Anywhere else the generator will not open.\n");
    }
    int freeSlot = -1;
    for (int s = 0; s < lic::SIGNER_SLOTS && freeSlot < 0; ++s) if (!lic::slotInUse(s)) freeSlot = s;
    if (freeSlot < 0) {
        printf("\nvendor_key.inc has no free slot: empty one first, then run this again.\n");
        return 3;
    }
    printf("\nput this in slot %d of src\\licensing\\vendor_key.inc:\n\n{\n", freeSlot);
    for (int i = 0; i < 64; ++i)
        printf("%s0x%02X%s", (i % 12 == 0 ? "    " : ""), pub[8 + i],
               i == 63 ? "\n" : ((i % 12 == 11) ? ",\n" : ","));
    printf("},\n\nand replace the three hash tables in src\\licensing\\license.hpp with:\n\n");
    unsigned char table[lic::SIGNER_SLOTS][64];
    memcpy(table, lic::SIGNERS, sizeof(table));
    memcpy(table[freeSlot], pub + 8, 64);
    printHash(table);
    printf("\nthen rebuild OctopusEAX and the generator.\n");
    return 0;
}
