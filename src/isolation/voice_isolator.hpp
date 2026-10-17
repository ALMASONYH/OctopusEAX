#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../audio/fixed_ring.hpp"
#include "../convolution/fft.hpp"

namespace iso {
enum Mode { MODE_GATE = 0, MODE_CLEAN = 1, MODE_STUDIO = 2, MODE_BROADCAST = 3 };
static const int NUM_MODES = 4;
static const wchar_t* MODE_NAME[NUM_MODES] = {L"Gate", L"Clean", L"Studio", L"Broadcast"};

struct VoiceIsolator {
    static const int N = 1024, HOP = 256, HALF = N / 2 + 1;
    static const int LOOK = 19;

    static constexpr double ALPHA_S = 0.80;
    static constexpr double ALPHA_D = 0.95;
    static constexpr double ALPHA_P = 0.20;
    static constexpr double ALPHA_DD = 0.98;
    static constexpr int MIN_WIN = 300, WARM_WIN = 25, WARM_FRAMES = 300;
    static constexpr double XI_MIN = 0.0032;
    static constexpr double GATE_OPEN = 0.12;
    static constexpr double GATE_HOLD = 0.40;
    static constexpr double GATE_OPEN_TAU = 0.004;
    static constexpr double GATE_CLOSE_TAU = 0.040;
    static constexpr int SOON = 8;
    static constexpr double NOISE_MULT = 3.0, GUARD_HARM = 0.50;
    static constexpr double SPEECH_SHARE = 0.35;
    static constexpr int PROTECT_AFTER = 20, LEAD_RUN = 12, LINK_GAP = 12, LEAD_MAX = 200;
    static constexpr double TOP_HOLD_CUT = 0.12;
    static constexpr int TOP_LEAD_CUT = 130;
    static constexpr double OWN_RISE = 0.25, OWN_SETTLE = 0.004, PEAK_FALL_DB = 0.25, CARRY_EXTRA_DB = 16.0;
    static constexpr double LOST_STEP = 0.05, LOST_NEAR_DB = 16.0, LOST_FAR = 0.25, RELEARN = 100.0;
    static constexpr int FOREIGN_BLOCK = 100, QUIET_BEFORE_CLICK = 6, CLEAR_AHEAD = 16, CLEAR_AFTER = 12;
    static const int SUB = 128, CLICK_MAX_SUBS = 14, CLICK_MIN_SUBS = 3, BANDS = 22;
    static constexpr double CLICK_HP_HZ = 1500.0, CLICK_ON = 6.0, CLICK_OFF = 2.5, CLICK_FLOOR = 1.3e-4, CLICK_TAIL = 0.030, CLICK_KEEP = 0.01;
    static constexpr double FLAT_MARGIN = 1.4, LEAD_MARGIN = 4.0, HINT_BACK = 0.08, HINT_AHEAD = 0.40;

    struct Frame {
        std::vector<conv::cf> bins;
        std::vector<float> soft, full, band;
        bool voiced = false, active = false, foreign = false, click = false, core = false, hinted = false, pitchOwn = false;
        uint64_t end = 0;
    };

    struct Click { uint64_t from, to; };

    bool suppressOn = true, gateOn = true;
    double gFloor = 0.0398;
    double gateDepth = 0.0032;
    double delta = 5.0;
    double overSub = 1.0;
    double bandLo = 60.0, bandHi = 16000.0;
    int mode = MODE_STUDIO, level = 60;

    conv::FFT fft;
    std::vector<float> win;
    std::vector<float> inBuf;
    std::vector<float> ola;
    std::vector<float> outQ;
    size_t outHead = 0;
    int hopFill = 0;
    std::vector<conv::cf> spec;
    std::vector<double> S, Smin, Stmp, P, noise, prevG2G, gain, whole;
    double olaNorm = 1.0, fs = 48000.0;
    int frames = 0, minCount = 0, binLo = 0, binHi = 0, binVoiceLo = 0, binVoiceHi = 0;
    double gate = 1.0, gateOpenStep = 0.0, gateCloseStep = 0.0;
    int holdFrames = 0, hang = 0;

