#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx_parts.hpp"
#include "glitcher.hpp"

namespace voice {

enum GlitchStyle {
    GLITCH_REPEAT, GLITCH_BEEP, GLITCH_STUTTER, GLITCH_REVERSE, GLITCH_TAPE, GLITCH_SCRATCH, GLITCH_ROBOT,
    GLITCH_CRUSH, GLITCH_CHIPMUNK, GLITCH_DEEP, GLITCH_ECHO, GLITCH_CUT, GLITCH_RANDOM, NUM_GLITCH_STYLES
};

struct GlitchLook {
    const wchar_t* name;
    const wchar_t* idle;
    const wchar_t* busy;
    bool ends;
};

static const GlitchLook GLITCH_LOOKS[NUM_GLITCH_STYLES] = {
    {L"Repeat",    L"Hold to repeat",     L"Repeating",     false},
    {L"Beep",      L"Hold to beep",       L"Beeping",       false},
    {L"Stutter",   L"Hold to stutter",    L"Stuttering",    false},
    {L"Reverse",   L"Hold to reverse",    L"Reversing",     false},
    {L"Tape stop", L"End with tape stop", L"Stopping",      true},
    {L"Scratch",   L"Hold to scratch",    L"Scratching",    false},
    {L"Robot",     L"End with robot",     L"Shutting down", true},
    {L"Crush",     L"End with crush",     L"Breaking up",   true},
    {L"Chipmunk",  L"End with chipmunk",  L"Speeding up",   true},
    {L"Deep",      L"End with deep",      L"Going deep",    true},
    {L"Echo",      L"End with echo",      L"Echoing out",   true},
    {L"Cut",       L"Hold to cut",        L"Cut",           false},
    {L"Glitch",    L"Hold to glitch",     L"Glitching",     false},
};

class GlitchPad {
public:
    static const int CHUNK = 256;
    static constexpr double MIN_SEC = 0.30, BEEP_HZ = 1000.0, STUTTER_SEC = 0.120, SHORTEST_SEC = 0.025;
    static constexpr double SCRATCH_SEC = 0.6, SCRATCH_HZ = 2.6, GRAIN_SEC = 0.080, JITTER_MOVING_SEC = 0.002, JITTER_HELD_SEC = 0.020;
    static constexpr double GLITCH_BACK_SEC = 0.3, REVERSE_SEC = 0.6, STOP_SEC = 0.8, TAPE_BACK_SEC = 0.12;
    static constexpr double ROBOT_SEC = 0.9, ROBOT_HZ = 55.0, CRUSH_SEC = 0.7;
    static constexpr double CHIPMUNK_SEC = 0.8, CHIPMUNK_FROM = 1.3, CHIPMUNK_TO = 2.6, DEEP_SEC = 0.5, DEEP_RATE = 0.55;
    static constexpr double ECHO_SEC = 0.30, ECHO_FEEDBACK = 0.55, ECHO_WET = 0.8, ECHO_OUT_SEC = 1.5;
    static constexpr double WORD_QUIET_SEC = 0.070, WORD_MAX_SEC = 1.0, WORD_SHORTEST_SEC = 0.35, WORD_BEFORE_SEC = 0.020, WORD_AFTER_SEC = 0.030, REVERSE_BACKLOG_SEC = 2.0;

