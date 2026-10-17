#pragma once
#include "../equalizer/graphic_equalizer.hpp"
#include "cmss3d.hpp"
#include "soft_clip.hpp"

namespace mix {

struct OutChain {
    eqb::GraphicEQ eq;
    CMSS3D cmss;
    SoftClip clip;

    void prepare(int fs) {
        eq.prepare(fs);
        cmss.prepare(fs);
    }
    inline void process(float& l, float& r, bool eqOn, bool cmssOn) {
        if (eqOn) eq.process(l, r);
        if (cmssOn) cmss.process(l, r);
        clip.process(l, r);
    }
};

}
