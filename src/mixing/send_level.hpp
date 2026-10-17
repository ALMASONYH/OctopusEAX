#pragma once
#include <cmath>

namespace mix {

struct SendLevel {
    static constexpr float THRESHOLD_DB = -4.0f, RATIO = 3.0f;
    static constexpr double WINDOW_SEC = 0.200;

    float keep = 0.0f, power = 0.0f, threshold = 0.0f, slope = 0.0f;

    void prepare(int fs, float thresholdDb = THRESHOLD_DB, float ratio = RATIO) {
        keep = (float)std::exp(-1.0 / (WINDOW_SEC * fs));
        threshold = powf(10.0f, thresholdDb / 10.0f);
        slope = 0.5f * (1.0f / ratio - 1.0f);
        power = 0.0f;
    }
    inline float process(float x) {
        power = keep * power + (1.0f - keep) * x * x;
        return power <= threshold ? 1.0f : powf(power / threshold, slope);
    }
};

}
