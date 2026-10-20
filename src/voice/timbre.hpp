#pragma once
#include <cmath>
#include "fx_parts.hpp"

namespace voice {

class Timbre {
public:
    static const int BANDS = 3;

    void prepare(double fs, const double (&want)[BANDS]) {
        fs_ = fs;
        for (int b = 0; b < BANDS; ++b) want_[b] = want[b];
        peakFall_ = (float)std::exp(-1.0 / (PEAK_SEC * fs));
        env_.prepare(fs, 0.005, 0.080);
        reset();
    }

    void reset() {
        for (int b = 0; b < BANDS; ++b) {
            gainDb_[b] = 0.0; heardDb_[b] = want_[b]; block_[b] = 0.0;
            eq_[b].clear(); probe_[b].clear();
            eq_[b].peak(fs_, CENTRE[b], EQ_Q, 0.0);
            probe_[b].bandpass(fs_, CENTRE[b], PROBE_Q);
        }
        env_.env = 0.0f; peak_ = 0.0f; tick_ = 0; talk_ = 0;
    }

    float run(float x) {
        float y = x;
        for (Filter& f : eq_) y = f.run(y);
        const float e = env_.run(y);
        peak_ = e > peak_ ? e : peak_ * peakFall_;
        if (e > GATE && e > 0.25f * peak_) ++talk_;
        for (int b = 0; b < BANDS; ++b) { const double p = probe_[b].run(y); block_[b] += p * p; }
        if (++tick_ >= STEP) {
            if (2 * talk_ > STEP) steer();
            for (double& v : block_) v = 0.0;
            tick_ = 0; talk_ = 0;
        }
        return y;
    }

private:
    static constexpr double CENTRE[BANDS] = {160.0, 420.0, 1100.0};
    static constexpr double EQ_Q = 0.9, PROBE_Q = 1.4, PEAK_SEC = 1.0, HEAR = 0.005, RATE = 0.004, MOST_DB = 6.0;
    static constexpr float GATE = 0.002f;
    static const int STEP = 240;

    double fs_ = 48000.0;
    double want_[BANDS] = {}, gainDb_[BANDS] = {}, heardDb_[BANDS] = {}, block_[BANDS] = {};
    Filter eq_[BANDS], probe_[BANDS];
    Follower env_;
    float peak_ = 0.0f, peakFall_ = 0.0f;
    int tick_ = 0, talk_ = 0;

    void steer() {
        double sum = 0.0;
        for (double p : block_) sum += p;
        if (sum < 1e-12) return;
        double mean = 0.0;
        for (int b = 0; b < BANDS; ++b) {
            heardDb_[b] += HEAR * (10.0 * std::log10(block_[b] / sum + 1e-6) - heardDb_[b]);
            gainDb_[b] += RATE * (want_[b] - heardDb_[b]);
            mean += gainDb_[b];
        }
        mean /= BANDS;
        for (int b = 0; b < BANDS; ++b) {
            double g = gainDb_[b] - mean;
            g = g < -MOST_DB ? -MOST_DB : (g > MOST_DB ? MOST_DB : g);
            gainDb_[b] = g;
            eq_[b].peak(fs_, CENTRE[b], EQ_Q, g);
        }
    }
};

}
