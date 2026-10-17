#pragma once
#include <cmath>
#include <complex>
#include <vector>
#include "../convolution/fft.hpp"

namespace voice {

struct ToneGains {
    double loDb = 0.0, midDb = 0.0, hiDb = 0.0;
    bool flat() const { return loDb == 0.0 && midDb == 0.0 && hiDb == 0.0; }
    bool same(const ToneGains& o) const { return loDb == o.loDb && midDb == o.midDb && hiDb == o.hiDb; }
};

struct ToneSection {
    static constexpr double PI = 3.14159265358979323846;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;

    void clear() { z1 = z2 = 0.0; }
    inline double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void set(double n0, double n1, double n2, double d0, double d1, double d2) {
        b0 = n0 / d0; b1 = n1 / d0; b2 = n2 / d0;
        a1 = d1 / d0; a2 = d2 / d0;
    }
    void lowShelf(double fs, double hz, double db) {
        const double A = std::pow(10.0, db / 40.0), w = 2.0 * PI * hz / fs, c = std::cos(w);
        const double sq = 2.0 * std::sqrt(A) * std::sin(w) / std::sqrt(2.0);
        set(A * ((A + 1.0) - (A - 1.0) * c + sq), 2.0 * A * ((A - 1.0) - (A + 1.0) * c), A * ((A + 1.0) - (A - 1.0) * c - sq),
            (A + 1.0) + (A - 1.0) * c + sq, -2.0 * ((A - 1.0) + (A + 1.0) * c), (A + 1.0) + (A - 1.0) * c - sq);
    }
    void highShelf(double fs, double hz, double db) {
        const double A = std::pow(10.0, db / 40.0), w = 2.0 * PI * hz / fs, c = std::cos(w);
        const double sq = 2.0 * std::sqrt(A) * std::sin(w) / std::sqrt(2.0);
        set(A * ((A + 1.0) + (A - 1.0) * c + sq), -2.0 * A * ((A - 1.0) + (A + 1.0) * c), A * ((A + 1.0) + (A - 1.0) * c - sq),
            (A + 1.0) - (A - 1.0) * c + sq, 2.0 * ((A - 1.0) - (A + 1.0) * c), (A + 1.0) - (A - 1.0) * c - sq);
    }
    void peak(double fs, double hz, double q, double db) {
        const double A = std::pow(10.0, db / 40.0), w = 2.0 * PI * hz / fs, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        set(1.0 + al * A, -2.0 * c, 1.0 - al * A, 1.0 + al / A, -2.0 * c, 1.0 - al / A);
    }
    double power(double fs, double hz) const {
        const std::complex<double> z = std::polar(1.0, -2.0 * PI * hz / fs);
        return std::norm((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z));
    }
};

struct VoiceSpectrum {
    static constexpr int FRAME = 2048, MOST_BANDS = 40, LOW_BINS = 9, ZERO_RUN = 64, LIVE_MIN = 256;
    static constexpr double FLOOR_DB = -55.0, SILENT_DB = -100.0, ABOVE_NOISE_DB = 12.0, NOISE_RISE_DB_PER_SEC = 1.0;
    static constexpr double RANGE_DB = 30.0, PEAK_FALL_DB_PER_SEC = 6.0, LEARN_SEC = 3.0, SEED_FRAMES = 16.0;
    static constexpr int SPEECH_POINTS = 25;
    static constexpr double SPEECH_HZ[SPEECH_POINTS] = {31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
                                                        630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000};
    static constexpr double SPEECH_DB[SPEECH_POINTS] = {30.0, 32.0, 35.0, 38.6, 43.5, 54.4, 57.7, 56.8, 58.2, 59.7, 60.0, 62.1, 62.1,
                                                        60.5, 56.8, 53.7, 53.0, 52.0, 48.7, 48.1, 46.8, 45.6, 44.5, 44.3, 43.7};

    double fs = 48000.0, rate = 0.0, peakDb = -200.0, peakFall = 0.0, noiseDb = 0.0, noiseRise = 0.0;
    bool noiseKnown = false;
    int bands = 0;
    double bandHz[MOST_BANDS] = {}, weight[MOST_BANDS] = {}, seed[MOST_BANDS] = {}, heard[MOST_BANDS] = {};
    double heardTotal = 0.0;
    int binLo[MOST_BANDS] = {}, binHi[MOST_BANDS] = {};
    std::vector<float> frame, window;
    std::vector<conv::cf> spec;
    conv::FFT fft;
    int fill = 0;
    unsigned learned = 0;

