#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "sound_board.hpp"

namespace clip {

inline float fadeCurve(double x) {
    if (x <= 0.0) return 0.0f;
    if (x >= 1.0) return 1.0f;
    return (float)std::sin(1.5707963267948966 * x);
}

inline short scaled(short v, float g) { return (short)std::lrint(v * g); }

inline std::shared_ptr<const Sound> withFades(std::shared_ptr<const Sound> s, double fs, double inSec, double outSec) {
    if (!s || (inSec <= 0.0 && outSec <= 0.0)) return s;
    auto out = std::make_shared<Sound>(*s);
    const size_t n = out->frames();
    double fi = std::max(0.0, inSec) * fs, fo = std::max(0.0, outSec) * fs;
    if (fi + fo > (double)n) {
        const double k = n / (fi + fo);
        fi *= k;
        fo *= k;
    }
    auto shape = [&](size_t i) {
        float g = 1.0f;
        if (i < fi) g *= fadeCurve((i + 0.5) / fi);
        if (n - i <= fo) g *= fadeCurve((n - i - 0.5) / fo);
        out->pcm[2 * i] = scaled(out->pcm[2 * i], g);
        out->pcm[2 * i + 1] = scaled(out->pcm[2 * i + 1], g);
    };
    const size_t head = std::min(n, (size_t)std::ceil(fi)), tail = n - std::min(n, (size_t)std::ceil(fo));
    for (size_t i = 0; i < head; ++i) shape(i);
    for (size_t i = std::max(head, tail); i < n; ++i) shape(i);
    return out;
}

class SoundEdit {
public:
    struct Part {
        size_t from, to;
    };

    struct Span {
        size_t from, to;
    };

    static const size_t JOIN = 144;

    void load(std::shared_ptr<const Sound> src, size_t peakStep) {
        src_ = std::move(src);
        parts_.clear();
        undo_.clear();
        redo_.clear();
        step_ = std::max<size_t>(1, peakStep);
        if (src_ && src_->frames() > 0) parts_.push_back({0, src_->frames()});
    }

    bool ready() const { return src_ && !parts_.empty(); }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    const std::vector<Part>& parts() const { return parts_; }

    size_t frames() const {
        size_t n = 0;
        for (const Part& p : parts_) n += p.to - p.from;
        return n;
    }

    std::vector<size_t> joins() const {
        std::vector<size_t> at;
        size_t t = 0;
        for (size_t k = 0; k + 1 < parts_.size(); ++k) {
            t += parts_[k].to - parts_[k].from;
            at.push_back(t);
        }
        return at;
    }

    bool cut(size_t a, size_t b) {
        const size_t total = frames();
        b = std::min(b, total);
        if (a >= b || (a == 0 && b >= total)) return false;
        std::vector<Part> next = slice(0, a);
        const std::vector<Part> rest = slice(b, total);
        next.insert(next.end(), rest.begin(), rest.end());
        push(std::move(next));
        return true;
    }

    bool keep(size_t a, size_t b) {
        const size_t total = frames();
        b = std::min(b, total);
        if (a >= b || (a == 0 && b >= total)) return false;
        push(slice(a, b));
        return true;
    }

    bool cutMany(const std::vector<Span>& spans) {
        const size_t total = frames();
        std::vector<Part> next;
        size_t at = 0;
        for (const Span& s : spans) {
            const size_t a = std::min(s.from, total), b = std::min(s.to, total);
            if (a < at || a >= b) return false;
            const std::vector<Part> piece = slice(at, a);
            next.insert(next.end(), piece.begin(), piece.end());
            at = b;
        }
        const std::vector<Part> rest = slice(at, total);
        next.insert(next.end(), rest.begin(), rest.end());
        if (next.empty() || spans.empty()) return false;
        push(std::move(next));
        return true;
    }