    void prepare(double fs, int channels) {
        fs_ = fs;
        channels_ = channels > 1 ? 2 : 1;
        size_t size = 1;
        while ((double)size < 2.5 * fs) size <<= 1;
        mask_ = (long long)size - 1;
        size_t echoSize = 1;
        while ((double)echoSize < 0.5 * fs) echoSize <<= 1;
        echoMask_ = (long long)echoSize - 1;
        size_t revSize = 1;
        while ((double)revSize < 4.0 * fs) revSize <<= 1;
        revMask_ = (long long)revSize - 1;
        combDelay_ = (int)(0.006 * fs);
        size_t combSize = 1;
        while ((int)combSize < combDelay_ + 4) combSize <<= 1;
        combMask_ = (long long)combSize - 1;
        clipMax_ = (int)(1.0 * fs);
        slice_ = (int)(0.160 * fs);
        reach_ = (int)(1.50 * fs);
        step_ = (int)(0.020 * fs);
        grain_ = (int)(0.010 * fs);
        fade_ = (int)(0.005 * fs);
        seam_ = (int)(0.0015 * fs);
        shortest_ = (int)(SHORTEST_SEC * fs);
        grainLen_ = (int)(GRAIN_SEC * fs) & ~1;
        grainHop_ = grainLen_ / 2;
        half_ = (int)fs;
        settle_ = (int)(0.050 * fs);
        echoDelay_ = (int)(ECHO_SEC * fs);
        echoLp_ = (float)(1.0 - std::exp(-TWO_PI * 2500.0 / fs));
        glide_ = (float)(1.0 / (0.006 * fs));
        quietRise_ = (float)std::pow(10.0, QUIET_RISE_DB_PER_SEC / 20.0 / fs);
        window_.assign((size_t)grainLen_, 0.0f);
        for (int k = 0; k < grainLen_; ++k) {
            const double s = std::sin(TWO_PI * 0.5 * k / grainLen_);
            window_[(size_t)k] = (float)(s * s);
        }
        for (int c = 0; c < 2; ++c) {
            hist_[c].assign(c < channels_ ? size : 0, 0.0f);
            clip_[c].assign(c < channels_ ? (size_t)clipMax_ + 8 : 0, 0.0f);
            prev_[c].assign(c < channels_ ? (size_t)clipMax_ + 8 : 0, 0.0f);
            echo_[c].assign(c < channels_ ? echoSize : 0, 0.0f);
            rev_[c].assign(c < channels_ ? revSize : 0, 0.0f);
            comb_[c].assign(c < channels_ ? combSize : 0, 0.0f);
            dry_[c].assign(CHUNK, 0.0f);
            glitchers_[c].prepare(fs);
        }
        mid_.assign(size, 0.0f);
        level_.prepare(fs, 0.003, 0.120);
        peak_.prepare(fs, 0.001, 1.5);
        word_.prepare(fs, 0.002, 0.030);
        reset();
    }

    void reset() {
        for (int c = 0; c < channels_; ++c) {
            std::fill(hist_[c].begin(), hist_[c].end(), 0.0f);
            std::fill(echo_[c].begin(), echo_[c].end(), 0.0f);
            std::fill(comb_[c].begin(), comb_[c].end(), 0.0f);
            glitchers_[c].reset();
            echoLow_[c] = crushHeld_[c] = 0.0f;
        }
        std::fill(mid_.begin(), mid_.end(), 0.0f);
        level_.env = peak_.env = word_.env = 0.0f;
        revW_ = revR_ = wordFrom_ = 0;
        quietRun_ = 0;
        heard_ = false;
        quiet_ = 1e9f;
        w_ = t_ = crushCount_ = 0;
        held_ = grainsOk_ = false;
        style_ = want_;
        mix_ = 0.0f;
        curMin_ = prevMin_ = 1e9f;
        floor_ = env_ = segLoud_ = 0.0f;
        swap_ = half_;
        warm_ = settle_;
        clipLen_ = period_ = pos_ = prevPeriod_ = prevPos_ = xf_ = liveXf_ = fill_ = loopLen_ = wraps_ = rollJoin_ = 0;
        left_ = nextGrain_ = 0;
        span_ = 1;
        ready_ = false;
        phase_ = robotPhase_ = tapePos_ = head_ = stopAt_ = 0.0;
        for (Grain& g : grains_) g = Grain{};
    }

    void set(int style) {
        want_ = style < 0 ? 0 : (style >= NUM_GLITCH_STYLES ? NUM_GLITCH_STYLES - 1 : style);
    }

