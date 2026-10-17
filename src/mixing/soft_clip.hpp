#pragma once
#include <cmath>

namespace mix {

struct SoftClip {
    static constexpr float KNEE = 0.97f;
    static inline float bend(float v) {
        const float a = fabsf(v);
        if (a <= KNEE) return v;
        const float y = KNEE + (1.0f - KNEE) * tanhf((a - KNEE) / (1.0f - KNEE));
        return v < 0.0f ? -y : y;
    }
    inline void process(float& l, float& r) { l = bend(l); r = bend(r); }
};

}
