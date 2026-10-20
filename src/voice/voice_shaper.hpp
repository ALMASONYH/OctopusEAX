#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "../audio/fixed_ring.hpp"
#include "pitch_tracker.hpp"
#include "voice_masks.hpp"

namespace voice {

class VoiceShaper {
public:
    static const int DELAY = 2600;
    static const int HOP = 480;
    static const int SIZE = 16384, MASK = SIZE - 1;

    void prepare(double fs) {
        fs_ = fs;
        tracker_.prepare(fs);
        hist_.assign(SIZE, 0.f);
        out_.assign(SIZE, 0.f);
        frame_.assign(PitchTracker::WIN, 0.f);
        maxPeriod_ = fs / 70.0;
        unvoicedT_ = fs * 0.015;
        levelStep_ = 1.0 / (0.3 * fs);
        glide_ = 1.0 - std::exp(-(double)HOP / (0.030 * fs));
        reset();
    }

    void reset() {
        std::fill(hist_.begin(), hist_.end(), 0.f);
        std::fill(out_.begin(), out_.end(), 0.f);
        wpos_ = 0; anaNext_ = 0; synth_ = 0.0;
        period_ = 0.0; f0_ = 0.0; f0Glide_ = 0.0; voiced_ = false; lostHops_ = 99;
        inE_ = outE_ = 0.0; match_ = 1.0;
        wobble_ = 0.0; rng_ = 0x2545F491u;
        logMean_ = 0.0; heard_ = 0; odd_ = false;
        marks_.clear();
        tracker_.reset();
    }

    void set(double pitchHz, int sizePercent, double ratio = 1.0) {
        if (sizePercent < 60) sizePercent = 60;
        if (sizePercent > 150) sizePercent = 150;
        if (pitchHz < -150.0) pitchHz = -150.0;
        if (pitchHz > 150.0) pitchHz = 150.0;
        if (ratio < 0.4) ratio = 0.4;
        if (ratio > 2.5) ratio = 2.5;
        pitchHz_ = pitchHz;
        alpha_ = 100.0 / sizePercent;
        ratio_ = ratio;
    }
    void anchor(const Anchor& a) { anchor_ = a; }
    bool neutral() const { return pitchHz_ == 0.0 && alpha_ == 1.0 && ratio_ == 1.0 && !anchor_.on; }
    double voiceHz() const { return voiced_ ? f0_ : 0.0; }
    double targetHz() const { return voiced_ ? target(f0Glide_) : 0.0; }

    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            hist_[wpos_ & MASK] = x[i];
            ++wpos_;
            analyseDue();
            synthesiseDue();
            const long long t = wpos_ - DELAY;
            float y = 0.f, xin = 0.f;
            if (t >= 0) { y = out_[t & MASK]; out_[t & MASK] = 0.f; xin = hist_[t & MASK]; }
            inE_ += levelStep_ * ((double)xin * xin - inE_);
            outE_ += levelStep_ * ((double)y * y - outE_);
            if ((wpos_ & 255) == 0 && inE_ > 1e-6 && outE_ > 1e-8) {
                double want = std::sqrt(inE_ / outE_);
                if (want < 0.5) want = 0.5;
                if (want > 2.0) want = 2.0;
                match_ += 0.05 * (want - match_);
            }
            x[i] = (float)(y * match_);
        }
    }