    bool strictVoice = false, strong = false;
    double harmThresh = 0.45;
    std::vector<double> pre;
    int lagMin = 0, lagMax = 0;
    double harm = 1.0, nacHp = 0.0;
    int voicedRun = 0, activeRun = 0, sinceVoice = 1 << 20, sinceSpoken = 1 << 20;
    int leadLeft = 0, leadMax = LEAD_MAX;
    double top = 0.0, ownDb = -300.0, peakDb = -300.0, ownGapDb = 99.0, peakGapDb = 0.0, lostDb = 0.0, lostAcc = 0.0;
    int sinceLoud = 1 << 20, sinceForeign = 1 << 20;

    std::vector<Frame> fifo;
    int fifoHead = 0, fifoCount = 0;

    dsp::FixedRing<Click, 32> clicks;
    uint64_t seen = 0, clickFrom = 0, hintFrom = 0, hintTo = 0;
    double hpA = 0.0, hp1 = 0.0, hp2 = 0.0, hpIn1 = 0.0, hpIn2 = 0.0, subE = 0.0, bgHi = 0.0, prevE1 = 0.0, prevE2 = 0.0;
    int subN = 0, clickSubs = 0;
    bool inClick = false;
    std::vector<float> calm, flat, bandGain, bandW;
    std::vector<int> bandOf, bandAt;

    void prepare(double fs_) {
        fs = fs_;
        fft.init(N);
        win.assign(N, 0.f);
        for (int i = 0; i < N; ++i)
            win[i] = (float)std::sqrt(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (i + 0.5) / N));
        double acc = 0.0;
        for (int i = 0; i < N; i += HOP) acc += (double)win[i] * win[i];
        olaNorm = 1.0 / acc;
        spec.assign(N, conv::cf(0.f, 0.f));
        S.assign(HALF, 0.0); Smin.assign(HALF, 0.0); Stmp.assign(HALF, 0.0);
        P.assign(HALF, 0.0); noise.assign(HALF, 0.0); prevG2G.assign(HALF, 0.0); gain.assign(HALF, 1.0); whole.assign(HALF, 1.0);
        binVoiceLo = (int)(120.0 * N / fs);
        binVoiceHi = (int)(6000.0 * N / fs);
        nacHp = std::exp(-2.0 * 3.14159265358979323846 * 250.0 / fs);
        gateOpenStep  = 1.0 - std::exp(-(double)HOP / (GATE_OPEN_TAU * fs));
        gateCloseStep = 1.0 - std::exp(-(double)HOP / (GATE_CLOSE_TAU * fs));
        pre.assign(N, 0.0);
        lagMin = (int)(fs / 400.0);
        lagMax = (int)(fs / 100.0);
        if (lagMax > N / 2) lagMax = N / 2;
        hpA = std::exp(-2.0 * 3.14159265358979323846 * CLICK_HP_HZ / fs);
        calm.assign(BANDS, 0.f);
        flat.assign(HALF, 1.f);
        bandGain.assign(BANDS, 1.f);
        static const double edge[BANDS + 1] = {0, 150, 300, 450, 600, 800, 1000, 1250, 1500, 1800, 2100, 2500, 3000, 3500, 4200, 5000, 6000, 7200, 8600, 10500, 13000, 16500, 1e9};
        double centre[BANDS];
        for (int b = 0; b < BANDS; ++b) centre[b] = 0.5 * (edge[b] + std::min(edge[b + 1], fs / 2));
        bandOf.assign(HALF, 0); bandAt.assign(HALF, 0); bandW.assign(HALF, 0.f);
        for (int k = 0; k < HALF; ++k) {
            const double hz = k * fs / N;
            int b = 0;
            while (b < BANDS - 1 && hz >= edge[b + 1]) ++b;
            bandOf[k] = b;
            int c = 0;
            while (c < BANDS - 2 && hz >= centre[c + 1]) ++c;
            bandAt[k] = c;
            const double w = (hz - centre[c]) / (centre[c + 1] - centre[c]);
            bandW[k] = (float)(w < 0.0 ? 0.0 : w > 1.0 ? 1.0 : w);
        }
        fifo.assign(LOOK + 1, Frame());
        for (auto& f : fifo) {
            f.bins.assign(HALF, conv::cf(0.f, 0.f));
            f.soft.assign(HALF, 0.f);
            f.full.assign(HALF, 0.f);
            f.band.assign(BANDS, 0.f);
        }
        configure(mode, level);
        reset();
    }

