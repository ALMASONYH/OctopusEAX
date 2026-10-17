#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx_parts.hpp"
#include "glitcher.hpp"
#include "timbre.hpp"
#include "vocoder.hpp"
#include "voice_masks.hpp"
#include "voice_shaper.hpp"

namespace voice {

class VoiceFx {
public:
    static const int MAX_BLOCK = 16384;

    void prepare(double fs, bool speech = true) {
        fs_ = fs;
        speech_ = speech;
        vbuf_.assign(4096, 0.0);
        vocoder_.prepare(fs);
        repeater_.prepare(fs);
        glitcher_.prepare(fs);
        wobble_.prepare((int)(0.060 * fs));
        chorus_.prepare((int)(0.040 * fs));
        room_.prepare((int)(0.400 * fs));
        up_.prepare(fs);
        down_.prepare(fs);
        crowdDry_.prepare(VoiceShaper::DELAY + 8);
        upBuf_.assign(MAX_BLOCK, 0.0f);
        downBuf_.assign(MAX_BLOCK, 0.0f);
        match_.prepare(fs);
        guard_.prepare(fs);
        onsets_.prepare(fs, -40.0, 0.25);
        voiceEnv_.prepare(fs, 0.005, 0.060);
        hissGate_.prepare(fs, 0.010, 0.080);
        timbre_.prepare(fs, STRANGER_TONE);
        reset();
    }

    void reset() {
        phase_ = 0.0; vpos_ = 0;
        std::fill(vbuf_.begin(), vbuf_.end(), 0.0);
        hx1_ = hx2_ = hy1_ = hy2_ = 0.0; lx1_ = lx2_ = ly1_ = ly2_ = 0.0;
        type_ = FX_NONE; a_ = b_ = 0.0; ready_ = false;
    }

    void configure(int type, double a, double b) {
        if (type == type_ && a == a_ && b == b_ && ready_) return;
        if (type != type_) {
            phase_ = 0.0; vpos_ = 0;
            std::fill(vbuf_.begin(), vbuf_.end(), 0.0);
            hx1_ = hx2_ = hy1_ = hy2_ = 0.0; lx1_ = lx2_ = ly1_ = ly2_ = 0.0;
        }
        if (type >= FX_ANONYMOUS && type != type_) start(type);
        type_ = type; a_ = a; b_ = b; ready_ = true;
        if (type_ == FX_RADIO) designRadio();
    }

    void process(float* x, int n) {
        switch (type_) {
            case FX_RING:    ring(x, n);    break;
            case FX_TREMOLO: tremolo(x, n); break;
            case FX_RADIO:   radio(x, n);   break;
            case FX_VIBRATO: vibrato(x, n); break;
            default:
                if (type_ >= FX_ANONYMOUS) {
                    for (int at = 0; at < n; at += MAX_BLOCK) own(x + at, std::min(MAX_BLOCK, n - at));
                }
                break;
        }
    }

private:
    static constexpr double ALIEN_BEND = 0.0533, UNDER_BEND = 0.0293;
    static constexpr double DRUNK_BEND_LO = 0.0413, DRUNK_BEND_HI = 0.0656, HICCUP = 400.0;
    static constexpr double ZOMBIE_BEND = 0.030, BABY_BEND = 0.012;
    static constexpr double GIANT_ECHO_SEC = 0.110, MEGAPHONE_ECHO_SEC = 0.170, CYBORG_COMB_SEC = 0.0053, HELMET_COMB_SEC = 0.0016;
    static constexpr double STRANGER_TONE[Timbre::BANDS] = {-6.5, -3.4, -6.8};
    static constexpr float HUSK = 0.2f;
    static constexpr double BREATH_WAIT_SEC = 0.8, BREATH_TALK_SEC = 0.05, BREATH_CYCLE_SEC = 4.4, BREATH_IN_SEC = 1.3, BREATH_HOLD_SEC = 0.4, BREATH_OUT_SEC = 1.8, BREATH_LEVEL = 0.12;

