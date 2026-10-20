#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "clip_util.hpp"
#include "sound_convert.hpp"

#ifndef MF_VERSION
#define MF_VERSION 0x00020070
#endif

namespace clip {
enum State { IDLE = 0, LOADING = 1, PLAYING = 2, DONE = 3, FAILED = 4 };

struct Player {
    static constexpr double REFILL_SEC = 0.5, REFILL_MOST_SEC = 2.0;

    std::atomic<int>  state{IDLE};
    std::atomic<bool> stopReq{false};
    std::atomic<int>  gen{0};
    std::atomic<bool> ended{false};
    std::atomic<bool> paused{false};
    std::atomic<void*> helper{nullptr};
    std::atomic<bool> helperBroken{false};
    std::atomic<double> durationSec{0.0};
    std::atomic<double> positionSec{0.0};
    std::atomic<double> seekTo{-1.0};
    std::atomic<unsigned> cuts{0};
    std::atomic<int> working{0};
    std::mutex mtx;
    std::wstring msg, lastUrl;
    std::vector<float> ring;
    size_t rHead = 0, rTail = 0, rCount = 0;
    std::thread th;
    double fs = 48000.0;
    double base = 0.0;
    long long pulled = 0;
    size_t prebuf = 0;
    double refill = REFILL_SEC;

    ~Player() { stopReq = true; if (th.joinable()) th.detach(); }

    std::wstring message() { std::lock_guard<std::mutex> g(mtx); return msg; }
    void setMsg(const std::wstring& m) { std::lock_guard<std::mutex> g(mtx); msg = m; }

    void start(const std::wstring& url, double from = 0.0) {
        stop();
        stopReq = false; ended = false; seekTo = -1.0; paused = false;
        {
            std::lock_guard<std::mutex> g(mtx);
            if (ring.empty()) ring.assign((size_t)(fs * 2 * 6), 0.f);
            prebuf = (size_t)(fs * 2 * 1.0);
            refill = REFILL_SEC;
            rHead = rTail = rCount = 0; msg = L"Opening...";
            lastUrl = url; base = from; pulled = 0;
        }
        ++cuts;
        positionSec = from;
        durationSec = 0.0;
        state = LOADING;
        const int myGen = ++gen;
        th = std::thread([this, url, myGen, from] { decode(url, myGen, from); });
    }
    void stop() {
        stopReq = true;
        if (void* hp = helper.exchange(nullptr)) endHelper((HANDLE)hp);
        if (th.joinable()) th.detach();
        ++gen;
        { std::lock_guard<std::mutex> g(mtx); rHead = rTail = rCount = 0; }
        ++cuts;
        state = IDLE; ended = false; paused = false;
        positionSec = 0.0;
    }
    bool settle(ULONGLONG ms) {
        stop();
        const ULONGLONG until = GetTickCount64() + ms;
        while (working.load() > 0 && GetTickCount64() < until) Sleep(10);
        return working.load() == 0;
    }
    void pause() { if (active()) paused = true; }
    void resume() { paused = false; }
    bool isPaused() const { return paused.load() && active(); }
    bool active() const { const int s = state.load(); return s == LOADING || s == PLAYING; }
    bool seekable() const { const int s = state.load(); return (s == LOADING || s == PLAYING || s == DONE) && durationSec.load() > 0.0; }

    void seek(double sec) {
        if (!seekable()) return;
        const double d = durationSec.load();
        if (sec < 0.0) sec = 0.0;
        if (sec > d - 0.5) sec = d > 0.5 ? d - 0.5 : 0.0;
        { wchar_t t[96]; swprintf_s(t, 96, L"seek to %.2f s (at %.2f, state %d)", sec, positionSec.load(), state.load()); logLine(t); }
        if (state.load() == DONE || ended.load()) {
            std::wstring url;
            { std::lock_guard<std::mutex> g(mtx); url = lastUrl; }
            const double keep = d;
            start(url, sec);
            durationSec = keep;
            return;
        }
        seekTo = sec;
        positionSec = sec;
    }
    void skip(double delta) { seek(positionSec.load() + delta); }

    void pull(float* L, float* R, int n) {
        std::lock_guard<std::mutex> g(mtx);
        int i = 0;
        if (state.load() == LOADING && (rCount >= prebuf || ended.load())) state = PLAYING;
        if (!ring.empty() && state.load() == PLAYING && !paused.load())
            for (; i < n && rCount >= 2; ++i) {
                L[i] = ring[rHead]; rHead = (rHead + 1) % ring.size();
                R[i] = ring[rHead]; rHead = (rHead + 1) % ring.size();
                rCount -= 2;
            }
        if (i > 0 && seekTo.load() < 0.0) { pulled += i; positionSec = base + pulled / fs; }
        if (i < n && state.load() == PLAYING && !ended.load() && !paused.load() && seekTo.load() < 0.0) {
            state = LOADING;
            prebuf = (size_t)(fs * 2 * refill);
            refill = std::min(refill * 2.0, REFILL_MOST_SEC);
        }
        for (; i < n; ++i) L[i] = R[i] = 0.f;
        if (rCount < 2 && ended.load() && state.load() == PLAYING) state = DONE;
    }

private:
    static constexpr size_t PUSH_STEP = 4096;

