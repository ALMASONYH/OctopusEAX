#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static bool decode(const wchar_t* path, std::vector<float>& L, std::vector<float>& R, int& rate) {
    IMFSourceReader* rd = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path, nullptr, &rd))) return false;
    IMFMediaType* want = nullptr;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want))) {
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (FAILED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want))) { want->Release(); rd->Release(); return false; }
    }
    want->Release();
    IMFMediaType* got = nullptr;
    if (FAILED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) { rd->Release(); return false; }
    UINT32 ch = 0, sr = 0, bits = 0; GUID sub = {};
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch); got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    got->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits); got->GetGUID(MF_MT_SUBTYPE, &sub);
    got->Release();
    const bool isFloat = (sub == MFAudioFormat_Float);
    if (ch < 1 || sr < 8000) { rd->Release(); return false; }
    rate = (int)sr;
    L.clear(); R.clear();
    for (;;) {
        DWORD flags = 0; IMFSample* smp = nullptr; LONGLONG ts = 0;
        if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &smp))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (smp) smp->Release(); break; }
        if (!smp) continue;
        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))) {
            BYTE* p = nullptr; DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                if (isFloat) {
                    const float* f = (const float*)p; const size_t n = len / sizeof(float) / ch;
                    for (size_t i = 0; i < n; ++i) { L.push_back(f[i * ch]); R.push_back(f[i * ch + (ch > 1 ? 1 : 0)]); }
                } else if (bits == 16) {
                    const short* s = (const short*)p; const size_t n = len / 2 / ch;
                    for (size_t i = 0; i < n; ++i) { L.push_back(s[i * ch] / 32768.f); R.push_back(s[i * ch + (ch > 1 ? 1 : 0)] / 32768.f); }
                } else if (bits == 32) {
                    const int* s = (const int*)p; const size_t n = len / 4 / ch;
                    for (size_t i = 0; i < n; ++i) { L.push_back((float)(s[i * ch] / 2147483648.0)); R.push_back((float)(s[i * ch + (ch > 1 ? 1 : 0)] / 2147483648.0)); }
                }
                buf->Unlock();
            }
            buf->Release();
        }
        smp->Release();
    }
    rd->Release();
    return !L.empty();
}

static std::vector<float> resample(const std::vector<float>& x, int from, int to) {
    if (from == to) return x;
    const double ratio = (double)from / to, cutoff = std::min(1.0, (double)to / from) * 0.95;
    const int taps = 32; const size_t n = (size_t)((double)x.size() * to / from);
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i) {
        const double c = i * ratio; const long long k0 = (long long)std::floor(c);
        double acc = 0;
        for (long long k = k0 - taps + 1; k <= k0 + taps; ++k) {
            if (k < 0 || k >= (long long)x.size()) continue;
            const double t = (double)k - c, a = 3.14159265358979323846 * t * cutoff;
            const double s = (std::fabs(t) < 1e-9) ? 1.0 : std::sin(a) / a;
            const double w = 0.5 + 0.5 * std::cos(3.14159265358979323846 * t / taps);
            acc += x[(size_t)k] * s * w * cutoff;
        }
        y[i] = (float)acc;
    }
    return y;
}

static bool writeWav(const wchar_t* p, const std::vector<float>& l, const std::vector<float>& r, int fs) {
    FILE* f = _wfopen(p, L"wb"); if (!f) return false;
    const unsigned n = (unsigned)l.size(), ds = n * 4, rs = 36 + ds;
    const unsigned short one = 1, ch = 2, ba = 4, bps = 16; const unsigned br = fs * 4, fsz = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fsz, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&fs, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
    for (unsigned i = 0; i < n; ++i) {
        float a = l[i], b = r[i]; if (a > 1) a = 1; if (a < -1) a = -1; if (b > 1) b = 1; if (b < -1) b = -1;
        const short s0 = (short)(a * 32767), s1 = (short)(b * 32767); fwrite(&s0, 2, 1, f); fwrite(&s1, 2, 1, f);
    }
    fclose(f); return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { printf("usage: %ls <in> <out.wav> [rate]\n", argv[0]); return 1; }
    const int rate = argc > 3 ? _wtoi(argv[3]) : 48000;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) { printf("Media Foundation not available\n"); return 1; }
    std::vector<float> L, R; int sr = 0;
    if (!decode(argv[1], L, R, sr)) { printf("cannot decode the input\n"); MFShutdown(); return 1; }
    std::vector<float> l = resample(L, sr, rate), r = resample(R, sr, rate);
    float pk = 0; for (size_t i = 0; i < l.size(); ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
    if (!writeWav(argv[2], l, r, rate)) { printf("cannot write the output\n"); MFShutdown(); return 1; }
    printf("decoded %.2f s at %d Hz -> %d Hz, peak %.2f dBFS\n", (double)L.size() / sr, sr, rate, 20 * std::log10(pk + 1e-9));
    MFShutdown(); CoUninitialize();
    return 0;
}
