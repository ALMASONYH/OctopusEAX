#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

namespace voice {

class Tempo {
public:
    static constexpr double STILL = 0.25;

    void prepare(double fs, int channels, double holdSec, double hopSec, double fadeSec, double seekSec) {
        channels_ = channels > 1 ? 2 : 1;
        hop_ = std::max(64, (int)(hopSec * fs));
        fade_ = std::max(16, (int)(fadeSec * fs));
        seek_ = std::max(8, (int)(seekSec * fs));
        glide_ = 1.0 - std::exp(-1.0 / (0.006 * fs));
        mute_ = 1.0 - std::exp(-1.0 / (0.004 * fs));
        ramp_.resize((size_t)fade_);
        for (int k = 0; k < fade_; ++k) {
            const double s = std::sin(1.5707963267948966 * (k + 0.5) / fade_);
            ramp_[(size_t)k] = s * s;
        }
        const double need = holdSec * fs + 8.0 * (hop_ + fade_ + seek_) + 64.0;
        size_t size = 1;
        while ((double)size < need) size <<= 1;
        mask_ = (long long)size - 1;
        for (int c = 0; c < channels_; ++c) buf_[c].assign(size, 0.0f);
        mono_.assign(channels_ == 2 ? size : 0, 0.0f);
        reset();
    }

    void reset() {
        for (int c = 0; c < channels_; ++c) std::fill(buf_[c].begin(), buf_[c].end(), 0.0f);
        std::fill(mono_.begin(), mono_.end(), 0.0f);
        w_ = 0;
        start(0.0);
    }

    void start(double at) {
        cur_ = Grain{at, true};
        old_ = Grain{};
        ana_ = at;
        rate_ = 1.0;
        gain_ = 1.0;
        age_ = hop_;
    }

    long long written() const { return w_; }
    long long capacity() const { return mask_ + 1; }
    double head() const { return tape_ ? cur_.pos : ana_; }
    int hop() const { return hop_; }
    int fade() const { return fade_; }
    int seek() const { return seek_; }
    int minLag(bool tape) const { return tape ? 6 : seek_ + fade_ + 8; }

    void write(const float* l, const float* r, int n) {
        for (int i = 0; i < n; ++i) {
            const size_t at = (size_t)(w_ & mask_);
            buf_[0][at] = l[i];
            if (channels_ == 2) {
                buf_[1][at] = r[i];
                mono_[at] = 0.5f * (l[i] + r[i]);
            }
            ++w_;
        }
    }

    template <class Rate>
    void render(float* l, float* r, int n, bool tape, long long edge0, Rate rateAt) {
        if (tape != tape_) switchMode(tape);
        const long long newest = w_ - 1;
        for (int i = 0; i < n; ++i) {
            const long long rt = edge0 > newest - i ? newest : edge0 + i;
            bool moving = true;
            if (tape_) {
                rate_ += (rateAt(i, cur_.pos, rt) - rate_) * glide_;
                double step = rate_ < 0.0 ? 0.0 : rate_;
                const double room = (double)(rt - 4) - cur_.pos;
                if (step > room) step = room > 0.0 ? room : 0.0;
                moving = step >= STILL;
                rate_ = step;
            } else {
                if (age_ >= hop_) decide(i, rt, rateAt);
                moving = rate_ >= STILL;
            }
            gain_ += ((moving ? 1.0 : 0.0) - gain_) * mute_;

            double gainIn = gain_, gainOut = 0.0;
            if (old_.on) {
                if (age_ < fade_) { gainOut = gain_ * (1.0 - ramp_[(size_t)age_]); gainIn = gain_ * ramp_[(size_t)age_]; }
                else old_.on = false;
            }
            double outL = gainIn * read(0, cur_.pos), outR = channels_ == 2 ? gainIn * read(1, cur_.pos) : outL;
            if (gainOut > 0.0) {
                outL += gainOut * read(0, old_.pos);
                outR += channels_ == 2 ? gainOut * read(1, old_.pos) : gainOut * read(0, old_.pos);
                old_.pos += 1.0;
            }
            l[i] = (float)outL;
            if (r != l) r[i] = (float)outR;

            if (tape_) {
                cur_.pos += rate_;
                ana_ = cur_.pos;
            } else {
                if (cur_.pos + 4.0 <= (double)newest) cur_.pos += 1.0;
                ana_ += rate_;
            }
            ++age_;
        }
    }

private:
    struct Grain {
        double pos = 0.0;
        bool on = false;
    };

    void switchMode(bool tape) {
        tape_ = tape;
        ana_ = cur_.pos;
        rate_ = 1.0;
        age_ = hop_;
    }