private:
    static constexpr double JITTER = 0.003, REF_HZ = 120.0, LEARN = 0.004, MOST_STEP = 0.7;

    double fs_ = 48000.0, maxPeriod_ = 686.0, unvoicedT_ = 480.0;
    double pitchHz_ = 0.0, alpha_ = 1.0, ratio_ = 1.0;
    double inE_ = 0.0, outE_ = 0.0, match_ = 1.0, levelStep_ = 0.0;
    double glide_ = 0.0, wobble_ = 0.0;
    unsigned rng_ = 0x2545F491u;
    PitchTracker tracker_;
    std::vector<float> hist_, out_, frame_;
    long long wpos_ = 0, anaNext_ = 0;
    double synth_ = 0.0;
    double period_ = 0.0, f0_ = 0.0, f0Glide_ = 0.0;
    bool voiced_ = false;
    int lostHops_ = 99;
    Anchor anchor_;
    double logMean_ = 0.0;
    long long heard_ = 0;
    bool odd_ = false;
    dsp::FixedRing<double, 64> marks_;

    void learn(double f0) {
        const double l = std::log(f0);
        if (heard_++ == 0) { logMean_ = l; return; }
        const double step = std::min(MOST_STEP, std::max(-MOST_STEP, l - logMean_));
        logMean_ += std::max(1.0 / (double)heard_, LEARN) * step;
    }

    double formant() const {
        if (!anchor_.on || heard_ == 0) return alpha_;
        const double a = alpha_ * std::pow(REF_HZ / std::exp(logMean_), anchor_.follow);
        return a < 0.55 ? 0.55 : (a > 1.6 ? 1.6 : a);
    }

    double target(double f0) const {
        double t = anchor_.on
            ? REF_HZ * ratio_ * std::pow(f0 / std::exp(logMean_), anchor_.spread) + pitchHz_
            : f0 * ratio_ + pitchHz_;
        if (t < 72.0) t = 72.0;
        if (t > 800.0) t = 800.0;
        return t;
    }

    double rnd() {
        rng_ = rng_ * 1664525u + 1013904223u;
        return ((rng_ >> 8) / 16777216.0) * 2.0 - 1.0;
    }

    void analyseDue() {
        const int half = PitchTracker::WIN / 2;
        while (anaNext_ + half <= wpos_) {
            const long long start = anaNext_ - half;
            for (int k = 0; k < PitchTracker::WIN; ++k)
                frame_[k] = start + k >= 0 ? hist_[(start + k) & MASK] : 0.f;
            if (tracker_.analyse(frame_.data())) {
                period_ = tracker_.period(); f0_ = tracker_.f0();
                learn(f0_);
                if (lostHops_ > 2) f0Glide_ = f0_;
                else f0Glide_ += glide_ * (f0_ - f0Glide_);
                lostHops_ = 0;
            } else ++lostHops_;
            voiced_ = period_ > 0.0 && lostHops_ <= 2;
            anaNext_ += HOP;
        }
    }

    float read(double pos) const {
        const long long i = (long long)std::floor(pos);
        const float t = (float)(pos - (double)i);
        const float y0 = hist_[(i - 1) & MASK], y1 = hist_[i & MASK];
        const float y2 = hist_[(i + 1) & MASK], y3 = hist_[(i + 2) & MASK];
        const float a = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c = 0.5f * (y2 - y0);
        return ((a * t + b) * t + c) * t + y1;
    }

    float pulse(long long k) const {
        return std::fabs(hist_[(k - 1) & MASK]) + std::fabs(hist_[k & MASK]) + std::fabs(hist_[(k + 1) & MASK]);
    }

    double peakBetween(double lo, double hi) const {
        const long long a = (long long)std::floor(lo), b = (long long)std::floor(hi);
        long long bestAt = a;
        float best = -1.f;
        for (long long k = a; k <= b; ++k) {
            const float v = pulse(k);
            if (v > best) { best = v; bestAt = k; }
        }
        return (double)bestAt;
    }

    double similarMark(double prev, double T0) const {
        const long long p = (long long)prev;
        const int L = (int)(0.5 * T0);
        double ep = 0.0;
        for (int n = -L; n <= L; ++n) { const double v = hist_[(p + n) & MASK]; ep += v * v; }
        const long long a = (long long)std::floor(prev + 0.75 * T0), b = (long long)std::floor(prev + 1.25 * T0);
        long long bestAt = (long long)std::floor(prev + T0);
        double best = -2.0;
        for (long long k = a; k <= b; ++k) {
            double c = 0.0, ek = 0.0;
            for (int n = -L; n <= L; ++n) {
                const double u = hist_[(p + n) & MASK], v = hist_[(k + n) & MASK];
                c += u * v; ek += v * v;
            }
            const double score = c / (std::sqrt(ep * ek) + 1e-12);
            if (score > best) { best = score; bestAt = k; }
        }
        return (double)bestAt;
    }

    void extendMarks(double T0) {
        if (marks_.empty() || marks_.back() < synth_ - 2.0 * T0) {
            const double first = peakBetween(synth_ - 0.5 * T0, synth_ + 0.5 * T0);
            marks_.clear();
            marks_.push_back(first);
        }
        while (marks_.back() < synth_ + 0.5 * T0 && marks_.back() + 1.75 * T0 + 2.0 <= (double)wpos_)
            marks_.push_back(similarMark(marks_.back(), T0));
        while (marks_.size() > 2 && marks_[2] <= synth_ - 0.5 * T0) marks_.pop_front();
    }

    size_t nearestMark() const {
        size_t best = 0;
        for (size_t k = 1; k < marks_.size(); ++k)
            if (std::fabs(marks_[k] - synth_) < std::fabs(marks_[best] - synth_)) best = k;
        return best;
    }

    void grain(double mark, double T0, double spacing, bool fixedLength = false) {
        const double beta = T0 / spacing, alpha = formant();
        int Lg = (int)(fixedLength ? 2.0 * T0 + 0.5 : 2.0 * T0 / alpha + 0.5);
        if (Lg < 32) Lg = 32;
        float gain = (float)(alpha / beta);
        if (!fixedLength && anchor_.rough > 0.0) {
            odd_ = !odd_;
            if (odd_) gain *= (float)((1.0 - anchor_.rough) / (1.0 + anchor_.rough));
        }
        const double startOut = synth_ - Lg * 0.5;
        const long long o0 = (long long)std::floor(startOut);
        const double frac = startOut - (double)o0;
        const double twoPi = 6.283185307179586;
        for (int i = 0; i < Lg; ++i) {
            const double rel = (double)i - frac;
            if (rel < 0.0 || rel > (double)Lg) continue;
            const double w = 0.5 - 0.5 * std::cos(twoPi * rel / Lg);
            const double src = std::min(mark + (rel - Lg * 0.5) * alpha, (double)wpos_ - 3.0);
            out_[(o0 + i) & MASK] += (float)(w * gain * read(src));
        }
    }

    void synthesiseDue() {
        const double lead = 1.5 * maxPeriod_ + 8.0;
        const double half = PitchTracker::WIN / 2;
        while (synth_ + lead <= (double)wpos_ && synth_ + half <= (double)wpos_) {
            if (voiced_) {
                extendMarks(period_);
                const size_t j = nearestMark();
                const double mark = marks_[j];
                double T0;
                if (j > 0) T0 = marks_[j] - marks_[j - 1];
                else if (marks_.size() > 1) T0 = marks_[1] - marks_[0];
                else T0 = period_;
                if (T0 < 0.5 * period_ || T0 > 1.5 * period_) T0 = period_;
                wobble_ += 0.35 * (rnd() - wobble_);
                double spacing = fs_ / (target(f0Glide_) * (fs_ / T0) / (f0Glide_ > 0.0 ? f0Glide_ : fs_ / T0));
                spacing *= 1.0 + JITTER * wobble_;
                if (pitchHz_ == 0.0 && ratio_ == 1.0 && !anchor_.on) { synth_ = mark; spacing = T0; }
                grain(mark, T0, spacing);
                synth_ += spacing;
            } else {
                marks_.clear();
                grain(synth_, unvoicedT_, unvoicedT_, true);
                synth_ += unvoicedT_;
            }
        }
    }
};

}
