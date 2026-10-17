#pragma once
#include <cmath>

namespace mix {

struct AmountLaw {
    static constexpr float WET_CAL = 0.855f, WET_EXP = 0.58f;
    static constexpr float CASCADE = 1.30f;
    static constexpr float CENTRE_CAL = 0.69f, CENTRE_EXP = -0.245f, CENTRE_MAX = 1.45f;
    static constexpr float CORE_CAL = 0.404f;
    static constexpr int   RISE_FROM = 10;
    static constexpr float GAP_DB = 11.0f, GAP_EXP_NEAR = 0.9f, GAP_EXP_FAR = 1.25f;
    static constexpr float CARD_ECHO_POWER = 0.063f;
    static constexpr float SLAP_CAL = 1.77f, SLAP_EXP = 1.40f, SLAP_MAX = 0.0765f;
    static constexpr double SLAP_SEC = 0.4867, SLAP_SEC_FULL = 0.4733;
    static constexpr int   SLAP_SLIDE_FROM = 20;

    static float send(float a, float cal = WET_CAL, float ex = WET_EXP) { return cal * powf(a, ex); }

    static float centre(int amt) {
        if (amt <= 0) return 1.0f;
        const float g = CENTRE_CAL * powf((float)amt / 100.0f, CENTRE_EXP);
        return g > CENTRE_MAX ? CENTRE_MAX : g;
    }
    static float cardLaw(int amt) { return CORE_CAL * send((float)amt / 100.0f) * centre(amt); }
    static float core(int amt) { return amt <= 0 ? 0.0f : cardLaw(amt) * lift(amt); }

    static float rise(int amt, float top, float exponent) {
        if (amt <= RISE_FROM) return 0.0f;
        return top * powf((float)(amt - RISE_FROM) / (float)(100 - RISE_FROM), exponent);
    }
    static float gapDb(int amt) { return rise(amt, GAP_DB, GAP_EXP_NEAR) + rise(amt, GAP_DB, GAP_EXP_FAR); }
    static float dryCutDb(int amt) {
        if (amt <= RISE_FROM) return 0.0f;
        const float g = cardLaw(amt) / cardLaw(RISE_FROM);
        const float own = CARD_ECHO_POWER * g * g, wanted = own * powf(10.0f, gapDb(amt) / 10.0f);
        return 10.0f * log10f((1.0f + wanted) / (1.0f + CARD_ECHO_POWER));
    }
    static float dry(int amt) { return powf(10.0f, -dryCutDb(amt) / 20.0f); }
    static float liftDb(int amt) { return gapDb(amt) - dryCutDb(amt); }
    static float lift(int amt) { return powf(10.0f, liftDb(amt) / 20.0f); }

    static float slap(int amt) {
        if (amt <= 0) return 0.0f;
        const float g = SLAP_CAL * powf((float)amt / 100.0f, SLAP_EXP);
        return g > SLAP_MAX ? SLAP_MAX : g;
    }
    static double slapSeconds(int amt) {
        double t = SLAP_SEC;
        if (amt > SLAP_SLIDE_FROM) t += (SLAP_SEC_FULL - SLAP_SEC) * (double)(amt - SLAP_SLIDE_FROM) / (double)(100 - SLAP_SLIDE_FROM);
        return t;
    }
    static int slapDelay(int fs, int amt) { return (int)(slapSeconds(amt) * fs + 0.5); }
};

}
