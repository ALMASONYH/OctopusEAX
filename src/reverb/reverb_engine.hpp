#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "reverb_params.hpp"
#include "biquad.hpp"
#include "delay_bank.hpp"

namespace eax {

class EAXReverb {
public:
    EAXReverb(double fs, const Params& p) : fs_(fs), p_(p) { setup(); }

    void process(const float* in, float* outL, float* outR, int n) {
        int i = 0;
        while (i < n) {
            int m = n - i;
            if (m > maxChunk_) m = maxChunk_;
            chunk(in + i, outL + i, outR + i, m, nullptr);
            i += m;
        }
    }

    void processStereo(const float* inL, const float* inR, float* outL, float* outR, int n) {
        int i = 0;
        while (i < n) {
            int m = n - i;
            if (m > maxChunk_) m = maxChunk_;
            chunk(inL + i, outL + i, outR + i, m, inR + i);
            i += m;
        }
    }

    double decayTime() const { return p_.decayTime; }

    void setCascade(double f) { cascade_ = f < 0.0 ? 0.0 : f; }
    void setCentre(double g) { centre_ = g < 0.0 ? 0.0 : g; }

private:
    double fs_;
    Params p_;
    double dmult_, mfDecay_, lfDecay_, hfDecay_;
    double earlyApCoeff_, earlyCoeff_, densityGain_, mixX_, mixY_;
    double modStep_, modDepth_;
    double lph_[4];
    double gain_, earlyGain_, lateGain_;
    double lateApCoeff_ = 0.0;
    static const int MAX_TABLE = 192;
    int tabN_ = 0, tabOff_[MAX_TABLE], tabMask_ = 0;
    double tabL_[MAX_TABLE], tabR_[MAX_TABLE];
    int feedN_ = 0, feedOff_[MAX_TABLE];
    double feedGain_[MAX_TABLE];
    std::vector<double> tabHeard_, tabFeed_, tabBuf2_, cascFeed_, centreOut_;
    double cascade_ = 0.0, centre_ = 1.0, lateMid_ = 1.0;
    Biquad lateMidShelf_;
    bool lateMidShelfOn_ = false;
    Biquad tabHF_, tabLF_, tabHF2_, tabLF2_, tabEq_, tabEq2_;
    bool tabEqOn_ = false;
    int nLateEq_ = 0, nWide_ = 0;
    Biquad lateEqL_[4], lateEqR_[4], wideM_[2], wideS_[2], t60Bell_[4];
    bool t60BellOn_ = false;
    int earlyTaps_[4], earlyApOff_[4], earlyOff_[4], lateOff_[4],
        lateApOff_[4], lateInTaps_[4];
    double t60Mid_[4];
    Biquad masterHF_[4], masterLF_[4], t60HF_[4], t60LF_[4];
    Line4 mainDelay_, earlyAp_, earlyDelay_, lateIn_, lateAp_, lateDelay_,
          echoLine_, firstPassAp_;
    bool washOn_ = false;
    struct Allpass {
        std::vector<double> b; int pos = 0;
        void init(int n) { b.assign((size_t)(n < 2 ? 2 : n), 0.0); pos = 0; }
        inline double run(double x, double g) {
            const double d = b[(size_t)pos], y = d - g * x;
            b[(size_t)pos] = x + g * y;
            if (++pos >= (int)b.size()) pos = 0;
            return y;
        }
    };
    Allpass washAp_[4][3];
    double washMid_ = 0.0, washSide_ = 0.0;
    double echoDepth_, echoCoeff_, echoDensityGain_;
    int echoOff_;
    Line4 shortLine_[2];
    double seDepth_[2], seCoeff_[2], seGain_[2];
    int seOff_[2];
    int maxChunk_;
    long long offset_;
    std::vector<double> tmp_[4], early_[4], late_[4], mod_[4], firstPass_[4];