    void hold(bool on) {
        if (on == held_) return;
        held_ = on;
        if (on) begin();
    }

    void process(float* l, float* r, int n) {
        for (int at = 0; at < n; at += CHUNK)
            chunk(l + at, channels_ == 2 ? r + at : nullptr, std::min(CHUNK, n - at));
    }

private:
    struct Grain {
        double pos = 0.0;
        int n = 0;
        bool live = false;
    };

    static constexpr float ABS_MIN = 0.010f, OVER_FLOOR = 2.0f;
    static constexpr float REV_LEAST = 0.004f, REV_OVER_QUIET = 2.0f;
    static constexpr double QUIET_RISE_DB_PER_SEC = 1.0;

    static float toward(float v, float target, float step) {
        return v < target ? std::min(target, v + step) : std::max(target, v - step);
    }

    int samples(double sec) const { return (int)(sec * fs_); }

    bool ends() const { return GLITCH_LOOKS[style_].ends; }

    bool cut() const { return held_ || left_ > 0; }

    void begin() {
        style_ = want_;
        t_ = 0;
        pos_ = 0;
        grainsOk_ = false;
        const int found = lastSound();
        const int ago = found >= 0 ? soundEnd(found) : 0;
        left_ = 0;
        switch (style_) {
        case GLITCH_REPEAT:
            ready_ = false;
            fill_ = 0;
            segLoud_ = 0.0f;
            if (found >= 0) grab(found);
            break;
        case GLITCH_BEEP:
            beepAmp_ = std::min(0.30f, std::max(0.05f, 0.8f * peak_.env));
            phase_ = 0.0;
            break;
        case GLITCH_STUTTER: {
            int best = ago;
            float top = -1.0f;
            for (int at = ago; at + shortest_ <= ago + slice_; at += fade_) {
                const float loud = loudness(shortest_, at);
                if (loud > top) { top = loud; best = at; }
            }
            take(best, samples(STUTTER_SEC), false);
            loopLen_ = clipLen_;
            rollJoin_ = clipLen_ - seam_;
            wraps_ = 0;
            break;
        }
        case GLITCH_SCRATCH:
            take(ago, samples(SCRATCH_SEC), false);
            break;
        case GLITCH_RANDOM:
            take(ago, clipMax_, false);
            startGrains((double)(clipLen_ - 2) - GLITCH_BACK_SEC * fs_);
            if (!grainsOk_) break;
            for (int c = 0; c < channels_; ++c) {
                glitchers_[c].reset();
                for (int k = 0; k < clipLen_; ++k) glitchers_[c].run(clip_[c][(size_t)k]);
                glitchers_[c].fire();
            }
            break;
        case GLITCH_REVERSE:
            revR_ = revW_;
            wordFrom_ = w_;
            heard_ = false;
            quietRun_ = 0;
            if (found >= 0) queueReversed(w_ - ago - samples(REVERSE_SEC), w_ - ago);
            break;
        case GLITCH_TAPE:
        case GLITCH_ROBOT:
            tapePos_ = (double)(w_ - 1 - ago - samples(TAPE_BACK_SEC));
            robotPhase_ = 0.0;
            for (int c = 0; c < channels_; ++c) std::fill(comb_[c].begin(), comb_[c].end(), 0.0f);
            left_ = samples(style_ == GLITCH_TAPE ? STOP_SEC : ROBOT_SEC);
            break;
        case GLITCH_CRUSH:
            take(ago, slice_, false);
            crushCount_ = 0;
            if (clipLen_ > 2 * seam_ + 8) left_ = samples(CRUSH_SEC);
            break;
        case GLITCH_CHIPMUNK:
            take(ago, samples(CHIPMUNK_SEC), false);
            if (clipLen_ > 8) left_ = (int)(clipLen_ * 2.0 / (CHIPMUNK_FROM + CHIPMUNK_TO));
            break;
        case GLITCH_DEEP:
            take(ago, samples(DEEP_SEC), false);
            if (clipLen_ > 8) left_ = (int)(clipLen_ / DEEP_RATE);
            break;
        case GLITCH_ECHO:
            seedEcho(ago);
            left_ = samples(ECHO_OUT_SEC);
            break;
        default:
            break;
        }
        if (!ends() && style_ != GLITCH_REPEAT) left_ = samples(MIN_SEC);
        span_ = std::max(1, left_);
    }