    static double speechDb(double hz) {
        if (hz <= SPEECH_HZ[0]) return SPEECH_DB[0] - 12.0 * std::log2(SPEECH_HZ[0] / hz);
        for (int i = 1; i < SPEECH_POINTS; ++i)
            if (hz <= SPEECH_HZ[i]) {
                const double t = std::log(hz / SPEECH_HZ[i - 1]) / std::log(SPEECH_HZ[i] / SPEECH_HZ[i - 1]);
                return SPEECH_DB[i - 1] + t * (SPEECH_DB[i] - SPEECH_DB[i - 1]);
            }
        return SPEECH_DB[SPEECH_POINTS - 1] - 1.5 * std::log2(hz / SPEECH_HZ[SPEECH_POINTS - 1]);
    }

    void addBand(int lo, int hi) {
        if (hi < lo || bands >= MOST_BANDS) return;
        const double binHz = fs / FRAME;
        binLo[bands] = lo;
        binHi[bands] = hi;
        bandHz[bands] = std::sqrt(lo * binHz * (hi * binHz));
        const double thirdOctave = std::pow(2.0, 1.0 / 6.0) - std::pow(2.0, -1.0 / 6.0);
        double typical = 0.0;
        for (int k = lo; k <= hi; ++k) typical += std::pow(10.0, speechDb(k * binHz) / 10.0) / (thirdOctave * k);
        seed[bands] = typical;
        ++bands;
    }

    void prepare(double rate_) {
        fs = rate_;
        frame.assign(FRAME, 0.0f);
        window.resize(FRAME);
        for (int i = 0; i < FRAME; ++i) window[i] = (float)(0.5 - 0.5 * std::cos(2.0 * ToneSection::PI * i / FRAME));
        spec.assign(FRAME, conv::cf(0.0f, 0.0f));
        fft.init(FRAME);
        bands = 0;
        for (int k = 1; k <= LOW_BINS; ++k) addBand(k, k);
        const double binHz = fs / FRAME, step = std::pow(2.0, 1.0 / 3.0);
        int next = LOW_BINS + 1;
        for (double edge = (LOW_BINS + 0.5) * binHz * step; next <= FRAME / 2; edge *= step) {
            int hi = (int)std::floor(edge / binHz);
            if (hi > FRAME / 2 || edge * step > fs / 2.0) hi = FRAME / 2;
            if (hi >= next) {
                addBand(next, hi);
                next = hi + 1;
            }
        }
        double sum = 0.0;
        for (int b = 0; b < bands; ++b) sum += seed[b];
        for (int b = 0; b < bands; ++b) {
            seed[b] /= sum;
            weight[b] = seed[b];
            heard[b] = 0.0;
        }
        heardTotal = 0.0;
        rate = 1.0 - std::exp(-FRAME / (LEARN_SEC * fs));
        peakFall = PEAK_FALL_DB_PER_SEC * FRAME / fs;
        peakDb = -200.0;
        noiseRise = NOISE_RISE_DB_PER_SEC * FRAME / fs;
        noiseDb = 0.0;
        noiseKnown = false;
        fill = 0;
        learned = 0;
    }

    void listen(const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            frame[fill++] = x[i];
            if (fill == FRAME) {
                analyse();
                fill = 0;
            }
        }
    }

    void shift(double db) {
        noiseDb += db;
        peakDb += db;
    }

    void analyse() {
        double e = 0.0;
        int zeros = 0, run = 0, longest = 0;
        for (float v : frame) {
            e += (double)v * v;
            if (v == 0.0f) {
                ++zeros;
                if (++run > longest) longest = run;
            } else {
                run = 0;
            }
        }
        const bool gapped = longest >= ZERO_RUN;
        const int live = FRAME - zeros;
        if (gapped && live < LIVE_MIN) return;
        const double db = 10.0 * std::log10(e / (gapped ? live : FRAME) + 1e-30);
        if (db > SILENT_DB) {
            noiseDb = !noiseKnown || db < noiseDb + noiseRise ? db : noiseDb + noiseRise;
            noiseKnown = true;
        }
        if (gapped) return;
        peakDb = db > peakDb - peakFall ? db : peakDb - peakFall;
        if (!(db >= FLOOR_DB && db >= noiseDb + ABOVE_NOISE_DB && db >= peakDb - RANGE_DB)) return;
        for (int i = 0; i < FRAME; ++i) spec[i] = conv::cf(frame[i] * window[i], 0.0f);
        fft.forward(spec.data());
        double band[MOST_BANDS], total = 0.0;
        for (int b = 0; b < bands; ++b) {
            band[b] = 0.0;
            for (int k = binLo[b]; k <= binHi[b]; ++k) band[b] += std::norm(spec[k]);
            total += band[b];
        }
        if (!(total > 0.0)) return;
        const double early = 1.0 / (learned + 1.0), r = early > rate ? early : rate;
        for (int b = 0; b < bands; ++b) heard[b] += r * (band[b] - heard[b]);
        heardTotal += r * (total - heardTotal);
        ++learned;
        const double k = SEED_FRAMES / (SEED_FRAMES + learned);
        for (int b = 0; b < bands; ++b) weight[b] = k * seed[b] + (1.0 - k) * heard[b] / heardTotal;
    }