    void configure(int m, int lvl) {
        if (m < 0) m = 0;
        if (m >= NUM_MODES) m = NUM_MODES - 1;
        if (lvl < 0) lvl = 0;
        if (lvl > 100) lvl = 100;
        mode = m; level = lvl;

        const double floorDb = 14.0 + 0.62 * lvl;
        const double gateDbAlone = 26.0 + 0.66 * lvl;
        const double hard = lvl > 40 ? (lvl - 40) / 60.0 : 0.0;
        suppressOn = (m != MODE_GATE);
        gateOn = (m != MODE_CLEAN) || lvl >= 30;
        double fdb = floorDb, gdb = floorDb + 24.0;
        delta = 5.0 + 4.0 * hard; overSub = 1.0 + 0.5 * hard; bandLo = 60.0; bandHi = 16000.0;
        if (m == MODE_GATE) { fdb = 0.0; gdb = gateDbAlone; }
        if (m == MODE_CLEAN) gdb = 30.0 + 0.50 * lvl;
        if (m == MODE_BROADCAST) { fdb = floorDb + 6.0; gdb = fdb + 24.0; delta += 1.5; bandLo = 80.0; bandHi = 9000.0; }
        strong = gateOn && lvl >= 70 && m != MODE_CLEAN;
        if (strong) {
            fdb += 8.0 + (lvl - 70) * 0.3; gdb = fdb + 24.0;
            if (bandLo < 80.0) bandLo = 80.0;
            if (bandHi > 8000.0) bandHi = 8000.0;
        }
        gFloor = std::pow(10.0, -fdb / 20.0);
        gateDepth = std::pow(10.0, -gdb / 20.0);
        binLo = (int)(bandLo * N / fs);
        binHi = (int)(bandHi * N / fs);
        top = lvl > 80 ? (lvl - 80) / 20.0 : 0.0;
        holdFrames = (int)((GATE_HOLD - 0.10 * hard - TOP_HOLD_CUT * top) * fs / HOP);
        leadMax = LEAD_MAX - (int)(TOP_LEAD_CUT * top);
        ownGapDb = lvl < 30 ? 99.0 : std::max(11.0, 30.0 - 0.36 * (lvl - 30));
        peakGapDb = lvl < 50 ? 0.0 : std::max(10.0, 16.0 - 0.2 * (lvl - 50));

        strictVoice = gateOn && (m == MODE_BROADCAST ? lvl >= 35 : m == MODE_CLEAN ? lvl >= 50 : lvl >= 45);
        harmThresh = (m == MODE_BROADCAST) ? 0.40 : 0.45;
    }
    void reset() {
        inBuf.assign(N, 0.f);
        ola.assign(N, 0.f);
        outQ.assign(HOP - 1, 0.f);
        outHead = 0; hopFill = 0; frames = 0; minCount = 0;
        for (int k = 0; k < HALF; ++k) { S[k] = Smin[k] = Stmp[k] = noise[k] = 0.0; P[k] = 0.0; prevG2G[k] = 0.0; gain[k] = 1.0; }
        gate = 1.0; hang = holdFrames;
        harm = 1.0; voicedRun = 0; activeRun = 0; sinceVoice = 1 << 20; sinceSpoken = 1 << 20; leadLeft = leadMax;
        ownDb = peakDb = -300.0;
        sinceLoud = sinceForeign = 1 << 20;
        lostAcc = 0.0;
        fifoHead = 0; fifoCount = 0;
        clicks.clear();
        seen = clickFrom = hintFrom = hintTo = 0;
        hp1 = hp2 = hpIn1 = hpIn2 = subE = bgHi = prevE1 = prevE2 = 0.0;
        subN = clickSubs = 0;
        inClick = false;
        std::fill(calm.begin(), calm.end(), 0.f);
    }

