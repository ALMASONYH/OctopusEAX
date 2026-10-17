#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "delay_bank.hpp"

namespace eax {

struct CoreRoom {
    double roomDb, roomHfDb, decayTime, decayHfRatio;
    double reflectionsDb, reflectionsDelay, reverbDb, reverbDelay;
    double size, diffusion, airAbsorptionDb;
    double echoDepth, echoTime, modulationDepth, modulationTime;
    double hfReference;
    bool decayHfLimit;
};

class CardCore {
public:
    CardCore(double fs, const CoreRoom& room) : fs_(fs), room_(room) { setup(); }

    void clear() {
        for (Ring* r : {&input_, &spread_, &allpassL_, &allpassR_, &taps_, &wobble_}) r->clear();
        for (Ring& r : late_) r.clear();
        hf1_ = hf2_ = dcIn_ = dcOut_ = 0.0;
        lfoSin_ = 0.0; lfoCos_ = 1.0;
        wobbleInt_ = (int)wobbleCentre_; wobbleFrac_ = 0.0;
        for (int k = 0; k < LATE; ++k) damped_[k] = 0.0;
        t_ = 0;
    }

    void process(const float* in, float* out, int n) {
        for (int i = 0; i < n; ++i) {
            hf1_ = in[i] * roomGain_ * (1.0 - hfPole_) + hf1_ * hfPole_;
            hf2_ = hf1_ * (1.0 - hfPole_) + hf2_ * hfPole_;
            dcOut_ = DC_POLE * dcOut_ + hf2_ * DC_GAIN + dcIn_;
            dcIn_ = -(hf2_ * DC_GAIN);
            input_.write(t_, dcOut_ * INPUT_SCALE);

            const double early = input_.read(t_, nReflections_) * reflectionsGain_ * 4.0;
            spread_.write(t_, early);
            const double inL = early * PAN, inR = spread_.read(t_, nSpread_) * PAN;
            double left = allpassL_.read(t_, nAllpassL_) + inL * ALLPASS;
            allpassL_.write(t_, inL - left * ALLPASS);
            double right = allpassR_.read(t_, nAllpassR_) + inR * ALLPASS;
            allpassR_.write(t_, inR - right * ALLPASS);

            const double feed = input_.read(t_, nReverb_) * reverbGain_;
            taps_.write(t_, feed);
            lfoSin_ += lfoCos_ * lfoStep_;
            lfoCos_ -= lfoSin_ * lfoStep_;

            const double inject[LATE] = {
                taps_.read(t_, nTap_[4]), taps_.read(t_, nTap_[0]) * tapGain_,
                taps_.read(t_, nTap_[5]), taps_.read(t_, nTap_[1]) * tapGain_,
                taps_.read(t_, nTap_[6]), taps_.read(t_, nTap_[2]) * tapGain_,
                feed,                     taps_.read(t_, nTap_[3]) * tapGain_};
            for (int k = 0; k < LATE; ++k) {
                double x;
                if (k == WOBBLED) {
                    wobble_.write(t_, late_[k].read(t_, nLate_[k]) * lateGain_[k]);
                    x = wobble_.read(t_, wobbleInt_ + 1) * wobbleFrac_ + wobble_.read(t_, wobbleInt_) * (1.0 - wobbleFrac_);
                } else x = late_[k].read(t_, nLate_[k]) * lateGain_[k];
                damped_[k] = (inject[k] + x) * (1.0 - damping_[k]) + damping_[k] * damped_[k];
            }

            const double* d = damped_;
            const double r = rotation_;
            const double a = (d[3] * r + d[2]) * r + (d[1] * r + d[0]);
            const double b = (d[3] * r + d[2]) - (d[1] * r + d[0]) * r;
            const double c = (d[7] * r + d[6]) * r + (d[5] * r + d[4]);
            const double e = (d[7] * r + d[6]) - (d[5] * r + d[4]) * r;
            const double f = (d[3] - d[2] * r) * r + (d[1] - d[0] * r);
            const double g = (d[3] - d[2] * r) - (d[1] - d[0] * r) * r;
            const double h = (d[7] - d[6] * r) * r + (d[5] - d[4] * r);
            const double j = (d[7] - d[6] * r) - (d[5] - d[4] * r) * r;
            const double frontL = c * r + a, frontR = e * r + b;
            late_[1].write(t_, frontL);
            late_[5].write(t_, c - a * r);
            late_[3].write(t_, frontR);
            late_[7].write(t_, e - b * r);
            late_[2].write(t_, h * r + f);
            late_[6].write(t_, h - f * r);
            late_[4].write(t_, j * r + g);
            late_[0].write(t_, j - g * r);
            left += frontL * PAN;
            right += frontR * PAN;

            const double at = lfoSin_ * wobbleDepth_ + wobbleCentre_;
            wobbleInt_ = (int)at;
            wobbleFrac_ = at - wobbleInt_;
            out[i] = (float)(left + right);
            ++t_;
        }
    }

private:
    static const int LATE = 8, WOBBLED = 6;
    static constexpr double ALLPASS = 0.4, PAN = 0.6665, INPUT_SCALE = 1.5;
    static constexpr double DC_POLE = 0.9987000227, DC_GAIN = (DC_POLE + 1.0) * 0.5;
    static const int ALLPASS_SHORT = 1, TAP_SHORT = 1, LATE_SHORT = 1;

