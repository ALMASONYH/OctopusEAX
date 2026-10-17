#pragma once
#include <algorithm>
#include <cmath>

namespace iso {

struct ToneBand {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;

    void clear() { z1 = z2 = 0.0; }
    inline float run(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
    void lowShelf(double fs, double hz, double db) {
        const double g = std::pow(10.0, db / 20.0), k = std::tan(3.141592653589793 * hz / fs);
        const double a0 = 1.0 + k;
        b0 = (1.0 + g * k) / a0; b1 = (g * k - 1.0) / a0; b2 = 0.0;
        a1 = (k - 1.0) / a0; a2 = 0.0;
    }
    void highShelf(double fs, double hz, double db) {
        const double g = std::pow(10.0, db / 20.0), k = std::tan(3.141592653589793 * hz / fs);
        const double a0 = 1.0 + k;
        b0 = (g + k) / a0; b1 = (k - g) / a0; b2 = 0.0;
        a1 = (k - 1.0) / a0; a2 = 0.0;
    }
    void peak(double fs, double hz, double q, double db) {
        const double A = std::pow(10.0, db / 40.0), w = 2.0 * 3.141592653589793 * hz / fs, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + al / A;
        b0 = (1.0 + al * A) / a0; b1 = -2.0 * c / a0; b2 = (1.0 - al * A) / a0;
        a1 = -2.0 * c / a0; a2 = (1.0 - al / A) / a0;
    }
};

constexpr double GATE_SLOPE = 0.95, GATE_THRESHOLD_MIN_DB = -66.0, GATE_THRESHOLD_NORMAL_DB = -17.0;
constexpr int GATE_LEVEL_NORMAL = 50;

inline double gateLevelThresholdDb(int level) {
    level = std::max(0, std::min(100, level));
    if (level >= GATE_LEVEL_NORMAL) return GATE_THRESHOLD_NORMAL_DB;
    return GATE_THRESHOLD_MIN_DB + level * (GATE_THRESHOLD_NORMAL_DB - GATE_THRESHOLD_MIN_DB) / GATE_LEVEL_NORMAL;
}

inline double gateLevelSlope(int level) {
    level = std::max(0, std::min(100, level));
    if (level <= GATE_LEVEL_NORMAL) return GATE_SLOPE;
    return GATE_SLOPE * (1.0 + (double)(level - GATE_LEVEL_NORMAL) / (100 - GATE_LEVEL_NORMAL));
}

struct GateShape {
    bool gateOn = true;
    double thresholdDb = GATE_THRESHOLD_NORMAL_DB, slope = GATE_SLOPE, loDb = 12.0, midDb = 12.0, hiDb = 12.0, outDb = -5.0;
};

struct ExpanderGate {
    static constexpr double LO_HZ = 60.5, MID_HZ = 652.0, MID_Q = 0.17, HI_HZ = 6310.0;
    static constexpr double DETECT_SEC = 0.0015, OPEN_SEC = 0.001, CLOSE_SEC = 0.060;

    double fs = 48000.0;
    GateShape shape;

    double detStep = 0.0, openStep = 0.0, closeStep = 0.0;
    double threshold = 0.1, det = 0.0, gain = 1.0;
    float outGain = 1.0f;
    ToneBand lo, mid, hi;

    void prepare(double rate) {
        fs = rate;
        detStep = 1.0 - std::exp(-1.0 / (DETECT_SEC * fs));
        openStep = 1.0 - std::exp(-1.0 / (OPEN_SEC * fs));
        closeStep = 1.0 - std::exp(-1.0 / (CLOSE_SEC * fs));
        configure(shape);
        reset();
    }

    void configure(const GateShape& s) {
        shape = s;
        threshold = std::pow(10.0, std::max(GATE_THRESHOLD_MIN_DB, std::min(0.0, s.thresholdDb)) / 20.0);
        lo.lowShelf(fs, LO_HZ, s.loDb);
        mid.peak(fs, MID_HZ, MID_Q, s.midDb);
        hi.highShelf(fs, HI_HZ, s.hiDb);
        outGain = (float)std::pow(10.0, s.outDb / 20.0);
    }

    void reset() {
        det = 0.0; gain = 1.0;
        lo.clear(); mid.clear(); hi.clear();
    }

    inline float run(float x) {
        const double a = std::fabs((double)x);
        det += detStep * (a - det);
        double target = 1.0;
        if (shape.gateOn && det < threshold) target = std::pow(std::max(det, 1e-9) / threshold, shape.slope);
        gain += (target > gain ? openStep : closeStep) * (target - gain);
        return hi.run(mid.run(lo.run((float)(x * gain)))) * outGain;
    }

    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) x[i] = run(x[i]);
    }
};

}
