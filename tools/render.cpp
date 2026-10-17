#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "equalizer/bass_boost.hpp"
#include "mixing/amount_law.hpp"
#include "mixing/echo_tap.hpp"
#include "mixing/mic_drive.hpp"
#include "mixing/mic_tone.hpp"
#include "mixing/send_level.hpp"
#include "mixing/step_back.hpp"
#include "mixing/soft_clip.hpp"
#include "mixing/subsonic.hpp"
#include "mixing/wet_peak.hpp"
#include "presets/card_presets.hpp"
#include "reverb/card_core.hpp"
#include "reverb/reverb_engine.hpp"

using mix::AmountLaw;

static std::map<std::string, double> kv;
static double opt(const char* k, double d) { auto it = kv.find(k); return it == kv.end() ? d : it->second; }

static bool readWav(const char* p, std::vector<float>& mid, int& fs) {
    FILE* f = fopen(p, "rb"); if (!f) return false;
    char id[4]; unsigned sz;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) { fclose(f); return false; }
    fread(&sz, 4, 1, f); fread(id, 1, 4, f);
    int ch = 0, bits = 0; fs = 0; long dp = 0; unsigned ds = 0;
    while (fread(id, 1, 4, f) == 4) {
        if (fread(&sz, 4, 1, f) != 1) break;
        if (!memcmp(id, "fmt ", 4)) { unsigned short a, c, al, bp; unsigned r, b; fread(&a, 2, 1, f); fread(&c, 2, 1, f); fread(&r, 4, 1, f); fread(&b, 4, 1, f); fread(&al, 2, 1, f); fread(&bp, 2, 1, f); ch = c; bits = bp; fs = (int)r; fseek(f, (long)sz - 16, SEEK_CUR); }
        else if (!memcmp(id, "data", 4)) { dp = ftell(f); ds = sz; break; }
        else fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR);
    }
    if (!dp || bits != 16 || ch < 1) { fclose(f); return false; }
    fseek(f, dp, SEEK_SET);
    std::vector<short> raw(ds / 2); const size_t got = fread(raw.data(), 2, raw.size(), f); fclose(f);
    const size_t ns = got / ch; mid.assign(ns, 0.f);
    for (size_t i = 0; i < ns; ++i) { float a = 0; for (int c = 0; c < ch; ++c) a += raw[i * ch + c] / 32768.f; mid[i] = a / ch; }
    return true;
}
static void writeWav(const char* p, const std::vector<float>& l, const std::vector<float>& r, int fs) {
    FILE* f = fopen(p, "wb"); if (!f) { printf("cannot write %s\n", p); exit(1); }
    const unsigned n = (unsigned)l.size(), ds = n * 4, rs = 36 + ds;
    const unsigned short one = 1, ch = 2, ba = 4, bps = 16; const unsigned br = fs * 4, fsz = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fsz, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&fs, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
    for (unsigned i = 0; i < n; ++i) {
        float a = l[i], b = r[i]; if (a > 1) a = 1; if (a < -1) a = -1; if (b > 1) b = 1; if (b < -1) b = -1;
        const short s0 = (short)(a * 32767), s1 = (short)(b * 32767); fwrite(&s0, 2, 1, f); fwrite(&s1, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: %s <voice.wav> <out.wav> <amount> <speechEndSec> [key=value ...]\n", argv[0]); return 1; }
    const char* tabFile = nullptr;
    for (int i = 5; i < argc; ++i) {
        if (!strncmp(argv[i], "tabfile:", 8)) { tabFile = argv[i] + 8; continue; }
        const char* e = strchr(argv[i], '='); if (!e) { printf("bad option %s (key=value)", argv[i]); putchar(10); return 1; } kv[std::string(argv[i], e - argv[i])] = atof(e + 1); }
    int fs = 0; std::vector<float> mid;
    if (!readWav(argv[1], mid, fs) || fs != 48000) { printf("cannot read %s (16-bit 48 kHz WAV expected)\n", argv[1]); return 1; }
    const int amt = atoi(argv[3]); const double sEnd = atof(argv[4]);
    std::vector<float> dry = mid;
    const int cut = (int)(sEnd * fs), fade = (int)(0.030 * fs);
    for (int i = 0; i < (int)dry.size(); ++i) { if (i >= cut + fade) dry[i] = 0.f; else if (i >= cut) dry[i] *= 1.f - (float)(i - cut) / fade; }
    if (opt("mictone", 0.0) != 0.0) {
        mix::MicTone::Tune mt;
        mt.lowDb = opt("mtlow", mt.lowDb); mt.lowHz = opt("mtlowhz", mt.lowHz);
        mt.dipDb = opt("mtdip", mt.dipDb); mt.dipHz = opt("mtdiphz", mt.dipHz); mt.dipQ = opt("mtdipq", mt.dipQ);
        mix::MicTone tone; tone.prepare(fs, mt);
        for (auto& v : dry) v = tone.process(v);
    }
    { const float ig = (float)opt("ingain", 1.0); if (ig != 1.0f) for (auto& v : dry) v *= ig; }
    const int fader = (int)opt("mic", 100.0);
    { const int loops = (int)opt("loop", 1.0); if (loops > 1) { std::vector<float> one = dry; for (int k = 1; k < loops; ++k) dry.insert(dry.end(), one.begin(), one.end()); } }
    const size_t N = dry.size();

    eax::Params p = CARD_PRESETS[HAUNTED_CAVERN_INDEX].p;
    static std::vector<std::array<double, 3>> tabExtra;
    if (tabFile) {
        if (opt("tabreplace", 0.0) == 0.0) for (int k = 0; k < p.reflTableN; ++k) tabExtra.push_back({p.reflTable[k][0], p.reflTable[k][1], p.reflTable[k][2]});
        if (FILE* tf = fopen(tabFile, "r")) { double a, b, c; while (fscanf(tf, "%lf %lf %lf", &a, &b, &c) == 3) tabExtra.push_back({a, b, c}); fclose(tf); }
        else { printf("cannot read %s", tabFile); putchar(10); return 1; }
        p.reflTable = reinterpret_cast<const double (*)[3]>(tabExtra.data()); p.reflTableN = (int)tabExtra.size();
    }
    p.density = opt("density", p.density); p.diffusion = opt("diffusion", p.diffusion); p.gain = opt("gain", p.gain);
    p.gainHF = opt("ghf", p.gainHF); p.gainLF = opt("glf", p.gainLF);
    p.decayTime = opt("decay", p.decayTime); p.decayHFRatio = opt("hfr", p.decayHFRatio); p.decayLFRatio = opt("lfr", p.decayLFRatio);
    p.reflectionsGain = opt("reflg", p.reflectionsGain); p.reflectionsDelay = opt("rd", p.reflectionsDelay);
    p.lateReverbGain = opt("lateg", p.lateReverbGain); p.lateReverbDelay = opt("lrd", p.lateReverbDelay);
    p.echoTime = opt("echot", p.echoTime); p.echoDepth = opt("echod", p.echoDepth);
    p.modulationTime = opt("modt", p.modulationTime); p.modulationDepth = opt("modd", p.modulationDepth);
    p.airAbsorptionGainHF = opt("air", p.airAbsorptionGainHF); p.hfReference = opt("hfref", p.hfReference); p.lfReference = opt("lfref", p.lfReference);
    p.decayHFLimit = (int)opt("hflimit", p.decayHFLimit); p.earlyDecay = opt("edec", p.earlyDecay);
    const char* ls[4] = {"ls0", "ls1", "ls2", "ls3"}; const char* ph[4] = {"ph0", "ph1", "ph2", "ph3"};
    for (int j = 0; j < 4; ++j) { p.lineScale[j] = opt(ls[j], p.lineScale[j]) * opt("lsall", 1.0); p.linePhase[j] = opt(ph[j], p.linePhase[j]); }
    p.shortEchoTime[0] = opt("se0t", p.shortEchoTime[0]); p.shortEchoDepth[0] = opt("se0d", p.shortEchoDepth[0]); p.shortEchoFeedback[0] = opt("se0f", p.shortEchoFeedback[0]);
    p.shortEchoTime[1] = opt("se1t", p.shortEchoTime[1]); p.shortEchoDepth[1] = opt("se1d", p.shortEchoDepth[1]); p.shortEchoFeedback[1] = opt("se1f", p.shortEchoFeedback[1]);
    p.reflTableGain = opt("tabgain", p.reflTableGain); p.reflTableLag = (int)opt("tablag", p.reflTableLag); if (opt("table", 1.0) == 0.0) { p.reflTable = nullptr; p.reflTableN = 0; }
    if (opt("tabn", 0.0) > 0.0 && (int)opt("tabn", 0.0) < p.reflTableN) p.reflTableN = (int)opt("tabn", 0.0);
    { const char* eq[4][3] = {{"leq0hz", "leq0db", "leq0q"}, {"leq1hz", "leq1db", "leq1q"}, {"leq2hz", "leq2db", "leq2q"}, {"leq3hz", "leq3db", "leq3q"}};
      for (int k = 0; k < 4; ++k) for (int c = 0; c < 3; ++c) p.lateEq[k][c] = opt(eq[k][c], p.lateEq[k][c]);
      const char* wd[2][3] = {{"wide0hz", "wide0db", "wide0q"}, {"wide1hz", "wide1db", "wide1q"}};
      for (int k = 0; k < 2; ++k) for (int c = 0; c < 3; ++c) p.lateWide[k][c] = opt(wd[k][c], p.lateWide[k][c]);
      p.lateFirstWash[0] = opt("washm", p.lateFirstWash[0]); p.lateFirstWash[1] = opt("washs", p.lateFirstWash[1]); p.lateMid[0] = opt("latemid", p.lateMid[0]); p.lateMid[1] = opt("latemidhz", p.lateMid[1]); p.lateMid[2] = opt("latemiddb", p.lateMid[2]);
      p.reflEq[0] = opt("reqhz", p.reflEq[0]); p.reflEq[1] = opt("reqdb", p.reflEq[1]); p.reflEq[2] = opt("reqq", p.reflEq[2]);
      p.t60Bell[0] = opt("tbhz", p.t60Bell[0]); p.t60Bell[1] = opt("tbdbs", p.t60Bell[1]); p.t60Bell[2] = opt("tbq", p.t60Bell[2]); }
    p.earlyTapScale = opt("ts", p.earlyTapScale); p.t60HFReference = opt("t60ref", p.t60HFReference);

    const float wetCal = (float)opt("wetcal", AmountLaw::WET_CAL), wetExp = (float)opt("wetexp", AmountLaw::WET_EXP);
    mix::WetPeak::Tune t;
    t.midDb = opt("middb", t.midDb); t.midHz = opt("midhz", t.midHz); t.midQ = opt("midq", t.midQ);
    t.hiDb = opt("hidb", t.hiDb); t.hiHz = opt("hihz", t.hiHz); t.hiQ = opt("hiq", t.hiQ);
    t.dipDb = opt("dipdb", t.dipDb); t.dipHz = opt("diphz", t.dipHz); t.dipQ = opt("dipq", t.dipQ);
    t.trimDb = opt("trim", t.trimDb); t.crossHz = opt("xhz", t.crossHz); t.cross = opt("cross", t.cross);

    const double loudDecay = opt("louddecay", HC_LOUD_DECAY);
    eax::Params pLoud = p; pLoud.decayTime = p.decayTime * loudDecay / HC_CORE.decayTime;
    eax::CoreRoom loudRoom = HC_CORE; loudRoom.decayTime = loudDecay;
    const auto rvBox = std::make_unique<eax::EAXReverb>(fs, p), rvLoudBox = std::make_unique<eax::EAXReverb>(fs, pLoud);
    eax::EAXReverb& rv = *rvBox;
    eax::EAXReverb& rvLoud = *rvLoudBox;
    eax::CardCore core(fs, HC_CORE), coreLoud(fs, loudRoom);
    mix::WetPeak wetPeak; wetPeak.prepare(fs, t);
    mix::EchoTap slap; slap.prepare(fs);
    mix::Subsonic subsonic; subsonic.prepare(fs);
    mix::SendLevel sendLevel; sendLevel.prepare(fs, (float)opt("sendthr", mix::SendLevel::THRESHOLD_DB), (float)opt("sendratio", mix::SendLevel::RATIO));
    const bool levelled = opt("sendlevel", 1.0) != 0.0;
    mix::SoftClip clipper; eqb::BassBoost bass; bass.prepare(fs);
    mix::MicDrive drive;
    const bool hardClip = opt("clip", 0.0) != 0.0, cable = opt("cable", 0.0) != 0.0, withCore = opt("core", 1.0) != 0.0;

    std::vector<float> send(N), tapIn(N), keep(N), coreOut(N), loudL(N), loudR(N), wantDb(N), voice(N), wl(N, 0.f), wr(N, 0.f), outL(N), outR(N);
    std::vector<int> micAt(N);
    auto stretch = [&](size_t from, size_t to, int a, int mic) {
        const int n = (int)(to - from);
        const float micGain = mic <= 0 ? 0.f : powf((float)mic / 100.0f, 1.3010f);
        drive.set(mic);
        bass.set(eqb::BassBoost::boostFor(micGain > 0.f ? 20.0 * log10((double)micGain) : 0.0));
        for (size_t i = from; i < to; ++i) { dry[i] = bass.run(dry[i] * micGain * drive.gain()); const float lev = sendLevel.process(dry[i]); keep[i] = levelled ? lev : 1.0f; tapIn[i] = subsonic.process(dry[i]); micAt[i] = mic; }
        const float gSend = a > 0 ? AmountLaw::send((float)a / 100.0f, wetCal, wetExp) : 0.0f;
        if (!cable) {
            rv.setCascade(opt("cascade", AmountLaw::CASCADE) * gSend * pow(10.0, t.trimDb / 20.0));
            rv.setCentre(opt("centre", AmountLaw::centre(a)));
            rvLoud.setCascade(opt("cascade", AmountLaw::CASCADE) * gSend * pow(10.0, t.trimDb / 20.0));
            rvLoud.setCentre(opt("centre", AmountLaw::centre(a)));
            for (size_t i = from; i < to; ++i) send[i] = tapIn[i] * keep[i] * gSend;
            rv.process(send.data() + from, wl.data() + from, wr.data() + from, n);
            for (size_t i = from; i < to; ++i) send[i] = tapIn[i] * (1.0f - keep[i]) * gSend;
            rvLoud.process(send.data() + from, loudL.data() + from, loudR.data() + from, n);
            for (size_t i = from; i < to; ++i) { wl[i] += loudL[i]; wr[i] += loudR[i]; }
            for (size_t i = from; i < to; ++i) wetPeak.process(wl[i], wr[i]);
            slap.setDelay(AmountLaw::slapDelay(fs, a));
            slap.process(tapIn.data() + from, AmountLaw::slap(a) * (float)opt("slapmul", 1.0), wl.data() + from, wr.data() + from, n);
        }
        if (withCore) {
            const float gCore = AmountLaw::core(a);
            for (size_t i = from; i < to; ++i) send[i] = tapIn[i] * keep[i] * gCore;
            core.process(send.data() + from, coreOut.data() + from, n);
            for (size_t i = from; i < to; ++i) { wl[i] += coreOut[i]; wr[i] += coreOut[i]; }
            for (size_t i = from; i < to; ++i) send[i] = tapIn[i] * (1.0f - keep[i]) * gCore;
            coreLoud.process(send.data() + from, coreOut.data() + from, n);
            for (size_t i = from; i < to; ++i) { wl[i] += coreOut[i]; wr[i] += coreOut[i]; }
        }
        const float cut = (float)opt("drycut", AmountLaw::dryCutDb(a));
        for (size_t i = from; i < to; ++i) wantDb[i] = cut;
    };
    const size_t turn = std::min(N, (size_t)(opt("at", 0.0) * fs));
    if (turn > 0 && turn < N) { stretch(0, turn, amt, fader); stretch(turn, N, (int)opt("then", amt), (int)opt("thenmic", fader)); }
    else stretch(0, N, amt, fader);
    mix::StepBack stepBack; stepBack.prepare(fs, opt("stepring", HC_CORE.decayTime));
    const bool stepping = opt("stepback", 1.0) != 0.0;
    float dryWant = 1.0f, dryNow = 1.0f;
    for (size_t i = 0; i < N; ++i) { if (i % 480 == 0) { drive.set(micAt[i]);
            const float held = stepping ? stepBack.cutDb(wantDb[i], tapIn.data() + i, (int)std::min<size_t>(480, N - i)) : wantDb[i];
            dryWant = powf(10.0f, -held / 20.0f); }
        dryNow += 0.002f * (dryWant - dryNow);
        const float v = dry[i] * dryNow; voice[i] = v; float l = drive.process(v + wl[i]), r = drive.process(v + wr[i]);
        if (hardClip) { if (l > 1.f) l = 1.f; if (l < -1.f) l = -1.f; if (r > 1.f) r = 1.f; if (r < -1.f) r = -1.f; } else clipper.process(l, r);
        outL[i] = l; outR[i] = r; }
    writeWav(argv[2], outL, outR, fs);
    if (opt("wetout", 0.0) != 0.0) { std::string wp = argv[2]; if (wp.size() > 4) wp.insert(wp.size() - 4, "_wet"); writeWav(wp.c_str(), wl, wr, fs); }
    if (opt("dryout", 0.0) != 0.0) { std::string dp = argv[2]; if (dp.size() > 4) dp.insert(dp.size() - 4, "_dry"); writeWav(dp.c_str(), voice, voice, fs); }
    printf("rendered %s amount %d\n", argv[2], amt);
    return 0;
}
