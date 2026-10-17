#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fontfile {

using Bytes = std::vector<unsigned char>;

inline uint16_t be16(const Bytes& b, size_t at) { return at + 2 <= b.size() ? (uint16_t)(b[at] << 8 | b[at + 1]) : 0; }
inline uint32_t be32(const Bytes& b, size_t at) { return at + 4 <= b.size() ? (uint32_t)be16(b, at) << 16 | be16(b, at + 2) : 0; }

inline std::wstring familyAt(const Bytes& b, size_t face) {
    const uint16_t tables = be16(b, face + 4);
    for (uint16_t t = 0; t < tables; ++t) {
        const size_t rec = face + 12 + 16 * (size_t)t;
        if (rec + 16 > b.size()) break;
        if (memcmp(&b[rec], "name", 4) != 0) continue;
        const size_t name = be32(b, rec + 8), strings = name + be16(b, name + 4);
        const uint16_t count = be16(b, name + 2);
        std::wstring best;
        int bestRank = 0;
        size_t budget = 1 << 16;
        for (uint16_t i = 0; i < count && bestRank < 2 && budget; ++i) {
            const size_t r = name + 6 + 12 * (size_t)i;
            if (r + 12 > b.size()) break;
            const uint16_t platform = be16(b, r), encoding = be16(b, r + 2), language = be16(b, r + 4), id = be16(b, r + 6);
            const size_t len = be16(b, r + 8), at = strings + be16(b, r + 10);
            if (id != 1 || platform != 3 || (encoding != 0 && encoding != 1 && encoding != 10) || at + len > b.size()) continue;
            std::wstring s;
            const size_t take = std::min(len, std::min<size_t>(512, budget));
            budget -= take;
            for (size_t k = 0; k + 1 < take; k += 2)
                if (const wchar_t c = (wchar_t)be16(b, at + k)) s.push_back(c);
            const int rank = language == 0x0409 ? 2 : 1;
            if (!s.empty() && rank > bestRank) {
                best = s;
                bestRank = rank;
            }
        }
        return best;
    }
    return L"";
}

inline std::vector<std::wstring> families(const std::wstring& path) {
    std::vector<std::wstring> out;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    Bytes b;
    if (GetFileSizeEx(f, &size) && size.QuadPart >= 12 && size.QuadPart < (64LL << 20)) {
        b.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, b.data(), (DWORD)b.size(), &got, nullptr) || got != b.size()) b.clear();
    }
    CloseHandle(f);
    if (b.size() < 12) return out;
    std::vector<size_t> faces;
    if (memcmp(b.data(), "ttcf", 4) == 0) {
        for (uint32_t i = 0, n = std::min<uint32_t>(be32(b, 8), 64); i < n; ++i) faces.push_back(be32(b, 12 + 4 * (size_t)i));
    } else {
        faces.push_back(0);
    }
    for (size_t i = 0; i < faces.size(); ++i) {
        const size_t face = faces[i];
        if (std::find(faces.begin(), faces.begin() + (std::ptrdiff_t)i, face) != faces.begin() + (std::ptrdiff_t)i) continue;
        const std::wstring name = familyAt(b, face);
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    return out;
}

}
