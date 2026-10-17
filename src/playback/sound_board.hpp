#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "clip_util.hpp"
#include "sound_convert.hpp"

#ifndef MF_VERSION
#define MF_VERSION 0x00020070
#endif

namespace clip {

struct Sound {
    std::vector<short> pcm;
    bool cut = false;

    size_t frames() const { return pcm.size() / 2; }
};

struct Outline {
    std::vector<float> peaks;
    double seconds = 0.0;
    int perSec = 1;
    float loudest = 0.0f;
};

inline IMFSourceReader* openReader(const std::wstring& path, double fs, std::wstring& why, StereoConverter& conv) {
    if (!mediaReady()) {
        why = NO_MEDIA;
        return nullptr;
    }
    IMFSourceReader* rd = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &rd)) || !rd) {
        why = L"Can't open that file";
        return nullptr;
    }
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (!pickStereoOutput(rd, fs, conv)) {
        rd->Release();
        why = L"Windows can't play this format";
        return nullptr;
    }
    return rd;
}

template <class Use>
inline void readFrames(IMFSourceReader* rd, StereoConverter& conv, Use use) {
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (s) s->Release();
            break;
        }
        if (!s) continue;
        bool more = true;
        IMFMediaBuffer* b = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && b) {
            BYTE* d = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(b->Lock(&d, nullptr, &len)) && d) {
                if (conv.direct) {
                    more = use((const float*)d, (size_t)(len / 8), ts);
                } else {
                    const std::vector<float>& lr = conv.convert(d, len / conv.frameBytes(), false);
                    more = use(lr.data(), lr.size() / 2, ts);
                }
                b->Unlock();
            }
            b->Release();
        }
        s->Release();
        if (!more) break;
    }
}

inline short toShort(float v) {
    const float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return (short)(c * 32767.0f);
}

inline void fadeEdges(Sound& sound, double fs, bool in, bool out) {
    const size_t n = sound.frames();
    const size_t len = std::min((size_t)(0.005 * fs), n / 4);
    for (size_t i = 0; i < len; ++i) {
        const float g = (float)i / (float)len;
        for (int ch = 0; ch < 2; ++ch) {
            if (in) sound.pcm[2 * i + ch] = (short)(sound.pcm[2 * i + ch] * g);
            if (out) sound.pcm[2 * (n - 1 - i) + ch] = (short)(sound.pcm[2 * (n - 1 - i) + ch] * g);
        }
    }
}

inline bool seeksExactly(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = path.substr(dot + 1);
    for (const wchar_t* exact : {L"wav", L"wave", L"flac", L"m4a", L"mp4"})
        if (_wcsicmp(ext.c_str(), exact) == 0) return true;
    return false;
}

inline std::shared_ptr<const Sound> decodePart(const std::wstring& path, double fs, double mostSec, std::wstring& why, double fromSec,
                                               double toSec, bool seek, const std::atomic<bool>* stop) {
    StereoConverter conv;
    IMFSourceReader* rd = openReader(path, fs, why, conv);
    if (!rd) return nullptr;
    const long long first = fromSec > 0.0 ? (long long)std::llround(fromSec * fs) : 0;
    const bool part = toSec > fromSec;
    long long last = first + (long long)(mostSec * fs);
    if (part) last = std::min(last, (long long)std::llround(toSec * fs));
    bool counting = true;
    LONGLONG origin = 0;
    if (first > 0 && seek) {
        readFrames(rd, conv, [&](const float*, size_t, LONGLONG ts) { origin = ts; return false; });
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = origin + (LONGLONG)(std::max(0.0, fromSec - 0.25) * 10000000.0);
        rd->Flush((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        counting = FAILED(rd->SetCurrentPosition(GUID_NULL, var));
        PropVariantClear(&var);
        conv.reset();
        if (counting) {
            rd->Release();
            rd = openReader(path, fs, why, conv);
            if (!rd) return nullptr;
        }
    }
    auto sound = std::make_shared<Sound>();
    long long at = 0;
    bool placed = counting, late = false;
    readFrames(rd, conv, [&](const float* f, size_t frames, LONGLONG ts) {
        if (!placed) {
            at = (long long)std::llround((double)(ts - origin) * fs / 10000000.0);
            placed = true;
            late = at > first;
            if (late) return false;
        }
        for (size_t i = 0; i < frames && at < last; ++i, ++at) {
            if (at < first) continue;
            sound->pcm.push_back(toShort(f[2 * i]));
            sound->pcm.push_back(toShort(f[2 * i + 1]));
        }
        return at < last && !(stop && stop->load());
    });
    rd->Release();
    if (stop && stop->load()) return nullptr;
    if (late) return decodePart(path, fs, mostSec, why, fromSec, toSec, false, stop);
    const bool stopped = at >= last;
    sound->cut = stopped && (!part || toSec - fromSec > mostSec);
    if (sound->pcm.empty()) {
        why = L"That file has no sound";
        return nullptr;
    }
    fadeEdges(*sound, fs, first > 0, stopped);
    return sound;
}

inline std::shared_ptr<const Sound> decodeSound(const std::wstring& path, double fs, double mostSec, std::wstring& why,
                                                double fromSec = 0.0, double toSec = 0.0, const std::atomic<bool>* stop = nullptr) {
    return decodePart(path, fs, mostSec, why, fromSec, toSec, seeksExactly(path), stop);
}

inline std::shared_ptr<const Outline> readOutline(const std::wstring& path, double fs, int perSec, std::atomic<int>& permille,
                                                  const std::atomic<bool>& stop, std::wstring& why) {
    StereoConverter conv;
    IMFSourceReader* rd = openReader(path, fs, why, conv);
    if (!rd) return nullptr;
    double total = 0.0;
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)) && var.vt == VT_UI8)
            total = var.uhVal.QuadPart / 10000000.0;
        PropVariantClear(&var);
    }
    auto out = std::make_shared<Outline>();
    out->perSec = perSec;
    const long long step = std::max(1LL, (long long)(fs / perSec));
    long long at = 0;
    float peak = 0.0f;
    readFrames(rd, conv, [&](const float* f, size_t frames, LONGLONG) {
        for (size_t i = 0; i < frames; ++i) {
            peak = std::max(peak, std::max(std::fabs(f[2 * i]), std::fabs(f[2 * i + 1])));
            if (++at % step == 0) {
                out->peaks.push_back(peak);
                peak = 0.0f;
            }
        }
        if (total > 0.0) permille = (int)std::min(999.0, at / fs / total * 1000.0);
        return !stop.load();
    });
    rd->Release();
    if (stop.load()) return nullptr;
    if (at % step) out->peaks.push_back(peak);
    if (at == 0) {
        why = L"That file has no sound";
        return nullptr;
    }
    out->seconds = at / fs;
    for (float p : out->peaks) out->loudest = std::max(out->loudest, std::min(p, 1.0f));
    return out;
}