    struct Bubble {
        double phase = 0.0, hz = 0.0, grow = 1.0, age = 0.0, life = 0.0;
        float level = 0.0f;
    };

    double fs_ = 48000.0;
    int type_ = FX_NONE;
    double a_ = 0.0, b_ = 0.0;
    bool ready_ = false, speech_ = true;
    double phase_ = 0.0;
    double hb0_ = 1, hb1_ = 0, hb2_ = 0, ha1_ = 0, ha2_ = 0, hx1_ = 0, hx2_ = 0, hy1_ = 0, hy2_ = 0;
    double lb0_ = 1, lb1_ = 0, lb2_ = 0, la1_ = 0, la2_ = 0, lx1_ = 0, lx2_ = 0, ly1_ = 0, ly2_ = 0;
    std::vector<double> vbuf_;
    int vpos_ = 0;

    Vocoder vocoder_;
    Repeater repeater_;
    Glitcher glitcher_;
    Wobble wobble_, chorus_, crowdDry_, room_;
    VoiceShaper up_, down_;
    std::vector<float> upBuf_, downBuf_;
    LevelMatch match_;
    PeakGuard guard_;
    Onsets onsets_;
    Follower voiceEnv_, hissGate_;
    Filter f1_, f2_, f3_, f4_;
    Timbre timbre_;
    Swing pitchSwing_, levelSwing_;
    Rng rng_{0x2545F491u};
    double bend_ = 0.0, clock_ = 0.0;
    int cooldown_ = 0, hiccup_ = 0;
    Bubble bubbles_[4];
    bool talking_ = false;
    int quietFor_ = 0, beepLeft_ = 0, beepLen_ = 0, squelchLeft_ = 0;
    double beepHz_ = 0.0, beepPhase_ = 0.0, breath_ = 0.0, quietSec_ = 0.0, loudSec_ = 0.0;
    float breathGain_ = 0.0f, holdLevel_ = 0.0f;
    int holdCount_ = 0;
    bool squelchNext_ = false;

