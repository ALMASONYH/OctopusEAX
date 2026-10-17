#pragma once
#include <cmath>

namespace mix {

struct MicDrive {
    static constexpr float RELEASE = 0.002f;

    float k = 0.0f, bend = 0.0f;

    void set(int fader) {
        const float amt = fader <= 100 ? 0.0f : (fader - 100) / 900.0f;
        k = amt <= 0.0f ? 0.0f : 1.0f + 6.0f * amt;
        const float want = k <= 0.0f ? 0.0f : tanhf(k);
        if (want >= bend) bend = want;
        else { bend -= RELEASE; if (bend < want) bend = want; }
    }
    float gain() const { return k <= 0.0f ? 1.0f : k / tanhf(k); }
    inline float process(float x) const {
        return bend <= 0.0f ? x : tanhf(x * bend) / bend;
    }
};

}