    float loudness(int len, int ago) const {
        if (len <= 0 || w_ < (long long)len + ago) return 0.0f;
        double e = 0.0;
        for (int i = 0; i < len; ++i) {
            const float v = mid_[(size_t)((w_ - ago - len + i) & mask_)];
            e += (double)v * v;
        }
        return (float)std::sqrt(e / len);
    }

    float wanted() const {
        const float want = OVER_FLOOR * floor_;
        return want > ABS_MIN ? want : ABS_MIN;
    }

    int lastSound() const {
        const float want = wanted();
        int spare = -1;
        for (int ago = 0; ago + slice_ <= reach_; ago += step_) {
            const float loud = loudness(slice_, ago);
            if (loud > want) return ago;
            if (spare < 0 && loud > ABS_MIN) spare = ago;
        }
        return spare;
    }

    int soundEnd(int ago) const {
        float top = 0.0f;
        for (int at = ago; at + grain_ <= ago + slice_; at += grain_) top = std::max(top, loudness(grain_, at));
        const float want = std::max(wanted(), 0.3f * top);
        for (int at = ago; at + grain_ <= ago + slice_; at += grain_)
            if (loudness(grain_, at) >= want) return at;
        return ago;
    }

    void take(int ago, int len, bool blend) {
        if (len > clipMax_) len = clipMax_;
        const long long have = w_ - ago;
        if (len > have) len = have > 0 ? (int)have : 0;
        if (blend && clipLen_ > 0) {
            for (int c = 0; c < channels_; ++c) std::swap(clip_[c], prev_[c]);
            prevPeriod_ = period_;
            prevPos_ = pos_;
            xf_ = fade_;
        } else {
            xf_ = 0;
        }
        const long long from = w_ - ago - len;
        for (int c = 0; c < channels_; ++c)
            for (int k = 0; k < len; ++k) clip_[c][(size_t)k] = hist_[c][(size_t)((from + k) & mask_)];
        clipLen_ = len;
        period_ = std::max(1, len - fade_);
        pos_ = 0;
    }

    void grab(int ago) {
        const bool looping = ready_;
        segLoud_ = loudness(slice_, ago);
        take(ago, slice_, looping);
        ready_ = clipLen_ > 2 * fade_;
        if (ready_ && !looping) liveXf_ = fade_;
    }

    void seedEcho(int ago) {
        const int len = echoDelay_;
        for (int c = 0; c < channels_; ++c)
            for (int k = 0; k < len; ++k) {
                const float edge = std::min(1.0f, (float)std::min(k, len - 1 - k) / (float)fade_);
                echo_[c][(size_t)((w_ - len + k) & echoMask_)] += edge * hist_[c][(size_t)((w_ - ago - len + k) & mask_)];
            }
    }

    static float loopAt(const std::vector<float>& b, int period, int seam, int pos) {
        if (pos < seam) {
            const float t = (float)pos / (float)seam;
            return b[(size_t)pos] * t + b[(size_t)(period + pos)] * (1.0f - t);
        }
        return b[(size_t)pos];
    }

    float clipAt(int c, double p) const {
        if (p < 0.0) p = 0.0;
        const double last = (double)(clipLen_ - 2);
        if (p > last) p = last;
        const int i = (int)p;
        const float t = (float)(p - i);
        return clip_[c][(size_t)i] * (1.0f - t) + clip_[c][(size_t)(i + 1)] * t;
    }

