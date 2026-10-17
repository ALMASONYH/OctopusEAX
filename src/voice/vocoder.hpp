#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "../convolution/fft.hpp"
#include "fx_parts.hpp"

namespace voice {

class Vocoder {
public:
    static const int N = 1024, HOP = 256, HALF = N / 2 + 1, BANDS = 36;

    struct Voice {
        bool whisper;
        double warp, warpSwing, pitchHz, pitchSwing;
    };

    void prepare(double rate) {
        fs_ = rate;
        fft_.init(N);
        win_.assign(N, 0.0f);
        for (int i = 0; i < N; ++i) win_[i] = (float)std::sqrt(0.5 - 0.5 * std::cos(TWO_PI * i / N));
        spec_.assign(N, conv::cf(0.0f, 0.0f));
        saw_.assign(N, conv::cf(0.0f, 0.0f));
        hiss_.assign(N, conv::cf(0.0f, 0.0f));
        sum_.assign(HALF + 1, 0.0);
        sawSum_.assign(HALF + 1, 0.0);
        hissSum_.assign(HALF + 1, 0.0);
        table_.assign(TABLE, 0.0f);
        const int harmonics = (int)(HIGH_HZ / LOWEST_PITCH_HZ);
        double sq = 0.0;
        for (int i = 0; i < TABLE; ++i) {
            double v = 0.0;
            for (int k = 1; k <= harmonics; ++k) v += std::cos(TWO_PI * k * i / TABLE + 3.141592653589793 * k * (k - 1) / harmonics);
            table_[i] = (float)v;
            sq += v * v;
        }
        const float norm = (float)(1.0 / std::sqrt(sq / TABLE));
        for (float& v : table_) v *= norm;
        const double binHz = fs_ / N;
        auto mel = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
        auto hz = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
        const double m0 = mel(LOW_HZ), m1 = mel(HIGH_HZ);
        for (int b = 0; b <= BANDS; ++b) edgeHz_[b] = hz(m0 + (m1 - m0) * b / BANDS);
        for (int b = 0; b < BANDS; ++b) centreBin_[b] = 0.5 * (edgeHz_[b] + edgeHz_[b + 1]) / binHz;
        for (int k = 0; k < HALF; ++k) {
            int b = 0;
            while (b < BANDS - 1 && centreBin_[b + 1] <= k) ++b;
            lowBand_[k] = b;
            const int up = b < BANDS - 1 ? b + 1 : b;
            const double span = centreBin_[up] - centreBin_[b];
            double t = span > 0.0 ? (k - centreBin_[b]) / span : 0.0;
            blend_[k] = (float)(t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
        }
        reset();
    }

    void set(const Voice& v) {
        if (v.whisper == voice_.whisper && v.warp == voice_.warp && v.warpSwing == voice_.warpSwing &&
            v.pitchHz == voice_.pitchHz && v.pitchSwing == voice_.pitchSwing) return;
        voice_ = v;
        warpNow_ = warpGoal_ = v.warp;
        pitchNow_ = pitchGoal_ = v.pitchHz;
    }

    void reset() {
        std::fill(inBuf_.begin(), inBuf_.end(), 0.0f);
        std::fill(sawBuf_.begin(), sawBuf_.end(), 0.0f);
        std::fill(hissBuf_.begin(), hissBuf_.end(), 0.0f);
        std::fill(ola_.begin(), ola_.end(), 0.0f);
        outQ_.assign(HOP - 1, 0.0f);
        outQ_.reserve(8192);
        outHead_ = 0; fill_ = 0; sawPhase_ = 0.0; voiced_ = 0.0;
        for (double& s : smooth_) s = -120.0;
        warpNow_ = warpGoal_ = voice_.warp;
        pitchNow_ = pitchGoal_ = voice_.pitchHz;
        driftWait_ = 0;
    }

    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            const double dt = pitchNow_ / fs_;
            sawPhase_ += dt;
            if (sawPhase_ >= 1.0) sawPhase_ -= 1.0;
            inBuf_[N - HOP + fill_] = x[i];
            const double at = sawPhase_ * TABLE;
            const int ti = (int)at;
            const float tf = (float)(at - ti);
            sawBuf_[N - HOP + fill_] = table_[ti & (TABLE - 1)] * (1.0f - tf) + table_[(ti + 1) & (TABLE - 1)] * tf;
            hissBuf_[N - HOP + fill_] = rng_.noise();
            if (++fill_ == HOP) {
                frame();
                std::copy(inBuf_.begin() + HOP, inBuf_.end(), inBuf_.begin());
                std::copy(sawBuf_.begin() + HOP, sawBuf_.end(), sawBuf_.begin());
                std::copy(hissBuf_.begin() + HOP, hissBuf_.end(), hissBuf_.begin());
                fill_ = 0;
            }
            x[i] = outHead_ < outQ_.size() ? outQ_[outHead_++] : 0.0f;
            if (outHead_ >= 4096) { outQ_.erase(outQ_.begin(), outQ_.begin() + (long long)outHead_); outHead_ = 0; }
        }
    }

private:
    static constexpr double LOW_HZ = 60.0, HIGH_HZ = 11000.0, LOWEST_PITCH_HZ = 90.0;
    static const int TABLE = 4096;