    void start(int type) {
        repeater_.clear();
        wobble_.clear();
        chorus_.clear();
        crowdDry_.clear();
        room_.clear();
        breath_ = quietSec_ = loudSec_ = 0.0;
        breathGain_ = holdLevel_ = 0.0f;
        holdCount_ = 0;
        phase_ = 0.0;
        match_.prepare(fs_);
        guard_.prepare(fs_);
        onsets_.prepare(fs_, -40.0, 0.25);
        voiceEnv_.env = hissGate_.env = 0.0f;
        for (Filter* f : {&f1_, &f2_, &f3_, &f4_}) { f->clear(); f->b0 = 1; f->b1 = f->b2 = f->a1 = f->a2 = 0; }
        cooldown_ = hiccup_ = 0;
        glitcher_.reset();
        clock_ = 0.0;
        for (Bubble& b : bubbles_) b.life = 0.0;
        talking_ = false; quietFor_ = 0; beepLeft_ = 0; squelchLeft_ = 0; squelchNext_ = false;
        switch (type) {
            case FX_ANONYMOUS:
                vocoder_.set({false, 1.12, 0.04, 96.0, 0.025});
                vocoder_.reset();
                break;
            case FX_GHOST:
                vocoder_.set({true, 0.95, 0.02, 100.0, 0.0});
                vocoder_.reset();
                break;
            case FX_ALIEN:
                pitchSwing_.prepare(6.0, 10.0, rng_);
                levelSwing_.prepare(10.0, 14.0, rng_);
                break;
            case FX_DRUNK:
                pitchSwing_.prepare(0.35, 0.8, rng_);
                bend_ = rng_.between(DRUNK_BEND_LO, DRUNK_BEND_HI);
                f1_.lowpass(fs_, 3200.0, 0.707);
                break;
            case FX_UNDERWATER:
                pitchSwing_.prepare(8.0, 14.0, rng_);
                levelSwing_.prepare(4.0, 8.0, rng_);
                f1_.lowpass(fs_, 800.0, 0.9);
                f2_.lowpass(fs_, 1300.0, 0.707);
                f3_.peak(fs_, 350.0, 1.0, 4.0);
                break;
            case FX_MONSTER:
                levelSwing_.prepare(38.0, 46.0, rng_);
                f1_.lowShelf(fs_, 160.0, 4.0);
                break;
            case FX_DUCK:
                f1_.highpass(fs_, 380.0, 0.707);
                f2_.peak(fs_, 1300.0, 3.0, 12.0);
                f3_.peak(fs_, 2600.0, 4.0, 7.0);
                break;
            case FX_CROWD:
                up_.reset(); down_.reset();
                up_.set(0.0, 92, 1.335);
                down_.set(0.0, 118, 0.749);
                break;
            case FX_ASTRONAUT:
                f1_.highpass(fs_, 320.0, 0.707);
                f2_.highpass(fs_, 320.0, 0.707);
                f3_.lowpass(fs_, 3200.0, 0.707);
                f4_.lowpass(fs_, 3200.0, 0.707);
                break;
            case FX_MOUSE:
                f1_.highpass(fs_, 260.0, 0.707);
                f2_.peak(fs_, 1500.0, 1.4, 6.0);
                f3_.peak(fs_, 3200.0, 1.0, 3.0);
                break;
            case FX_BABY:
                pitchSwing_.prepare(6.0, 8.0, rng_);
                f1_.peak(fs_, 2600.0, 1.0, 3.0);
                break;
            case FX_GOBLIN:
                f1_.highpass(fs_, 180.0, 0.707);
                f2_.peak(fs_, 900.0, 1.2, 5.0);
                break;
            case FX_GIANT:
                f1_.lowShelf(fs_, 120.0, 5.0);
                f2_.lowpass(fs_, 4200.0, 0.707);
                break;
            case FX_ZOMBIE:
                pitchSwing_.prepare(2.0, 3.0, rng_);
                levelSwing_.prepare(26.0, 34.0, rng_);
                f1_.lowpass(fs_, 2800.0, 0.707);
                f2_.lowShelf(fs_, 200.0, 3.0);
                break;
            case FX_DARK_LORD:
                f1_.lowpass(fs_, 3200.0, 0.707);
                f2_.peak(fs_, 420.0, 1.0, 4.0);
                f3_.lowpass(fs_, 1400.0, 1.2);
                f4_.highpass(fs_, 500.0, 0.9);
                break;
            case FX_CYBORG:
                f1_.highpass(fs_, 220.0, 0.707);
                f2_.lowpass(fs_, 5500.0, 0.707);
                break;
            case FX_MEGAPHONE:
                f1_.highpass(fs_, 550.0, 0.8);
                f2_.lowpass(fs_, 3600.0, 0.8);
                f3_.peak(fs_, 1800.0, 1.2, 6.0);
                break;
            case FX_PHONE:
                f1_.highpass(fs_, 320.0, 0.707);
                f2_.highpass(fs_, 320.0, 0.707);
                f3_.lowpass(fs_, 3300.0, 0.707);
                f4_.lowpass(fs_, 3300.0, 0.707);
                break;
            case FX_WHISPER:
                vocoder_.set({true, 1.0, 0.0, 100.0, 0.0});
                vocoder_.reset();
                f1_.highpass(fs_, 240.0, 0.707);
                break;
            case FX_STRANGER:
                timbre_.reset();
                f1_.lowShelf(fs_, 160.0, 3.0);
                f2_.lowpass(fs_, 6500.0, 0.707);
                f3_.highpass(fs_, 1500.0, 0.707);
                f4_.lowpass(fs_, 6000.0, 0.707);
                break;
            default:
                break;
        }
    }