    std::vector<Span> quiet(size_t window, float floorDb, float ceilingDb, float belowLoudDb, size_t least, size_t pad, size_t edge) const {
        std::vector<Span> out;
        const size_t total = frames();
        if (!ready() || !window || total < least) return out;
        std::vector<float> level((total + window - 1) / window);
        std::vector<double> power(level.size(), 0.0);
        size_t t = 0;
        for (const Part& p : parts_)
            for (size_t s = p.from; s < p.to; ++s, ++t) {
                const double l = src_->pcm[2 * s], r = src_->pcm[2 * s + 1];
                power[t / window] += (l * l + r * r) * 0.5;
            }
        for (size_t k = 0; k < level.size(); ++k) {
            const size_t n = std::min(window, total - k * window);
            level[k] = (float)(10.0 * std::log10(power[k] / n / (32768.0 * 32768.0) + 1e-12));
        }
        std::vector<float> sorted(level);
        const size_t pick = std::min(sorted.size() - 1, sorted.size() * 98 / 100);
        std::nth_element(sorted.begin(), sorted.begin() + (std::ptrdiff_t)pick, sorted.end());
        const float threshold = std::max(floorDb, std::min(ceilingDb, sorted[pick] - belowLoudDb));
        for (size_t k = 0; k < level.size();) {
            if (level[k] >= threshold) {
                ++k;
                continue;
            }
            size_t e = k;
            while (e < level.size() && level[e] < threshold) ++e;
            const size_t a = k * window, b = std::min(total, e * window);
            k = e;
            if (b - a < least || (a == 0 && b >= total)) continue;
            Span s = {a + pad, b - pad};
            if (a == 0) s = {0, b - std::min(b, edge)};
            else if (b >= total) s = {a + std::min(b - a, edge), total};
            if (s.from < s.to) out.push_back(s);
        }
        return out;
    }

    bool plain(const std::shared_ptr<const Sound>& s) const {
        return s && src_ == s && parts_.size() == 1 && parts_[0].from == 0 && parts_[0].to == s->frames();
    }

    void swap(std::shared_ptr<const Sound> next) {
        if (!next || !next->frames()) return;
        redo_.clear();
        src_ = std::move(next);
        parts_.assign(1, Part{0, src_->frames()});
    }

    void replace(std::shared_ptr<const Sound> next) {
        if (!next || !next->frames()) return;
        undo_.push_back({src_, parts_});
        redo_.clear();
        src_ = std::move(next);
        parts_.assign(1, Part{0, src_->frames()});
    }

    bool undo() {
        if (undo_.empty()) return false;
        redo_.push_back({src_, parts_});
        src_ = std::move(undo_.back().src);
        parts_ = std::move(undo_.back().parts);
        undo_.pop_back();
        return true;
    }

    bool redo() {
        if (redo_.empty()) return false;
        undo_.push_back({src_, parts_});
        src_ = std::move(redo_.back().src);
        parts_ = std::move(redo_.back().parts);
        redo_.pop_back();
        return true;
    }
    std::shared_ptr<Sound> render(size_t a, size_t b, size_t fadeIn, size_t fadeOut) const {
        auto out = std::make_shared<Sound>();
        const size_t total = frames();
        b = std::min(b, total);
        if (!ready() || a >= b) return out;
        double fi = (double)std::max(fadeIn, JOIN), fo = (double)std::max(fadeOut, JOIN);
        if (fi + fo > (double)total) {
            const double k = total / (fi + fo);
            fi *= k;
            fo *= k;
        }
        out->pcm.reserve((b - a) * 2);
        size_t start = 0;
        for (size_t k = 0; k < parts_.size(); ++k) {
            const Part& p = parts_[k];
            const size_t end = start + (p.to - p.from);
            const bool joinBefore = k > 0, joinAfter = k + 1 < parts_.size();
            for (size_t x = std::max(a, start), hi = std::min(b, end); x < hi; ++x) {
                float g = 1.0f;
                if (x < fi) g *= fadeCurve((x + 0.5) / fi);
                if (total - x <= fo) g *= fadeCurve((total - x - 0.5) / fo);
                if (joinBefore && x - start < JOIN) g *= fadeCurve((x - start + 0.5) / JOIN);
                if (joinAfter && end - x <= JOIN) g *= fadeCurve((end - x - 0.5) / JOIN);
                const size_t s = p.from + (x - start);
                out->pcm.push_back(scaled(src_->pcm[2 * s], g));
                out->pcm.push_back(scaled(src_->pcm[2 * s + 1], g));
            }
            start = end;
        }
        return out;
    }

