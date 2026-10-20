#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace voice {

class SongShifter {
public:
    static constexpr double LOWEST = 0.4, HIGHEST = 2.5;

    void prepare(double fs) {
        hop_ = (int)(0.050 * fs);
        fade_ = (int)(0.010 * fs);
        seek_ = (int)(0.012 * fs);
        ramp_.resize((size_t)fade_);
        for (int k = 0; k < fade_; ++k) {
            const double s = std::sin(1.5707963267948966 * (k + 0.5) / fade_);
            ramp_[(size_t)k] = s * s;
        }
        const double reach = (HIGHEST - 1.0) * (hop_ + fade_) + 2.0 * seek_ + fade_ + 16.0;
        size_t size = 1;
        while ((double)size < reach) size <<= 1;
        mask_ = (long long)size - 1;
        bufL_.assign(size, 0.0f);
        bufR_.assign(size, 0.0f);
        mono_.assign(size, 0.0f);
        reset();
    }

    void reset() {
        std::fill(bufL_.begin(), bufL_.end(), 0.0f);
        std::fill(bufR_.begin(), bufR_.end(), 0.0f);
        std::fill(mono_.begin(), mono_.end(), 0.0f);
        w_ = 0;
        restart();
    }

    void restart() {
        age_ = hop_;
        cur_.on = old_.on = false;
    }

    void set(double ratio) {
        ratio_ = ratio < LOWEST ? LOWEST : (ratio > HIGHEST ? HIGHEST : ratio);
    }

    bool neutral() const { return std::fabs(ratio_ - 1.0) < 1e-4; }

    void feed(const float* l, const float* r, int n) {
        for (int i = 0; i < n; ++i) write(l[i], r[i]);
    }

    void process(float* l, float* r, int n) {
        for (int i = 0; i < n; ++i) {
            write(l[i], r[i]);
            if (age_ >= hop_) begin();
            double gainIn = 1.0, gainOut = 0.0;
            if (old_.on) {
                if (age_ < fade_) { gainIn = ramp_[(size_t)age_]; gainOut = 1.0 - gainIn; }
                else old_.on = false;
            }
            double outL = gainIn * read(bufL_, cur_.pos), outR = gainIn * read(bufR_, cur_.pos);
            cur_.pos += cur_.rate;
            if (gainOut > 0.0) {
                outL += gainOut * read(bufL_, old_.pos);
                outR += gainOut * read(bufR_, old_.pos);
                old_.pos += old_.rate;
            }
            l[i] = (float)outL;
            r[i] = (float)outR;
            ++age_;
        }
    }

private:
    struct Grain {
        double pos = 0.0, rate = 1.0;
        bool on = false;
    };

    void write(float l, float r) {
        const long long at = w_ & mask_;
        bufL_[(size_t)at] = l;
        bufR_[(size_t)at] = r;
        mono_[(size_t)at] = 0.5f * (l + r);
        ++w_;
    }

    void begin() {
        const double reach = (ratio_ > 1.0 ? ratio_ - 1.0 : 0.0) * (hop_ + fade_);
        const double back = reach + seek_ + fade_ + 4.0;
        const long long newest = w_ - 1, at = (long long)std::floor((double)newest - back);
        if (cur_.on) {
            const double from = std::floor(cur_.pos);
            const int shift = align((long long)from, at);
            old_ = cur_;
            cur_.pos = (double)(at - shift) + (old_.pos - from);
        } else {
            old_ = cur_;
            cur_.pos = (double)newest - back;
        }
        cur_.rate = ratio_;
        cur_.on = true;
        age_ = 0;
    }

    double score(long long from, long long cand, int step) const {
        double xy = 0.0, yy = 0.0;
        for (int j = 0; j < fade_; j += step) {
            const double a = mono_[(size_t)((from + j) & mask_)], b = mono_[(size_t)((cand + j) & mask_)];
            xy += a * b;
            yy += b * b;
        }
        return xy / std::sqrt(yy + 1e-12);
    }

    int align(long long from, long long at) const {
        double energy = 0.0;
        for (int j = 0; j < fade_; j += 2) {
            const double a = mono_[(size_t)((from + j) & mask_)];
            energy += a * a;
        }
        if (energy < 1e-10 * fade_) return 0;
        int best = 0;
        double top = -1e300;
        for (int s = -seek_; s <= seek_; s += 4) {
            const double v = score(from, at - s, 2);
            if (v > top) { top = v; best = s; }
        }
        const int lo = std::max(-seek_, best - 3), hi = std::min(seek_, best + 3);
        top = -1e300;
        int fine = best;
        for (int s = lo; s <= hi; ++s) {
            const double v = score(from, at - s, 1);
            if (v > top) { top = v; fine = s; }
        }
        return fine;
    }

    double read(const std::vector<float>& b, double pos) const {
        const long long i = (long long)std::floor(pos);
        const double t = pos - (double)i;
        const double y0 = b[(size_t)((i - 1) & mask_)], y1 = b[(size_t)(i & mask_)];
        const double y2 = b[(size_t)((i + 1) & mask_)], y3 = b[(size_t)((i + 2) & mask_)];
        const double a = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
        const double c1 = 0.5 * (y2 - y0);
        return ((a * t + c2) * t + c1) * t + y1;
    }

    int hop_ = 2400, fade_ = 480, seek_ = 576, age_ = 0;
    double ratio_ = 1.0;
    long long w_ = 0, mask_ = 0;
    Grain cur_, old_;
    std::vector<double> ramp_;
    std::vector<float> bufL_, bufR_, mono_;
};

}
