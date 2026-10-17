#pragma once
#include <cmath>
#include "eq_config.hpp"
#include "biquad.hpp"

namespace eqb {

inline void peaking(Biquad& q, double fs, double f0, double dB, double octaves) {
    if (dB == 0.0) { q.identity(); return; }
    const double A  = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * PI_ * f0 / fs;
    const double sn = std::sin(w0), cs = std::cos(w0);
    const double alpha = sn * std::sinh(std::log(2.0) / 2.0 * octaves * w0 / sn);
    const double a0 = 1.0 + alpha / A;
    q.b0 = (1.0 + alpha * A) / a0;
    q.b1 = (-2.0 * cs) / a0;
    q.b2 = (1.0 - alpha * A) / a0;
    q.a1 = (-2.0 * cs) / a0;
    q.a2 = (1.0 - alpha / A) / a0;
}

inline void lowShelf(Biquad& q, double fs, double f0, double dB) {
    if (dB == 0.0) { q.identity(); return; }
    const double A  = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * PI_ * f0 / fs;
    const double sn = std::sin(w0), cs = std::cos(w0);
    const double alpha = sn / 2.0 * std::sqrt(2.0);
    const double sq = 2.0 * std::sqrt(A) * alpha;
    const double a0 = (A + 1.0) + (A - 1.0) * cs + sq;
    q.b0 = (A * ((A + 1.0) - (A - 1.0) * cs + sq)) / a0;
    q.b1 = (2.0 * A * ((A - 1.0) - (A + 1.0) * cs)) / a0;
    q.b2 = (A * ((A + 1.0) - (A - 1.0) * cs - sq)) / a0;
    q.a1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cs)) / a0;
    q.a2 = ((A + 1.0) + (A - 1.0) * cs - sq) / a0;
}

}