    static double clampd(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void setup() {
        dmult_ = calcDelayLengthMult(p_.density);
        double hfRatio = p_.decayHFRatio;
        if (p_.decayHFLimit && p_.airAbsorptionGainHF < 1.0) {
            double limit = 1.0 / SPEED_OF_SOUND /
                calcDecayLength(p_.airAbsorptionGainHF, p_.decayTime);
            if (limit < hfRatio) hfRatio = limit;
        }
        mfDecay_ = p_.decayTime;
        lfDecay_ = clampd(p_.decayTime * p_.decayLFRatio, MIN_DECAY, MAX_DECAY);
        hfDecay_ = clampd(p_.decayTime * hfRatio, MIN_DECAY, MAX_DECAY);

        const double hf0 = std::min(p_.hfReference / fs_, 0.49);
        const double lf0 = std::min(p_.lfReference / fs_, 0.49);
        const double hfT = p_.t60HFReference > 0.0 ? std::min(p_.t60HFReference / fs_, 0.49) : hf0;

        for (int j = 0; j < NUM_LINES; ++j) {
            masterHF_[j].setShelfAtRef(true, hf0, p_.gainHF);
            masterLF_[j].setShelfAtRef(false, lf0, p_.gainLF);
        }
        tabHF_.setShelfAtRef(true, hf0, p_.gainHF);
        tabLF_.setShelfAtRef(false, lf0, p_.gainLF);
        tabHF2_.setShelfAtRef(true, hf0, p_.gainHF);
        tabLF2_.setShelfAtRef(false, lf0, p_.gainLF);
        tabEqOn_ = p_.reflEq[0] > 0.0 && p_.reflEq[1] != 0.0;
        if (tabEqOn_) {
            tabEq_.setPeak(p_.reflEq[0] / fs_, p_.reflEq[1], p_.reflEq[2]);
            tabEq2_.setPeak(p_.reflEq[0] / fs_, p_.reflEq[1], p_.reflEq[2]);
        }

        int maxTap = 0;
        for (int j = 0; j < NUM_LINES; ++j) {
            earlyTaps_[j] = (int)std::floor(
                (EARLY_TAP_LENGTHS[j] * p_.earlyTapScale * dmult_ + p_.reflectionsDelay) * fs_ + 0.5);
            if (earlyTaps_[j] > maxTap) maxTap = earlyTaps_[j];
        }
        mainDelay_.init(maxTap);

        earlyApCoeff_ = p_.diffusion * p_.diffusion * INV_SQRT2;
        lateApCoeff_ = earlyApCoeff_;
        int m1 = 0, m2 = 0;
        for (int j = 0; j < NUM_LINES; ++j) {
            earlyApOff_[j] = (int)std::floor(EARLY_ALLPASS_LENGTHS[j] * dmult_ * fs_ + 0.5);
            earlyOff_[j] = (int)std::floor(EARLY_LINE_LENGTHS[j] * dmult_ * fs_ + 0.5);
            if (earlyApOff_[j] > m1) m1 = earlyApOff_[j];
            if (earlyOff_[j] > m2) m2 = earlyOff_[j];
        }
        earlyAp_.init(m1);
        earlyDelay_.init(m2);
        double avgEarly = 0;
        for (int j = 0; j < NUM_LINES; ++j) avgEarly += EARLY_LINE_LENGTHS[j];
        avgEarly = avgEarly / NUM_LINES * dmult_;
        earlyCoeff_ = calcDecayCoeff(avgEarly, p_.earlyDecay > 0.0 ? p_.earlyDecay : p_.decayTime);

        calcMatrixCoeffs(p_.diffusion, mixX_, mixY_);

        double modTime = p_.modulationTime > 1e-4 ? p_.modulationTime : 1e-4;
        modStep_ = 1.0 / (fs_ * modTime);
        double baseTime = (modTime >= DEFAULT_MOD_TIME) ? DEFAULT_MOD_TIME : modTime;

        modDepth_ = MOD_DEPTH_COEFF / 4.0 * baseTime * p_.modulationDepth * fs_;
        for (int j = 0; j < NUM_LINES; ++j) lph_[j] = p_.linePhase[j];

        double lengths[4];
        int m3 = 0, m4 = 0, m5 = 0;
        for (int j = 0; j < NUM_LINES; ++j) {
            lengths[j] = LATE_LINE_LENGTHS[j] * p_.lineScale[j] * dmult_;
            int o = (int)std::floor(lengths[j] * fs_ + 0.5 + 0.5) - 1;
            lateOff_[j] = o < 0 ? 0 : o;

            double lt = (lengths[j] - lengths[0]) / NUM_LINES + p_.lateReverbDelay;
            lateInTaps_[j] = (int)std::floor(lt * fs_ + 0.5);
            lateApOff_[j] = (int)std::floor(LATE_ALLPASS_LENGTHS[j] * dmult_ * fs_ + 0.5);
            if (lateOff_[j] > m3) m3 = lateOff_[j];
            if (lateInTaps_[j] > m4) m4 = lateInTaps_[j];
            if (lateApOff_[j] > m5) m5 = lateApOff_[j];
        }
        lateAp_.init(m5);
        washOn_ = p_.lateFirstWash[1] >= 0.0;
        washMid_ = p_.lateFirstWash[0];
        washSide_ = p_.lateFirstWash[1];
        if (washOn_) {
            firstPassAp_.init(m5);
            for (int j = 0; j < NUM_LINES; ++j)
                for (int k = 0; k < 3; ++k) washAp_[j][k].init((int)(WASH_ALLPASS_MS[k] * (1.0 + WASH_LINE_SPREAD * j) * 0.001 * fs_ + 0.5));
        }
        lateIn_.init(m4);

        double lateAllpassAvg = 0, lateDelayAvg = 0;
        for (int j = 0; j < NUM_LINES; ++j) {
            lateAllpassAvg += LATE_ALLPASS_LENGTHS[j];
            lateDelayAvg += LATE_LINE_LENGTHS[j] * p_.lineScale[j];
        }
        lateAllpassAvg /= NUM_LINES;
        lateDelayAvg = lateDelayAvg / NUM_LINES + lateAllpassAvg;

        double nw = fs_ / 20000.0;
        double decayW = lf0 * nw * lfDecay_ + (hf0 - lf0) * nw * mfDecay_
                        + (1.0 - hf0 * nw) * hfDecay_;
        densityGain_ = calcDensityGain(calcDecayCoeff(lateDelayAvg * dmult_, decayW));

        t60BellOn_ = p_.t60Bell[0] > 0.0 && p_.t60Bell[1] != 0.0;
        for (int j = 0; j < NUM_LINES; ++j) {
            const double len = (LATE_ALLPASS_LENGTHS[j]
                                + (lateAllpassAvg - LATE_ALLPASS_LENGTHS[j]) * p_.diffusion)
                               * dmult_ + modDepth_ / fs_ + lengths[j];
            const double m = calcDecayCoeff(len, mfDecay_);
            t60Mid_[j] = m;
            t60HF_[j].setShelf(true, hfT, calcDecayCoeff(len, hfDecay_) / m);
            t60LF_[j].setShelf(false, lf0, calcDecayCoeff(len, lfDecay_) / m);
            if (t60BellOn_) t60Bell_[j].setPeak(p_.t60Bell[0] / fs_, p_.t60Bell[1] * len, p_.t60Bell[2]);
        }
        int maxMod = (int)std::ceil(modDepth_ * 2.0) + 8;
        lateDelay_.init(m3 + maxMod);

        echoDepth_ = p_.echoDepth;
        echoOff_ = 0;
        if (echoDepth_ > 0.0) {
            double et = p_.echoTime > 0.03 ? p_.echoTime : 0.03;
            echoOff_ = (int)std::floor(et * fs_ + 0.5);
            if (echoOff_ < 16) echoOff_ = 16;
            echoCoeff_ = calcDecayCoeff(et, p_.decayTime);
            echoDensityGain_ = calcDensityGain(echoCoeff_);
            echoLine_.init(echoOff_);
        }
        for (int k = 0; k < 2; ++k) {
            seDepth_[k] = p_.shortEchoDepth[k];
            seOff_[k] = 0;
            seCoeff_[k] = 0.0;
            seGain_[k] = 1.0;
            if (seDepth_[k] > 0.0) {
                double t = p_.shortEchoTime[k] > 0.002 ? p_.shortEchoTime[k] : 0.002;
                seOff_[k] = (int)std::floor(t * fs_ + 0.5);
                if (seOff_[k] < 16) seOff_[k] = 16;
                double g = clampd(p_.shortEchoFeedback[k], 0.0, 0.98);
                seCoeff_[k] = g;
                seGain_[k] = calcDensityGain(g);
                shortLine_[k].init(seOff_[k]);
            }
        }

        gain_ = p_.gain;
        earlyGain_ = p_.reflectionsGain;
        lateGain_ = p_.lateReverbGain;
        tabN_ = 0;
        feedN_ = 0;
        if (p_.reflTable && p_.reflTableN > 0) {
            tabN_ = p_.reflTableN > MAX_TABLE ? MAX_TABLE : p_.reflTableN;
            int longest = 0;
            for (int k = 0; k < tabN_; ++k) {
                tabOff_[k] = (int)std::floor(p_.reflTable[k][0] * 0.001 * fs_ + 0.5) - p_.reflTableLag;
                tabL_[k] = p_.reflTable[k][1] * p_.reflTableGain;
                tabR_[k] = p_.reflTable[k][2] * p_.reflTableGain;
                if (tabOff_[k] > longest) longest = tabOff_[k];
            }
            if (p_.feedTable) feedN_ = p_.feedTableN > MAX_TABLE ? MAX_TABLE : p_.feedTableN;
            for (int k = 0; k < feedN_; ++k) {
                feedOff_[k] = (int)std::floor(p_.feedTable[k][0] * 0.001 * fs_ + 0.5) - p_.reflTableLag;
                feedGain_[k] = p_.feedTable[k][1] * p_.reflTableGain;
                if (feedOff_[k] > longest) longest = feedOff_[k];
            }
            int size = 1; while (size < longest + 264) size <<= 1;
            tabHeard_.assign((size_t)size, 0.0); tabFeed_.assign((size_t)size, 0.0); tabBuf2_.assign((size_t)size, 0.0); tabMask_ = size - 1;
        }
        nLateEq_ = 0;
        for (int k = 0; k < 4; ++k) {
            if (p_.lateEq[k][0] <= 0.0 || p_.lateEq[k][1] == 0.0) continue;
            lateEqL_[nLateEq_].setPeak(p_.lateEq[k][0] / fs_, p_.lateEq[k][1], p_.lateEq[k][2]);
            lateEqR_[nLateEq_].setPeak(p_.lateEq[k][0] / fs_, p_.lateEq[k][1], p_.lateEq[k][2]);
            ++nLateEq_;
        }
        lateMid_ = p_.lateMid[0];
        lateMidShelfOn_ = p_.lateMid[1] > 0.0 && p_.lateMid[2] != 0.0;
        if (lateMidShelfOn_) lateMidShelf_.setShelf(true, p_.lateMid[1] / fs_, std::pow(10.0, p_.lateMid[2] / 20.0));
        nWide_ = 0;
        for (int k = 0; k < 2; ++k) {
            if (p_.lateWide[k][0] <= 0.0 || p_.lateWide[k][1] == 0.0) continue;
            wideM_[nWide_].setPeak(p_.lateWide[k][0] / fs_, -0.5 * p_.lateWide[k][1], p_.lateWide[k][2]);
            wideS_[nWide_].setPeak(p_.lateWide[k][0] / fs_, 0.5 * p_.lateWide[k][1], p_.lateWide[k][2]);
            ++nWide_;
        }

        int lim = 256;
        for (int j = 0; j < NUM_LINES; ++j) {
            if (earlyApOff_[j] < lim) lim = earlyApOff_[j];
            if (lateApOff_[j] < lim) lim = lateApOff_[j];
            if (earlyOff_[j] < lim) lim = earlyOff_[j];
            int lo = (int)(lateOff_[j] - 2 * modDepth_ - 6);
            if (lo < lim) lim = lo;
        }
        if (echoDepth_ > 0.0 && echoOff_ < lim) lim = echoOff_;
        maxChunk_ = lim < 16 ? 16 : lim;
        offset_ = 0;
        cascFeed_.assign((size_t)maxChunk_, 0.0);
        centreOut_.assign((size_t)maxChunk_, 0.0);
        for (int j = 0; j < NUM_LINES; ++j) {
            tmp_[j].resize(maxChunk_);
            early_[j].resize(maxChunk_);
            late_[j].resize(maxChunk_);
            firstPass_[j].resize(maxChunk_);
            mod_[j].resize(maxChunk_);
        }
    }

    inline void scatter(double v[4]) const {
        double a = v[0], b = v[1], c = v[2], d = v[3];
        v[0] = mixX_ * a + mixY_ * (b - c + d);
        v[1] = mixX_ * b + mixY_ * (-a + c + d);
        v[2] = mixX_ * c + mixY_ * (a - b + d);
        v[3] = mixX_ * d + mixY_ * (-a - b - c);
    }

    void chunk(const float* x, float* outL, float* outR, int n, const float* x2) {
        long long off = offset_;

        if (tabN_ > 0) {
            const double g = earlyGain_;
            for (int i = 0; i < n; ++i) {
                const double in = (x2 ? 0.5 * ((double)x[i] + (double)x2[i]) : (double)x[i]) * gain_;
                double s = tabHF_.process(in);
                s = tabLF_.process(s);
                if (tabEqOn_) s = tabEq_.process(s);
                tabHeard_[(size_t)((off + i) & tabMask_)] = s * centre_;
                tabFeed_[(size_t)((off + i) & tabMask_)] = s * cascade_;
                double fed = 0.0, c = 0.0;
                for (int k = 0; k < feedN_; ++k) fed += tabFeed_[(size_t)((off + i - feedOff_[k]) & tabMask_)] * feedGain_[k];
                for (int k = 0; k < tabN_; ++k) {
                    if (tabL_[k] != tabR_[k]) continue;
                    c += tabHeard_[(size_t)((off + i - tabOff_[k]) & tabMask_)] * tabL_[k];
                    fed += tabFeed_[(size_t)((off + i - tabOff_[k]) & tabMask_)] * tabL_[k];
                }
                centreOut_[i] = c * g;
                const double back = fed * g;
                cascFeed_[i] = gain_ > 1e-9 ? back / gain_ : 0.0;
                double u = tabHF2_.process(back);
                u = tabLF2_.process(u);
                if (tabEqOn_) u = tabEq2_.process(u);
                tabBuf2_[(size_t)((off + i) & tabMask_)] = s + u;
            }
        }

        for (int j = 0; j < NUM_LINES; ++j) {
            double* mb = mainDelay_.ch(j);
            const float* src = (x2 && (j & 1)) ? x2 : x;
            for (int i = 0; i < n; ++i)
                mb[(int)((off + i) & mainDelay_.mask)] = 0.5 * ((double)src[i] + (tabN_ > 0 ? cascFeed_[i] : 0.0));
        }

        for (int j = 0; j < NUM_LINES; ++j) {
            const double* mb = mainDelay_.ch(j);
            double* ab = earlyAp_.ch(j);
            for (int i = 0; i < n; ++i) {
                double v = mb[(int)((off + i - earlyTaps_[j]) & mainDelay_.mask)] * gain_;
                double d = ab[(int)((off + i - earlyApOff_[j]) & earlyAp_.mask)];
                double y = d - earlyApCoeff_ * v;
                ab[(int)((off + i) & earlyAp_.mask)] = v + earlyApCoeff_ * y;
                y = masterHF_[j].process(y);
                y = masterLF_[j].process(y);
                tmp_[j][i] = y;
            }
        }

        for (int i = 0; i < n; ++i) {
            double s = tmp_[0][i] + tmp_[1][i] + tmp_[2][i] + tmp_[3][i];
            for (int j = 0; j < NUM_LINES; ++j)
                earlyDelay_.ch(j)[(int)((off + i) & earlyDelay_.mask)] = tmp_[j][i] - 0.5 * s;
        }
        for (int j = 0; j < NUM_LINES; ++j) {
            const double* eb = earlyDelay_.ch(j);
            for (int i = 0; i < n; ++i) {
                const double d = eb[(int)((off + i - earlyOff_[j]) & earlyDelay_.mask)] * earlyCoeff_;
                early_[j][i] = d + tmp_[j][i];
            }
        }

        for (int i = 0; i < n; ++i) {
            double v[4] = {early_[0][i], early_[1][i], early_[2][i], early_[3][i]};
            scatter(v);
            for (int j = 0; j < NUM_LINES; ++j)
                lateIn_.ch(j)[(int)((off + i) & lateIn_.mask)] = v[j];
        }

        for (int j = 0; j < NUM_LINES; ++j) {
            for (int i = 0; i < n; ++i) {
                double ph = lph_[j] + i * modStep_;
                ph -= std::floor(ph);
                const double lfo = (ph < 0.5) ? (-16.0 * ph * ph + 8.0 * ph)
                                              : (16.0 * ph * ph - 24.0 * ph + 8.0);
                mod_[j][i] = (lfo + 1.0) * modDepth_;
            }
        }
        for (int j = 0; j < NUM_LINES; ++j) {
            lph_[j] += n * modStep_;
            lph_[j] -= std::floor(lph_[j]);
        }

        for (int j = 0; j < NUM_LINES; ++j) {
            const double* lb = lateDelay_.ch(j);
            const double* ib = lateIn_.ch(j);
            int msk = lateDelay_.mask;
            for (int i = 0; i < n; ++i) {
                double D = lateOff_[j] + mod_[j][i];
                long long k = (long long)std::floor(D);
                double frac = D - (double)k;
                long long pos = off + i - k;

                double s[6];
                s[0] = lb[(int)((pos + 2) & msk)];
                s[1] = lb[(int)((pos + 1) & msk)];
                s[2] = lb[(int)(pos & msk)];
                s[3] = lb[(int)((pos - 1) & msk)];
                s[4] = lb[(int)((pos - 2) & msk)];
                s[5] = lb[(int)((pos - 3) & msk)];
                const double u = frac;
                const double p2 = u + 2.0, p1 = u + 1.0;
                const double m1 = u - 1.0, m2 = u - 2.0, m3 = u - 3.0;
                const double y = s[0] * (-(p1 * u * m1 * m2 * m3) * (1.0 / 120.0))
                               + s[1] * ((p2 * u * m1 * m2 * m3) * (1.0 / 24.0))
                               + s[2] * (-(p2 * p1 * m1 * m2 * m3) * (1.0 / 12.0))
                               + s[3] * ((p2 * p1 * u * m2 * m3) * (1.0 / 12.0))
                               + s[4] * (-(p2 * p1 * u * m1 * m3) * (1.0 / 24.0))
                               + s[5] * ((p2 * p1 * u * m1 * m2) * (1.0 / 120.0));
                double v = y;
                v *= t60Mid_[j];
                v = t60HF_[j].process(v);
                v = t60LF_[j].process(v);
                if (t60BellOn_) v = t60Bell_[j].process(v);
                const double inj = ib[(int)((off + i - lateInTaps_[j]) & lateIn_.mask)] * densityGain_;
                firstPass_[j][i] = inj;
                late_[j][i] = v + inj;
            }
        }

        for (int i = 0; i < n; ++i) {
            double stored[4];
            for (int j = 0; j < NUM_LINES; ++j) {
                double* ab = lateAp_.ch(j);
                double d = ab[(int)((off + i - lateApOff_[j]) & lateAp_.mask)];
                double y = d - lateApCoeff_ * late_[j][i];
                stored[j] = late_[j][i] + lateApCoeff_ * y;
                late_[j][i] = y;
            }
            scatter(stored);
            for (int j = 0; j < NUM_LINES; ++j)
                lateAp_.ch(j)[(int)((off + i) & lateAp_.mask)] = stored[j];
        }

        for (int i = 0; i < n; ++i) {
            double v[4] = {late_[3][i], late_[2][i], late_[1][i], late_[0][i]};
            scatter(v);
            for (int j = 0; j < NUM_LINES; ++j)
                lateDelay_.ch(j)[(int)((off + i) & lateDelay_.mask)] = v[j];
        }

        if (washOn_) {
            for (int i = 0; i < n; ++i) {
                double stored[4], w[4], sum = 0.0;
                for (int j = 0; j < NUM_LINES; ++j) {
                    const double d = firstPassAp_.ch(j)[(int)((off + i - lateApOff_[j]) & firstPassAp_.mask)];
                    const double y = d - lateApCoeff_ * firstPass_[j][i];
                    stored[j] = firstPass_[j][i] + lateApCoeff_ * y;
                    w[j] = y;
                    for (int k = 0; k < 3; ++k) w[j] = washAp_[j][k].run(w[j], WASH_ALLPASS_COEFF);
                    sum += w[j];
                    late_[j][i] -= y;
                }
                const double common = (washMid_ - washSide_) * 0.25 * sum;
                for (int j = 0; j < NUM_LINES; ++j) late_[j][i] += washSide_ * w[j] + common;
                scatter(stored);
                for (int j = 0; j < NUM_LINES; ++j)
                    firstPassAp_.ch(j)[(int)((off + i) & firstPassAp_.mask)] = stored[j];
            }
        }

        if (echoDepth_ > 0.0) {
            for (int j = 0; j < NUM_LINES; ++j) {
                double* eb = echoLine_.ch(j);
                for (int i = 0; i < n; ++i) {
                    double d = eb[(int)((off + i - echoOff_) & echoLine_.mask)]
                               * echoCoeff_;
                    eb[(int)((off + i) & echoLine_.mask)] =
                        d + late_[j][i] * echoDensityGain_;
                    late_[j][i] = late_[j][i] * (1.0 - echoDepth_) + d * echoDepth_;
                }
            }
        }
        for (int k = 0; k < 2; ++k) {
            if (seDepth_[k] <= 0.0) continue;
            const double dp = seDepth_[k], g = seCoeff_[k], dg = seGain_[k];
            const int so = seOff_[k], mk = shortLine_[k].mask;
            for (int j = 0; j < NUM_LINES; ++j) {
                double* eb = shortLine_[k].ch(j);
                for (int i = 0; i < n; ++i) {
                    double d = eb[(int)((off + i - so) & mk)] * g;
                    eb[(int)((off + i) & mk)] = d + late_[j][i] * dg;
                    late_[j][i] = late_[j][i] * (1.0 - dp) + d * dp;
                }
            }
        }

        const double eg = tabN_ > 0 ? 0.0 : earlyGain_;
        for (int i = 0; i < n; ++i) {
            double e0 = early_[0][i] * eg, e1 = early_[1][i] * eg;
            double e2 = early_[2][i] * eg, e3 = early_[3][i] * eg;
            double l0 = late_[0][i] * lateGain_, l1 = late_[1][i] * lateGain_;
            double l2 = late_[2][i] * lateGain_, l3 = late_[3][i] * lateGain_;
            if (nLateEq_ > 0) {
                double lL = (0.5 * (l0 + l1 + l2 + l3) + 0.5 * (l0 - l1 + l2 - l3)) * INV_SQRT2;
                double lR = (0.5 * (l0 + l1 + l2 + l3) - 0.5 * (l0 - l1 + l2 - l3)) * INV_SQRT2;
                for (int k = 0; k < nLateEq_; ++k) { lL = lateEqL_[k].process(lL); lR = lateEqR_[k].process(lR); }
                if (nWide_ > 0 || lateMid_ != 1.0) {
                    double m = 0.5 * (lL + lR), s = 0.5 * (lL - lR);
                    for (int k = 0; k < nWide_; ++k) { m = wideM_[k].process(m); s = wideS_[k].process(s); }
                    m *= lateMid_;
                    if (lateMidShelfOn_) m = lateMidShelf_.process(m);
                    lL = m + s; lR = m - s;
                }
                const double ew = 0.5 * (e0 + e1 + e2 + e3), ey = 0.5 * (e0 - e1 + e2 - e3);
                outL[i] = (float)((ew + ey) * INV_SQRT2 + lL);
                outR[i] = (float)((ew - ey) * INV_SQRT2 + lR);
            } else {
                double w = 0.5 * (e0 + e1 + e2 + e3) + 0.5 * (l0 + l1 + l2 + l3);
                double yy = 0.5 * (e0 - e1 + e2 - e3) + 0.5 * (l0 - l1 + l2 - l3);
                outL[i] = (float)((w + yy) * INV_SQRT2);
                outR[i] = (float)((w - yy) * INV_SQRT2);
            }
            if (tabN_ > 0) {
                double tl = 0.0, tr = 0.0;
                for (int k = 0; k < tabN_; ++k) {
                    if (tabL_[k] == tabR_[k]) continue;
                    const double v = tabBuf2_[(size_t)((off + i - tabOff_[k]) & tabMask_)];
                    tl += v * tabL_[k]; tr += v * tabR_[k];
                }
                outL[i] += (float)(tl * earlyGain_ + centreOut_[i]);
                outR[i] += (float)(tr * earlyGain_ + centreOut_[i]);
            }
        }
        offset_ = off + n;
    }
};

}
