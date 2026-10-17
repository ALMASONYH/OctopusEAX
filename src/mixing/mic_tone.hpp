#pragma once
#include <cmath>

namespace mix {

struct MicTone {
    struct Tune {
        double lowDb = 5.0,  lowHz = 200.0;
        double dipDb = -1.5, dipHz = 1700.0, dipQ = 1.0;
    };
    struct Bq {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        bool on = false;
        inline float run(float x) {
            if (!on) return x;
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float)y;
        }
    };
    Bq low, dip;
    bool on = true;

    static void lowShelf(Bq& s, double dB, double hz, int fs) {
        const double A = std::pow(10.0, dB / 40.0), w0 = 2.0 * 3.14159265358979323846 * hz / fs;
        const double cs = std::cos(w0), sn = std::sin(w0), al = sn / 2.0 * std::sqrt(2.0), sq = 2.0 * std::sqrt(A) * al;
        const double B0 = A * ((A + 1) - (A - 1) * cs + sq), B1 = 2 * A * ((A - 1) - (A + 1) * cs), B2 = A * ((A + 1) - (A - 1) * cs - sq);
        const double A0 = (A + 1) + (A - 1) * cs + sq,       A1 = -2 * ((A - 1) + (A + 1) * cs),    A2 = (A + 1) + (A - 1) * cs - sq;
        s.b0 = B0 / A0; s.b1 = B1 / A0; s.b2 = B2 / A0; s.a1 = A1 / A0; s.a2 = A2 / A0;
    }
    static void peaking(Bq& s, double dB, double hz, double q, int fs) {
        const double A = std::pow(10.0, dB / 40.0), w0 = 2.0 * 3.14159265358979323846 * hz / fs;
        const double cs = std::cos(w0), al = std::sin(w0) / (2.0 * q), a0 = 1.0 + al / A;
        s.b0 = (1.0 + al * A) / a0; s.b1 = -2.0 * cs / a0; s.b2 = (1.0 - al * A) / a0;
        s.a1 = -2.0 * cs / a0;      s.a2 = (1.0 - al / A) / a0;
    }
    void prepare(int fs) { prepare(fs, Tune()); }
    void prepare(int fs, const Tune& t) {
        low = Bq(); dip = Bq();
        if (t.lowDb != 0.0) { low.on = true; lowShelf(low, t.lowDb, t.lowHz, fs); }
        if (t.dipDb != 0.0) { dip.on = true; peaking(dip, t.dipDb, t.dipHz, t.dipQ, fs); }
    }
    inline float process(float x) {
        if (!on) return x;
        return dip.run(low.run(x));
    }
};

}