    double levelDb(const ToneSection& lo, const ToneSection& mid, const ToneSection& hi) const {
        double num = 0.0, den = 0.0;
        for (int b = 0; b < bands; ++b) {
            num += weight[b] * lo.power(fs, bandHz[b]) * mid.power(fs, bandHz[b]) * hi.power(fs, bandHz[b]);
            den += weight[b];
        }
        return den > 0.0 && num > 0.0 ? 10.0 * std::log10(num / den) : 0.0;
    }
};

struct VoiceTone {
    static constexpr double LO_HZ = 200.0, MID_HZ = 1000.0, MID_Q = 0.8, HI_HZ = 3500.0;
    static constexpr double GLIDE_SEC = 0.03, SNAP_DB = 0.01, TRIM_SEC = 0.05;
    static constexpr int SETTLE_BLOCKS = 3, STEP = 32;

    double fs = 48000.0, trimStep = 0.0, trimNow = 1.0, trimTarget = 1.0;
    ToneGains target, now;
    ToneSection lo, mid, hi;
    VoiceSpectrum voiceSpectrum;
    bool running = false;
    int flatBlocks = 0;
    unsigned trimSeen = 0;

    void prepare(double rate) {
        fs = rate;
        target = now = ToneGains();
        running = false;
        flatBlocks = 0;
        trimStep = 1.0 - std::exp(-1.0 / (TRIM_SEC * fs));
        trimNow = trimTarget = 1.0;
        voiceSpectrum.prepare(rate);
        trimSeen = voiceSpectrum.learned;
        shape();
        clear();
    }
    void set(const ToneGains& t) { target = t; }
    void shiftLevel(double db) { voiceSpectrum.shift(db); }
    void clear() { lo.clear(); mid.clear(); hi.clear(); }
    void retrim() {
        const double db = voiceSpectrum.levelDb(lo, mid, hi);
        trimTarget = db > 0.0 ? std::pow(10.0, -db / 20.0) : 1.0;
        trimSeen = voiceSpectrum.learned;
    }
    void shape() {
        lo.lowShelf(fs, LO_HZ, now.loDb);
        mid.peak(fs, MID_HZ, MID_Q, now.midDb);
        hi.highShelf(fs, HI_HZ, now.hiDb);
        retrim();
    }
    static double glide(double from, double to, double k) {
        const double v = from + (to - from) * k;
        return std::fabs(to - v) < SNAP_DB ? to : v;
    }

    void process(float* x, int n) {
        if (n <= 0) return;
        voiceSpectrum.listen(x, n);
        if (!running) {
            if (target.flat()) return;
            running = true;
            flatBlocks = 0;
            clear();
        }
        if (trimSeen != voiceSpectrum.learned) retrim();
        for (int at = 0; at < n; at += STEP) {
            const int len = n - at < STEP ? n - at : STEP;
            if (!now.same(target)) {
                const double k = 1.0 - std::exp(-len / (GLIDE_SEC * fs));
                now.loDb = glide(now.loDb, target.loDb, k);
                now.midDb = glide(now.midDb, target.midDb, k);
                now.hiDb = glide(now.hiDb, target.hiDb, k);
                shape();
            }
            for (int i = at; i < at + len; ++i) {
                trimNow += (trimTarget - trimNow) * trimStep;
                x[i] = (float)(trimNow * hi.run(mid.run(lo.run((double)x[i]))));
            }
        }
        if (now.flat() && target.flat() && std::fabs(trimNow - 1.0) < 1e-5) {
            if (++flatBlocks >= SETTLE_BLOCKS) {
                running = false;
                trimNow = 1.0;
            }
        } else {
            flatBlocks = 0;
        }
    }
};

}