    float histAt(int c, double p) const {
        const long long i = (long long)std::floor(p);
        const float t = (float)(p - (double)i);
        return hist_[c][(size_t)(i & mask_)] * (1.0f - t) + hist_[c][(size_t)((i + 1) & mask_)] * t;
    }

    double loudestAt(double from, double to) const {
        const int a = std::max(0, (int)from), b = std::min(clipLen_ - shortest_, (int)to);
        double best = (double)a, top = -1.0;
        for (int at = a; at <= b; at += fade_) {
            double e = 0.0;
            for (int c = 0; c < channels_; ++c)
                for (int k = at; k < at + shortest_; ++k) e += (double)clip_[c][(size_t)k] * clip_[c][(size_t)k];
            if (e > top) { top = e; best = (double)at; }
        }
        return best;
    }

    double grainHigh() const { return (double)(clipLen_ - 2 - grainLen_); }

    bool keepHead() {
        const double held = JITTER_HELD_SEC * fs_, lo = held, hi = std::max(lo, std::min(grainHigh() - held, stopAt_));
        const double was = head_;
        head_ = std::min(hi, std::max(lo, head_));
        return head_ == was;
    }

    double placeGrain() {
        const bool moving = keepHead();
        const double spread = (moving ? JITTER_MOVING_SEC : JITTER_HELD_SEC) * fs_;
        const double p = head_ + rng_.between(-spread, spread);
        return std::min(grainHigh(), std::max(0.0, p));
    }

    void startGrains(double head) {
        grainsOk_ = grainHigh() > 2.0 * JITTER_HELD_SEC * fs_ + 2.0;
        if (!grainsOk_) return;
        const double last = (double)(clipLen_ - 2);
        stopAt_ = loudestAt(last - slice_, last - fade_);
        head_ = head;
        keepHead();
        grains_[0].pos = head_;
        grains_[0].n = grainHop_;
        grains_[0].live = true;
        grains_[1].pos = head_;
        grains_[1].n = 0;
        grains_[1].live = true;
        nextGrain_ = grainHop_;
    }

    void grains(float* y) {
        for (int c = 0; c < channels_; ++c) {
            float v = 0.0f;
            for (const Grain& g : grains_)
                if (g.live) v += window_[(size_t)g.n] * clipAt(c, g.pos);
            y[c] = v;
        }
        for (Grain& g : grains_) {
            if (!g.live) continue;
            g.pos += 1.0;
            if (++g.n >= grainLen_) g.live = false;
        }
        if (--nextGrain_ <= 0) {
            for (Grain& g : grains_) {
                if (g.live) continue;
                g.pos = placeGrain();
                g.n = 0;
                g.live = true;
                break;
            }
            nextGrain_ = grainHop_;
        }
        head_ += 1.0;
    }

    void track(float mid) {
        env_ = level_.run(mid);
        peak_.run(mid);
        if (warm_ > 0) {
            --warm_;
            return;
        }
        quiet_ = env_ < quiet_ ? env_ : quiet_ * quietRise_;
        if (env_ < curMin_) curMin_ = env_;
        floor_ = curMin_ < prevMin_ ? curMin_ : prevMin_;
        if (--swap_ <= 0) {
            swap_ = half_;
            prevMin_ = curMin_;
            curMin_ = env_;
        }
    }

