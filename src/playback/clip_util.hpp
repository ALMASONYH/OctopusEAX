#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <urlmon.h>
#include <wininet.h>
#include <string>
#include <cwctype>
#include <cstdio>
#include <functional>
#include <mutex>

namespace clip {

inline bool isWebPage(const std::wstring& u) {
    std::wstring l = u;
    for (auto& c : l) c = (wchar_t)towlower(c);
    if (l.compare(0, 7, L"http://") != 0 && l.compare(0, 8, L"https://") != 0) return false;
    size_t q = l.find_first_of(L"?#");
    std::wstring path = q == std::wstring::npos ? l : l.substr(0, q);
    static const wchar_t* media[] = {L".mp3", L".m4a", L".aac", L".wav", L".wma", L".flac", L".ogg", L".opus", L".mp4", L".webm", L".m4b", L".aif", L".aiff"};
    for (const wchar_t* e : media) {
        size_t n = wcslen(e);
        if (path.size() > n && path.compare(path.size() - n, n, e) == 0) return false;
    }
    return true;
}
static const wchar_t* const NO_MEDIA = L"Windows is missing its Media Feature Pack, install it to play sounds";

inline bool mediaReady() {
    static const bool ok = [] {
        wchar_t dir[MAX_PATH] = {0};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (!n || n >= MAX_PATH) return false;
        const std::wstring base(dir);
        return LoadLibraryW((base + L"\\mfplat.dll").c_str()) != nullptr && LoadLibraryW((base + L"\\mfreadwrite.dll").c_str()) != nullptr;
    }();
    return ok;
}

inline std::wstring toolsDir() {
    wchar_t pd[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring base = (n && n < MAX_PATH) ? pd : L"C:\\ProgramData";
    return base + L"\\OctopusEAX\\tools";
}
inline std::wstring ytdlpPath() { return toolsDir() + L"\\yt-dlp.exe"; }
inline long long fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}
inline std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

inline void logLine(const std::wstring& line) {
    const std::wstring path = toolsDir() + L"\\clip.log";
    const long long sz = fileSize(path);
    FILE* f = _wfopen(path.c_str(), sz > 200000 ? L"wb" : L"ab");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d  %s\n", t.wHour, t.wMinute, t.wSecond, narrow(line).c_str());
    fclose(f);
}

struct FetchWatch : IBindStatusCallback {
    ULONG announced = 0;
    std::function<bool()> giveUp;
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (id == IID_IUnknown || id == IID_IBindStatusCallback) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnStartBinding(DWORD, IBinding*) override { return E_NOTIMPL; }
    STDMETHODIMP GetPriority(LONG*) override { return E_NOTIMPL; }
    STDMETHODIMP OnLowResource(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP OnProgress(ULONG, ULONG total, ULONG, LPCWSTR) override {
        if (total > announced) announced = total;
        return giveUp && giveUp() ? E_ABORT : S_OK;
    }
    STDMETHODIMP OnStopBinding(HRESULT, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHODIMP GetBindInfo(DWORD*, BINDINFO*) override { return E_NOTIMPL; }
    STDMETHODIMP OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP OnObjectAvailable(REFIID, IUnknown*) override { return E_NOTIMPL; }
};

static const long long HELPER_MIN_BYTES = 4000000;

inline bool looksLikeHelper(const std::wstring& p) {
    if (fileSize(p) < HELPER_MIN_BYTES) return false;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    char mz[2] = {0, 0};
    const size_t n = fread(mz, 1, 2, f);
    fclose(f);
    return n == 2 && mz[0] == 'M' && mz[1] == 'Z';
}

inline bool ensureYtDlp(std::wstring& why, bool replace = false, std::function<bool()> giveUp = nullptr) {
    static std::mutex fetching;
    const std::lock_guard<std::mutex> one(fetching);
    const std::wstring exe = ytdlpPath(), part = exe + L".part";
    if (!replace && looksLikeHelper(exe)) return true;
    if (giveUp && giveUp()) return false;
    const std::wstring t = toolsDir();
    CreateDirectoryW(t.substr(0, t.find_last_of(L"\\/")).c_str(), nullptr);
    CreateDirectoryW(t.c_str(), nullptr);
    logLine(replace ? L"replacing yt-dlp.exe" : L"fetching yt-dlp.exe");
    static const wchar_t* const HELPER_URL = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
    DeleteUrlCacheEntryW(HELPER_URL);
    DeleteFileW(part.c_str());
    FetchWatch watch;
    watch.giveUp = giveUp;
    const HRESULT hr = URLDownloadToFileW(nullptr, HELPER_URL, part.c_str(), 0, &watch);
    const long long got = fileSize(part);
    { wchar_t s[160]; swprintf_s(s, 160, L"fetch result 0x%08lx, %lld bytes of %lu announced", (unsigned long)hr, got, (unsigned long)watch.announced); logLine(s); }
    const bool whole = SUCCEEDED(hr) && looksLikeHelper(part) && (watch.announced == 0 || got == (long long)watch.announced);
    if (!whole) {
        DeleteFileW(part.c_str());
        why = L"Couldn't download the helper. Check your connection and try again.";
        logLine(why);
        return false;
    }
    if (!MoveFileExW(part.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(part.c_str());
        why = L"Couldn't save the download helper. Your antivirus may be blocking it.";
        logLine(why);
        return false;
    }
    return true;
}

static const wchar_t* const FETCH_FORMATS[3] = {
    L"bestaudio[ext=m4a][protocol!*=m3u8]/bestaudio[ext=mp3][protocol!*=m3u8]/bestaudio[acodec^=mp4a][protocol!*=m3u8]/bestaudio[acodec=mp3][protocol!*=m3u8]",
    L"bestaudio[ext=webm][protocol!*=m3u8]/bestaudio[acodec=opus][protocol!*=m3u8]/bestaudio[protocol!*=m3u8]",
    L"bestaudio/best",
};

}