    template <class Rate>
    void decide(int i, long long rt, Rate& rateAt) {
        double r = rateAt(i, ana_, rt);
        const double most = 1.0 + ((double)rt - ana_ - minLag(false)) / hop_;
        if (r > most) r = most;
        if (r < 0.0) r = 0.0;
        age_ = 0;
        if (std::fabs(r - 1.0) < 1e-9) {
            rate_ = 1.0;
            ana_ = cur_.pos;
            return;
        }
        rate_ = r;
        const long long at = (long long)std::floor(ana_);
        const double from = std::floor(cur_.pos);
        long long room = rt - 4 - fade_ - at;
        if (room < -seek_) room = -seek_;
        const int hi = (int)std::min<long long>(seek_, room);
        const int shift = align((long long)from, at, -seek_, hi);
        old_ = cur_;
        cur_.pos = (double)(at + shift) + (old_.pos - from);
        cur_.on = true;
    }

    float sample(long long i) const {
        const size_t at = (size_t)(i & mask_);
        return channels_ == 2 ? mono_[at] : buf_[0][at];
    }

    double score(long long from, long long cand, int step) const {
        double xy = 0.0, yy = 0.0;
        for (int j = 0; j < fade_; j += step) {
            const double a = sample(from + j), b = sample(cand + j);
            xy += a * b;
            yy += b * b;
        }
        return xy / std::sqrt(yy + 1e-12);
    }

    int align(long long from, long long at, int lo, int hi) const {
        if (hi < lo) return hi;
        double energy = 0.0;
        for (int j = 0; j < fade_; j += 2) {
            const double a = sample(from + j);
            energy += a * a;
        }
        if (energy < 1e-10 * fade_) return std::min(hi, std::max(lo, 0));
        int best = lo;
        double top = -1e300;
        for (int s = lo; s <= hi; s += 4) {
            const double v = score(from, at + s, 2);
            if (v > top) { top = v; best = s; }
        }
        const int a = std::max(lo, best - 3), b = std::min(hi, best + 3);
        int fine = best;
        top = -1e300;
        for (int s = a; s <= b; ++s) {
            const double v = score(from, at + s, 1);
            if (v > top) { top = v; fine = s; }
        }
        return fine;
    }

    double read(int c, double pos) const {
        const std::vector<float>& b = buf_[c];
        const long long i = (long long)std::floor(pos);
        const double t = pos - (double)i;
        const double y1 = b[(size_t)(i & mask_)];
        if (t == 0.0) return y1;
        const double y0 = b[(size_t)((i - 1) & mask_)];
        const double y2 = b[(size_t)((i + 1) & mask_)], y3 = b[(size_t)((i + 2) & mask_)];
        const double a = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
        const double c1 = 0.5 * (y2 - y0);
        return ((a * t + c2) * t + c1) * t + y1;
    }

    int channels_ = 1, hop_ = 1728, fade_ = 384, seek_ = 576, age_ = 0;
    double glide_ = 0.003, mute_ = 0.005, ana_ = 0.0, rate_ = 1.0, gain_ = 1.0;
    bool tape_ = false;
    long long w_ = 0, mask_ = 0;
    Grain cur_, old_;
    std::vector<double> ramp_;
    std::vector<float> buf_[2], mono_;
};

class VoiceSpeed {
public:
    static constexpr double MOST_LAG = 3.0, CATCH_UP = 4.0, LOOK = 0.040, HANG = 0.250, EASE = 0.15, EASE_SLOW = 0.5, FLOOR_WINDOW = 1.5;
    static constexpr double ABOVE_FLOOR_DB = 7.0, SURE_OVER_FLOOR_DB = 10.0, BELOW_VOICE_DB = 30.0, VOICE_OVER_FLOOR_DB = 20.0;
    static const int STEADY_FRAMES = 3;
    static constexpr double NEAR_SEC = 0.010, CALM_SEC = 0.060;
    static constexpr double GAP_BACK = 0.020, GAP_AHEAD = 0.030, GAP_SLOWEST = 0.5, LEAD_MIN = 0.4, LEAD_MAX = 1.6, LEAD_TALK = 3.0;
    static const int CHUNK = 1024;
    static const unsigned char SPEECH = 1, SOUND = 2;

