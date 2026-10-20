#pragma once
#include "eq_config.hpp"
#include "biquad.hpp"
#include "filter_design.hpp"

namespace eqb {

struct BassBoost {
    Biquad l, r;
    double fs = 48000.0, dB = 0.0;
    void prepare(double fs_) { fs = fs_; l.reset(); r.reset(); set(0.0); }

    static double boostFor(double micGainDb) {
        return micGainDb <= 0.0 ? 0.0 : 0.50 * micGainDb;
    }
    void set(double db) {
        if (db == dB && (db != 0.0 || l.b0 == 1.0)) return;
        if (dB == 0.0 && db != 0.0) { l.reset(); r.reset(); }
        dB = db;
        lowShelf(l, fs, BASS_FREQ, dB);
        r.shape(l);
    }
    inline void process(float& L, float& R) {
        if (dB == 0.0) return;
        L = l.run(L); R = r.run(R);
    }
    inline float run(float x) { return dB == 0.0 ? x : l.run(x); }
};

}