    void own(float* x, int n) {
        match_.measureIn(x, n);
        guard_.measureIn(x, n);
        switch (type_) {
            case FX_ANONYMOUS: vocoder_.process(x, n); break;
            case FX_GHOST:     vocoder_.process(x, n); ghost(x, n); break;
            case FX_ALIEN:     alien(x, n); break;
            case FX_DRUNK:     drunk(x, n); break;
            case FX_UNDERWATER: underwater(x, n); break;
            case FX_GLITCH:    glitch(x, n); break;
            case FX_MONSTER:   monster(x, n); break;
            case FX_DUCK:      duck(x, n); break;
            case FX_CROWD:     crowd(x, n); break;
            case FX_ASTRONAUT: radioLink(x, n); break;
            case FX_MOUSE:     mouse(x, n); break;
            case FX_BABY:      baby(x, n); break;
            case FX_GOBLIN:    goblin(x, n); break;
            case FX_GIANT:     giant(x, n); break;
            case FX_ZOMBIE:    zombie(x, n); break;
            case FX_DARK_LORD: darkLord(x, n); break;
            case FX_CYBORG:    cyborg(x, n); break;
            case FX_MEGAPHONE: megaphone(x, n); break;
            case FX_PHONE:     phone(x, n); break;
            case FX_BEE:       bee(x, n); break;
            case FX_WHISPER:   whisper(x, n); break;
            case FX_STRANGER:  stranger(x, n); break;
            default: break;
        }
        match_.apply(x, n);
        guard_.apply(x, n);
        if (type_ == FX_ASTRONAUT) beeps(x, n);
        if (type_ == FX_DARK_LORD) breathe(x, n);
    }

    void alien(float* x, int n) {
        const double centre = ALIEN_BEND * fs_ / (TWO_PI * 6.0) + 4.0;
        for (int i = 0; i < n; ++i) {
            float v = x[i];
            if (cooldown_ > 0) --cooldown_;
            if (onsets_.run(v) && cooldown_ == 0 && rng_.uniform() < 0.4) {
                repeater_.arm((int)(rng_.between(0.070, 0.110) * fs_), rng_.uniform() < 0.3 ? 2 : 1, 1.0);
                cooldown_ = (int)(1.2 * fs_);
            }
            v = repeater_.run(v);
            const double depth = ALIEN_BEND * fs_ / (TWO_PI * pitchSwing_.rate);
            v = wobble_.run(v, centre + depth * pitchSwing_.run(fs_, rng_));
            x[i] = v * (1.0f - 0.28f * (0.5f + 0.5f * levelSwing_.run(fs_, rng_)));
        }
    }

    void drunk(float* x, int n) {
        const double centre = DRUNK_BEND_HI * fs_ / (TWO_PI * 0.35) + HICCUP + 8.0;
        const int rise = (int)(0.040 * fs_), fall = (int)(0.250 * fs_);
        for (int i = 0; i < n; ++i) {
            const float in = x[i];
            if (cooldown_ > 0) --cooldown_;
            if (onsets_.run(in) && cooldown_ == 0 && hiccup_ == 0 && rng_.uniform() < 0.15) {
                hiccup_ = rise + fall;
                cooldown_ = (int)(3.0 * fs_);
            }
            double lift = 0.0;
            if (hiccup_ > 0) {
                const int t = rise + fall - hiccup_;
                lift = t < rise ? -HICCUP * t / rise : -HICCUP * (1.0 - (double)(t - rise) / fall);
                --hiccup_;
            }
            const double before = pitchSwing_.phase;
            const double depth = bend_ * fs_ / (TWO_PI * pitchSwing_.rate);
            const float s = pitchSwing_.run(fs_, rng_);
            if (pitchSwing_.phase < before) bend_ = rng_.between(DRUNK_BEND_LO, DRUNK_BEND_HI);
            x[i] = f1_.run(wobble_.run(in, centre + depth * s + lift));
        }
    }