    void prepare(double fs) {
        fs_ = fs;
        core_.prepare(fs, 1, MOST_LAG + 1.0, 0.036, 0.008, 0.012);
        frame_ = std::max(32, (int)(0.005 * fs));
        flags_.assign((size_t)(core_.capacity() / frame_) + 4, 0);
        hangFrames_ = (int)(HANG * fs / frame_);
        windowFrames_ = (int)(FLOOR_WINDOW * fs / frame_);
        look_ = (int)(LOOK * fs);
        gapBack_ = (int)(GAP_BACK * fs);
        gapAhead_ = (int)(GAP_AHEAD * fs);
        nearStep_ = 1.0 - std::exp(-(double)frame_ / (NEAR_SEC * fs));
        calmStep_ = 1.0 - std::exp(-(double)frame_ / (CALM_SEC * fs));
        blend_ = (float)(1.0 / (0.015 * fs));
        dry_.assign(CHUNK, 0.0f);
        reset();
    }

    void reset() {
        core_.reset();
        std::fill(flags_.begin(), flags_.end(), (unsigned char)0);
        acc_ = 0.0;
        accN_ = 0;
        frames_ = 0;
        floorDb_ = -60.0;
        curMinDb_ = prevMinDb_ = 0.0;
        near_ = calm_ = -1.0;
        voiceDb_ = curMaxDb_ = prevMaxDb_ = -120.0;
        windowLeft_ = windowFrames_;
        hang_ = over_ = 0;
        lastSpeech_ = lastSound_ = false;
        engaged_ = false;
        mix_ = 0.0f;
        lag_ = 0.0;
    }

    double lagSeconds() const { return lag_ / fs_; }

    void process(float* x, int n, int speedPct, bool keepPitch) {
        for (int at = 0; at < n; at += CHUNK) chunk(x + at, std::min(CHUNK, n - at), speedPct, !keepPitch);
    }

private:
    void chunk(float* x, int n, int speedPct, bool tape) {
        listen(x, n);
        core_.write(x, x, n);
        const bool want = speedPct != 100;
        const long long edge0 = core_.written() - n;
        if (!engaged_) {
            if (!want) { lag_ = 0.0; return; }
            engaged_ = true;
            mix_ = 0.0f;
            core_.start((double)(edge0 - core_.minLag(tape)));
        }
        std::copy(x, x + n, dry_.begin());
        const double s = want ? speedPct / 100.0 : 1.0;
        core_.render(x, x, n, tape, edge0, [this, s, tape](int, double head, long long rt) { return rateFor(head, rt, s, tape); });
        const long long newest = core_.written() - 1;
        lag_ = (double)newest - core_.head();
        const bool settle = !want && lag_ <= core_.minLag(tape) + core_.seek() + frame_ && !speechIn((long long)core_.head() - core_.seek(), newest);
        const float target = settle ? 0.0f : 1.0f;
        for (int i = 0; i < n; ++i) {
            mix_ = mix_ < target ? std::min(target, mix_ + blend_) : std::max(target, mix_ - blend_);
            x[i] = dry_[(size_t)i] + mix_ * (x[i] - dry_[(size_t)i]);
        }
        if (settle && mix_ <= 0.0f) {
            engaged_ = false;
            lag_ = 0.0;
        }
    }

    double leadFor(double s, bool tape) const {
        return s > 1.0 ? std::min(LEAD_MAX, std::max(LEAD_MIN, LEAD_TALK * (1.0 - 1.0 / s))) * fs_ : (double)core_.minLag(tape);
    }

    double rateFor(double head, long long rt, double s, bool tape) const {
        const double lag = (double)rt - head;
        const long long from = (long long)std::floor(head);
        if (speechIn(from, std::min<long long>(from + look_, rt))) {
            if (s <= 1.0) return s + (1.0 - s) * std::min(1.0, std::max(0.0, (lag - (MOST_LAG - EASE_SLOW) * fs_) / (EASE_SLOW * fs_)));
            const double r = 1.0 + (s - 1.0) * std::min(1.0, std::max(0.0, (lag - core_.minLag(tape)) / (EASE * fs_)));
            if (soundIn(from - gapBack_, std::min<long long>(from + gapAhead_, rt))) return r;
            return std::min(r, std::max(GAP_SLOWEST, 1.0 + (lag - leadFor(s, tape)) / core_.hop()));
        }
        const double r = 1.0 + (lag - leadFor(s, tape)) / core_.hop();
        return r < 0.0 ? 0.0 : (r > CATCH_UP ? CATCH_UP : r);
    }