    void shiftLevel(double db) {
        if (ownDb > -200.0) ownDb += db;
        if (peakDb > -300.0) peakDb += db;
        lostDb += db;
    }

    void inputHint() {
        const uint64_t back = (uint64_t)(HINT_BACK * fs);
        hintFrom = seen > back ? seen - back : 0;
        hintTo = seen + (uint64_t)(HINT_AHEAD * fs);
        for (int i = 0; i < fifoCount; ++i) {
            Frame& g = fifo[(fifoHead + i) % (LOOK + 1)];
            if (g.end > hintFrom) g.hinted = true;
        }
    }

    static double expint1(double x) {
        if (x <= 0.0) return 50.0;
        if (x < 1.0) {
            return -0.57721566490153286 - std::log(x)
                 + x * (1.0 + x * (-0.25 + x * (1.0 / 18.0 + x * (-1.0 / 96.0 + x * (1.0 / 600.0)))));
        }
        if (x > 50.0) return 0.0;
        const double num = x * x + 2.334733 * x + 0.250621;
        const double den = x * x + 3.330657 * x + 1.681534;
        return std::exp(-x) / x * num / den;
    }

    bool clickNear(uint64_t from, uint64_t to) const {
        if (inClick && clickFrom < to) return true;
        for (size_t i = 0; i < clicks.size(); ++i)
            if (clicks[i].from < to && clicks[i].to > from) return true;
        return false;
    }

    void watchClicks(float x) {
        const double h1 = hpA * (hp1 + x - hpIn1);
        hpIn1 = x;
        hp1 = h1;
        const double h2 = hpA * (hp2 + h1 - hpIn2);
        hpIn2 = h1;
        hp2 = h2;
        subE += h2 * h2;
        if (++subN < SUB) return;
        const double e = subE;
        const uint64_t start = seen + 1 - SUB;
        subE = 0.0;
        subN = 0;
        if (bgHi <= 0.0) bgHi = e;
        const double ref = std::max(bgHi, std::max(prevE1, prevE2));
        if (!inClick && e > CLICK_ON * ref && e > CLICK_FLOOR) { inClick = true; clickFrom = start; clickSubs = 0; }
        if (inClick) {
            ++clickSubs;
            if ((e < CLICK_OFF * bgHi && clickSubs >= CLICK_MIN_SUBS) || clickSubs >= CLICK_MAX_SUBS) {
                inClick = false;
                clicks.push_back({clickFrom, start + SUB});
            }
        } else {
            bgHi += (e < 3.0 * bgHi ? 0.03 : 0.001) * (e - bgHi);
        }
        prevE2 = prevE1;
        prevE1 = e;
        while (!clicks.empty() && clicks.front().to + 4 * (uint64_t)N < seen) clicks.pop_front();
    }

