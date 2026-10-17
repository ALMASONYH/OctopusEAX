#pragma once
#include <complex>
#include <vector>
#include <cmath>

namespace conv {
typedef std::complex<float> cf;

struct FFT {
    int n = 0, log2n = 0;
    std::vector<cf> tw;
    std::vector<int> rev;
    void init(int size) {
        n = size; log2n = 0;
        while ((1 << log2n) < n) ++log2n;
        tw.resize(n / 2);
        for (int k = 0; k < n / 2; ++k) {
            const double a = -2.0 * 3.14159265358979323846 * k / n;
            tw[k] = cf((float)cos(a), (float)sin(a));
        }
        rev.resize(n);
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < log2n; ++b) if (i & (1 << b)) r |= 1 << (log2n - 1 - b);
            rev[i] = r;
        }
    }
    void forward(cf* a) const { run(a, false); }
    void inverse(cf* a) const {
        run(a, true);
        const float s = 1.0f / n;
        for (int i = 0; i < n; ++i) a[i] *= s;
    }
private:
    void run(cf* a, bool inv) const {
        for (int i = 0; i < n; ++i) if (i < rev[i]) std::swap(a[i], a[rev[i]]);
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    cf w = tw[j * step];
                    if (inv) w = std::conj(w);
                    const cf u = a[i + j], v = a[i + j + half] * w;
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }
};

}
