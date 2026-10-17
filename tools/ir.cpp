#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <complex>
#include <string>
#include <algorithm>
#include <random>

typedef std::complex<double> cd;
static const int FS = 48000;
static const double PI = 3.14159265358979323846;

static void fft(std::vector<cd>& a, bool inv) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2 * PI / len * (inv ? 1 : -1);
        const cd wl(cos(ang), sin(ang));
        for (int i = 0; i < n; i += len) {
            cd w(1, 0);
            for (int j = 0; j < len / 2; ++j) {
                const cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inv) for (auto& x : a) x /= n;
}
static int pow2(size_t n) { int p = 1; while ((size_t)p < n) p <<= 1; return p; }

static std::vector<double> convolve(const std::vector<double>& x, const std::vector<double>& h) {
    const size_t n = x.size() + h.size() - 1;
    const int N = pow2(n);
    std::vector<cd> A(N), B(N);
    for (size_t i = 0; i < x.size(); ++i) A[i] = x[i];
    for (size_t i = 0; i < h.size(); ++i) B[i] = h[i];
    fft(A, false); fft(B, false);
    for (int i = 0; i < N; ++i) A[i] *= B[i];
    fft(A, true);
    std::vector<double> y(n);
    for (size_t i = 0; i < n; ++i) y[i] = A[i].real();
    return y;
}

struct Wav { int fs = 0; std::vector<double> L, R; };

static bool readWav(const std::string& path, Wav& w) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
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
            fmt = a; ch = c; w.fs = (int)r; bits = bp;
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) { dp = ftell(f); ds = sz; break; }
        else fseek(f, (sz + 1) & ~1u, SEEK_CUR);
    }
    if (!dp || ch < 1 || bits < 16) { fclose(f); return false; }
    fseek(f, dp, SEEK_SET);
    const int bytes = bits / 8;
    std::vector<unsigned char> raw(ds);
    const size_t got = fread(raw.data(), 1, ds, f);
    fclose(f);
    const size_t nn = got / bytes / ch;
    w.L.resize(nn); w.R.resize(nn);
    for (size_t i = 0; i < nn; ++i) {
        double acc[2] = {0, 0};
        for (int c = 0; c < ch; ++c) {
            const unsigned char* q = raw.data() + (i * ch + c) * bytes;
            double v = 0;
            if (bits == 16) { int16_t x; memcpy(&x, q, 2); v = x / 32768.0; }
            else if (bits == 24) { int32_t x = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); v = (x >> 8) / 8388608.0; }
            else if (bits == 32 && fmt == 3) { float x; memcpy(&x, q, 4); v = x; }
            else if (bits == 32) { int32_t x; memcpy(&x, q, 4); v = x / 2147483648.0; }
            if (c < 2) acc[c] = v;
        }
        w.L[i] = acc[0];
        w.R[i] = ch > 1 ? acc[1] : acc[0];
    }
    return true;
}
static bool writeWavF32(const std::string& path, const std::vector<double>& L,
                        const std::vector<double>& R, int fs) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t n = L.size();
    uint32_t d = (uint32_t)(n * 8), r = 36 + d;
    uint16_t fmt = 3, two = 2, b32 = 32, ba = 8; uint32_t sz = 16, br = fs * 8;
    fwrite("RIFF", 1, 4, f); fwrite(&r, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&two, 2, 1, f);
    fwrite(&fs, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&b32, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f);
    for (size_t i = 0; i < n; ++i) { float a = (float)L[i], b = (float)R[i]; fwrite(&a, 4, 1, f); fwrite(&b, 4, 1, f); }
    fclose(f);
    return true;
}
static bool writeWav16(const std::string& path, const std::vector<double>& x, int fs) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t n = x.size();
    uint32_t d = (uint32_t)(n * 2), r = 36 + d;
    uint16_t one = 1, b16 = 16, ba = 2, fmt = 1; uint32_t sz = 16, br = fs * 2;
    fwrite("RIFF", 1, 4, f); fwrite(&r, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&one, 2, 1, f);
    fwrite(&fs, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&b16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f);
    for (size_t i = 0; i < n; ++i) { int16_t v = (int16_t)lrint(std::max(-1.0, std::min(1.0, x[i])) * 32767.0); fwrite(&v, 2, 1, f); }
    fclose(f);
    return true;
}

static const double F1 = 20.0, F2 = 20000.0, T = 10.0, LEVEL = 0.25;

