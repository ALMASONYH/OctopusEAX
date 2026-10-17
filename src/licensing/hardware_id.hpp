#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace lic {

inline std::string trimSerial(const std::string& in) {
    std::string t;
    for (char c : in)
        if ((unsigned char)c > ' ' && (unsigned char)c < 127) t += c;
    return t;
}

inline bool placeholderSerial(std::string v) {
    for (auto& c : v) c = (char)tolower((unsigned char)c);
    if (v.size() < 4) return true;
    static const char* junk[] = {
        "to be filled", "default string", "o.e.m", "not specified", "not available", "none",
        "unknown", "system serial number", "base board serial number", "123456789",
        "00000000", "ffffffff", "n/a", "empty", "invalid", "chassis"
    };
    for (const char* j : junk)
        if (v.find(j) != std::string::npos) return true;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] != v[0]) return false;
    return true;
}

inline std::string systemDiskSerial() {
    wchar_t win[MAX_PATH];
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return "";
    wchar_t vol[8] = {L'\\', L'\\', L'.', L'\\', win[0], L':', 0};
    HANDLE hv = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hv == INVALID_HANDLE_VALUE) return "";
    STORAGE_DEVICE_NUMBER sdn{};
    DWORD got = 0;
    const bool haveNumber = DeviceIoControl(hv, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                                            &sdn, sizeof sdn, &got, nullptr) != 0;
    CloseHandle(hv);
    if (!haveNumber) return "";
    wchar_t path[64];
    swprintf_s(path, 64, L"\\\\.\\PhysicalDrive%lu", (unsigned long)sdn.DeviceNumber);
    HANDLE hd = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hd == INVALID_HANDLE_VALUE) return "";
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<unsigned char> buf(4096);
    const bool ok = DeviceIoControl(hd, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                                    buf.data(), (DWORD)buf.size(), &got, nullptr) != 0;
    CloseHandle(hd);
    if (!ok || got < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return "";
    const STORAGE_DEVICE_DESCRIPTOR* dd = (const STORAGE_DEVICE_DESCRIPTOR*)buf.data();
    if (!dd->SerialNumberOffset || dd->SerialNumberOffset >= got) return "";
    buf[buf.size() - 1] = 0;
    const std::string s = trimSerial((const char*)buf.data() + dd->SerialNumberOffset);
    return placeholderSerial(s) ? "" : s;
}

inline std::string smbiosString(const unsigned char* strs, const unsigned char* end, int index) {
    if (index <= 0) return "";
    const unsigned char* s = strs;
    for (int k = 1; s < end; ++k) {
        const unsigned char* e = s;
        while (e < end && *e) ++e;
        if (k == index) return std::string((const char*)s, (const char*)e);
        if (e >= end) break;
        s = e + 1;
        if (s < end && *s == 0) break;
    }
    return "";
}

inline void readSmbios(std::string& sysSerial, std::string& boardSerial, std::string& uuid) {
    const DWORD provider = 0x52534D42;
    const UINT size = GetSystemFirmwareTable(provider, 0, nullptr, 0);
    if (size < 8) return;
    std::vector<unsigned char> raw(size);
    if (GetSystemFirmwareTable(provider, 0, raw.data(), size) != size) return;
    const unsigned char* p = raw.data() + 8;
    const unsigned char* end = raw.data() + size;
    while (p + 4 <= end) {
        const unsigned char type = p[0], len = p[1];
        if (len < 4 || p + len > end) break;
        const unsigned char* strs = p + len;
        const unsigned char* e = strs;
        while (e + 1 < end && !(e[0] == 0 && e[1] == 0)) ++e;
        if (type == 1 && len >= 0x08) {
            sysSerial = trimSerial(smbiosString(strs, e + 1, p[7]));
            if (len >= 0x19) {
                bool zeros = true, ones = true;
                for (int i = 0; i < 16; ++i) { if (p[8 + i] != 0x00) zeros = false; if (p[8 + i] != 0xFF) ones = false; }
                if (!zeros && !ones) {
                    char h[40];
                    for (int i = 0; i < 16; ++i) sprintf(h + 2 * i, "%02X", p[8 + i]);
                    h[32] = 0;
                    uuid = h;
                }
            }
        } else if (type == 2 && len >= 0x08) {
            boardSerial = trimSerial(smbiosString(strs, e + 1, p[7]));
        }
        if (type == 127) break;
        p = e + 2;
    }
}

inline std::string hardwareSerials() {
    std::string sys, board, uuid, out;
    readSmbios(sys, board, uuid);
    auto add = [&](const std::string& v) {
        if (!v.empty() && !placeholderSerial(v)) { out += '|'; out += v; }
    };
    add(systemDiskSerial());
    add(sys);
    add(board);
    add(uuid);
    return out;
}

}
