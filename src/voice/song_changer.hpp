#pragma once
#include <algorithm>
#include <vector>
#include "glitch_pad.hpp"
#include "song_shifter.hpp"
#include "voice_fx.hpp"
#include "voice_masks.hpp"

namespace voice {

class SongChanger {
public:
    static constexpr double REF_HZ = 300.0;
    static const int CHUNK = 256;

    void prepare(double fs) {
        shifter_.prepare(fs);
        fxL_.prepare(fs, false);
        fxR_.prepare(fs, false);
        glitch_.prepare(fs, 2);
        glide_ = (float)(1.0 / (0.040 * fs));
        mid_.assign(CHUNK, 0.0f);
        dryL_.assign(CHUNK, 0.0f);
        dryR_.assign(CHUNK, 0.0f);
        reset();
    }

    void reset() {
        shifter_.reset();
        start();
    }

    void start() {
        shifter_.restart();
        fxL_.reset();
        fxR_.reset();
        glitch_.reset();
        wet_ = shiftWet_ = 0.0f;
    }

    void set(int pitchHz, int ratioPermille, int mask) {
        shifter_.set(ratioPermille / 1000.0 * (REF_HZ + pitchHz) / REF_HZ);
        mask_ = mask;
    }

    void hold(bool on, int style) {
        glitch_.set(style);
        glitch_.hold(on);
    }

    void listen(const float* l, const float* r, int n) { shifter_.feed(l, r, n); }

    bool idle() const { return !on_ && wet_ <= 0.0f; }

    void process(float* l, float* r, int n, bool on) {
        on_ = on;
        for (int at = 0; at < n; at += CHUNK) chunk(l + at, r + at, std::min(CHUNK, n - at));
    }

private:
    static float toward(float v, float target, float step) {
        return v < target ? std::min(target, v + step) : std::max(target, v - step);
    }

    void chunk(float* l, float* r, int n) {
        std::copy(l, l + n, dryL_.begin());
        std::copy(r, r + n, dryR_.begin());

        const bool shift = !shifter_.neutral();
        if (shift || shiftWet_ > 0.0f) {
            if (shiftWet_ <= 0.0f) shifter_.restart();
            shifter_.process(l, r, n);
            for (int i = 0; i < n; ++i) {
                shiftWet_ = toward(shiftWet_, shift ? 1.0f : 0.0f, glide_);
                l[i] = dryL_[(size_t)i] + shiftWet_ * (l[i] - dryL_[(size_t)i]);
                r[i] = dryR_[(size_t)i] + shiftWet_ * (r[i] - dryR_[(size_t)i]);
            }
        } else shifter_.feed(l, r, n);

        const int fx = mask_ >= 0 && mask_ < NUM_MASKS ? MASKS[mask_].fx : FX_NONE;
        const double a = fx != FX_NONE ? MASKS[mask_].a : 0.0, b = fx != FX_NONE ? MASKS[mask_].b : 0.0;
        if (fx == FX_NONE) {
            fxL_.configure(FX_NONE, 0.0, 0.0);
            fxR_.configure(FX_NONE, 0.0, 0.0);
        } else if (fx < FX_ANONYMOUS) {
            fxL_.configure(fx, a, b);
            fxR_.configure(fx, a, b);
            fxL_.process(l, n);
            fxR_.process(r, n);
        } else {
            fxL_.configure(fx, a, b);
            for (int i = 0; i < n; ++i) mid_[(size_t)i] = 0.5f * (l[i] + r[i]);
            fxL_.process(mid_.data(), n);
            for (int i = 0; i < n; ++i) l[i] = r[i] = mid_[(size_t)i];
        }

        glitch_.process(l, r, n);
        for (int i = 0; i < n; ++i) {
            wet_ = toward(wet_, on_ ? 1.0f : 0.0f, glide_);
            l[i] = dryL_[(size_t)i] + wet_ * (l[i] - dryL_[(size_t)i]);
            r[i] = dryR_[(size_t)i] + wet_ * (r[i] - dryR_[(size_t)i]);
        }
    }

    SongShifter shifter_;
    VoiceFx fxL_, fxR_;
    GlitchPad glitch_;
    std::vector<float> mid_, dryL_, dryR_;
    int mask_ = MASK_NEUTRAL;
    float glide_ = 0.0005f, wet_ = 0.0f, shiftWet_ = 0.0f;
    bool on_ = false;
};

}
