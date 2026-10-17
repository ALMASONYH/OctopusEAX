#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace clip {

struct StereoConverter {
    int channels = 2, bits = 32, rate = 48000;
    bool isFloat = true, direct = true;
    double step = 1.0, pos = 0.0;
    float prevL = 0.0f, prevR = 0.0f;
    bool primed = false;
    std::vector<float> gainL, gainR, in, out;

    static DWORD usualLayout(int n) {
        switch (n) {
        case 1: return SPEAKER_FRONT_CENTER;
        case 3: return 0x7;
        case 4: return 0x33;
        case 5: return 0x37;
        case 6: return 0x3F;
        case 8: return 0x63F;
        default: return 0x3;
        }
    }

    void setup(const WAVEFORMATEX* f, int fs) {
        channels = std::max<int>(1, f->nChannels);
        bits = f->wBitsPerSample;
        rate = (int)f->nSamplesPerSec;
        isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        DWORD layout = usualLayout(channels);
        if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
            static const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
            const auto* e = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
            isFloat = IsEqualGUID(e->SubFormat, kFloat) != FALSE;
            if (e->dwChannelMask) layout = e->dwChannelMask;
        }
        const DWORD fullLeft = SPEAKER_FRONT_LEFT, fullRight = SPEAKER_FRONT_RIGHT;
        const DWORD left = SPEAKER_BACK_LEFT | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_SIDE_LEFT | SPEAKER_TOP_FRONT_LEFT | SPEAKER_TOP_BACK_LEFT;
        const DWORD right = SPEAKER_BACK_RIGHT | SPEAKER_FRONT_RIGHT_OF_CENTER | SPEAKER_SIDE_RIGHT | SPEAKER_TOP_FRONT_RIGHT | SPEAKER_TOP_BACK_RIGHT;
        gainL.assign((size_t)channels, 0.0f);
        gainR.assign((size_t)channels, 0.0f);
        DWORD rest = layout;
        for (int c = 0; c < channels; ++c) {
            DWORD spot = 0;
            if (rest) {
                spot = rest & (~rest + 1);
                rest &= rest - 1;
            } else {
                spot = (c % 2) ? fullRight : fullLeft;
            }
            if (channels == 1) {
                gainL[0] = gainR[0] = 1.0f;
            } else if (spot & fullLeft) {
                gainL[(size_t)c] = 1.0f;
            } else if (spot & fullRight) {
                gainR[(size_t)c] = 1.0f;
            } else if (spot & left) {
                gainL[(size_t)c] = 0.7071f;
            } else if (spot & right) {
                gainR[(size_t)c] = 0.7071f;
            } else if (!(spot & SPEAKER_LOW_FREQUENCY)) {
                gainL[(size_t)c] = gainR[(size_t)c] = 0.7071f;
            }
        }
        step = rate / (double)fs;
        reset();
    }

    void reset() {
        pos = 0.0;
        primed = false;
    }

    size_t frameBytes() const { return (size_t)channels * (size_t)(bits / 8); }

    float sample(const BYTE* frame, int ch) const {
        if (isFloat) return reinterpret_cast<const float*>(frame)[ch];
        if (bits == 16) return reinterpret_cast<const short*>(frame)[ch] / 32768.0f;
        if (bits == 32) return reinterpret_cast<const int*>(frame)[ch] / 2147483648.0f;
        if (bits == 24) {
            const BYTE* b = frame + 3 * ch;
            const int v = (int)((unsigned)b[0] << 8 | (unsigned)b[1] << 16 | (unsigned)b[2] << 24);
            return v / 2147483648.0f;
        }
        return 0.0f;
    }

    const std::vector<float>& convert(const BYTE* data, size_t frames, bool silent) {
        const size_t stride = frameBytes();
        in.resize(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            float l = 0.0f, r = 0.0f;
            if (!silent) {
                const BYTE* fr = data + i * stride;
                for (int c = 0; c < channels; ++c) {
                    const float v = sample(fr, c);
                    l += gainL[(size_t)c] * v;
                    r += gainR[(size_t)c] * v;
                }
            }
            in[2 * i] = l;
            in[2 * i + 1] = r;
        }
        if (step == 1.0) return in;
        out.clear();
        if (!frames) return out;
        if (!primed) {
            prevL = in[0];
            prevR = in[1];
            primed = true;
        }
        const long last = (long)frames - 1;
        for (;;) {
            const long i0 = (long)std::floor(pos);
            if (i0 + 1 > last) break;
            const double f = pos - i0;
            const float l0 = i0 < 0 ? prevL : in[2 * i0], r0 = i0 < 0 ? prevR : in[2 * i0 + 1];
            const float l1 = in[2 * (i0 + 1)], r1 = in[2 * (i0 + 1) + 1];
            out.push_back(l0 + (float)((l1 - l0) * f));
            out.push_back(r0 + (float)((r1 - r0) * f));
            pos += step;
        }
        pos -= (double)frames;
        prevL = in[2 * last];
        prevR = in[2 * last + 1];
        return out;
    }
};

inline bool askAudioType(IMFSourceReader* rd, UINT32 rate, UINT32 channels, bool isFloat) {
    IMFMediaType* mt = nullptr;
    if (FAILED(MFCreateMediaType(&mt)) || !mt) return false;
    const UINT32 bits = isFloat ? 32 : 16, align = channels * bits / 8;
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    mt->SetGUID(MF_MT_SUBTYPE, isFloat ? MFAudioFormat_Float : MFAudioFormat_PCM);
    mt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    mt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
    mt->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, align);
    mt->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * align);
    mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    const bool ok = SUCCEEDED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mt));
    mt->Release();
    return ok;
}

inline bool pickStereoOutput(IMFSourceReader* rd, double fs, StereoConverter& conv) {
    conv.direct = true;
    if (askAudioType(rd, (UINT32)fs, 2, true)) return true;
    IMFMediaType* native = nullptr;
    if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native)) || !native) return false;
    UINT32 rate = 0, channels = 0, mask = 0;
    native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    native->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &mask);
    native->Release();
    if (rate < 8000 || channels < 1 || channels > 8) return false;
    for (const bool isFloat : {true, false}) {
        if (!askAudioType(rd, rate, channels, isFloat)) continue;
        WAVEFORMATEXTENSIBLE f = {};
        f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f.Format.nChannels = (WORD)channels;
        f.Format.nSamplesPerSec = rate;
        f.Format.wBitsPerSample = isFloat ? 32 : 16;
        f.Format.nBlockAlign = (WORD)(channels * f.Format.wBitsPerSample / 8);
        f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
        f.Format.cbSize = 22;
        f.Samples.wValidBitsPerSample = f.Format.wBitsPerSample;
        f.dwChannelMask = mask;
        f.SubFormat = isFloat ? MFAudioFormat_Float : MFAudioFormat_PCM;
        conv.setup(&f.Format, (int)fs);
        conv.direct = false;
        return true;
    }
    return false;
}

}
