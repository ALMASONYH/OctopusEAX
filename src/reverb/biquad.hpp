#pragma once
#include <cmath>

namespace eax {

struct Biquad {
    double b0, b1, b2, a1, a2, z0, z1;
    Biquad() : b0(1), b1(0), b2(0), a1(0), a2(0), z0(0), z1(0) {}
    void setShelf(bool high, double f0norm, double gain) {
        if (gain < 1e-3) gain = 1e-3;
        double A = std::sqrt(gain);
        double w0 = 2.0 * 3.14159265358979323846 * f0norm;
        double sn = std::sin(w0), cs = std::cos(w0);
        double alpha = sn / 2.0 * 1.41421356237309505;
        double sq = 2.0 * std::sqrt(A) * alpha;
        double B0, B1, B2, A0, A1, A2;
        if (high) {
            B0 = A * ((A + 1) + (A - 1) * cs + sq);
            B1 = -2 * A * ((A - 1) + (A + 1) * cs);
            B2 = A * ((A + 1) + (A - 1) * cs - sq);
            A0 = (A + 1) - (A - 1) * cs + sq;
            A1 = 2 * ((A - 1) - (A + 1) * cs);
            A2 = (A + 1) - (A - 1) * cs - sq;
        } else {
            B0 = A * ((A + 1) - (A - 1) * cs + sq);
            B1 = 2 * A * ((A - 1) - (A + 1) * cs);
            B2 = A * ((A + 1) - (A - 1) * cs - sq);
            A0 = (A + 1) + (A - 1) * cs + sq;
            A1 = -2 * ((A - 1) + (A + 1) * cs);
            A2 = (A + 1) + (A - 1) * cs - sq;
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0;
        a1 = A1 / A0; a2 = A2 / A0;
        z0 = z1 = 0.0;
    }

    static constexpr double SHELF_CORNER = 1.41421356237309505;
    void setShelfAtRef(bool high, double f0norm, double gainAtF0) {
        setShelf(high, high ? f0norm / SHELF_CORNER : f0norm * SHELF_CORNER,
                 gainAtF0 * gainAtF0);
    }

    void setPeak(double f0norm, double gainDb, double q) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * f0norm;
        const double alpha = std::sin(w0) / (2.0 * q), cs = std::cos(w0);
        const double A0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / A0; b1 = -2.0 * cs / A0; b2 = (1.0 - alpha * A) / A0;
        a1 = -2.0 * cs / A0; a2 = (1.0 - alpha / A) / A0;
        z0 = z1 = 0.0;
    }

    inline double process(double x) {
        double y = b0 * x + z0;
        z0 = b1 * x - a1 * y + z1;
        z1 = b2 * x - a2 * y;
        return y;
    }
};

}