class SoundBoard {
public:
    static const int VOICES = 8;

    void prepare(double fs) { fadeStep_ = (float)(1.0 / (0.005 * fs)); }

    void play(int pad, std::shared_ptr<const Sound> sound) {
        if (!sound) return;
        hold(sound);
        std::lock_guard<std::mutex> g(mx_);
        heads_[pad & 31] = 0;
        starts_.push_back(Start{pad, std::move(sound)});
        waiting_ = true;
    }

    void stopPad(int pad) {
        std::lock_guard<std::mutex> g(mx_);
        starts_.push_back(Start{pad, nullptr});
        waiting_ = true;
    }

    void stopAll() {
        std::lock_guard<std::mutex> g(mx_);
        starts_.clear();
        halt_ = true;
        waiting_ = true;
    }

    bool active() const { return playing_.load() != 0 || waiting_.load(); }

    unsigned playing() const { return playing_.load(); }

    size_t position(int pad) const { return heads_[pad & 31].load(); }

    void sweep() {
        held_.erase(std::remove_if(held_.begin(), held_.end(), [](const std::shared_ptr<const Sound>& s) { return s.use_count() == 1; }), held_.end());
    }

    void mix(float* l, float* r, int n) {
        take();
        unsigned mask = 0;
        for (Voice& v : voices_) {
            if (!v.sound) continue;
            const std::vector<short>& pcm = v.sound->pcm;
            const size_t frames = v.sound->frames();
            int k = 0;
            for (; k < n && v.at < frames; ++k, ++v.at) {
                if (v.fading) {
                    v.gain -= fadeStep_;
                    if (v.gain <= 0.0f) break;
                }
                const float g = v.gain * (1.0f / 32768.0f);
                l[k] += pcm[2 * v.at] * g;
                r[k] += pcm[2 * v.at + 1] * g;
            }
            if (!v.fading) heads_[v.pad & 31] = (unsigned)v.at;
            if (v.at >= frames || (v.fading && v.gain <= 0.0f)) v = Voice{};
            else mask |= 1u << (v.pad & 31);
        }
        playing_ = mask;
    }

private:
    struct Start {
        int pad = 0;
        std::shared_ptr<const Sound> sound;
    };

    struct Voice {
        std::shared_ptr<const Sound> sound;
        size_t at = 0;
        int pad = 0;
        float gain = 1.0f;
        bool fading = false;
    };

    void hold(const std::shared_ptr<const Sound>& sound) {
        sweep();
        for (const auto& s : held_)
            if (s == sound) return;
        held_.push_back(sound);
    }

    void take() {
        if (!waiting_.load()) return;
        std::unique_lock<std::mutex> g(mx_, std::try_to_lock);
        if (!g.owns_lock()) return;
        if (halt_) {
            for (Voice& v : voices_) v.fading = true;
            halt_ = false;
        }
        for (Start& s : starts_) {
            for (Voice& v : voices_)
                if (v.sound && v.pad == s.pad) v.fading = true;
            if (!s.sound) continue;
            Voice* slot = nullptr;
            for (Voice& v : voices_)
                if (!v.sound) { slot = &v; break; }
            if (!slot) {
                slot = &voices_[0];
                for (Voice& v : voices_)
                    if (v.at > slot->at) slot = &v;
            }
            *slot = Voice{std::move(s.sound), 0, s.pad, 1.0f, false};
        }
        starts_.clear();
        waiting_ = false;
    }

    std::vector<std::shared_ptr<const Sound>> held_;
    std::mutex mx_;
    std::vector<Start> starts_;
    bool halt_ = false;
    std::atomic<bool> waiting_{false};
    std::atomic<unsigned> playing_{0};
    std::atomic<unsigned> heads_[32] = {};
    Voice voices_[VOICES];
    float fadeStep_ = 0.004f;
};

}