    void analyse() {
        for (int i = 0; i < N; ++i) spec[i] = conv::cf(inBuf[i] * win[i], 0.f);
        fft.forward(spec.data());
        double voiceBins = 0.0, voiceOn = 0.0;
        double curE = 0.0, noiseE = 0.0;
        for (int k = 0; k < HALF; ++k) {
            const double re = spec[k].real(), im = spec[k].imag();
            double y2 = re * re + im * im;
            if (k > 0 && k < HALF - 1) {
                const double r1 = spec[k - 1].real(), i1 = spec[k - 1].imag();
                const double r2 = spec[k + 1].real(), i2 = spec[k + 1].imag();
                y2 = 0.5 * y2 + 0.25 * (r1 * r1 + i1 * i1) + 0.25 * (r2 * r2 + i2 * i2);
            }
            if (frames == 0) { S[k] = Smin[k] = Stmp[k] = noise[k] = y2; }

            S[k] = ALPHA_S * S[k] + (1.0 - ALPHA_S) * y2;
            if (S[k] < Smin[k]) Smin[k] = S[k];
            if (S[k] < Stmp[k]) Stmp[k] = S[k];
            const double ind = (S[k] > delta * Smin[k]) ? 1.0 : 0.0;
            P[k] = ALPHA_P * P[k] + (1.0 - ALPHA_P) * ind;
            const double ad = ALPHA_D + (1.0 - ALPHA_D) * P[k];
            noise[k] = ad * noise[k] + (1.0 - ad) * y2;

            const double nz = (noise[k] > 1e-20 ? noise[k] : 1e-20) * overSub;
            const double gamma = y2 / nz;
            double xi = ALPHA_DD * prevG2G[k] + (1.0 - ALPHA_DD) * (gamma > 1.0 ? gamma - 1.0 : 0.0);
            if (xi < XI_MIN) xi = XI_MIN;
            double gf = 1.0, gw = 1.0;
            if (suppressOn) {
                const double v = xi / (1.0 + xi) * gamma;
                double g = xi / (1.0 + xi) * std::exp(0.5 * expint1(v));
                if (g > 1.0) g = 1.0;
                const double p = P[k];
                gf = std::pow(g, p) * std::pow(gFloor, 1.0 - p);
                if (gf < gFloor) gf = gFloor;
                gw = g < gFloor ? gFloor : g;
            }
            if (k < binLo || k > binHi) gf = gw = suppressOn ? gFloor : 0.0316;
            prevG2G[k] = gf * gf * gamma;
            gain[k] = gf;
            whole[k] = gw;

            if (k >= binVoiceLo && k <= binVoiceHi) {
                voiceBins += 1.0;
                if (P[k] > 0.5 || gamma > 3.0) voiceOn += 1.0;
                curE += y2;
                noiseE += noise[k];
            }
        }
        ++frames;

        const int winNow = frames < WARM_FRAMES ? WARM_WIN : MIN_WIN;
        if (++minCount >= winNow) {
            minCount = 0;
            for (int k = 0; k < HALF; ++k) { Smin[k] = Stmp[k] < S[k] ? Stmp[k] : S[k]; Stmp[k] = S[k]; }
        }

        const double share = voiceBins > 0 ? voiceOn / voiceBins : 0.0;

        pre[0] = 0.0;
        double ePre = 0.0, eHp = 0.0;
        if (strong) {
            double h1 = 0.0, h2 = 0.0, px = 0.0, p1 = 0.0;
            for (int n = 1; n < N; ++n) {
                const double d = inBuf[n] - 0.95 * inBuf[n - 1];
                h1 = nacHp * (h1 + d - px);
                h2 = nacHp * (h2 + h1 - p1);
                px = d; p1 = h1;
                pre[n] = h2;
                ePre += d * d; eHp += h2 * h2;
            }
        } else {
            for (int n = 1; n < N; ++n) pre[n] = inBuf[n] - 0.95 * inBuf[n - 1];
        }
        const bool tilted = strong && eHp < 0.05 * ePre;
        const int W = N - lagMax;
        double e0 = 0.0;
        for (int n = 0; n < W; ++n) e0 += pre[n] * pre[n];
        double best = 0.0;
        for (int L = lagMin; L <= lagMax; ++L) {
            double cross = 0.0, eL = 0.0;
            for (int n = 0; n < W; ++n) { const double b = pre[n + L]; cross += pre[n] * b; eL += b * b; }
            const double nac = cross / (std::sqrt(e0 * eL) + 1e-20);
            if (nac > best) best = nac;
        }
        harm = best;

        const uint64_t winEnd = seen, winStart = seen > (uint64_t)N ? seen - N : 0;
        const uint64_t tail = (uint64_t)(CLICK_TAIL * fs + 0.5);
        const bool clickCore = clickNear(winStart + N / 4, winEnd - N / 4);
        const bool clicky = clickNear(winStart > tail ? winStart - tail : 0, winEnd);
        const bool guarded = strong && gate < 0.5 && sinceVoice > holdFrames;
        const double thrNow = guarded ? GUARD_HARM : harmThresh;
        const bool loud = curE > NOISE_MULT * noiseE;
        const bool pitchFree = harm > thrNow && !tilted && loud;
        const bool pitchedRaw = pitchFree && (!clicky || voicedRun > 0 || sinceLoud <= QUIET_BEFORE_CLICK);
        if (loud && !clicky) sinceLoud = 0; else if (!clicky && sinceLoud < (1 << 20)) ++sinceLoud;
        const double eDb = 10.0 * std::log10(curE + 1e-20);
        const bool known = ownDb > -200.0;
        if (pitchedRaw && eDb > peakDb) peakDb = eDb;
        const bool ownNear = !known || peakGapDb <= 0.0 || peakDb > ownDb - peakGapDb;
        if (peakDb > -300.0) peakDb -= PEAK_FALL_DB;
        const bool ownLevel = (!known || eDb > ownDb - ownGapDb) && ownNear;
        if (pitchedRaw && ownLevel) {
            if (!known) ownDb = eDb;
            else ownDb += (eDb > ownDb ? OWN_RISE : OWN_SETTLE) * (eDb - ownDb);
        }
        if (pitchedRaw && known && !ownLevel) {
            lostDb = lostAcc <= 0.0 ? eDb : lostDb + LOST_STEP * (eDb - lostDb);
            lostAcc += eDb > ownDb - ownGapDb - LOST_NEAR_DB ? 1.0 : LOST_FAR;
            if (lostAcc >= RELEARN) { ownDb = peakDb = lostDb; lostAcc = 0.0; }
        } else if (pitchedRaw && ownLevel) lostAcc = std::max(0.0, lostAcc - 1.0);
        const bool pitched = pitchedRaw && ownLevel;
        const bool foreign = known && pitchedRaw && !pitched && !ownNear;
        if (pitched) ++voicedRun; else voicedRun = 0;
        const bool active = loud && share > SPEECH_SHARE && !clickCore;
        if (foreign) sinceForeign = 0; else if (sinceForeign < (1 << 20)) ++sinceForeign;
        if (!clickCore) { if (active && !foreign) ++activeRun; else activeRun = 0; }
        const bool tone = voicedRun >= 2;
        if (tone || !active) leadLeft = leadMax; else if (leadLeft > 0) --leadLeft;
        const bool carried = active && !clicky && eDb > ownDb - ownGapDb - CARRY_EXTRA_DB && sinceForeign > FOREIGN_BLOCK
                          && leadLeft > 0 && (sinceVoice <= LINK_GAP || activeRun >= LEAD_RUN);
        const bool voiced = strictVoice ? (tone || carried) : share > GATE_OPEN;
        if (voiced) sinceVoice = 0; else if (sinceVoice < (1 << 20)) ++sinceVoice;

        Frame& f = fifo[(fifoHead + fifoCount) % (LOOK + 1)];
        f.voiced = voiced;
        f.active = active;
        f.foreign = foreign;
        f.click = clicky;
        f.core = clickCore;
        f.hinted = hintTo > winStart && hintFrom < winEnd;
        f.pitchOwn = pitchFree && ownLevel;
        f.end = winEnd;
        std::fill(f.band.begin(), f.band.end(), 0.f);
        for (int k = 0; k < HALF; ++k) {
            f.bins[k] = spec[k];
            f.band[bandOf[k]] += std::norm(spec[k]);
            f.soft[k] = (float)gain[k];
            f.full[k] = (float)whole[k];
        }
        ++fifoCount;
        if (voiced)
            for (int i = fifoCount - 2; i >= 0; --i) {
                Frame& b = fifo[(fifoHead + i) % (LOOK + 1)];
                if (b.voiced || !(b.active || (b.click && b.pitchOwn))) break;
                b.voiced = true;
            }
    }

