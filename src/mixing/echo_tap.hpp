#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace mix {

struct EchoTap {
    static constexpr double MAX_SECONDS = 0.5, CUTOFF_HZ = 3500.0, SHELF_DB = 5.5, SHELF_HZ = 500.0;
    static constexpr float RIGHT_RATIO = -0.2f;

    std::vector<float> line;
    int len = 0, pos = 0, delay = 1;
    double lp = 0.0, s1 = 0.0, s2 = 0.0;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;

    void prepare(int fs) {
        len = (int)(MAX_SECONDS * fs + 0.5) + 1;
        if (len < 2) len = 2;
        line.assign((size_t)len, 0.f);
        pos = 0; delay = len - 1;
        s1 = s2 = 0.0; z1 = z2 = 0.0;
        lp = std::exp(-2.0 * 3.14159265358979323846 * CUTOFF_HZ / fs);
        const double A = std::pow(10.0, SHELF_DB / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * SHELF_HZ / fs;
        const double cs = std::cos(w0), sn = std::sin(w0);
        const double alpha = sn / 2.0 * std::sqrt(2.0);
        const double sq = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1.0) + (A - 1.0) * cs + sq;
        b0 = (A * ((A + 1.0) - (A - 1.0) * cs + sq)) / a0;
        b1 = (2.0 * A * ((A - 1.0) - (A + 1.0) * cs)) / a0;
        b2 = (A * ((A + 1.0) - (A - 1.0) * cs - sq)) / a0;
        a1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cs)) / a0;
        a2 = ((A + 1.0) + (A - 1.0) * cs - sq) / a0;
    }

    void setDelay(int samples) { delay = std::max(1, std::min(len - 1, samples)); }

    void clear() {
        std::fill(line.begin(), line.end(), 0.f);
        s1 = s2 = 0.0; z1 = z2 = 0.0;
    }

    inline void process(const float* in, float gain, float* addL, float* addR, int n) {
        for (int i = 0; i < n; ++i) {
            const float d = line[(size_t)pos];
            line[(size_t)pos] = 0.f;
            int w = pos + delay;
            if (w >= len) w -= len;
            line[(size_t)w] += in[i] * gain;
            s1 += (1.0 - lp) * ((double)d - s1);
            s2 += (1.0 - lp) * (s1 - s2);
            const double y = b0 * s2 + z1;
            z1 = b1 * s2 - a1 * y + z2;
            z2 = b2 * s2 - a2 * y;
            const float out = (float)y;
            if (++pos >= len) pos = 0;
            addL[i] += out;
            addR[i] += out * RIGHT_RATIO;
        }
    }
};

}
