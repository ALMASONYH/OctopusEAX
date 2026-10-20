#pragma once
#include <cmath>

namespace mix {

struct Subsonic {
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void prepare(int fs) {
        x1 = x2 = y1 = y2 = 0;
        const double w = 2.0 * 3.14159265358979323846 * 20.0 / fs;
        const double c = cos(w), sn = sin(w);
        const double alpha = sn / (2.0 * 0.7071067811865476);
        const double a0 = 1.0 + alpha;
        b0 = ((1.0 + c) / 2.0) / a0;
        b1 = (-(1.0 + c)) / a0;
        b2 = b0;
        a1 = (-2.0 * c) / a0;
        a2 = (1.0 - alpha) / a0;
    }
    inline float process(float in) {
        const double x = in;
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return (float)y;
    }
};

}
