#pragma once
#include <cmath>

namespace eax {
static const int NUM_LINES = 4;
static const double REVERB_DECAY_GAIN = 0.001;
static const double MOD_DEPTH_COEFF = 0.05;
static const double DEFAULT_MOD_TIME = 0.25;
static const double DENSITY_SCALE = 1000.0;
static const double SPEED_OF_SOUND = 343.3;
static const double INV_SQRT2 = 0.70710678118654752440;
static const double MIN_DECAY = 0.1, MAX_DECAY = 20.0;

static const double EARLY_TAP_LENGTHS[4] =
    {0.000000e+0, 1.010676e-3, 2.126553e-3, 3.358580e-3};
static const double EARLY_ALLPASS_LENGTHS[4] =
    {4.854840e-4, 5.360178e-4, 5.918117e-4, 6.534130e-4};
static const double EARLY_LINE_LENGTHS[4] =
    {2.992520e-3, 5.456575e-3, 7.688329e-3, 9.709681e-3};

static const double LINE_PHASE[4] = {0.0, 0.5, 0.25, 0.75};
static const double LATE_ALLPASS_LENGTHS[4] =
    {8.091400e-4, 1.019453e-3, 1.407968e-3, 1.618280e-3};

static const double LATE_LINE_TRIM = 0.96;
static const double LATE_LINE_LENGTHS[4] =
    {9.709681e-3 * LATE_LINE_TRIM, 1.223343e-2 * LATE_LINE_TRIM,
     1.689561e-2 * LATE_LINE_TRIM, 1.941936e-2 * LATE_LINE_TRIM};

static const double WASH_ALLPASS_MS[3] = {3.1, 5.3, 8.9};
static const double WASH_ALLPASS_COEFF = 0.7;
static const double WASH_LINE_SPREAD = 0.11;

struct Params {
    double density, diffusion, gain, gainHF, gainLF;
    double decayTime, decayHFRatio, decayLFRatio;
    double reflectionsGain, reflectionsDelay;
    double lateReverbGain, lateReverbDelay;
    double echoTime, echoDepth;
    double modulationTime, modulationDepth;
    double airAbsorptionGainHF, hfReference, lfReference;
    int decayHFLimit;

    double lineScale[4] = {1.0, 1.0, 1.0, 1.0};

    double shortEchoTime[2]     = {0.0, 0.0};
    double shortEchoDepth[2]    = {0.0, 0.0};
    double shortEchoFeedback[2] = {0.0, 0.0};

    double earlyDecay = 0.0;
    double linePhase[4] = {LINE_PHASE[0], LINE_PHASE[1], LINE_PHASE[2], LINE_PHASE[3]};
    double earlyTapScale = 1.0;
    double t60HFReference = 0.0;

    const double (*reflTable)[3] = nullptr;
    int    reflTableN = 0;
    double reflTableGain = 1.0;
    int    reflTableLag = 0;
    double reflEq[3] = {0.0, 0.0, 1.0};

    double lateEq[4][3] = {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}};
    double lateWide[2][3] = {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}};
    double t60Bell[3] = {0.0, 0.0, 1.0};
    double lateFirstWash[2] = {-1.0, -1.0};
    const double (*feedTable)[2] = nullptr;
    int    feedTableN = 0;
    double lateMid[3] = {1.0, 0.0, 0.0};
};

inline double calcDecayCoeff(double len, double decay) {
    return std::pow(REVERB_DECAY_GAIN, len / decay);
}
inline double calcDecayLength(double coeff, double decay) {
    return std::log10(coeff < 1e-12 ? 1e-12 : coeff) * decay / -3.0;
}
inline double calcDensityGain(double a) {
    double v = 1.0 - a * a;
    return std::sqrt(v < 0 ? 0 : v);
}
inline double calcDelayLengthMult(double density) {
    double m = std::cbrt(density * DENSITY_SCALE);
    return m < 1.0 ? 1.0 : m;
}
inline void calcMatrixCoeffs(double diffusion, double& x, double& y) {
    const double n = 1.7320508075688772;
    double t = diffusion * std::atan(n);
    x = std::cos(t);
    y = std::sin(t) / n;
}

}
