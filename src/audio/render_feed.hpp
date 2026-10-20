#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <cstring>
#include <vector>

namespace wasapi {

struct RenderFeed {
    static const UINT32 GROW = 96, WARMUP = 100;

    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    UINT32 bufFrames = 0, target = 0, slack = 0, most = 0, pushes = 0;
    bool primed = false;
    double level = 0.0;
    int sinceTrim = 0;
    std::vector<float> scratch, hush;

    bool attach(IAudioClient* client, UINT32 frames, UINT32 fillFrames, UINT32 slackFrames) {
        ac = client; bufFrames = frames;
        most = bufFrames / 2 > 1 ? bufFrames / 2 : 1;
        target = fillFrames < most ? (fillFrames > 0 ? fillFrames : 1) : most;
        slack = slackFrames > 0 ? slackFrames : 1;
        primed = false; level = (double)target; sinceTrim = 0; pushes = 0;
        return SUCCEEDED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc)) && rc;
    }
    void release() {
        if (rc) rc->Release();
        rc = nullptr; ac = nullptr;
    }
    bool live() const { return rc != nullptr; }

    bool push(const float* inter, UINT32 n) {
        if (!rc) return false;
        if (n < 4) return n == 0 ? true : put(inter, n);
        UINT32 pad = 0;
        if (FAILED(ac->GetCurrentPadding(&pad))) return false;
        if (pushes < WARMUP) ++pushes;
        if (!primed || pad == 0) {
            if (primed && pushes >= WARMUP && target < most) target = target + GROW < most ? target + GROW : most;
            if (!quiet(target > pad ? target - pad : 0)) return false;
            primed = true;
            pad = target;
            level = (double)target;
        }
        level += 0.02 * ((double)pad - level);
        ++sinceTrim;
        int delta = 0;
        if (sinceTrim >= 2) {
            if (level > (double)(target + slack)) delta = -1;
            else if (level + (double)slack < (double)target) delta = 1;
        }
        if (delta == 0) return put(inter, n);
        sinceTrim = 0;
        level += delta;
        const UINT32 m = n + delta;
        scratch.resize((size_t)m * 2);
        const double step = (double)(n - 1) / (double)(m - 1);
        for (UINT32 i = 0; i < m; ++i) {
            const double pos = i * step;
            UINT32 k = (UINT32)pos;
            if (k >= n - 1) k = n - 2;
            const float t = (float)(pos - k);
            scratch[2 * i]     = inter[2 * k]     + (inter[2 * k + 2] - inter[2 * k])     * t;
            scratch[2 * i + 1] = inter[2 * k + 1] + (inter[2 * k + 3] - inter[2 * k + 1]) * t;
        }
        return put(scratch.data(), m);
    }

private:
    bool quiet(UINT32 n) {
        if (n == 0) return true;
        hush.assign((size_t)n * 2, 0.0f);
        return put(hush.data(), n);
    }

    bool put(const float* src, UINT32 count) {
        UINT32 pad = 0;
        if (FAILED(ac->GetCurrentPadding(&pad))) return false;
        const UINT32 room = pad < bufFrames ? bufFrames - pad : 0;
        const UINT32 part = count < room ? count : room;
        if (part == 0) return true;
        BYTE* buf = nullptr;
        if (FAILED(rc->GetBuffer(part, &buf))) return false;
        memcpy(buf, src, (size_t)part * 8);
        rc->ReleaseBuffer(part, 0);
        return true;
    }
};

}
