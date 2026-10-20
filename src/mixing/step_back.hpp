#pragma once
#include <cmath>

namespace mix {

struct StepBack {
    static constexpr float HEARD_DB = -45.0f;
    static constexpr double WINDOW_SEC = 0.050, RELEASE_RINGS = 2.0;

    float heldDb = 0.0f, fallPerSample = 0.0f, power = 0.0f, keep = 0.0f, floor = 0.0f;

    void prepare(int fs, double ringSeconds) {
        fallPerSample = (float)(60.0 / (RELEASE_RINGS * ringSeconds * fs));
        keep = (float)std::exp(-1.0 / (WINDOW_SEC * fs));
        floor = powf(10.0f, HEARD_DB / 10.0f);
        reset();
    }
    void reset() { heldDb = 0.0f; power = 0.0f; }
    void idle(int n) {
        power *= powf(keep, (float)n);
        heldDb -= fallPerSample * (float)n;
        if (heldDb < 0.0f) heldDb = 0.0f;
    }

    float cutDb(float wantDb, const float* in, int n) {
        for (int i = 0; i < n; ++i) power = keep * power + (1.0f - keep) * in[i] * in[i];
        heldDb -= fallPerSample * (float)n;
        if (heldDb < 0.0f) heldDb = 0.0f;
        if (power > floor && wantDb > heldDb) heldDb = wantDb;
        return wantDb > heldDb ? wantDb : heldDb;
    }
};

}