    void underwater(float* x, int n) {
        const double centre = UNDER_BEND * fs_ / (TWO_PI * 8.0) + 4.0;
        for (int i = 0; i < n; ++i) {
            const float in = x[i];
            const float env = voiceEnv_.run(in);
            const double depth = UNDER_BEND * fs_ / (TWO_PI * pitchSwing_.rate);
            float v = wobble_.run(in, centre + depth * pitchSwing_.run(fs_, rng_));
            v *= 1.0f - 0.4f * (0.5f + 0.5f * levelSwing_.run(fs_, rng_));
            v = f3_.run(f2_.run(f1_.run(v)));
            if (env > 0.008f && rng_.uniform() < 5.0 / fs_) {
                for (Bubble& b : bubbles_) {
                    if (b.age < b.life) continue;
                    b.phase = 0.0; b.age = 0.0;
                    b.life = rng_.between(0.025, 0.050);
                    b.hz = rng_.between(300.0, 700.0);
                    b.grow = std::pow(rng_.between(2.0, 3.0), 1.0 / (b.life * fs_));
                    b.level = 0.6f * env;
                    break;
                }
            }
            for (Bubble& b : bubbles_) {
                if (b.age >= b.life) continue;
                const double t = b.age;
                const double shape = (1.0 - std::exp(-t / 0.001)) * std::exp(-t / 0.015);
                v += (float)(b.level * shape * std::sin(TWO_PI * b.phase));
                b.phase += b.hz / fs_;
                if (b.phase >= 1.0) b.phase -= 1.0;
                b.hz *= b.grow;
                b.age += 1.0 / fs_;
            }
            x[i] = v;
        }
    }

    void ghost(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            clock_ += 1.0 / fs_;
            if (clock_ > 1000.0) clock_ -= 1000.0;
            chorus_.push(x[i]);
            const double d1 = (0.018 + 0.004 * std::sin(TWO_PI * 0.21 * clock_)) * fs_;
            const double d2 = (0.027 + 0.005 * std::sin(TWO_PI * 0.33 * clock_ + 1.0)) * fs_;
            const float v = 0.7f * x[i] + 0.5f * chorus_.tap(d1) + 0.45f * chorus_.tap(d2);
            x[i] = v * (float)(1.0 - 0.25 * (0.5 + 0.5 * std::sin(TWO_PI * 0.45 * clock_)));
        }
    }

