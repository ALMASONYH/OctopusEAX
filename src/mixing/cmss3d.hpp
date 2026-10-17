#pragma once
#include <cmath>

namespace mix {

struct CMSS3D {
    static const int ITD = 12;
    float dl[ITD] = {0}; int pos = 0;
    float lpX = 0.f, lpS = 0.f;
    float aX = 0.f, aS = 0.f;
    void prepare(int fs) {
        aX = expf(-2.0f * 3.14159265f * 1400.0f / fs);
        aS = expf(-2.0f * 3.14159265f * 200.0f / fs);
        for (int i = 0; i < ITD; ++i) dl[i] = 0.f;
        pos = 0; lpX = lpS = 0.f;
    }
    inline void process(float& l, float& r) {
        const float m = 0.5f * (l + r), s = 0.5f * (l - r);

        const float md = dl[pos]; dl[pos] = m; if (++pos >= ITD) pos = 0;
        lpX += (1.0f - aX) * (md - lpX);
        const float x = 0.35f * lpX;

        lpS += (1.0f - aS) * (s - lpS);
        const float sw = s + 0.8f * (s - lpS);
        const float G = 0.89f;
        l = G * (m + x + sw);
        r = G * (m + x - sw);
    }
};

}
