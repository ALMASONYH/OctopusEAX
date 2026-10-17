#pragma once
#include <cmath>

namespace mix {

struct WetPeak {
    struct Tune {
        double midDb = 0.12, midHz = 427.0,  midQ = 1.0;
        double hiDb  = 3.0,  hiHz  = 5000.0, hiQ  = 0.9;
        double dipDb = -0.6, dipHz = 500.0,  dipQ = 3.0;
        double trimDb = 1.2;
        double crossHz = 2500.0, cross = 0.12;
    };

    struct Bell {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        void set(double dB, double hz, double q, int fs) {
            const double A  = std::pow(10.0, dB / 40.0);
            const double w0 = 2.0 * 3.14159265358979323846 * hz / fs;
            const double cs = std::cos(w0), al = std::sin(w0) / (2.0 * q);
            const double a0 = 1.0 + al / A;
            b0 = (1.0 + al * A) / a0; b1 = -2.0 * cs / a0; b2 = (1.0 - al * A) / a0;
            a1 = -2.0 * cs / a0;      a2 = (1.0 - al / A) / a0;
        }
        inline double run(double x, double& z1, double& z2) const {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    struct Side { double h1 = 0, h2 = 0, d1 = 0, d2 = 0, lp = 0; };

    Bell hi, dip, midBell;
    Side left, right;
    double mz1 = 0, mz2 = 0;
    double g = 1, xk = 0, cross = 0;
    bool   mid = false;

    void prepare(int fs) { prepare(fs, Tune()); }
    void prepare(int fs, const Tune& t) {
        mid = t.midDb != 0.0;
        if (mid) midBell.set(t.midDb, t.midHz, t.midQ, fs);
        hi.set(t.hiDb, t.hiHz, t.hiQ, fs);
        dip.set(t.dipDb, t.dipHz, t.dipQ, fs);
        g  = std::pow(10.0, t.trimDb / 20.0);
        xk = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * t.crossHz / fs);
        cross = t.cross;
        left = Side(); right = Side();
        mz1 = mz2 = 0;
    }
    inline float one(float x, Side& s) const {
        const double y = hi.run(x, s.h1, s.h2);
        const float v = (float)dip.run((float)y, s.d1, s.d2);
        return (float)(v * g);
    }
    void process(float& l, float& r) {
        l = one(l, left); r = one(r, right);
        if (mid) {
            const double m = 0.5 * ((double)l + (double)r), sd = 0.5 * ((double)l - (double)r);
            const double y = midBell.run(m, mz1, mz2);
            l = (float)(y + sd); r = (float)(y - sd);
        }
        left.lp += xk * (l - left.lp); right.lp += xk * (r - right.lp);
        const double hl = l - left.lp, hr = r - right.lp;
        l = (float)(l + cross * hr); r = (float)(r + cross * hl);
    }
};

}