    void glitch(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            const float in = x[i];
            const bool onset = onsets_.run(in);
            const float env = voiceEnv_.run(in);
            if (cooldown_ > 0) --cooldown_;
            if (!glitcher_.busy() && cooldown_ == 0 && env > 0.006f &&
                (onset ? rng_.uniform() < 0.8 : rng_.uniform() < 1.5 / fs_)) {
                glitcher_.fire();
                cooldown_ = (int)(rng_.between(0.25, 0.70) * fs_);
            }
            x[i] = glitcher_.run(in);
        }
    }

    void monster(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            float v = f1_.run(x[i]);
            v *= 0.72f + 0.28f * levelSwing_.run(fs_, rng_);
            x[i] = std::tanh(2.2f * v) / 2.2f;
        }
    }

    void duck(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            clock_ += 1.0 / fs_;
            if (clock_ > 1000.0) clock_ -= 1000.0;
            const float v = f3_.run(f2_.run(f1_.run(x[i])));
            x[i] = v * (float)(1.0 - 0.25 * (0.5 + 0.5 * std::sin(TWO_PI * 95.0 * clock_)));
        }
    }

    void crowd(float* x, int n) {
        std::copy(x, x + n, upBuf_.begin());
        std::copy(x, x + n, downBuf_.begin());
        up_.process(upBuf_.data(), n);
        down_.process(downBuf_.data(), n);
        for (int i = 0; i < n; ++i) {
            crowdDry_.push(x[i]);
            x[i] = 0.62f * crowdDry_.tap((double)VoiceShaper::DELAY) + 0.5f * upBuf_[i] + 0.5f * downBuf_[i];
        }
    }

    void radioLink(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            const float v = f4_.run(f3_.run(f2_.run(f1_.run(x[i]))));
            x[i] = std::tanh(1.8f * v) / 1.8f;
        }
    }

    void mouse(float* x, int n) {
        for (int i = 0; i < n; ++i) x[i] = f3_.run(f2_.run(f1_.run(x[i])));
    }

    void baby(float* x, int n) {
        const double centre = BABY_BEND * fs_ / (TWO_PI * 6.0) + 4.0;
        for (int i = 0; i < n; ++i) {
            const double depth = BABY_BEND * fs_ / (TWO_PI * pitchSwing_.rate);
            x[i] = f1_.run(wobble_.run(x[i], centre + depth * pitchSwing_.run(fs_, rng_)));
        }
    }

    void goblin(float* x, int n) {
        for (int i = 0; i < n; ++i) x[i] = std::tanh(1.8f * f2_.run(f1_.run(x[i]))) / 1.8f;
    }

    void giant(float* x, int n) {
        const double at = GIANT_ECHO_SEC * fs_;
        for (int i = 0; i < n; ++i) {
            const float v = f2_.run(f1_.run(x[i]));
            const float back = room_.tap(at);
            room_.push(v + 0.30f * back);
            x[i] = v + 0.35f * back;
        }
    }

    void zombie(float* x, int n) {
        const double centre = ZOMBIE_BEND * fs_ / (TWO_PI * 2.0) + 4.0;
        for (int i = 0; i < n; ++i) {
            const double depth = ZOMBIE_BEND * fs_ / (TWO_PI * pitchSwing_.rate);
            float v = wobble_.run(x[i], centre + depth * pitchSwing_.run(fs_, rng_));
            v *= 0.75f + 0.25f * levelSwing_.run(fs_, rng_);
            x[i] = f2_.run(f1_.run(std::tanh(2.4f * v) / 2.4f));
        }
    }

    void darkLord(float* x, int n) {
        const double at = HELMET_COMB_SEC * fs_;
        for (int i = 0; i < n; ++i) {
            const float v = f2_.run(f1_.run(x[i]));
            const float back = room_.tap(at);
            room_.push(v + 0.35f * back);
            x[i] = 0.8f * v + 0.4f * back;
        }
    }

    void breathe(float* x, int n) {
        const float edge = (float)(1.0 / (0.030 * fs_));
        const double pi = TWO_PI / 2.0, outFrom = BREATH_IN_SEC + BREATH_HOLD_SEC;
        for (int i = 0; i < n; ++i) {
            loudSec_ = voiceEnv_.run(x[i]) > 0.0126f ? loudSec_ + 1.0 / fs_ : 0.0;
            const bool talking = loudSec_ > BREATH_TALK_SEC;
            quietSec_ = talking ? 0.0 : quietSec_ + 1.0 / fs_;
            const bool on = quietSec_ > BREATH_WAIT_SEC;
            if (!on) breath_ = 0.0;
            double shape = 0.0;
            if (breath_ < BREATH_IN_SEC) {
                const double s = std::sin(pi * breath_ / BREATH_IN_SEC);
                shape = 0.8 * s * s;
            } else if (breath_ >= outFrom && breath_ < outFrom + BREATH_OUT_SEC) {
                const double s = std::sin(pi * (breath_ - outFrom) / BREATH_OUT_SEC);
                shape = s * s;
            }
            breathGain_ = toward(breathGain_, on ? 1.0f : 0.0f, edge);
            if (on) {
                breath_ += 1.0 / fs_;
                if (breath_ >= BREATH_CYCLE_SEC) breath_ -= BREATH_CYCLE_SEC;
            }
            const float air = f4_.run(f3_.run(rng_.noise()));
            x[i] += breathGain_ * (float)(BREATH_LEVEL * shape) * air;
        }
    }

    void cyborg(float* x, int n) {
        const double at = CYBORG_COMB_SEC * fs_, step = TWO_PI * 38.0 / fs_;
        for (int i = 0; i < n; ++i) {
            const float v = f2_.run(f1_.run(x[i]));
            const float back = room_.tap(at);
            room_.push(v + 0.62f * back);
            const double ring = 0.7 + 0.3 * std::cos(phase_);
            phase_ += step;
            if (phase_ > TWO_PI) phase_ -= TWO_PI;
            x[i] = (float)((0.55f * v + 0.55f * back) * ring);
        }
    }

    void megaphone(float* x, int n) {
        const double at = MEGAPHONE_ECHO_SEC * fs_;
        for (int i = 0; i < n; ++i) {
            const float v = std::tanh(3.5f * f3_.run(f2_.run(f1_.run(x[i])))) / 2.0f;
            const float back = room_.tap(at);
            room_.push(v + 0.32f * back);
            x[i] = v + 0.30f * back;
        }
    }

    void phone(float* x, int n) {
        const float law = 255.0f, steps = 64.0f, norm = std::log1p(law);
        for (int i = 0; i < n; ++i) {
            const float v = std::tanh(1.6f * f4_.run(f3_.run(f2_.run(f1_.run(x[i]))))) / 1.6f;
            if (holdCount_++ % 3 == 0) {
                const float a = std::min(1.0f, std::fabs(v));
                const float c = std::floor(std::log1p(law * a) / norm * steps + 0.5f) / steps;
                holdLevel_ = std::copysign(std::expm1(c * norm) / law, v);
            }
            x[i] = holdLevel_;
        }
    }

    void bee(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            clock_ += 1.0 / fs_;
            if (clock_ > 1000.0) clock_ -= 1000.0;
            const double flutter = 1.0 - 0.55 * (0.5 + 0.5 * std::sin(TWO_PI * 34.0 * clock_));
            const double buzz = 0.8 + 0.2 * std::cos(TWO_PI * 190.0 * clock_);
            x[i] = (float)(x[i] * flutter * buzz);
        }
    }

    void whisper(float* x, int n) {
        vocoder_.process(x, n);
        for (int i = 0; i < n; ++i) x[i] = f1_.run(x[i]);
    }

    void stranger(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            const float v = speech_ ? timbre_.run(x[i]) : x[i];
            x[i] = f2_.run(f1_.run(v + HUSK * f4_.run(f3_.run(v * rng_.noise()))));
        }
    }

    static float toward(float v, float target, float step) {
        return v < target ? std::min(target, v + step) : std::max(target, v - step);
    }

    void beeps(float* x, int n) {
        const int quietLimit = (int)(0.35 * fs_), beepLen = (int)(0.25 * fs_), squelchLen = (int)(0.12 * fs_), edge = (int)(0.005 * fs_);
        for (int i = 0; i < n; ++i) {
            const float env = voiceEnv_.run(x[i]);
            const bool active = env > 0.0126f;
            if (active) {
                quietFor_ = 0;
                if (!talking_) { talking_ = true; startBeep(2525.0, beepLen); squelchNext_ = false; }
            } else if (talking_ && ++quietFor_ > quietLimit) {
                talking_ = false;
                startBeep(2475.0, beepLen);
                squelchNext_ = true;
            }
            const float gate = hissGate_.run(talking_ ? 1.0f : 0.0f);
            float add = rng_.noise() * 0.0035f * gate;
            if (beepLeft_ > 0) {
                const int done = beepLen_ - beepLeft_;
                float shape = 1.0f;
                if (done < edge) shape = (float)done / edge;
                else if (beepLeft_ < edge) shape = (float)beepLeft_ / edge;
                add += 0.1f * shape * (float)std::sin(TWO_PI * beepPhase_);
                beepPhase_ += beepHz_ / fs_;
                if (beepPhase_ >= 1.0) beepPhase_ -= 1.0;
                if (--beepLeft_ == 0 && squelchNext_) { squelchLeft_ = squelchLen; squelchNext_ = false; }
            } else if (squelchLeft_ > 0) {
                add += rng_.noise() * 0.05f * (float)squelchLeft_ / squelchLen;
                --squelchLeft_;
            }
            x[i] += add;
        }
    }

    void startBeep(double hz, int len) { beepHz_ = hz; beepPhase_ = 0.0; beepLeft_ = beepLen_ = len; squelchLeft_ = 0; }

    void vibrato(float* x, int n) {
        const int L = (int)vbuf_.size();
        const double step = TWO_PI * a_ / fs_;
        const double centre = 0.007 * fs_, span = 0.0045 * fs_ * b_;
        for (int i = 0; i < n; ++i) {
            vbuf_[vpos_] = x[i];
            const double d = centre + span * std::sin(phase_);
            phase_ += step; if (phase_ > TWO_PI) phase_ -= TWO_PI;
            double rp = (double)vpos_ - d;
            while (rp < 0.0) rp += L;
            const int i0 = (int)rp; const double fr = rp - i0; const int i1 = (i0 + 1) % L;
            x[i] = (float)(vbuf_[i0] * (1.0 - fr) + vbuf_[i1] * fr);
            if (++vpos_ >= L) vpos_ = 0;
        }
    }

    void ring(float* x, int n) {
        const double step = TWO_PI * a_ / fs_, mix = b_;
        for (int i = 0; i < n; ++i) {
            const double c = std::cos(phase_);
            phase_ += step; if (phase_ > TWO_PI) phase_ -= TWO_PI;
            x[i] = (float)(x[i] * ((1.0 - mix) + mix * c));
        }
    }
    void tremolo(float* x, int n) {
        const double step = TWO_PI * a_ / fs_, depth = b_;
        for (int i = 0; i < n; ++i) {
            const double g = 1.0 - depth * (0.5 - 0.5 * std::cos(phase_));
            phase_ += step; if (phase_ > TWO_PI) phase_ -= TWO_PI;
            x[i] = (float)(x[i] * g);
        }
    }
    void radio(float* x, int n) {
        const double drive = a_;
        for (int i = 0; i < n; ++i) {
            double v = biquad(x[i], hb0_, hb1_, hb2_, ha1_, ha2_, hx1_, hx2_, hy1_, hy2_);
            v = biquad(v, lb0_, lb1_, lb2_, la1_, la2_, lx1_, lx2_, ly1_, ly2_);
            v = std::tanh(drive * v) * 0.9;
            x[i] = (float)v;
        }
    }
    static double biquad(double in, double b0, double b1, double b2, double a1, double a2,
                         double& x1, double& x2, double& y1, double& y2) {
        const double y = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in; y2 = y1; y1 = y; return y;
    }
    void highpass(double f, double& b0, double& b1, double& b2, double& a1, double& a2) {
        const double w = TWO_PI * f / fs_, c = std::cos(w), s = std::sin(w), al = s / (2.0 * 0.707);
        const double a0 = 1.0 + al;
        b0 = ((1.0 + c) / 2.0) / a0; b1 = (-(1.0 + c)) / a0; b2 = b0;
        a1 = (-2.0 * c) / a0; a2 = (1.0 - al) / a0;
    }
    void lowpass(double f, double& b0, double& b1, double& b2, double& a1, double& a2) {
        const double w = TWO_PI * f / fs_, c = std::cos(w), s = std::sin(w), al = s / (2.0 * 0.707);
        const double a0 = 1.0 + al;
        b0 = ((1.0 - c) / 2.0) / a0; b1 = (1.0 - c) / a0; b2 = b0;
        a1 = (-2.0 * c) / a0; a2 = (1.0 - al) / a0;
    }
    void designRadio() {
        highpass(500.0, hb0_, hb1_, hb2_, ha1_, ha2_);
        lowpass(2700.0, lb0_, lb1_, lb2_, la1_, la2_);
    }
};

}