    struct Ring {
        std::vector<float> buf;
        unsigned mask = 0;
        void init(int maxDelay) { const int size = nextPow2(maxDelay + 4); buf.assign((size_t)size, 0.f); mask = (unsigned)size - 1u; }
        void clear() { std::fill(buf.begin(), buf.end(), 0.f); }
        inline void write(unsigned t, double v) { buf[t & mask] = (float)v; }
        inline double read(unsigned t, int delay) const { return buf[(t - (unsigned)delay) & mask]; }
    };

    int samples(double seconds) const { return (int)((float)seconds * (float)fs_); }

    void setup() {
        const CoreRoom& p = room_;
        nReflections_ = samples(p.reflectionsDelay);
        nReverb_ = samples(p.reflectionsDelay + p.reverbDelay);
        const float spread = (float)((p.reverbDelay + 0.0099999998f) * 9.090909f * 0.024f);
        nSpread_ = samples(spread * 0.723f);
        const float y = (float)(p.reverbDelay * 10.0f);
        nAllpassL_ = samples(0.010470001f + 0.017940002f * y) - ALLPASS_SHORT;
        nAllpassR_ = samples(0.0099100005f + 0.01682f * y) - ALLPASS_SHORT;
        {
            float a = 0.0491f;
            const float ratio = (float)std::pow(0.1853360624, 0.1428571429);
            for (int i = 0; i < 7; ++i) { nTap_[i] = samples(a - 0.0091000004f) - TAP_SHORT; a *= ratio; }
        }

        const double swing = p.modulationDepth * p.modulationTime * 4.0;
        const double lengthCap = swing <= 1.0 ? 1.0 : 1.3 * swing / (0.3 * swing + 1.0);
        wobbleCentre_ = (double)(int)(fs_ * 0.010000001f) * 0.5;
        wobbleDepth_ = (float)((wobbleCentre_ - 2.0) * (swing <= 1.0 ? swing : 1.0));
        lfoStep_ = (float)(2.0 * std::sin(3.1415927 / (p.modulationTime * fs_)));

        double lateTime[LATE];
        {
            double limit = 0.0625 * p.size * p.size * p.size;
            if (lengthCap < limit) limit = lengthCap;
            limit = limit >= 1.0 ? 1.0 : (limit <= 0.0625 ? 0.0625 : limit);
            const double u = std::pow(p.echoDepth, 0.7142857143), v = u * 0.4761904762;
            const double w = std::pow((v + 1.0) / (2.0 - v), 0.3333333333);
            const double q = 1.0 - (0.9 * std::pow(1.0 - u, 8.0) + 0.1 * (1.0 - u));
            const double mean = p.echoTime * q + 0.25 * (1.0 - q);
            const double norm = (1.0 - std::pow(w, 4.0)) / (1.0 - w) * 0.25;
            const double split[4] = {0.37, 0.28, 0.64, 0.8};
            for (int k = 0; k < 4; ++k) {
                double pairTime = std::pow(w, (double)k) * mean / norm;
                if (!(mean * 4.0 < limit * 1.05)) pairTime *= limit / (mean * 4.0);
                lateTime[2 * k] = pairTime / (1.0 + split[k]);
                lateTime[2 * k + 1] = pairTime / (1.0 + 1.0 / split[k]);
            }
        }
        static const double LATE_MAX[LATE] = {0.251, 0.093, 0.213, 0.06, 0.152, 0.0975, 0.1375, 0.11};
        for (int k = 0; k < LATE; ++k) {
            if (lateTime[k] > LATE_MAX[k]) lateTime[k] = LATE_MAX[k];
            nLate_[k] = samples(lateTime[k]) - LATE_SHORT;
        }

        roomGain_ = std::pow(10.0, p.roomDb * 0.05);
        {
            const double x = (2.7857 - p.echoTime * 7.1429) * p.echoDepth * 0.2, m = p.modulationDepth * 0.3;
            double dif = (1.0 - (x > m ? x : m)) * p.diffusion;
            dif = dif >= 1.0 ? 1.0 : (dif <= 0.0 ? 0.0 : dif);
            const double angle = (std::pow(dif, 8.0) * 0.4 + dif * 0.6) * 0.7853981853;
            rotation_ = std::tan(angle);
            spreadLoss_ = std::pow(std::cos(angle), 3.0);
        }
        tapGain_ = 1.0 - p.echoDepth;
        double hfDecay = (p.decayHfRatio < 0.1 ? 0.1 : p.decayHfRatio) * p.decayTime;
        if (p.decayHfLimit && p.airAbsorptionDb < 0.0) {
            const double limit = -60.0 / (p.airAbsorptionDb * 340.0);
            if (hfDecay > limit) hfDecay = limit;
        }
        double energy = 0.0;
        {
            double hfRatio = hfDecay / p.decayTime;
            if (hfRatio <= 0.1) hfRatio = 0.1;
            const double w = hfRatio > 1.0 ? 3.141592741 : p.hfReference / fs_ * 6.2831855;
            const double power = 2.0 - 2.0 / hfRatio, span = 1.0 - std::cos(w);
            for (int k = 0; k < LATE; ++k) {
                const double gain = std::pow(10.0, -60.0 * (float)lateTime[k] / p.decayTime * 0.05);
                energy += gain * gain;
                const double ratio = (std::pow(gain, power) - 1.0) / span;
                double pole = 0.0;
                if (ratio != 0.0) {
                    const double bq = -2.0 * ratio - 2.0, root = std::sqrt(bq * bq - 4.0 * ratio * ratio);
                    pole = (root - bq) / (2.0 * ratio);
                    if (!(std::fabs(pole) <= 1.0)) pole = (-bq - root) / (2.0 * ratio);
                }
                if (pole >= 0.98500001f) pole = 0.98500001f;
                damping_[k] = (float)pole;
                lateGain_[k] = (float)(gain * spreadLoss_);
            }
        }
        const double tapSpread = std::sqrt((1.0 + tapGain_ * tapGain_) * 0.5);
        const double level = std::sqrt(2.0 * spreadLoss_ * spreadLoss_ / 8.0 * (1.0 - energy * 0.125)) / tapSpread;
        reverbGain_ = (float)(level * std::pow(10.0, p.reverbDb * 0.05));
        reflectionsGain_ = (float)(std::pow(10.0, p.reflectionsDb * 0.05) * 0.125);
        {
            const double g = std::pow(10.0, p.roomHfDb * 0.05);
            double pole = 0.0;
            if (g < 1.0) {
                const double cs = std::cos(p.hfReference / fs_ * 6.2831855);
                const double a = 1.0 - cs * g, b = 2.0 * g * (1.0 - cs) - (1.0 - cs * cs) * g * g;
                pole = (a - std::sqrt(b)) / (1.0 - g);
                pole = pole >= 1.0 ? 1.0 : (pole <= 0.0 ? 0.0 : pole);
            }
            hfPole_ = (float)pole;
        }

        input_.init(nReverb_);
        spread_.init(nSpread_);
        allpassL_.init(nAllpassL_);
        allpassR_.init(nAllpassR_);
        taps_.init(nTap_[0]);
        for (int k = 0; k < LATE; ++k) late_[k].init(nLate_[k]);
        wobble_.init((int)(wobbleCentre_ * 2.0) + 2);
        clear();
    }

    double fs_;
    CoreRoom room_;
    int nReflections_ = 0, nReverb_ = 0, nSpread_ = 0, nAllpassL_ = 0, nAllpassR_ = 0, nTap_[7] = {}, nLate_[LATE] = {};
    double roomGain_ = 1.0, hfPole_ = 0.0, reflectionsGain_ = 0.0, reverbGain_ = 0.0, tapGain_ = 1.0;
    double rotation_ = 0.0, spreadLoss_ = 1.0, lateGain_[LATE] = {}, damping_[LATE] = {};
    double lfoStep_ = 0.0, wobbleCentre_ = 0.0, wobbleDepth_ = 0.0;

    Ring input_, spread_, allpassL_, allpassR_, taps_, wobble_, late_[LATE];
    double hf1_ = 0.0, hf2_ = 0.0, dcIn_ = 0.0, dcOut_ = 0.0;
    double lfoSin_ = 0.0, lfoCos_ = 1.0, wobbleFrac_ = 0.0, damped_[LATE] = {};
    int wobbleInt_ = 0;
    unsigned t_ = 0;
};

}
