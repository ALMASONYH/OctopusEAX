#pragma once
#include <cmath>
#include "eq_config.hpp"
#include "biquad.hpp"
#include "filter_design.hpp"

namespace eqb {

struct GraphicEQ {
    Biquad l[EQ_BANDS], r[EQ_BANDS];
    double fs = 48000.0;
    float levelGain = 1.0f;
    int bandDb[EQ_BANDS] = {0, 0, 0, 0, 0, 0, 0};
    int levelDb = 0;

    void prepare(double fs_) {
        fs = fs_;
        for (int k = 0; k < EQ_BANDS; ++k) { l[k].reset(); r[k].reset(); }
        set(bandDb, levelDb);
    }

    void set(const int* bands, int level) {
        for (int k = 0; k < EQ_BANDS; ++k) {
            int d = bands[k];
            if (d > EQ_RANGE) d = EQ_RANGE;
            if (d < -EQ_RANGE) d = -EQ_RANGE;
            bandDb[k] = d;
            peaking(l[k], fs, EQ_FREQ[k], (double)d, 1.0);
            r[k].shape(l[k]);
        }
        if (level < 0) level = 0;
        if (level > EQ_LEVEL_RANGE) level = EQ_LEVEL_RANGE;
        levelDb = level;
        levelGain = (float)std::pow(10.0, -level / 20.0);
    }
    inline void process(float& L, float& R) {
        for (int k = 0; k < EQ_BANDS; ++k) { L = l[k].run(L); R = r[k].run(R); }
        L *= levelGain; R *= levelGain;
    }
};

}