static std::vector<double> sweep() {
    const int n = (int)(T * FS);
    std::vector<double> s(n);
    const double R = log(F2 / F1);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / FS;
        s[i] = sin(2 * PI * F1 * T / R * (exp(t * R / T) - 1));
    }
    const int m = FS / 20;
    for (int i = 0; i < m; ++i) { s[i] *= (double)i / m; s[n - 1 - i] *= (double)i / m; }
    return s;
}

static std::vector<double> inverse(const std::vector<double>& s) {
    const int n = (int)s.size();
    std::vector<double> inv(n);
    const double R = log(F2 / F1);
    for (int i = 0; i < n; ++i) inv[i] = s[n - 1 - i] * exp(-((double)i / FS) * R / T);
    std::vector<double> k = convolve(s, inv);
    double pk = 0; for (double v : k) pk = std::max(pk, fabs(v));
    for (double& v : inv) v /= pk;
    return inv;
}

static double passbandGain(const std::vector<double>& s, const std::vector<double>& inv) {
    std::vector<double> k = convolve(s, inv);
    const int N = pow2(k.size());
    std::vector<cd> K(N);
    for (size_t i = 0; i < k.size(); ++i) K[i] = k[i];
    fft(K, false);
    double acc = 0; int cnt = 0;
    for (int i = 0; i <= N / 2; ++i) {
        const double f = (double)i * FS / N;
        if (f >= 100 && f <= 15000) { acc += std::abs(K[i]); ++cnt; }
    }
    return acc / std::max(1, cnt);
}

static int doProbe(const std::string& out) {
    std::vector<double> x;
    auto quiet = [&](double sec) { x.insert(x.end(), (size_t)(sec * FS), 0.0); };
    quiet(1.0);
    x.push_back(0.5); quiet(0.01);
    quiet(20.0);
    std::vector<double> s = sweep();
    for (double v : s) x.push_back(LEVEL * v);
    quiet(22.0);
    if (!writeWav16(out, x, FS)) { printf("  could not write %s\n", out.c_str()); return 1; }
    printf("  probe written: %s  (%.1f s)\n", out.c_str(), x.size() / (double)FS);
    return 0;
}

static double rmsOf(const std::vector<double>& x, size_t a, size_t b) {
    double s = 0; size_t n = 0;
    for (size_t i = a; i < b && i < x.size(); ++i) { s += x[i] * x[i]; ++n; }
    return sqrt(s / std::max<size_t>(1, n));
}

static void extendChannel(std::vector<double>& h, size_t nClean, size_t nTotal,
                          double rate, std::mt19937& rng) {
    const size_t xf = (size_t)(0.5 * FS);
    const size_t a = nClean > xf ? nClean - xf : 0;
    const size_t refN = (size_t)(0.3 * FS);
    const size_t r0 = nClean > refN ? nClean - refN : 0;

    const int NR = 16384;
    std::vector<cd> Rf(NR);
    for (size_t i = r0; i < nClean && i - r0 < (size_t)NR; ++i) Rf[i - r0] = h[i];
    fft(Rf, false);
    std::vector<double> mag(NR / 2 + 1), sm(NR / 2 + 1);
    for (int i = 0; i <= NR / 2; ++i) mag[i] = std::abs(Rf[i]);
    for (int i = 0; i <= NR / 2; ++i) {
        const double f = (double)i * FS / NR;
        const int lo = std::max(1, (int)(f / pow(2.0, 1.0 / 6.0) * NR / FS));
        const int hi = std::min(NR / 2, (int)(f * pow(2.0, 1.0 / 6.0) * NR / FS) + 1);
        double s = 0; int c = 0;
        for (int j = lo; j <= hi; ++j) { s += mag[j]; ++c; }
        sm[i] = c ? s / c : mag[i];
    }

    const size_t need = nTotal - a;
    const int N = pow2(need + FS);
    std::vector<cd> X(N);
    std::normal_distribution<double> g(0.0, 1.0);
    for (int i = 0; i < N; ++i) X[i] = g(rng);
    fft(X, false);
    for (int i = 0; i <= N / 2; ++i) {
        const double f = (double)i * FS / N;
        const double pos = f * NR / FS;
        const int k = std::min(NR / 2 - 1, (int)pos);
        const double fr = pos - k;
        const double m = sm[k] * (1 - fr) + sm[k + 1] * fr;
        X[i] *= m;
        if (i > 0 && i < N / 2) X[N - i] = std::conj(X[i]);
    }
    fft(X, true);
    std::vector<double> nz(need);
    for (size_t i = 0; i < need; ++i) nz[i] = X[i + FS].real();

    const double want = rmsOf(h, r0, nClean) * pow(10.0, rate * 0.15 / 20.0);
    const double have = rmsOf(nz, 0, refN);
    const double scale = have > 0 ? want / have : 0.0;

    for (size_t i = 0; i < need; ++i) {
        const double t = ((double)(a + i) - (double)nClean) / FS;
        nz[i] *= scale * pow(10.0, rate * t / 20.0);
    }
    if (h.size() < nTotal) h.resize(nTotal, 0.0);
    for (size_t i = a; i < nClean; ++i) {
        const double w = (double)(i - a) / (double)(nClean - a);
        h[i] = h[i] * (1 - w) + nz[i - a] * w;
    }
    for (size_t i = nClean; i < nTotal; ++i) h[i] = nz[i - a];
    h.resize(nTotal);
    const size_t fd = (size_t)(0.2 * FS);
    for (size_t i = 0; i < fd; ++i) h[nTotal - fd + i] *= 1.0 - (double)i / fd;
}

