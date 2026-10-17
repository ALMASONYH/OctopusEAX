#pragma once
#include <cmath>
#include "fx_parts.hpp"

namespace voice {

class Glitcher {
public:
    void prepare(double fs) {
        fs_ = fs;
        settle_ = (int)(0.050 * fs);
        chopStep_ = (float)(1.0 - std::exp(-1.0 / (0.0015 * fs)));
        stutter_.prepare(fs);
        breaker_.prepare(fs);
        reset();
    }

    void reset() {
        stutter_.clear();
        breaker_.clear();
        since_ = settle_;
        tape_ = 0; chopLeft_ = 0; chopPos_ = 0; chopOn_ = 0; chopPeriod_ = 0; ringLeft_ = 0;
        chopGain_ = 1.0f; ringPhase_ = 0.0; ringHz_ = 0.0; tapeStep_ = 0.0;
    }

    bool busy() const { return stutter_.on || chopLeft_ > 0 || ringLeft_ > 0; }

    void fire() {
        switch ((int)(rng_.uniform() * 6.0) % 6) {
            case 0:
                stutter_.start((int)(rng_.between(0.060, 0.150) * fs_), 2 + (int)(rng_.uniform() * 4.0), 1.0, false);
                break;
            case 1: {
                const int slice = (int)(rng_.between(0.015, 0.040) * fs_);
                stutter_.start(slice, (int)(rng_.between(0.25, 0.50) * fs_ / slice), 1.0, false);
                break;
            }
            case 2:
                stutter_.start((int)(rng_.between(0.120, 0.250) * fs_), 1 + (int)(rng_.uniform() * 2.0), 1.0, true);
                break;
            case 3: {
                const int slice = (int)(rng_.between(0.100, 0.200) * fs_);
                stutter_.start(slice, 2 + (int)(rng_.uniform() * 3.0), 1.0, false);
                tape_ = (int)(rng_.between(0.150, 0.400) * fs_);
                tapeStep_ = 0.75 / tape_;
                break;
            }
            case 4:
                chopLeft_ = (int)(rng_.between(0.250, 0.600) * fs_);
                chopPeriod_ = (int)(rng_.between(0.040, 0.090) * fs_);
                chopOn_ = (int)(chopPeriod_ * rng_.between(0.40, 0.65));
                chopPos_ = 0;
                break;
            default:
                ringLeft_ = (int)(rng_.between(0.080, 0.250) * fs_);
                ringHz_ = rng_.between(90.0, 450.0);
                ringPhase_ = 0.0;
                break;
        }
        if (rng_.uniform() < 0.30)
            breaker_.crushFor((int)(rng_.between(0.040, 0.140) * fs_), 3 + (int)(rng_.uniform() * 6.0),
                              (float)std::pow(2.0, 3 + (int)(rng_.uniform() * 3.0)));
    }

    inline float run(float x) {
        if (tape_ > 0) {
            --tape_;
            stutter_.speed -= tapeStep_;
            if (stutter_.speed < 0.25) stutter_.speed = 0.25;
            if (tape_ == 0) stutter_.stop();
        }
        float y = stutter_.run(x);
        if (since_ < settle_) ++since_;
        if (stutter_.wrapped && tape_ == 0) turn();
        if (chopLeft_ > 0) {
            --chopLeft_;
            if (++chopPos_ >= chopPeriod_) chopPos_ = 0;
            const float want = chopPos_ < chopOn_ ? 1.0f : 0.0f;
            chopGain_ += (want - chopGain_) * chopStep_;
            y *= chopGain_;
        } else if (chopGain_ < 1.0f) {
            chopGain_ += (1.0f - chopGain_) * chopStep_;
            y *= chopGain_;
        }
        if (ringLeft_ > 0) {
            --ringLeft_;
            const float m = (float)std::sin(TWO_PI * ringPhase_);
            ringPhase_ += ringHz_ / fs_;
            if (ringPhase_ >= 1.0) ringPhase_ -= 1.0;
            y = y * (0.3f + 0.7f * m);
        }
        return breaker_.run(y);
    }

private:
    void turn() {
        if (since_ < settle_) return;
        since_ = 0;
        const double r = rng_.uniform();
        if (r < 0.24) stutter_.shrink(0.5);
        else if (r < 0.40) stutter_.rev = !stutter_.rev;
        else if (r < 0.50) stutter_.speed = stutter_.speed > 1.5 ? 1.0 : 2.0;
        else if (r < 0.57) stutter_.speed = stutter_.speed < 0.75 ? 1.0 : 0.5;
        else if (r < 0.80) { stutter_.grow(); stutter_.speed = 1.0; }
        if (rng_.uniform() < 0.22) breaker_.dropFor((int)(rng_.between(0.010, 0.045) * fs_));
        if (rng_.uniform() < 0.16)
            breaker_.crushFor((int)(rng_.between(0.040, 0.140) * fs_), 3 + (int)(rng_.uniform() * 6.0),
                              (float)std::pow(2.0, 3 + (int)(rng_.uniform() * 3.0)));
    }

    double fs_ = 48000.0;
    int since_ = 0, settle_ = 0;
    int tape_ = 0, chopLeft_ = 0, chopPeriod_ = 0, chopOn_ = 0, chopPos_ = 0, ringLeft_ = 0;
    double tapeStep_ = 0.0, ringPhase_ = 0.0, ringHz_ = 0.0;
    float chopGain_ = 1.0f, chopStep_ = 0.0f;
    Stutter stutter_;
    Breaker breaker_;
    Rng rng_{0x9E3779B9u};
};

}