    double fs_ = 48000.0;
    Voice voice_{false, 1.0, 0.0, 100.0, 0.0};
    conv::FFT fft_;
    std::vector<float> win_, table_;
    std::vector<conv::cf> spec_, saw_, hiss_;
    std::vector<double> sum_, sawSum_, hissSum_;
    std::vector<float> inBuf_ = std::vector<float>(N, 0.0f), sawBuf_ = std::vector<float>(N, 0.0f),
                       hissBuf_ = std::vector<float>(N, 0.0f), ola_ = std::vector<float>(N, 0.0f), outQ_;
    size_t outHead_ = 0;
    int fill_ = 0;
    double edgeHz_[BANDS + 1] = {}, centreBin_[BANDS] = {}, smooth_[BANDS] = {};
    int lowBand_[HALF] = {};
    float blend_[HALF] = {};
    double sawPhase_ = 0.0, voiced_ = 0.0;
    double warpNow_ = 1.0, warpGoal_ = 1.0, pitchNow_ = 100.0, pitchGoal_ = 100.0;
    int driftWait_ = 0;
    Rng rng_{0x6D2B79F5u}, drift_{0x1B873593u};

    void transform(const std::vector<float>& src, std::vector<conv::cf>& dst, std::vector<double>& cum) {
        for (int i = 0; i < N; ++i) dst[i] = conv::cf(src[i] * win_[i], 0.0f);
        fft_.forward(dst.data());
        cum[0] = 0.0;
        for (int k = 0; k < HALF; ++k) cum[k + 1] = cum[k] + (double)std::norm(dst[k]);
    }

    static double area(const std::vector<double>& cum, double bin) {
        if (bin <= 0.0) return 0.0;
        if (bin >= HALF) return cum[HALF];
        const int i = (int)bin;
        return cum[i] + (cum[i + 1] - cum[i]) * (bin - i);
    }
    static double bandMean(const std::vector<double>& cum, double lo, double hi) {
        const double w = hi - lo;
        return w > 1e-9 ? (area(cum, hi) - area(cum, lo)) / w : 0.0;
    }

    void drift() {
        if (--driftWait_ <= 0) {
            driftWait_ = (int)(drift_.between(1.2, 2.6) * fs_ / HOP);
            warpGoal_ = voice_.warp + voice_.warpSwing * (2.0 * drift_.uniform() - 1.0);
            pitchGoal_ = voice_.pitchHz * (1.0 + voice_.pitchSwing * (2.0 * drift_.uniform() - 1.0));
        }
        warpNow_ += 0.02 * (warpGoal_ - warpNow_);
        pitchNow_ += 0.02 * (pitchGoal_ - pitchNow_);
    }

    void frame() {
        drift();
        transform(inBuf_, spec_, sum_);
        transform(sawBuf_, saw_, sawSum_);
        transform(hissBuf_, hiss_, hissSum_);
        const double binHz = fs_ / N;

        const double low = bandMean(sum_, 80.0 / binHz, 1000.0 / binHz), high = bandMean(sum_, 2500.0 / binHz, 8000.0 / binHz);
        const double ratioDb = 10.0 * std::log10((low + 1e-20) / (high + 1e-20));
        double v = (ratioDb + 2.0) / 12.0;
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        voiced_ += 0.5 * (v - voiced_);
        const double voicing = voice_.whisper ? 0.0 : voiced_;

        float gs[BANDS], gn[BANDS], ws[BANDS], wn[BANDS];
        for (int b = 0; b < BANDS; ++b) {
            const double lo = edgeHz_[b] / binHz, hi = edgeHz_[b + 1] / binHz;
            const double heard = bandMean(sum_, lo * warpNow_, hi * warpNow_);
            const double db = 10.0 * std::log10(heard + 1e-20);
            smooth_[b] += (db > smooth_[b] ? 0.6 : 0.25) * (db - smooth_[b]);
            const double want = std::pow(10.0, smooth_[b] / 10.0);
            const double ps = bandMean(sawSum_, lo, hi), pn = bandMean(hissSum_, lo, hi);
            gs[b] = (float)std::min(30.0, std::sqrt(want / (ps + 1e-12)));
            gn[b] = (float)std::min(30.0, std::sqrt(want / (pn + 1e-12)));
            const double fc = 0.5 * (edgeHz_[b] + edgeHz_[b + 1]);
            double airy = (fc - 2500.0) / 5000.0;
            airy = airy < 0.0 ? 0.0 : (airy > 1.0 ? 1.0 : airy);
            const double vb = voicing * (1.0 - 0.6 * airy);
            ws[b] = (float)std::sqrt(vb);
            wn[b] = (float)std::sqrt(1.0 - vb);
        }
        for (int k = 0; k < HALF; ++k) {
            const int b = lowBand_[k], c = b < BANDS - 1 ? b + 1 : b;
            const float t = blend_[k];
            const float s = (gs[b] * ws[b]) * (1.0f - t) + (gs[c] * ws[c]) * t;
            const float h = (gn[b] * wn[b]) * (1.0f - t) + (gn[c] * wn[c]) * t;
            spec_[k] = saw_[k] * s + hiss_[k] * h;
        }
        for (int k = 1; k < HALF - 1; ++k) spec_[N - k] = std::conj(spec_[k]);
        fft_.inverse(spec_.data());
        for (int i = 0; i < N; ++i) ola_[i] += spec_[i].real() * win_[i] * 0.5f;
        for (int i = 0; i < HOP; ++i) outQ_.push_back(ola_[i]);
        std::copy(ola_.begin() + HOP, ola_.end(), ola_.begin());
        std::fill(ola_.end() - HOP, ola_.end(), 0.0f);
    }
};

}