    void listen(const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            acc_ += (double)x[i] * x[i];
            if (++accN_ < frame_) continue;
            const double power = acc_ / frame_;
            if (near_ < 0.0) near_ = calm_ = power;
            near_ += nearStep_ * (power - near_);
            calm_ += calmStep_ * (power - calm_);
            const double calmDb = 10.0 * std::log10(calm_ + 1e-12);
            const double db = 10.0 * std::log10(near_ + 1e-12);
            curMinDb_ = std::min(curMinDb_, calmDb);
            curMaxDb_ = std::max(curMaxDb_, db);
            if (--windowLeft_ <= 0) {
                prevMinDb_ = curMinDb_;
                prevMaxDb_ = curMaxDb_;
                curMinDb_ = calmDb;
                curMaxDb_ = db;
                windowLeft_ = windowFrames_;
            }
            floorDb_ = std::min(curMinDb_, prevMinDb_);
            voiceDb_ = std::max(curMaxDb_, prevMaxDb_);
            const bool voiced = voiceDb_ > floorDb_ + VOICE_OVER_FLOOR_DB;
            over_ = db > std::max(floorDb_ + ABOVE_FLOOR_DB, -62.0) ? over_ + 1 : 0;
            const bool loud = over_ >= STEADY_FRAMES || db > std::max(floorDb_ + SURE_OVER_FLOOR_DB, -62.0) || (voiced && db > voiceDb_ - BELOW_VOICE_DB);
            if (loud) hang_ = hangFrames_;
            else if (hang_ > 0) --hang_;
            lastSpeech_ = loud || hang_ > 0;
            lastSound_ = loud;
            flags_[(size_t)(frames_ % (long long)flags_.size())] = (unsigned char)((lastSpeech_ ? SPEECH : 0) | (loud ? SOUND : 0));
            ++frames_;
            acc_ = 0.0;
            accN_ = 0;
        }
    }

    bool flagIn(long long a, long long b, unsigned char bit, bool newest) const {
        if (b < a) b = a;
        const long long oldest = frames_ - (long long)flags_.size() + 1;
        for (long long f = std::max(0LL, a) / frame_; f <= b / frame_; ++f) {
            if (f >= frames_) return newest;
            if (f < oldest) continue;
            if (flags_[(size_t)(f % (long long)flags_.size())] & bit) return true;
        }
        return false;
    }
    bool speechIn(long long a, long long b) const { return flagIn(a, b, SPEECH, lastSpeech_); }
    bool soundIn(long long a, long long b) const { return flagIn(a, b, SOUND, lastSound_); }

    double fs_ = 48000.0, acc_ = 0.0, floorDb_ = -60.0, curMinDb_ = -60.0, prevMinDb_ = -60.0, lag_ = 0.0;
    double voiceDb_ = -120.0, curMaxDb_ = -120.0, prevMaxDb_ = -120.0;
    double near_ = -1.0, calm_ = -1.0, nearStep_ = 0.4, calmStep_ = 0.08;
    int frame_ = 240, accN_ = 0, hang_ = 0, over_ = 0, hangFrames_ = 40, windowFrames_ = 300, windowLeft_ = 300, look_ = 1920, gapBack_ = 960, gapAhead_ = 1440;
    long long frames_ = 0;
    bool lastSpeech_ = false, lastSound_ = false, engaged_ = false;
    float mix_ = 0.0f, blend_ = 0.0014f;
    Tempo core_;
    std::vector<unsigned char> flags_;
    std::vector<float> dry_;
};

class ClipSpeed {
public:
    static const int CHUNK = 1024;

    void prepare(double fs) {
        core_.prepare(fs, 2, 1.0, 0.060, 0.010, 0.015);
        inL_.assign(CHUNK, 0.0f);
        inR_.assign(CHUNK, 0.0f);
        reset();
    }

    void reset() {
        core_.reset();
        engaged_ = false;
    }

    template <class Pull>
    void render(float* l, float* r, int n, int speedPct, bool keepPitch, Pull pull) {
        if (!engaged_ && speedPct == 100) { pull(l, r, n); return; }
        if (!engaged_) {
            engaged_ = true;
            core_.reset();
        }
        const double s = speedPct / 100.0;
        const double reach = core_.head() + std::max(s, 1.0) * (n + core_.hop() + core_.fade()) + core_.seek() + 2.0 * core_.fade() + 16.0;
        for (long long need = (long long)std::ceil(reach) - core_.written(); need > 0;) {
            const int m = (int)std::min<long long>(need, CHUNK);
            pull(inL_.data(), inR_.data(), m);
            core_.write(inL_.data(), inR_.data(), m);
            need -= m;
        }
        core_.render(l, r, n, !keepPitch, LLONG_MAX / 4, [s](int, double, long long) { return s; });
    }

private:
    Tempo core_;
    std::vector<float> inL_, inR_;
    bool engaged_ = false;
};

}
