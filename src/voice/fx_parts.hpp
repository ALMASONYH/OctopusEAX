#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace voice {

static constexpr double TWO_PI = 6.283185307179586;

struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed) {}
    double uniform() { state = state * 1664525u + 1013904223u; return (state >> 8) / 16777216.0; }
    double between(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    float noise() { return (float)(2.0 * uniform() - 1.0); }
};

struct Filter {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void clear() { z1 = z2 = 0.0; }
    inline float run(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
    void lowpass(double fs, double hz, double q) {
        const double w = TWO_PI * hz / fs, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        take((1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0, 1.0 + al, -2.0 * c, 1.0 - al);
    }
    void highpass(double fs, double hz, double q) {
        const double w = TWO_PI * hz / fs, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        take((1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0, 1.0 + al, -2.0 * c, 1.0 - al);
    }
    void bandpass(double fs, double hz, double q) {
        const double w = TWO_PI * hz / fs, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        take(al, 0.0, -al, 1.0 + al, -2.0 * c, 1.0 - al);
    }
    void peak(double fs, double hz, double q, double db) {
        const double A = std::pow(10.0, db / 40.0), w = TWO_PI * hz / fs, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        take(1.0 + al * A, -2.0 * c, 1.0 - al * A, 1.0 + al / A, -2.0 * c, 1.0 - al / A);
    }
    void lowShelf(double fs, double hz, double db) {
        const double A = std::pow(10.0, db / 40.0), w = TWO_PI * hz / fs, c = std::cos(w), s = std::sin(w);
        const double sq = 2.0 * std::sqrt(A) * s / 2.0 * std::sqrt(2.0);
        take(A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
             (A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
    }

private:
    void take(double B0, double B1, double B2, double A0, double A1, double A2) {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
};

struct Follower {
    float env = 0.0f, up = 0.0f, down = 0.0f;

    void prepare(double fs, double attackSec, double releaseSec) {
        up = (float)(1.0 - std::exp(-1.0 / (attackSec * fs)));
        down = (float)(1.0 - std::exp(-1.0 / (releaseSec * fs)));
        env = 0.0f;
    }
    inline float run(float x) {
        const float a = std::fabs(x);
        env += (a > env ? up : down) * (a - env);
        return env;
    }
};

struct Onsets {
    Follower fast, slow;
    float floor = 0.0f;
    int wait = 0, spacing = 0;

    void prepare(double fs, double floorDb, double spacingSec) {
        fast.prepare(fs, 0.001, 0.015);
        slow.prepare(fs, 0.080, 0.080);
        floor = (float)std::pow(10.0, floorDb / 20.0);
        spacing = (int)(spacingSec * fs);
        wait = 0;
    }
    inline bool run(float x) {
        const float f = fast.run(x), s = slow.run(x);
        if (wait > 0) { --wait; return false; }
        if (f > floor && f > 2.2f * s) { wait = spacing; return true; }
        return false;
    }
};

struct Wobble {
    std::vector<float> buf;
    int mask = 0;
    long long w = 0;

    void prepare(int maxDelay) {
        int size = 16;
        while (size < maxDelay + 8) size <<= 1;
        buf.assign((size_t)size, 0.0f);
        mask = size - 1;
        w = 0;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
    inline void push(float x) { buf[(size_t)(w & mask)] = x; ++w; }
    inline float tap(double delay) const {
        const double p = (double)(w - 1) - delay;
        const long long i = (long long)std::floor(p);
        const float t = (float)(p - (double)i);
        return buf[(size_t)(i & mask)] * (1.0f - t) + buf[(size_t)((i + 1) & mask)] * t;
    }
    inline float run(float x, double delay) { push(x); return tap(delay); }
};

struct Swing {
    double phase = 0.0, rate = 1.0, lo = 1.0, hi = 1.0;

    void prepare(double loHz, double hiHz, Rng& rng) { lo = loHz; hi = hiHz; phase = 0.0; rate = rng.between(lo, hi); }
    inline float run(double fs, Rng& rng) {
        phase += rate / fs;
        if (phase >= 1.0) { phase -= 1.0; rate = rng.between(lo, hi); }
        return (float)std::sin(TWO_PI * phase);
    }
};

struct Repeater {
    static const int SIZE = 1 << 16, MASK = SIZE - 1, FADE = 240;

    std::vector<float> buf;
    long long w = 0;
    double rd = 0.0, speed = 1.0, oldRd = 0.0, oldSpeed = 1.0;
    int fade = 0;
    long long segStart = 0, armedAt = -1;
    int segLen = 0, repeatsLeft = 0, armedLen = 0, armedRepeats = 0;
    double segSpeed = 1.0, armedSpeed = 1.0, maxLag = 0.0;
    Follower inEnv, outEnv;
    float peak = 0.0f, peakFall = 0.0f;

    void prepare(double fs) {
        buf.assign(SIZE, 0.0f);
        maxLag = 0.45 * fs;
        inEnv.prepare(fs, 0.002, 0.030);
        outEnv.prepare(fs, 0.002, 0.030);
        peakFall = (float)std::exp(-1.0 / (2.0 * fs));
        clear();
    }
    void clear() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        w = 0; rd = 0.0; speed = 1.0; fade = 0;
        armedAt = -1; repeatsLeft = 0; peak = 0.0f;
        inEnv.env = outEnv.env = 0.0f;
    }
    bool busy() const { return armedAt >= 0 || repeatsLeft > 0; }
    void arm(int len, int repeats, double spd) {
        if (busy() || len < FADE * 2 || repeats < 1) return;
        armedAt = w; armedLen = len; armedRepeats = repeats; armedSpeed = spd;
    }
    inline float tap(double p) const {
        const long long i = (long long)std::floor(p);
        const float t = (float)(p - (double)i);
        return buf[(size_t)(i & MASK)] * (1.0f - t) + buf[(size_t)((i + 1) & MASK)] * t;
    }
    inline float run(float x) {
        buf[(size_t)(w & MASK)] = x;
        ++w;
        const double latest = (double)(w - 1);
        const float ie = inEnv.run(x);
        peak = ie > peak ? ie : peak * peakFall;
        if (fade == 0) {
            if (armedAt >= 0 && repeatsLeft == 0 && rd >= (double)(armedAt + armedLen)) {
                segStart = armedAt; segLen = armedLen; repeatsLeft = armedRepeats; segSpeed = armedSpeed;
                armedAt = -1;
                jump((double)segStart, segSpeed);
            } else if (repeatsLeft > 0 && rd >= (double)(segStart + segLen)) {
                if (--repeatsLeft > 0) jump((double)segStart, segSpeed);
                else jump((double)(segStart + segLen), 1.0);
            } else if (repeatsLeft == 0 && armedAt < 0) {
                const double lag = latest - rd;
                const float quiet = peak * 0.06f + 1e-4f;
                if (lag > 1.0 && ((ie < quiet && outEnv.env < quiet) || lag > maxLag)) jump(latest, 1.0);
            }
        }
        if (rd > latest) rd = latest;
        float y = tap(rd);
        rd += speed;
        if (fade > 0) {
            const float t = (float)fade / (float)FADE;
            if (oldRd > latest) oldRd = latest;
            y = y * (1.0f - t) + tap(oldRd) * t;
            oldRd += oldSpeed;
            --fade;
        }
        outEnv.run(y);
        return y;
    }

private:
    void jump(double to, double spd) { oldRd = rd; oldSpeed = speed; fade = FADE; rd = to; speed = spd; }
};

struct Stutter {
    static const int SIZE = 1 << 15, MASK = SIZE - 1, FADE = 96;

    std::vector<float> buf, seg;
    long long w = 0;
    int segLen = 0, len = 0, left = 0, fade = 0, minLen = 0, maxLen = 0;
    double rd = 0.0, speed = 1.0, oldRd = 0.0, oldSpeed = 1.0;
    float mix = 0.0f, mixStep = 0.0f;
    bool on = false, rev = false, wrapped = false;

    void prepare(double fs) {
        buf.assign(SIZE, 0.0f);
        maxLen = (int)(0.400 * fs);
        seg.assign((size_t)maxLen + 4, 0.0f);
        minLen = (int)(0.012 * fs);
        mixStep = (float)(1.0 / (0.004 * fs));
        clear();
    }
    void clear() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        std::fill(seg.begin(), seg.end(), 0.0f);
        w = 0; segLen = len = left = fade = 0;
        rd = oldRd = 0.0; speed = oldSpeed = 1.0;
        mix = 0.0f; on = rev = wrapped = false;
    }
    void start(int length, int repeats, double spd, bool reverse) {
        if (length < minLen) length = minLen;
        if (length > maxLen) length = maxLen;
        if (w < length) return;
        for (int i = 0; i < length; ++i) seg[(size_t)i] = buf[(size_t)((w - length + i) & MASK)];
        segLen = len = length; left = repeats; speed = spd; rev = reverse;
        rd = rev ? (double)(len - 1) : 0.0;
        fade = 0; on = true;
    }
    void stop() { on = false; left = 0; }
    void shrink(double by) {
        int want = (int)(len * by);
        if (want < minLen) want = minLen;
        if (want > segLen) want = segLen;
        len = want;
    }
    void grow() { len = segLen; }
    inline float tap(double p) const {
        int i = (int)std::floor(p);
        const float t = (float)(p - i);
        i %= segLen; if (i < 0) i += segLen;
        int j = i + 1; if (j >= segLen) j = 0;
        return seg[(size_t)i] * (1.0f - t) + seg[(size_t)j] * t;
    }
    inline float run(float x) {
        buf[(size_t)(w & MASK)] = x;
        ++w;
        wrapped = false;
        const float target = on ? 1.0f : 0.0f;
        if (mix < target) { mix += mixStep; if (mix > target) mix = target; }
        else if (mix > target) { mix -= mixStep; if (mix < target) mix = target; }
        if (mix <= 0.0f || segLen <= 0) return x;
        float y = tap(rd);
        if (fade > 0) {
            const float t = (float)fade / (float)FADE;
            y = y * (1.0f - t) + tap(oldRd) * t;
            oldRd += rev ? -oldSpeed : oldSpeed;
            --fade;
        }
        rd += rev ? -speed : speed;
        if (on && (rev ? rd <= 0.0 : rd >= (double)len)) {
            if (left > 0 && --left == 0) on = false;
            else {
                oldRd = rd; oldSpeed = speed; fade = FADE;
                rd = rev ? (double)(len - 1) : 0.0;
                wrapped = true;
            }
        }
        return x * (1.0f - mix) + y * mix;
    }
};

struct Breaker {
    int drop = 0, dropLen = 0, dropEdge = 1;
    int crush = 0, crushLen = 0, crushHold = 1, crushCount = 0, crushEdge = 1;
    float bits = 32.0f, held = 0.0f;

    void prepare(double fs) {
        dropEdge = (int)(0.001 * fs) + 1;
        crushEdge = (int)(0.003 * fs) + 1;
        clear();
    }
    void clear() { drop = dropLen = 0; crush = crushLen = 0; crushCount = 0; held = 0.0f; }
    void dropFor(int samples) { if (drop == 0 && samples > 2 * dropEdge) { dropLen = drop = samples; } }
    void crushFor(int samples, int hold, float levels) {
        if (crush > 0 || samples <= 2 * crushEdge) return;
        crushLen = crush = samples; crushHold = hold < 1 ? 1 : hold; bits = levels; crushCount = 0;
    }
    inline float run(float v) {
        if (crush > 0) {
            if (crushCount++ % crushHold == 0) held = std::floor(v * bits + 0.5f) / bits;
            const int done = crushLen - crush;
            float wet = 1.0f;
            if (done < crushEdge) wet = (float)done / crushEdge;
            else if (crush < crushEdge) wet = (float)crush / crushEdge;
            v = v * (1.0f - wet) + held * wet;
            --crush;
        }
        if (drop > 0) {
            const int done = dropLen - drop;
            float g = 0.0f;
            if (done < dropEdge) g = 1.0f - (float)done / dropEdge;
            else if (drop < dropEdge) g = 1.0f - (float)drop / dropEdge;
            v *= g;
            --drop;
        }
        return v;
    }
};

struct LevelMatch {
    static constexpr double WINDOW_SEC = 1.0, FLOOR = 1e-5, MOST_DB = 12.0, DB_PER_SEC = 6.0;

    double inPow = 0.0, outPow = 0.0, keep = 0.0, fs = 48000.0, gainDb = 0.0;
    float gain = 1.0f;

    void prepare(double rate) { fs = rate; keep = std::exp(-1.0 / (WINDOW_SEC * fs)); inPow = outPow = gainDb = 0.0; gain = 1.0f; }
    void measureIn(const float* x, int n) { for (int i = 0; i < n; ++i) inPow = keep * inPow + (1.0 - keep) * x[i] * x[i]; }
    void apply(float* x, int n) {
        for (int i = 0; i < n; ++i) outPow = keep * outPow + (1.0 - keep) * x[i] * x[i];
        double wantDb = gainDb;
        if (inPow > FLOOR && outPow > FLOOR) {
            wantDb = 10.0 * std::log10(inPow / outPow);
            wantDb = wantDb < -MOST_DB ? -MOST_DB : (wantDb > MOST_DB ? MOST_DB : wantDb);
        }
        const double most = DB_PER_SEC * n / fs, d = wantDb - gainDb;
        gainDb += d > most ? most : (d < -most ? -most : d);
        const float target = (float)std::pow(10.0, gainDb / 20.0);
        const float step = (target - gain) / (float)(n > 0 ? n : 1);
        for (int i = 0; i < n; ++i) { gain += step; x[i] *= gain; }
    }
};

struct PeakGuard {
    static constexpr float ROOM = 1.12f;
    static const int AHEAD = 64;

    Follower in;
    float held = 0.0f, gain = 1.0f, attack = 0.0f, release = 0.0f;
    float line[AHEAD] = {};
    int pos = 0;

    void prepare(double fs) {
        in.prepare(fs, 0.0005, 0.600);
        attack = (float)(1.0 - std::exp(-1.0 / (0.0003 * fs)));
        release = (float)(1.0 - std::exp(-1.0 / (0.080 * fs)));
        held = 0.0f; gain = 1.0f; pos = 0;
        for (float& v : line) v = 0.0f;
    }
    void measureIn(const float* x, int n) {
        held = 0.0f;
        for (int i = 0; i < n; ++i) { const float e = in.run(x[i]); if (e > held) held = e; }
    }
    void apply(float* x, int n) {
        const float limit = held * ROOM + 1e-6f;
        for (int i = 0; i < n; ++i) {
            const float a = std::fabs(x[i]);
            const float want = a > limit ? limit / a : 1.0f;
            gain += (want < gain ? attack : release) * (want - gain);
            if (want < gain) gain = gain * 0.5f + want * 0.5f;
            const float late = line[pos];
            line[pos] = x[i];
            pos = (pos + 1) % AHEAD;
            x[i] = late * gain;
        }
    }
};

}
