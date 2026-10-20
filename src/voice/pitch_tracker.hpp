#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace voice {

class PitchTracker {
public:
    static const int WIN = 2048;

    void prepare(double fs) {
        fs_ = fs;
        lagMin_ = (int)(fs / 400.0);
        lagMax_ = (int)(fs / 70.0);
        if (lagMax_ > WIN / 2) lagMax_ = WIN / 2;
        pre_.assign(WIN, 0.0);
        half_.assign(WIN / 2, 0.0);
        nac_.assign(lagMax_ / 2 + 3, 0.0);
        reset();
    }

    void reset() {
        f0_ = period_ = conf_ = 0.0;
        hist_[0] = hist_[1] = hist_[2] = 0.0;
    }

    bool analyse(const float* x) {
        pre_[0] = 0.0;
        for (int n = 1; n < WIN; ++n) pre_[n] = x[n] - 0.95 * x[n - 1];
        for (int n = 0; n < WIN / 2; ++n) half_[n] = 0.5 * (pre_[2 * n] + pre_[2 * n + 1]);

        const int hMin = lagMin_ / 2, hMax = lagMax_ / 2;
        const int W = WIN / 2 - hMax;
        double e0 = 0.0;
        for (int n = 0; n < W; ++n) e0 += half_[n] * half_[n];
        if (e0 < 1e-7) { conf_ = 0.0; return false; }

        double best = 0.0;
        int bestLag = hMin;
        for (int L = hMin; L <= hMax; ++L) {
            double c = 0.0, eL = 0.0;
            for (int n = 0; n < W; ++n) { const double b = half_[n + L]; c += half_[n] * b; eL += b * b; }
            const double v = c / (std::sqrt(e0 * eL) + 1e-20);
            nac_[L] = v;
            if (v > best) { best = v; bestLag = L; }
        }
        if (best < 0.5) { conf_ = best; return false; }

        int pick = bestLag;
        for (int L = hMin + 1; L < bestLag; ++L)
            if (nac_[L] >= 0.85 * best && nac_[L] > nac_[L - 1] && nac_[L] >= nac_[L + 1]) { pick = L; break; }

        const int Wf = WIN - lagMax_;
        double ef = 0.0;
        for (int n = 0; n < Wf; ++n) ef += pre_[n] * pre_[n];
        double vals[5];
        double fb = -1.0;
        int fbi = 2;
        for (int k = -2; k <= 2; ++k) {
            const int L = 2 * pick + k;
            if (L < lagMin_ || L > lagMax_) { vals[k + 2] = -1.0; continue; }
            double c = 0.0, eL = 0.0;
            for (int n = 0; n < Wf; ++n) { const double b = pre_[n + L]; c += pre_[n] * b; eL += b * b; }
            vals[k + 2] = c / (std::sqrt(ef * eL) + 1e-20);
            if (vals[k + 2] > fb) { fb = vals[k + 2]; fbi = k + 2; }
        }
        double lag = 2.0 * pick + (fbi - 2);
        if (fbi > 0 && fbi < 4 && vals[fbi - 1] >= 0.0 && vals[fbi + 1] >= 0.0) {
            const double a = vals[fbi - 1], b = vals[fbi], c = vals[fbi + 1];
            const double den = a - 2.0 * b + c;
            if (den < 0.0) lag += 0.5 * (a - c) / den;
        }
        conf_ = fb > 0.0 ? fb : best;

        hist_[2] = hist_[1]; hist_[1] = hist_[0]; hist_[0] = lag;
        double m = lag;
        if (hist_[1] > 0.0 && hist_[2] > 0.0)
            m = std::max(std::min(hist_[0], hist_[1]), std::min(std::max(hist_[0], hist_[1]), hist_[2]));
        period_ = m;
        f0_ = fs_ / m;
        return conf_ > 0.5;
    }

    double period() const { return period_; }
    double f0() const { return f0_; }

private:
    double fs_ = 48000.0;
    int lagMin_ = 0, lagMax_ = 0;
    std::vector<double> pre_, half_, nac_;
    double f0_ = 0.0, period_ = 0.0, conf_ = 0.0;
    double hist_[3] = {0.0, 0.0, 0.0};
};

}