    void repeat(const float* x, float* y) {
        if (held_) {
            const float want = wanted();
            if (fill_ > 0) { if (--fill_ == 0) grab(0); }
            else if (segLoud_ <= want && env_ > want) fill_ = slice_;
        }
        if (!ready_) return;
        for (int c = 0; c < channels_; ++c) {
            float v = loopAt(clip_[c], period_, fade_, pos_);
            if (xf_ > 0) {
                const float t = 1.0f - (float)xf_ / (float)fade_;
                v = v * t + loopAt(prev_[c], prevPeriod_, fade_, prevPos_) * (1.0f - t);
            }
            if (liveXf_ > 0) {
                const float t = 1.0f - (float)liveXf_ / (float)fade_;
                v = v * t + x[c] * (1.0f - t);
            }
            y[c] = v;
        }
        if (++pos_ >= period_) pos_ = 0;
        if (xf_ > 0) {
            if (++prevPos_ >= prevPeriod_) prevPos_ = 0;
            --xf_;
        }
        if (liveXf_ > 0) --liveXf_;
    }

    void stutter(float* y) {
        if (clipLen_ / 4 <= 2 * seam_) return;
        const int period = loopLen_ - seam_, base = clipLen_ - loopLen_;
        for (int c = 0; c < channels_; ++c) {
            float v = clip_[c][(size_t)(base + pos_)];
            if (pos_ < seam_) {
                const float t = (float)pos_ / (float)seam_;
                v = v * t + clip_[c][(size_t)(rollJoin_ + pos_)] * (1.0f - t);
            }
            y[c] = v;
        }
        if (++pos_ >= period) {
            pos_ = 0;
            rollJoin_ = base + period;
            if (++wraps_ % 2 == 0 && loopLen_ / 2 >= shortest_) loopLen_ /= 2;
        }
    }

    void scratch(float* y) const {
        if (clipLen_ <= 8) return;
        const double reachOut = std::min(0.15 * fs_, (clipLen_ - 2) * 0.45);
        const double turn = TWO_PI * SCRATCH_HZ / fs_, ph = turn * (double)t_;
        const double p = (clipLen_ - 2) - reachOut * (1.0 - std::cos(ph));
        const double speed = std::fabs(reachOut * turn * std::sin(ph));
        const float gain = (float)std::min(1.0, 0.25 + 0.75 * speed);
        for (int c = 0; c < channels_; ++c) y[c] = clipAt(c, p) * gain;
    }

    void queueReversed(long long from, long long to) {
        from = std::max(from, std::max(0LL, w_ - mask_));
        const long long len = to - from;
        if (len <= 2 * fade_) return;
        for (long long k = 0; k < len; ++k) {
            const float edge = std::min(1.0f, (float)std::min(k, len - 1 - k) / (float)fade_);
            for (int c = 0; c < channels_; ++c) rev_[c][(size_t)((revW_ + k) & revMask_)] = edge * hist_[c][(size_t)((to - 1 - k) & mask_)];
        }
        revW_ += len;
        const long long most = samples(REVERSE_BACKLOG_SEC);
        if (revW_ - revR_ > most) revR_ = revW_ - most;
    }

    long long quietestIn(long long from, long long to) const {
        long long best = to;
        double low = 1e300;
        for (long long at = from; at + grain_ <= to; at += grain_ / 2) {
            double e = 0.0;
            for (int k = 0; k < grain_; ++k) {
                const float v = mid_[(size_t)((at + k) & mask_)];
                e += (double)v * v;
            }
            if (e < low) { low = e; best = at + grain_ / 2; }
        }
        return best;
    }

    void reverseLive(float* y) {
        const float want = std::max(REV_LEAST, REV_OVER_QUIET * quiet_);
        if (word_.env > want) {
            heard_ = true;
            quietRun_ = 0;
        } else if (heard_) {
            ++quietRun_;
        }
        if (!heard_) {
            wordFrom_ = std::max(wordFrom_, w_ + 1 - samples(WORD_BEFORE_SEC));
        } else {
            const bool over = quietRun_ >= samples(WORD_QUIET_SEC);
            if (over || w_ + 1 - wordFrom_ >= samples(WORD_MAX_SEC)) {
                const long long to = over ? w_ + 1 - quietRun_ + samples(WORD_AFTER_SEC) : quietestIn(wordFrom_ + samples(WORD_SHORTEST_SEC), w_ + 1 - grain_);
                queueReversed(wordFrom_, to);
                wordFrom_ = to;
                heard_ = !over;
            }
        }
        if (revR_ < revW_) {
            for (int c = 0; c < channels_; ++c) y[c] = rev_[c][(size_t)(revR_ & revMask_)];
            ++revR_;
        }
    }