static int doExtract(const std::string& in, const std::string& out) {
    Wav w;
    if (!readWav(in, w)) {
        printf("  cannot open %s\n", in.c_str());
        printf("  That file is the recording the card makes while playing probe_haunted.wav\n");
        printf("  (see the readme). Record it first, then run this on it.\n");
        return 1;
    }
    if (w.fs != FS) {
        printf("  the recording is at %d Hz; record at 48000 Hz.\n", w.fs);
        return 1;
    }
    const size_t n = w.L.size();
    std::vector<double> mono(n);
    for (size_t i = 0; i < n; ++i) mono[i] = 0.5 * (w.L[i] + w.R[i]);
    std::vector<double> s = sweep();
    std::vector<double> inv = inverse(s);
    const double kpb = passbandGain(s, inv);
    for (double& v : s) v *= LEVEL;

    const int D = 8;
    std::vector<double> md(n / D), sd(s.size() / D);
    for (size_t i = 0; i < md.size(); ++i) { double a = 0; for (int j = 0; j < D; ++j) a += mono[i * D + j]; md[i] = a / D; }
    for (size_t i = 0; i < sd.size(); ++i) { double a = 0; for (int j = 0; j < D; ++j) a += s[i * D + j]; sd[i] = a / D; }
    std::vector<double> sr(sd.rbegin(), sd.rend());
    std::vector<double> c = convolve(md, sr);
    size_t best = 0; double bv = -1;
    for (size_t i = sd.size() - 1; i < c.size(); ++i) if (fabs(c[i]) > bv) { bv = fabs(c[i]); best = i; }
    long coarse = (long)(best - (sd.size() - 1)) * D;
    const long margin = FS;
    const long w0 = std::max(0L, coarse - margin);
    const long w1 = std::min((long)n, coarse + (long)s.size() + margin);
    std::vector<double> seg(mono.begin() + w0, mono.begin() + w1);
    std::vector<double> srf(s.rbegin(), s.rend());
    std::vector<double> cf = convolve(seg, srf);
    best = 0; bv = -1;
    for (size_t i = s.size() - 1; i < cf.size(); ++i) if (fabs(cf[i]) > bv) { bv = fabs(cf[i]); best = i; }
    const long start = w0 + (long)(best - (s.size() - 1));
    double amp = 0;
    for (long i = start; i < start + (long)s.size() && i < (long)n; ++i) amp = std::max(amp, std::max(fabs(w.L[i]), fabs(w.R[i])));
    printf("  sweep found at %.2f s, peak level in the recording %.3f\n", start / (double)FS, amp);
    if (amp > 0.98) printf("  WARNING: the recording clips. Lower the card or the recorder and record again.\n");

    const long y0 = std::max(0L, start - FS);
    const long y1 = std::min((long)n, start + (long)s.size() + 25L * FS);
    std::vector<std::vector<double>> hs(2);
    double floorMax = 0, direct = 1;
    for (int ch = 0; ch < 2; ++ch) {
        const std::vector<double>& src = ch ? w.R : w.L;
        std::vector<double> y(src.begin() + y0, src.begin() + y1);
        std::vector<double> h = convolve(y, inv);
        const long i0 = (long)s.size() + (start - y0);
        long k = i0 - FS / 20; double pk = -1;
        for (long i = i0 - FS / 20; i < i0 + FS / 20 && i < (long)h.size(); ++i)
            if (i >= 0 && fabs(h[i]) > pk) { pk = fabs(h[i]); k = i; }
        direct = h[k];
        double fl = rmsOf(h, (size_t)std::max(0L, k - 3L * FS), (size_t)std::max(0L, k - FS / 2)) / fabs(direct);
        floorMax = std::max(floorMax, fl);
        std::vector<double> r(h.begin() + k, h.end());
        for (double& v : r) v /= direct * kpb;
        for (int i = 0; i < (int)(0.004 * FS) && i < (int)r.size(); ++i) r[i] = 0.0;
        hs[ch] = r;
    }
    printf("  direct click recovered at %.3f of full scale; floor %.1f dB below it\n",
           fabs(direct), 20 * log10(floorMax + 1e-30));
    size_t len = std::min(hs[0].size(), hs[1].size());
    hs[0].resize(len); hs[1].resize(len);

    std::vector<double> m2(len);
    for (size_t i = 0; i < len; ++i) { const double v = 0.5 * (hs[0][i] + hs[1][i]); m2[i] = v * v; }
    const size_t win = FS / 5;
    std::vector<double> lvl(len, -300.0);
    { double acc = 0; size_t cnt = 0;
      for (size_t i = 0; i < len; ++i) {
          acc += m2[i]; ++cnt;
          if (i >= win) { acc -= m2[i - win]; --cnt; }
          if (i >= win / 2) lvl[i - win / 2] = 10 * log10(acc / cnt + 1e-30);
      } }
    const double thr = 20 * log10(floorMax + 1e-30) + 8.0;
    size_t lastClean = 0;
    for (size_t i = 0; i < len; ++i) if (lvl[i] > thr) lastClean = i;
    double tClean = std::max(1.0, std::min(lastClean / (double)FS, 20.0));

    double sx = 0, sy = 0, sxx = 0, sxy = 0; size_t cnt = 0;
    for (size_t i = (size_t)(0.5 * FS); i < (size_t)(tClean * FS) && i < len; i += 48) {
        const double t = (double)i / FS; sx += t; sy += lvl[i]; sxx += t * t; sxy += t * lvl[i]; ++cnt;
    }
    double rate = -3.6;
    if (cnt > 100) rate = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
    printf("  measured cleanly to %.1f s; it falls at %.2f dB/s there (RT60 %.1f s)\n",
           tClean, rate, rate < 0 ? -60.0 / rate : 0.0);
    const double total = std::min(22.0, std::max(tClean, rate < 0 ? -80.0 / rate : tClean));
    std::mt19937 rng(7);
    for (int ch = 0; ch < 2; ++ch)
        extendChannel(hs[ch], (size_t)(tClean * FS), (size_t)(total * FS), rate, rng);
    if (!writeWavF32(out, hs[0], hs[1], FS)) { printf("  could not write %s\n", out.c_str()); return 1; }
    double pk = 0; for (int ch = 0; ch < 2; ++ch) for (double v : hs[ch]) pk = std::max(pk, fabs(v));
    printf("  response written: %.1f s, peak %.3f  ->  %s\n", hs[0].size() / (double)FS, pk, out.c_str());
    double e1 = 0; for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < FS; ++i) e1 += hs[ch][i] * hs[ch][i];
    printf("  the room, against a unit direct click: %.1f dB over its first second\n", 10 * log10(e1 / (2.0 * FS) + 1e-30));
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "probe"))
        return doProbe(argc >= 3 ? argv[2] : "probe_haunted.wav");
    if (argc >= 3 && !strcmp(argv[1], "extract")) {
        std::string out = argc >= 4 ? argv[3] : "haunted_cavern_ir.wav";
        return doExtract(argv[2], out);
    }
    printf("%s - the card's own room\n\n", argv[0]);
    printf("  %s probe   [probe_haunted.wav]\n", argv[0]);
    printf("      writes the sweep for the card to play\n\n");
    printf("  %s extract card_recording.wav   [haunted_cavern_ir.wav]\n", argv[0]);
    printf("      turns the recording of it into the response OctopusEAX loads\n");
    return 1;
}