    void flatten(const Frame& f, bool on, bool lead) {
        if (!on) {
            std::fill(flat.begin(), flat.end(), 1.f);
            if (!f.click) std::copy(f.band.begin(), f.band.end(), calm.begin());
            return;
        }
        const Frame* next = nullptr;
        for (int i = 1; i < fifoCount && !next; ++i) {
            const Frame& g = fifo[(fifoHead + i) % (LOOK + 1)];
            if (!g.click) next = &g;
        }
        for (int b = 0; b < BANDS; ++b) {
            const float after = next ? next->band[b] : calm[b];
            const float ref = lead ? (float)LEAD_MARGIN * std::max(calm[b], after) : (float)FLAT_MARGIN * std::min(calm[b], after);
            const float p = f.band[b];
            const float g = p > ref ? std::sqrt(ref / p) : 1.f;
            bandGain[b] = g < (float)CLICK_KEEP ? (float)CLICK_KEEP : g;
        }
        for (int k = 0; k < HALF; ++k) flat[k] = bandGain[bandAt[k]] + bandW[k] * (bandGain[bandAt[k] + 1] - bandGain[bandAt[k]]);
    }

    void emit() {
        Frame& f = fifo[fifoHead];
        bool aheadVoiced = false, nearVoiced = false, soonVoiced = false;
        for (int i = 1; i < fifoCount; ++i) {
            const Frame& g = fifo[(fifoHead + i) % (LOOK + 1)];
            if (g.voiced) { aheadVoiced = true; if (i <= SOON) nearVoiced = true; if (i <= CLEAR_AHEAD) soonVoiced = true; }
        }
        const bool lone = f.click && !f.voiced && !f.pitchOwn && !soonVoiced && sinceSpoken > CLEAR_AFTER && (f.hinted || f.core);
        if (lone) hang = 0;
        bool open;
        if (f.voiced) { hang = holdFrames; open = true; }
        else if (aheadVoiced) open = true;
        else if (hang > 0 && !f.foreign) { --hang; open = true; }
        else open = false;
        if (!gateOn) open = true;
        const double target = open ? 1.0 : gateDepth;
        gate += (target - gate) * (target > gate ? gateOpenStep : gateCloseStep);

        const double duck = lone ? (suppressOn ? std::min(gateDepth, gFloor) : gateDepth) : 1.0;
        const bool spoken = f.voiced || nearVoiced || sinceSpoken <= PROTECT_AFTER;
        if (f.voiced) sinceSpoken = 0; else if (sinceSpoken < (1 << 20)) ++sinceSpoken;
        const std::vector<float>& shape = spoken ? f.full : f.soft;
        const float gg = (float)(gate * duck);
        flatten(f, f.click && f.hinted && !f.voiced && !lone, nearVoiced);
        for (int k = 0; k < HALF; ++k) {
            spec[k] = f.bins[k] * (shape[k] * gg * flat[k]);
            if (k > 0 && k < N / 2) spec[N - k] = std::conj(spec[k]);
        }
        fifoHead = (fifoHead + 1) % (LOOK + 1);
        --fifoCount;

        fft.inverse(spec.data());
        for (int i = 0; i < N; ++i) ola[i] += spec[i].real() * win[i] * (float)olaNorm;
        for (int i = 0; i < HOP; ++i) outQ.push_back(ola[i]);
        for (int i = 0; i < N - HOP; ++i) ola[i] = ola[i + HOP];
        for (int i = N - HOP; i < N; ++i) ola[i] = 0.f;
    }

    void frame() {
        analyse();
        if (fifoCount > LOOK) emit();
        else for (int i = 0; i < HOP; ++i) outQ.push_back(0.f);
    }

    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            inBuf[N - HOP + hopFill] = x[i];
            watchClicks(x[i]);
            ++seen;
            ++hopFill;
            if (hopFill == HOP) {
                frame();
                for (int j = 0; j < N - HOP; ++j) inBuf[j] = inBuf[j + HOP];
                hopFill = 0;
            }
            if (outHead < outQ.size()) { x[i] = outQ[outHead++]; }
            else x[i] = 0.f;
            if (outHead >= 4096) { outQ.erase(outQ.begin(), outQ.begin() + outHead); outHead = 0; }
        }
    }
};

}