    void ending(float* y) {
        const double u = (double)t_ / (double)span_;
        switch (style_) {
        case GLITCH_TAPE:
        case GLITCH_ROBOT: {
            const double rate = std::max(0.0, 1.0 - u);
            const float gain = (float)std::min(1.0, rate * 4.0);
            for (int c = 0; c < channels_; ++c) y[c] = histAt(c, tapePos_) * gain;
            tapePos_ += rate;
            if (style_ == GLITCH_TAPE) break;
            const float ring = (float)std::sin(TWO_PI * robotPhase_);
            robotPhase_ += ROBOT_HZ * (0.4 + 0.6 * rate) / fs_;
            if (robotPhase_ >= 1.0) robotPhase_ -= 1.0;
            for (int c = 0; c < channels_; ++c) {
                const float back = comb_[c][(size_t)((w_ - combDelay_) & combMask_)];
                const float s = y[c] * ring + 0.35f * back;
                comb_[c][(size_t)(w_ & combMask_)] = s;
                y[c] = s;
            }
            break;
        }
        case GLITCH_CRUSH: {
            const float levels = (float)(16.0 * std::pow(0.25, u));
            const int holdFor = 1 + (int)(47.0 * u * u);
            const float gain = (float)std::min(1.0, (1.0 - u) * 3.0);
            const int period = clipLen_ - seam_;
            for (int c = 0; c < channels_; ++c) {
                if (crushCount_ % holdFor == 0) {
                    const float q = std::floor(loopAt(clip_[c], period, seam_, pos_) * levels + 0.5f) / levels;
                    crushHeld_[c] = std::max(-1.0f, std::min(1.0f, q));
                }
                y[c] = crushHeld_[c] * gain;
            }
            ++crushCount_;
            if (++pos_ >= period) pos_ = 0;
            break;
        }
        case GLITCH_CHIPMUNK: {
            const double t = (double)t_;
            const double p = CHIPMUNK_FROM * t + (CHIPMUNK_TO - CHIPMUNK_FROM) * t * t / (2.0 * span_);
            for (int c = 0; c < channels_; ++c) y[c] = clipAt(c, p);
            break;
        }
        case GLITCH_DEEP: {
            const float gain = (float)std::min(1.0, (1.0 - u) / 0.4);
            for (int c = 0; c < channels_; ++c) y[c] = clipAt(c, DEEP_RATE * (double)t_) * gain;
            break;
        }
        default:
            break;
        }
        if (left_ < fade_) {
            const float g = (float)left_ / (float)fade_;
            y[0] *= g;
            y[1] *= g;
        }
    }

    void shape(const float* x, float* y) {
        if (style_ == GLITCH_REPEAT) {
            repeat(x, y);
            return;
        }
        y[0] = y[1] = 0.0f;
        if (ends()) {
            if (left_ > 0) ending(y);
            return;
        }
        switch (style_) {
        case GLITCH_BEEP: {
            const float v = beepAmp_ * (float)std::sin(TWO_PI * phase_);
            phase_ += BEEP_HZ / fs_;
            if (phase_ >= 1.0) phase_ -= 1.0;
            y[0] = y[1] = v;
            break;
        }
        case GLITCH_STUTTER:
            stutter(y);
            break;
        case GLITCH_REVERSE:
            reverseLive(y);
            break;
        case GLITCH_SCRATCH:
            scratch(y);
            break;
        case GLITCH_RANDOM:
            if (!grainsOk_) break;
            grains(y);
            for (int c = 0; c < channels_; ++c) y[c] = glitchers_[c].run(y[c]);
            break;
        default:
            break;
        }
    }

