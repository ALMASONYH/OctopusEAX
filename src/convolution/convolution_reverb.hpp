#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "partition.hpp"

namespace conv {

class Reverb {
public:
    static const int HEAD_BLOCK = 512;
    static const int TAIL_BLOCK = 4096;
    static const int SPLIT = 8192;

    bool load(const std::wstring& path, int fs, int skip = HEAD_BLOCK) {
        std::vector<float> L, R; int rate = 0;
        if (!readWav(path, L, R, rate)) return false;
        if (rate != fs || (int)L.size() <= skip) return false;
        if (skip > 0) { L.erase(L.begin(), L.begin() + skip); R.erase(R.begin(), R.begin() + skip); }
        int end = (int)L.size();
        while (end > 1 && fabsf(L[end - 1]) < 1e-5f && fabsf(R[end - 1]) < 1e-5f) --end;
        const int headLen = std::min(end, SPLIT);
        head_.init(HEAD_BLOCK, L.data(), R.data(), headLen);
        hasTail_ = end > SPLIT;
        if (hasTail_) {
            tail_.init(TAIL_BLOCK, L.data() + SPLIT, R.data() + SPLIT, end - SPLIT);
            hold_ = HEAD_BLOCK + SPLIT - TAIL_BLOCK;
            dlL_.assign(hold_, 0.f); dlR_.assign(hold_, 0.f); dpos_ = 0;
        }
        return true;
    }
    void clear() {
        head_.clear();
        if (hasTail_) tail_.clear();
        std::fill(dlL_.begin(), dlL_.end(), 0.f);
        std::fill(dlR_.begin(), dlR_.end(), 0.f);
        dpos_ = 0;
    }
    void process(const float* in, float* outL, float* outR, int n) {
        for (int i = 0; i < n; ++i) {
            float hl, hr; head_.push(in[i], hl, hr);
            if (hasTail_) {
                float tl, tr; tail_.push(in[i], tl, tr);
                const float ol = dlL_[dpos_], orr = dlR_[dpos_];
                dlL_[dpos_] = tl; dlR_[dpos_] = tr;
                if (++dpos_ >= hold_) dpos_ = 0;
                hl += ol; hr += orr;
            }
            outL[i] = hl; outR[i] = hr;
        }
    }

private:
    static bool readWav(const std::wstring& path, std::vector<float>& L,
                        std::vector<float>& R, int& fs) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        const long fileBytes = ftell(f);
        fseek(f, 0, SEEK_SET);
        char id[4]; uint32_t sz;
        if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) { fclose(f); return false; }
        fread(&sz, 4, 1, f); fread(id, 1, 4, f);
        int ch = 0, bits = 0, fmt = 1; long dp = 0; uint32_t ds = 0;
        while (fread(id, 1, 4, f) == 4) {
            if (fread(&sz, 4, 1, f) != 1) break;
            if (!memcmp(id, "fmt ", 4)) {
                uint16_t a, c, al, bp; uint32_t r, b;
                fread(&a, 2, 1, f); fread(&c, 2, 1, f); fread(&r, 4, 1, f);
                fread(&b, 4, 1, f); fread(&al, 2, 1, f); fread(&bp, 2, 1, f);
                fmt = a; ch = c; fs = (int)r; bits = bp;
                uint32_t used = 16;
                if (a == 0xFFFE && sz >= 40) {
                    unsigned char ext[24];
                    if (fread(ext, 1, 24, f) == 24) { fmt = ext[8] | ext[9] << 8; used = 40; }
                }
                if (sz > used) fseek(f, (long)(((sz + 1) & ~1u) - used), SEEK_CUR);
            } else if (!memcmp(id, "data", 4)) { dp = ftell(f); ds = (uint32_t)std::min<long>((long)std::min<uint32_t>(sz, 0x7FFFFFFFu), fileBytes - dp); break; }
            else fseek(f, (sz + 1) & ~1u, SEEK_CUR);
        }
        if (!dp || ch < 1 || ch > 2 || bits < 16 || bits > 64 || bits % 8 || (fmt != 1 && fmt != 3)) { fclose(f); return false; }
        fseek(f, dp, SEEK_SET);
        const int bytes = bits / 8;
        const size_t n = ds / bytes / ch;
        L.resize(n); R.resize(n);
        std::vector<unsigned char> raw(ds);
        if (fread(raw.data(), 1, ds, f) != ds) { fclose(f); return false; }
        fclose(f);
        for (size_t i = 0; i < n; ++i) {
            for (int c = 0; c < ch; ++c) {
                const unsigned char* q = raw.data() + (i * ch + c) * bytes;
                double v = 0.0;
                if (fmt == 3 && bits == 32) { float x; memcpy(&x, q, 4); v = x; }
                else if (fmt == 3 && bits == 64) { double x; memcpy(&x, q, 8); v = x; }
                else if (fmt == 3) v = 0.0;
                else if (bits == 16) { int16_t x; memcpy(&x, q, 2); v = x / 32768.0; }
                else if (bits == 24) { int32_t x = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); v = (x >> 8) / 8388608.0; }
                else if (bits == 32) { int32_t x; memcpy(&x, q, 4); v = x / 2147483648.0; }
                if (c == 0) L[i] = (float)v;
                if (c == 1 || ch == 1) R[i] = (float)v;
            }
        }
        return true;
    }
    Tier head_, tail_;
    bool hasTail_ = false;
    int hold_ = 0, dpos_ = 0;
    std::vector<float> dlL_, dlR_;
};

}