    bool stale(int myGen) const { return stopReq.load() || gen.load() != myGen; }

    static void endHelper(HANDLE h) {
        if (!TerminateJobObject(h, 1)) TerminateProcess(h, 1);
        CloseHandle(h);
    }

    bool push(const float* d, size_t nFloats, int myGen) {
        size_t done = 0;
        while (done < nFloats) {
            if (stale(myGen)) return false;
            if (seekTo.load() >= 0.0) return true;
            bool full = false;
            {
                std::lock_guard<std::mutex> g(mtx);
                if (gen.load() != myGen) return false;
                const size_t room = ring.size() - rCount;
                const size_t k = std::min(std::min(room, nFloats - done), PUSH_STEP);
                for (size_t j = 0; j < k; ++j) { ring[rTail] = d[done + j]; rTail = (rTail + 1) % ring.size(); }
                rCount += k; done += k;
                full = k == room;
            }
            if (full && done < nFloats) Sleep(5);
        }
        return true;
    }
    void fail(const std::wstring& m, int myGen) {
        logLine(L"FAILED: " + m);
        if (gen.load() == myGen && !stopReq.load()) { setMsg(m); state = FAILED; }
    }

    bool runHelper(const std::wstring& args, std::string& out, int myGen) {
        if (stale(myGen)) return false;
        SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        std::wstring cmd = L"\"" + ytdlpPath() + L"\" " + args;
        logLine(L"run: " + args);
        STARTUPINFOW si = {}; si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        const std::wstring cwd = toolsDir();
        BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                 nullptr, cwd.c_str(), &si, &pi);
        CloseHandle(wr);
        if (!ok) { CloseHandle(rd); logLine(L"could not start the helper"); helperBroken = true; return false; }
        HANDLE ender = CreateJobObjectW(nullptr, nullptr);
        if (ender) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
            lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(ender, JobObjectExtendedLimitInformation, &lim, sizeof lim) || !AssignProcessToJobObject(ender, pi.hProcess)) {
                CloseHandle(ender);
                ender = nullptr;
            }
        }
        if (!ender && !DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &ender, 0, FALSE, DUPLICATE_SAME_ACCESS)) ender = nullptr;
        void* idle = nullptr;
        const bool owned = ender && helper.compare_exchange_strong(idle, (void*)ender);
        if (ender && (!owned || stale(myGen))) {
            void* mine = (void*)ender;
            if (!owned || helper.compare_exchange_strong(mine, nullptr)) endHelper(ender);
            ender = nullptr;
        }
        ResumeThread(pi.hThread);
        char tmp[4096]; DWORD got = 0;
        while (ReadFile(rd, tmp, sizeof tmp, &got, nullptr) && got) out.append(tmp, got);
        CloseHandle(rd);
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD ec = 1; GetExitCodeProcess(pi.hProcess, &ec);
        if (out.find("PyInstaller") != std::string::npos || out.find("PKG archive") != std::string::npos) { helperBroken = true; ec = 1; }
        if (ender) {
            void* mine = (void*)ender;
            if (helper.compare_exchange_strong(mine, nullptr)) CloseHandle(ender);
        }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        {
            std::vector<std::string> lines; size_t p = 0;
            while (p < out.size()) {
                size_t e = out.find('\n', p); if (e == std::string::npos) e = out.size();
                std::string ln = out.substr(p, e - p);
                while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
                if (!ln.empty()) lines.push_back(ln);
                p = e + 1;
            }
            for (size_t i = lines.size() > 6 ? lines.size() - 6 : 0; i < lines.size(); ++i) logLine(L"  | " + widen(lines[i]));
            wchar_t s[64]; swprintf_s(s, 64, L"helper exit code %lu", (unsigned long)ec); logLine(s);
        }
        return ec == 0 && !stale(myGen);
    }

    static std::wstring quotedUrl(const std::wstring& url) {
        std::wstring s;
        for (const wchar_t c : url) {
            if (c == L'"' || c == L'\\' || c == L' ' || c < 0x20 || c == 0x7F) {
                wchar_t hex[4];
                swprintf_s(hex, 4, L"%%%02X", (unsigned)c);
                s += hex;
            } else {
                s += c;
            }
        }
        return s;
    }

    std::wstring fetchPage(const std::wstring& url, const wchar_t* fmt, const std::wstring& tag,
                              std::wstring& why, int myGen) {
        const std::wstring dir = toolsDir();
        WIN32_FIND_DATAW fd;

        HANDLE hf = FindFirstFileW((dir + L"\\clip*.*").c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do { std::wstring n = fd.cFileName;
                 if (n != L"clip.log") DeleteFileW((dir + L"\\" + n).c_str());
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        const std::wstring stem = L"clip_" + tag;
        const std::wstring args = L"--no-playlist --playlist-items 1 --no-warnings --no-progress -f \"" + std::wstring(fmt) +
                                  L"\" -o \"" + dir + L"\\" + stem + L".%(ext)s\" -- \"" + quotedUrl(url) + L"\"";
        std::string out;
        const bool ok = runHelper(args, out, myGen);
        if (stale(myGen)) return L"";
        if (ok) {
            std::wstring found;
            HANDLE h2 = FindFirstFileW((dir + L"\\" + stem + L".*").c_str(), &fd);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    std::wstring n = fd.cFileName;
                    if (n.size() > 5 && n.substr(n.size() - 5) == L".part") continue;
                    found = dir + L"\\" + n;
                } while (FindNextFileW(h2, &fd));
                FindClose(h2);
            }
            if (found.empty()) { why = L"The download helper didn't save anything"; logLine(why); return L""; }
            const long long sz = fileSize(found);
            wchar_t s[512]; swprintf_s(s, 512, L"got %ls (%lld bytes)", found.c_str(), sz); logLine(s);
            if (sz < 20000) { why = L"The site refused the download"; logLine(why); return L""; }
            return found;
        }
        std::string last; size_t p = 0;
        while (p < out.size()) {
            size_t e = out.find('\n', p); if (e == std::string::npos) e = out.size();
            std::string ln = out.substr(p, e - p);
            while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
            if (!ln.empty()) last = ln;
            p = e + 1;
        }
        if (last.empty()) last = "the helper could not fetch that link";
        std::wstring w = widen(last);
        if (w.size() > 120) w = w.substr(0, 117) + L"...";
        why = w;
        return L"";
    }

    bool position(IMFSourceReader* rd, double sec) {
        PROPVARIANT var; PropVariantInit(&var);
        var.vt = VT_I8; var.hVal.QuadPart = (LONGLONG)(sec * 10000000.0);
        rd->Flush((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        const HRESULT hr = rd->SetCurrentPosition(GUID_NULL, var);
        PropVariantClear(&var);
        return SUCCEEDED(hr);
    }

    bool restartFrom(double sec, int myGen) {
        std::lock_guard<std::mutex> g(mtx);
        if (gen.load() != myGen || stopReq.load()) return false;
        rHead = rTail = rCount = 0;
        base = sec; pulled = 0;
        prebuf = (size_t)(fs * 2 * 0.3);
        positionSec = sec;
        ++cuts;
        return true;
    }

    bool decodeOnce(const std::wstring& url, int myGen, std::wstring& why, double& from) {
        logLine(L"open: " + url);
        IMFAttributes* attr = nullptr;
        MFCreateAttributes(&attr, 1);
        if (attr) attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        IMFSourceReader* rd = nullptr;
        HRESULT hr = MFCreateSourceReaderFromURL(url.c_str(), attr, &rd);
        if (attr) attr->Release();
        if (FAILED(hr) || !rd) {
            wchar_t s[128]; swprintf_s(s, 128, L"MFCreateSourceReaderFromURL failed 0x%08lx", (unsigned long)hr); logLine(s);
            why = L"Can't open that link";
            return false;
        }
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        StereoConverter conv;
        if (!pickStereoOutput(rd, fs, conv)) {
            logLine(L"no audio type could be set (no decoder for it)");
            rd->Release();
            why = L"Windows can't play this format";
            return false;
        }
        if (!conv.direct) logLine(L"converting the sound to 48 kHz stereo here");
        {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)) && var.vt == VT_UI8 && !stale(myGen))
                durationSec = var.uhVal.QuadPart / 10000000.0;
            PropVariantClear(&var);
        }
        if (from > 0.0) {
            const double at = from;
            from = 0.0;
            restartFrom(position(rd, at) ? at : 0.0, myGen);
            conv.reset();
        }
        if (!stale(myGen)) setMsg(L"Playing");
        bool first = true; long samples = 0;
        for (;;) {
            if (stale(myGen)) break;
            const double want = seekTo.load();
            if (want >= 0.0) {
                seekTo = -1.0;
                const bool moved = position(rd, want);
                if (moved && restartFrom(want, myGen)) { ended = false; state = LOADING; conv.reset(); }
                { wchar_t t[96]; swprintf_s(t, 96, L"seek %s at %.2f s", moved ? L"applied" : L"REFUSED", want); logLine(t); }
            }
            DWORD flags = 0; IMFSample* s = nullptr;
            hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s);
            if (FAILED(hr)) { wchar_t t[96]; swprintf_s(t, 96, L"ReadSample failed 0x%08lx", (unsigned long)hr); logLine(t); why = L"The sound stopped halfway"; break; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (s) s->Release();
                if (seekTo.load() >= 0.0) continue;
                break;
            }
            if (!s) continue;
            IMFMediaBuffer* b = nullptr;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && b) {
                BYTE* d = nullptr; DWORD len = 0;
                if (SUCCEEDED(b->Lock(&d, nullptr, &len)) && d) {
                    first = false;
                    const float* lr = (const float*)d;
                    size_t floats = len / sizeof(float);
                    if (!conv.direct) {
                        const std::vector<float>& made = conv.convert(d, len / conv.frameBytes(), false);
                        lr = made.data();
                        floats = made.size();
                    }
                    samples += (long)(floats / 2);
                    const bool ok = push(lr, floats, myGen);
                    b->Unlock();
                    if (!ok) { b->Release(); s->Release(); break; }
                }
                b->Release();
            }
            s->Release();
        }
        rd->Release();
        { wchar_t t[96]; swprintf_s(t, 96, L"decoded %ld frames (%.1f s)", samples, samples / fs); logLine(t); }
        return !first;
    }

    void decode(std::wstring url, int myGen, double from) {
        struct Busy {
            std::atomic<int>& n;
            explicit Busy(std::atomic<int>& count) : n(count) { ++n; }
            ~Busy() { --n; }
        } busy(working);
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!mediaReady()) {
            fail(NO_MEDIA, myGen);
            if (SUCCEEDED(com)) CoUninitialize();
            return;
        }
        logLine(L"---- start: " + url);
        std::wstring why;
        bool gotSound = false;
        if (isWebPage(url)) {
            setMsg(L"Getting the sound from that page...");
            if (!ensureYtDlp(why, false, [this, myGen] { return stale(myGen); })) { fail(why, myGen); if (SUCCEEDED(com)) CoUninitialize(); return; }
            bool updated = false;
            std::wstring lastFile;
            for (int attempt = 0; attempt < 3 && !stale(myGen); ++attempt) {
                wchar_t tag[32]; swprintf_s(tag, 32, L"%d_%d", myGen, attempt);
                std::wstring file = fetchPage(url, FETCH_FORMATS[attempt], tag, why, myGen);
                if (stale(myGen)) break;
                if (file.empty()) {
                    if (!updated) {
                        updated = true;
                        if (helperBroken.exchange(false)) {
                            setMsg(L"Fixing the download helper...");
                            if (!ensureYtDlp(why, true, [this, myGen] { return stale(myGen); })) break;
                        } else {
                            setMsg(L"Updating the download helper...");
                            std::string upd; runHelper(L"-U", upd, myGen);
                        }
                        if (stale(myGen)) break;
                        setMsg(L"Getting the sound from that page...");
                        file = fetchPage(url, FETCH_FORMATS[attempt], tag, why, myGen);
                        if (stale(myGen)) break;
                    }
                    if (file.empty()) continue;
                }
                lastFile = file;
                { std::lock_guard<std::mutex> g(mtx); lastUrl = file; }
                std::wstring w2;
                if (decodeOnce(file, myGen, w2, from)) { gotSound = true; break; }
                if (stale(myGen)) break;
                why = w2;
                setMsg(L"No sound in that one, trying another format...");
            }
            if (!gotSound && !stale(myGen)) {
                std::wstring nm = lastFile.empty() ? L"" : lastFile.substr(lastFile.find_last_of(L"\\/") + 1);
                if (why.empty() || why == L"The sound stopped halfway" || why == L"Windows can't play this format" ||
                    why == L"Can't open that link")
                    why = nm.empty() ? L"Couldn't get any sound from that page"
                                     : L"Windows can't play " + nm;
                fail(why, myGen);
            }
        } else {
            gotSound = decodeOnce(url, myGen, why, from);
            if (!gotSound && !stale(myGen)) {
                if (why.empty()) why = L"No sound came out of that link";
                fail(why, myGen);
            }
        }
        if (gotSound && !stale(myGen)) { ended = true; logLine(L"done"); }
        else if (!gotSound && !stale(myGen) && state.load() == LOADING) state = FAILED;
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

}