    void chunk(float* l, float* r, int n) {
        float* io[2] = {l, r};
        for (int c = 0; c < channels_; ++c) std::copy(io[c], io[c] + n, dry_[c].begin());
        if (style_ == GLITCH_RANDOM && cut() && grainsOk_ && !glitchers_[0].busy())
            for (int c = 0; c < channels_; ++c) glitchers_[c].fire();

        for (int i = 0; i < n; ++i) {
            const float x[2] = {dry_[0][(size_t)i], channels_ == 2 ? dry_[1][(size_t)i] : 0.0f};
            const float mid = channels_ == 2 ? 0.5f * (x[0] + x[1]) : x[0];
            const size_t at = (size_t)(w_ & mask_);
            for (int c = 0; c < channels_; ++c) hist_[c][at] = x[c];
            mid_[at] = mid;
            track(mid);
            word_.run(mid);

            const bool on = cut();
            mix_ = toward(mix_, on ? 1.0f : 0.0f, glide_);
            float y[2] = {x[0], x[1]};
            if (on || mix_ > 0.0f) {
                shape(x, y);
                ++t_;
                if (left_ > 0) --left_;
            }

            for (int c = 0; c < channels_; ++c) {
                const float back = echo_[c][(size_t)((w_ - echoDelay_) & echoMask_)];
                echoLow_[c] += echoLp_ * (back - echoLow_[c]);
                echo_[c][(size_t)(w_ & echoMask_)] = (float)ECHO_FEEDBACK * echoLow_[c];
                io[c][i] = x[c] + mix_ * (y[c] - x[c]) + (float)ECHO_WET * back;
            }
            ++w_;
        }
    }

    double fs_ = 48000.0, phase_ = 0.0, robotPhase_ = 0.0, tapePos_ = 0.0, head_ = 0.0, stopAt_ = 0.0;
    int channels_ = 1, want_ = GLITCH_REPEAT, style_ = GLITCH_REPEAT;
    int clipMax_ = 0, slice_ = 0, reach_ = 0, step_ = 0, grain_ = 0, fade_ = 0, seam_ = 0, shortest_ = 0, half_ = 0, settle_ = 0;
    int grainLen_ = 0, grainHop_ = 0, nextGrain_ = 0, echoDelay_ = 0, combDelay_ = 0;
    int clipLen_ = 0, period_ = 0, pos_ = 0, prevPeriod_ = 0, prevPos_ = 0, xf_ = 0, liveXf_ = 0, fill_ = 0;
    int loopLen_ = 0, wraps_ = 0, rollJoin_ = 0, swap_ = 0, warm_ = 0, left_ = 0, span_ = 1, quietRun_ = 0;
    long long w_ = 0, mask_ = 0, echoMask_ = 0, combMask_ = 0, revMask_ = 0, t_ = 0, crushCount_ = 0, revW_ = 0, revR_ = 0, wordFrom_ = 0;
    float glide_ = 0.003f, mix_ = 0.0f, echoLp_ = 0.3f, beepAmp_ = 0.15f;
    float curMin_ = 1e9f, prevMin_ = 1e9f, floor_ = 0.0f, env_ = 0.0f, segLoud_ = 0.0f, quiet_ = 1e9f, quietRise_ = 1.0f;
    float echoLow_[2] = {0.0f, 0.0f}, crushHeld_[2] = {0.0f, 0.0f};
    bool held_ = false, ready_ = false, grainsOk_ = false, heard_ = false;
    std::vector<float> hist_[2], clip_[2], prev_[2], echo_[2], comb_[2], rev_[2], dry_[2], mid_, window_;
    Grain grains_[2];
    Rng rng_{0x2545F491u};
    Follower level_, peak_, word_;
    Glitcher glitchers_[2];
};

}