    std::shared_ptr<Outline> outline(double fs) const {
        auto o = std::make_shared<Outline>();
        o->perSec = (int)std::lround(fs / (double)step_);
        const size_t total = frames();
        std::vector<int> peak((total + step_ - 1) / step_, 0);
        size_t t = 0;
        for (const Part& p : parts_)
            for (size_t s = p.from; s < p.to; ++s, ++t) {
                const int a = std::max(std::abs((int)src_->pcm[2 * s]), std::abs((int)src_->pcm[2 * s + 1]));
                int& slot = peak[t / step_];
                if (a > slot) slot = a;
            }
        o->peaks.reserve(peak.size());
        for (int v : peak) o->peaks.push_back(v / 32768.0f);
        o->seconds = frames() / fs;
        for (float v : o->peaks) o->loudest = std::max(o->loudest, std::min(v, 1.0f));
        return o;
    }

private:
    std::vector<Part> slice(size_t a, size_t b) const {
        std::vector<Part> out;
        size_t t = 0;
        for (const Part& p : parts_) {
            const size_t len = p.to - p.from, lo = std::max(a, t), hi = std::min(b, t + len);
            if (lo < hi) out.push_back({p.from + (lo - t), p.from + (hi - t)});
            t += len;
        }
        return out;
    }

    void push(std::vector<Part> next) {
        std::vector<Part> merged;
        for (const Part& p : next) {
            if (!merged.empty() && merged.back().to == p.from) merged.back().to = p.to;
            else merged.push_back(p);
        }
        undo_.push_back({src_, parts_});
        redo_.clear();
        parts_ = std::move(merged);
    }

    struct State {
        std::shared_ptr<const Sound> src;
        std::vector<Part> parts;
    };

    std::shared_ptr<const Sound> src_;
    std::vector<Part> parts_;
    std::vector<State> undo_, redo_;
    size_t step_ = 1;
};

inline bool writeWav(const std::wstring& path, const Sound& s, int fs) {
    if (s.pcm.size() * sizeof(short) > 0xFFFFFFFFull - 36) return false;
    const uint32_t data = (uint32_t)(s.pcm.size() * sizeof(short));
    struct Header {
        char riff[4];
        uint32_t size;
        char wave[4], fmt[4];
        uint32_t fmtSize;
        uint16_t format, channels;
        uint32_t rate, bytesPerSec;
        uint16_t align, bits;
        char dataTag[4];
        uint32_t dataSize;
    } h = {{'R', 'I', 'F', 'F'}, 36 + data, {'W', 'A', 'V', 'E'}, {'f', 'm', 't', ' '}, 16, 1, 2, (uint32_t)fs, (uint32_t)fs * 4, 4, 16,
           {'d', 'a', 't', 'a'}, data};
    static_assert(sizeof(Header) == 44, "a WAV header is 44 bytes");
    const std::wstring temp = path + L".part";
    const HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, &h, (DWORD)sizeof h, &put, nullptr) && put == sizeof h;
    const char* bytes = (const char*)s.pcm.data();
    for (uint32_t at = 0; ok && at < data;) {
        const DWORD chunk = (DWORD)std::min<uint32_t>(data - at, 1u << 20);
        ok = WriteFile(f, bytes + at, chunk, &put, nullptr) && put == chunk;
        at += chunk;
    }
    CloseHandle(f);
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

}
