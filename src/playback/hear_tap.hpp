#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "recorder.hpp"
#include "sound_convert.hpp"

namespace clip {

class HearTap {
public:
    enum { IDLE, LISTENING, FAILED };

    ~HearTap() { stop(); }

    void start(int fs, std::function<std::wstring()> device, Recorder* live, Recorder* back) {
        if (worker_.joinable()) return;
        quit_ = false;
        worker_ = std::thread([this, fs, device, live, back] { run(fs, device, live, back); });
    }

    void stop() {
        quit_ = true;
        if (worker_.joinable()) worker_.join();
        state_ = IDLE;
    }

    bool running() const { return worker_.joinable(); }
    int state() const { return state_.load(); }

private:
    static LONGLONG nowHns() {
        LARGE_INTEGER c, f;
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&f);
        return (LONGLONG)((double)c.QuadPart * 10000000.0 / (double)f.QuadPart);
    }

    static IAudioClient* open(IMMDevice* dev, int fs, StereoConverter& conv) {
        IAudioClient* ac = nullptr;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || !ac) return nullptr;
        WAVEFORMATEX want = {};
        want.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        want.nChannels = 2;
        want.nSamplesPerSec = (DWORD)fs;
        want.wBitsPerSample = 32;
        want.nBlockAlign = 8;
        want.nAvgBytesPerSec = (DWORD)fs * 8;
        const REFERENCE_TIME buffer = 2000000;
        if (SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     buffer, 0, &want, nullptr))) {
            conv.setup(&want, fs);
            return ac;
        }
        ac->Release();
        ac = nullptr;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || !ac) return nullptr;
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(ac->GetMixFormat(&mix)) || !mix) {
            ac->Release();
            return nullptr;
        }
        const bool usable = mix->nSamplesPerSec >= 8000 && (mix->wBitsPerSample == 16 || mix->wBitsPerSample == 24 || mix->wBitsPerSample == 32);
        const bool ok = usable && SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, buffer, 0, mix, nullptr));
        if (ok) conv.setup(mix, fs);
        CoTaskMemFree(mix);
        if (ok) return ac;
        ac->Release();
        return nullptr;
    }

    void run(int fs, std::function<std::wstring()> device, Recorder* live, Recorder* back) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) en = nullptr;
        std::vector<float> quiet;
        auto give = [&](const float* lr, size_t frames) {
            live->feed(lr, frames);
            back->feed(lr, frames);
        };
        const LONGLONG GAP_MOST = 50000000;
        auto silence = [&](size_t frames) {
            frames = std::min<size_t>(frames, (size_t)(GAP_MOST / 10000000 * fs));
            while (frames) {
                const size_t n = std::min<size_t>(frames, (size_t)fs / 10);
                quiet.assign(n * 2, 0.0f);
                give(quiet.data(), n);
                frames -= n;
            }
        };
        LONGLONG ended = 0;
        while (!quit_.load()) {
            const std::wstring asked = device();
            IMMDevice* dev = nullptr;
            if (en) {
                if (!asked.empty()) en->GetDevice(asked.c_str(), &dev);
                if (!dev) en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
            }
            StereoConverter conv;
            IAudioClient* ac = dev ? open(dev, fs, conv) : nullptr;
            IAudioCaptureClient* cap = nullptr;
            if (ac && (FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap)) || FAILED(ac->Start()))) {
                if (cap) cap->Release();
                cap = nullptr;
            }
            if (!cap) {
                if (ac) ac->Release();
                if (dev) dev->Release();
                state_ = FAILED;
                for (int i = 0; i < 20 && !quit_.load(); ++i) Sleep(50);
                continue;
            }
            state_ = LISTENING;
            const double hnsPerFrame = 10000000.0 / conv.rate;
            LONGLONG next = ended ? std::max(ended, nowHns() - GAP_MOST) : nowHns();
            ULONGLONG checked = GetTickCount64();
            bool lost = false;
            while (!quit_.load() && !lost) {
                Sleep(10);
                UINT32 packet = 0;
                bool got = false;
                for (;;) {
                    if (FAILED(cap->GetNextPacketSize(&packet))) { lost = true; break; }
                    if (!packet) break;
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    UINT64 devPos = 0, qpc = 0;
                    if (FAILED(cap->GetBuffer(&data, &frames, &flags, &devPos, &qpc))) { lost = true; break; }
                    got = true;
                    const LONGLONG at = qpc ? (LONGLONG)qpc : next;
                    size_t skip = 0;
                    if (at > next + 200000) {
                        silence((size_t)((at - next) / hnsPerFrame * fs / conv.rate));
                    } else if (at < next - 200000) {
                        skip = std::min<size_t>(frames, (size_t)((next - at) / hnsPerFrame));
                    }
                    if (skip < frames) {
                        const std::vector<float>& lr = conv.convert(data + skip * (size_t)(conv.channels * (conv.bits / 8)), frames - skip, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                        give(lr.data(), lr.size() / 2);
                    }
                    next = std::max(next, at + (LONGLONG)(frames * hnsPerFrame));
                    cap->ReleaseBuffer(frames);
                }
                const LONGLONG now = nowHns();
                if (!got && now - next > 1000000) {
                    const LONGLONG upTo = now - 600000;
                    silence((size_t)((upTo - next) / 10000000.0 * fs));
                    next = upTo;
                }
                if (GetTickCount64() - checked >= 500) {
                    checked = GetTickCount64();
                    if (device() != asked) lost = true;
                }
            }
            ac->Stop();
            cap->Release();
            ac->Release();
            dev->Release();
            ended = next;
            if (lost && !quit_.load()) Sleep(200);
        }
        if (en) en->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        state_ = IDLE;
    }

    std::thread worker_;
    std::atomic<bool> quit_{false};
    std::atomic<int> state_{IDLE};
};

}
