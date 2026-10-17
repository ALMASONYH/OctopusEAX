#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <softpub.h>
#include <urlmon.h>
#include <wininet.h>
#include <wintrust.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <shldisp.h>
#include <oleauto.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace cable {

static const wchar_t* PAGE_URL = L"https://vb-audio.com/Cable/";
static const wchar_t* PACK_BASE = L"https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack";
static const int KNOWN_PACKS[] = {45, 43};

inline std::wstring folder() {
    wchar_t la[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    const std::wstring base = (n && n < MAX_PATH) ? std::wstring(la) : std::wstring(L"C:\\Users\\Public");
    return base + L"\\OctopusEAX\\vbcable";
}

inline void makeFolder(const std::wstring& dir) {
    const size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos) CreateDirectoryW(dir.substr(0, cut).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
}

inline bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline long long sizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return -1;
    return ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

inline std::wstring setupIn(const std::wstring& dir) {
    static const wchar_t* NAMES[] = {L"VBCABLE_Setup_x64.exe", L"VBCABLE_Setup.exe"};
    for (const wchar_t* n : NAMES) {
        const std::wstring direct = dir + L"\\" + n;
        if (exists(direct)) return direct;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        for (const wchar_t* n : NAMES) {
            const std::wstring inner = dir + L"\\" + fd.cFileName + L"\\" + n;
            if (exists(inner)) { found = inner; break; }
        }
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

inline bool fetch(const std::wstring& url, const std::wstring& file) {
    DeleteUrlCacheEntryW(url.c_str());
    DeleteFileW(file.c_str());
    return SUCCEEDED(URLDownloadToFileW(nullptr, url.c_str(), file.c_str(), 0, nullptr)) && sizeOf(file) > 0;
}

inline std::vector<std::wstring> packUrls(const std::wstring& pageFile) {
    std::vector<int> packs;
    if (!pageFile.empty()) {
        FILE* f = _wfopen(pageFile.c_str(), L"rb");
        if (f) {
            std::string page;
            char buf[8192];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, f)) > 0) page.append(buf, got);
            fclose(f);
            const std::string mark = "Download_CABLE/VBCABLE_Driver_Pack";
            for (size_t at = page.find(mark); at != std::string::npos; at = page.find(mark, at + 1)) {
                size_t p = at + mark.size();
                int num = 0, digits = 0;
                while (p < page.size() && page[p] >= '0' && page[p] <= '9' && digits < 4) { num = num * 10 + (page[p] - '0'); ++p; ++digits; }
                if (digits > 0 && page.compare(p, 4, ".zip") == 0) packs.push_back(num);
            }
        }
    }
    for (int k : KNOWN_PACKS) packs.push_back(k);
    std::sort(packs.begin(), packs.end(), [](int a, int b) { return a > b; });
    packs.erase(std::unique(packs.begin(), packs.end()), packs.end());
    std::vector<std::wstring> urls;
    for (int k : packs) urls.push_back(std::wstring(PACK_BASE) + std::to_wstring(k) + L".zip");
    return urls;
}

inline long long treeSize(const std::wstring& dir) {
    long long total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += treeSize(dir + L"\\" + fd.cFileName);
        else total += ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

inline bool shellUnzip(const std::wstring& zip, const std::wstring& dir) {
    IShellDispatch* shell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER, IID_IShellDispatch, (void**)&shell)) || !shell) return false;
    VARIANT from, to;
    VariantInit(&from);
    VariantInit(&to);
    from.vt = VT_BSTR;
    from.bstrVal = SysAllocString(zip.c_str());
    to.vt = VT_BSTR;
    to.bstrVal = SysAllocString(dir.c_str());
    Folder* src = nullptr;
    Folder* dst = nullptr;
    FolderItems* items = nullptr;
    bool started = false;
    if (SUCCEEDED(shell->NameSpace(from, &src)) && src && SUCCEEDED(shell->NameSpace(to, &dst)) && dst &&
        SUCCEEDED(src->Items(&items)) && items) {
        VARIANT what, how;
        VariantInit(&what);
        VariantInit(&how);
        what.vt = VT_DISPATCH;
        what.pdispVal = items;
        how.vt = VT_I4;
        how.lVal = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI;
        started = SUCCEEDED(dst->CopyHere(what, how));
    }
    if (items) items->Release();
    if (src) src->Release();
    if (dst) dst->Release();
    VariantClear(&from);
    VariantClear(&to);
    shell->Release();
    if (!started) return false;
    long long last = -1;
    int calm = 0;
    for (int i = 0; i < 240 && calm < 6; ++i) {
        Sleep(500);
        const long long now = treeSize(dir);
        calm = (now == last && !setupIn(dir).empty()) ? calm + 1 : 0;
        last = now;
    }
    return calm >= 6;
}

inline bool unzip(const std::wstring& zip, const std::wstring& dir) {
    wchar_t sys[MAX_PATH] = {0};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (!exists(tar)) return shellUnzip(zip, dir);
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ec == 0;
}

inline bool signedByVbAudio(HANDLE state) {
    CRYPT_PROVIDER_DATA* prov = WTHelperProvDataFromStateData(state);
    CRYPT_PROVIDER_SGNR* signer = prov ? WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0) : nullptr;
    if (!signer || !signer->csCertChain || !signer->pasCertChain || !signer->pasCertChain[0].pCert) return false;
    wchar_t name[256] = L"";
    CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
    std::wstring low = name;
    for (wchar_t& c : low) c = (wchar_t)towlower(c);
    return low.find(L"burel") != std::wstring::npos || low.find(L"vb-audio") != std::wstring::npos;
}

inline bool signedFile(const std::wstring& path, HANDLE opened = nullptr) {
    WINTRUST_FILE_INFO file = {};
    file.cbStruct = sizeof file;
    file.pcwszFilePath = path.c_str();
    file.hFile = opened;
    WINTRUST_DATA data = {};
    data.cbStruct = sizeof data;
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG rc = WinVerifyTrust(nullptr, &action, &data);
    const bool ours = rc == ERROR_SUCCESS && signedByVbAudio(data.hWVTStateData);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return ours;
}

inline bool isSetupName(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    const std::wstring name = cut == std::wstring::npos ? path : path.substr(cut + 1);
    return _wcsicmp(name.c_str(), L"VBCABLE_Setup_x64.exe") == 0 || _wcsicmp(name.c_str(), L"VBCABLE_Setup.exe") == 0;
}

}
