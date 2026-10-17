#pragma once
#include <vector>
#include <algorithm>
#include "fft.hpp"

namespace conv {

class Tier {
public:
    void init(int block, const float* irL, const float* irR, int len) {
        B_ = block; N_ = 2 * block; half_ = N_ / 2 + 1;
        parts_ = (len + B_ - 1) / B_;
        fft_.init(N_);
        specL_.assign((size_t)parts_ * half_, cf(0.f, 0.f));
        specR_.assign((size_t)parts_ * half_, cf(0.f, 0.f));
        std::vector<cf> buf(N_);
        for (int p = 0; p < parts_; ++p) {
            for (int ch = 0; ch < 2; ++ch) {
                const float* src = ch ? irR : irL;
                for (int i = 0; i < N_; ++i) {
                    const int k = p * B_ + i;
                    buf[i] = (i < B_ && k < len) ? cf(src[k], 0.f) : cf(0.f, 0.f);
                }
                fft_.forward(buf.data());
                cf* dst = (ch ? specR_.data() : specL_.data()) + (size_t)p * half_;
                for (int i = 0; i < half_; ++i) dst[i] = buf[i];
            }
        }
        fdl_.assign((size_t)parts_ * half_, cf(0.f, 0.f));
        X_.resize(N_); accL_.resize(N_); accR_.resize(N_);
        in_.assign(N_, 0.f);
        outL_.assign(B_, 0.f); outR_.assign(B_, 0.f);
        pos_ = 0; fill_ = 0;
    }
    void clear() {
        std::fill(fdl_.begin(), fdl_.end(), cf(0.f, 0.f));
        std::fill(in_.begin(), in_.end(), 0.f);
        std::fill(outL_.begin(), outL_.end(), 0.f);
        std::fill(outR_.begin(), outR_.end(), 0.f);
        pos_ = 0; fill_ = 0;
    }
    inline void push(float x, float& l, float& r) {
        in_[B_ + fill_] = x;
        l = outL_[fill_]; r = outR_[fill_];
        if (++fill_ == B_) { crank(); fill_ = 0; }
    }
private:
    void crank() {
        for (int i = 0; i < N_; ++i) X_[i] = cf(in_[i], 0.f);
        fft_.forward(X_.data());
        pos_ = (pos_ + parts_ - 1) % parts_;
        cf* slot = fdl_.data() + (size_t)pos_ * half_;
        for (int i = 0; i < half_; ++i) slot[i] = X_[i];
        std::fill(accL_.begin(), accL_.begin() + half_, cf(0.f, 0.f));
        std::fill(accR_.begin(), accR_.begin() + half_, cf(0.f, 0.f));
        for (int p = 0; p < parts_; ++p) {
            const cf* xk = fdl_.data() + (size_t)((pos_ + p) % parts_) * half_;
            const cf* hl = specL_.data() + (size_t)p * half_;
            const cf* hr = specR_.data() + (size_t)p * half_;
            for (int i = 0; i < half_; ++i) {
                accL_[i] += xk[i] * hl[i];
                accR_[i] += xk[i] * hr[i];
            }
        }

        for (int i = 1; i < half_ - 1; ++i) {
            accL_[N_ - i] = std::conj(accL_[i]);
            accR_[N_ - i] = std::conj(accR_[i]);
        }
        fft_.inverse(accL_.data());
        fft_.inverse(accR_.data());
        for (int i = 0; i < B_; ++i) { outL_[i] = accL_[B_ + i].real(); outR_[i] = accR_[B_ + i].real(); }
        for (int i = 0; i < B_; ++i) in_[i] = in_[B_ + i];
    }
    FFT fft_;
    int B_ = 0, N_ = 0, half_ = 0, parts_ = 0, pos_ = 0, fill_ = 0;
    std::vector<cf> specL_, specR_, fdl_, X_, accL_, accR_;
    std::vector<float> in_, outL_, outR_;
};

}
