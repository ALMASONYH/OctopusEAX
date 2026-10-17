#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

typedef std::vector<double> vd;
typedef std::complex<double> cd;
static const int FS = 48000;
static const double PI = 3.14159265358979323846;

struct Wav { vd L, R; int fs = 0; };
static bool readWav(const char* path, Wav& w) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    char id[4]; unsigned sz;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) { fclose(f); return false; }
    fread(&sz, 4, 1, f); fread(id, 1, 4, f);
    int ch = 0, bits = 0; long dp = 0; unsigned ds = 0;
    while (fread(id, 1, 4, f) == 4) {
        if (fread(&sz, 4, 1, f) != 1) break;
        if (!memcmp(id, "fmt ", 4)) {
            unsigned short tag, c, al, bp; unsigned r, b;
            fread(&tag, 2, 1, f); fread(&c, 2, 1, f); fread(&r, 4, 1, f); fread(&b, 4, 1, f); fread(&al, 2, 1, f); fread(&bp, 2, 1, f);
            ch = c; bits = bp; w.fs = (int)r; fseek(f, (long)sz - 16, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) { dp = ftell(f); ds = sz; break; }
        else fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR);
    }
    if (!dp || bits != 16 || ch < 1) { fclose(f); return false; }
    fseek(f, dp, SEEK_SET);
    std::vector<short> raw(ds / 2);
    size_t got = fread(raw.data(), 2, raw.size(), f); fclose(f);
    const size_t n = got / ch;
    w.L.resize(n); w.R.resize(n);
    for (size_t i = 0; i < n; ++i) {
        w.L[i] = raw[i * ch] / 32768.0;
        w.R[i] = raw[i * ch + (ch > 1 ? 1 : 0)] / 32768.0;
    }
    return true;
}

static void fft(std::vector<cd>& a, bool inv) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * PI / (double)len * (inv ? 1 : -1);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                const cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl;
            }
        }
    }
    if (inv) for (auto& v : a) v /= (double)n;
}
static size_t pow2at(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }
static vd hann(int N) { vd h(N); for (int i = 0; i < N; ++i) h[i] = 0.5 - 0.5 * std::cos(2 * PI * i / (N - 1)); return h; }

static vd welch(const double* y, size_t n, int N, int hop, const double* y2 = nullptr) {
    vd acc(N / 2 + 1, 0.0), h = hann(N); int k = 0;
    std::vector<cd> buf(N);
    const double* srcs[2] = {y, y2};
    for (int s = 0; s < 2; ++s) {
        if (!srcs[s]) continue;
        for (size_t i = 0; i + N < n; i += hop) {
            for (int j = 0; j < N; ++j) buf[j] = cd(srcs[s][i + j] * h[j], 0.0);
            fft(buf, false);
            for (int j = 0; j <= N / 2; ++j) acc[j] += std::norm(buf[j]);
            ++k;
        }
    }
    for (auto& v : acc) v /= (k > 0 ? k : 1);
    return acc;
}
static double db(double p) { return 10.0 * std::log10(p + 1e-30); }

struct Bq { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
static Bq lp2(double fc, double q, double fs) {
    Bq s; const double w = 2 * PI * fc / fs, cs = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    s.b0 = (1 - cs) / 2 / a0; s.b1 = (1 - cs) / a0; s.b2 = s.b0; s.a1 = -2 * cs / a0; s.a2 = (1 - al) / a0; return s;
}
static Bq hp2(double fc, double q, double fs) {
    Bq s; const double w = 2 * PI * fc / fs, cs = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    s.b0 = (1 + cs) / 2 / a0; s.b1 = -(1 + cs) / a0; s.b2 = s.b0; s.a1 = -2 * cs / a0; s.a2 = (1 - al) / a0; return s;
}
static void filt(vd& y, const Bq& s) {
    double z1 = 0, z2 = 0;
    for (auto& x : y) { const double v = s.b0 * x + z1; z1 = s.b1 * x - s.a1 * v + z2; z2 = s.b2 * x - s.a2 * v; x = v; }
}
static void filtfilt(vd& y, const std::vector<Bq>& secs) {
    for (const auto& s : secs) filt(y, s);
    std::reverse(y.begin(), y.end());
    for (const auto& s : secs) filt(y, s);
    std::reverse(y.begin(), y.end());
}
static vd bp(const vd& x, double lo, double hi, double fs = FS) {
    vd y = x; std::vector<Bq> s;
    s.push_back(hp2(lo, 0.54119610, fs)); s.push_back(hp2(lo, 1.30656296, fs));
    const double h = std::min(hi, fs * 0.48);
    s.push_back(lp2(h, 0.54119610, fs)); s.push_back(lp2(h, 1.30656296, fs));
    filtfilt(y, s); return y;
}
static vd lowpass2(const vd& x, double fc, double fs) { vd y = x; filtfilt(y, {lp2(fc, 0.70710678, fs)}); return y; }
static vd bandpass2(const vd& x, double lo, double hi, double fs) { vd y = x; filtfilt(y, {hp2(lo, 0.70710678, fs), lp2(hi, 0.70710678, fs)}); return y; }

static size_t clampi(long long i, size_t n) { return i < 0 ? 0 : (i > (long long)n ? n : (size_t)i); }
static double lv(const vd& y, double a, double b) {
    const size_t i0 = clampi((long long)(a * FS), y.size()), i1 = clampi((long long)(b * FS), y.size());
    if (i1 <= i0) return -300.0;
    double s = 0; for (size_t i = i0; i < i1; ++i) s += y[i] * y[i];
    return 10 * std::log10(s / (double)(i1 - i0) + 1e-20);
}
static double meanv(const vd& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }
static double rms(const vd& v) { double s = 0; for (double x : v) s += x * x; return v.empty() ? 0 : std::sqrt(s / v.size()); }
static double pearson(const vd& a, const vd& b) {
    const size_t n = std::min(a.size(), b.size()); double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; } ma /= n; mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; ++i) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb); }
    return sab / (std::sqrt(saa * sbb) + 1e-20);
}
static double percentile(vd v, double p) {
    if (v.empty()) return 0; std::sort(v.begin(), v.end());
    const double k = p / 100.0 * (v.size() - 1); const size_t lo = (size_t)k; const double f = k - lo;
    return lo + 1 < v.size() ? v[lo] * (1 - f) + v[lo + 1] * f : v[lo];
}
static double slope(const vd& x, const vd& y) {
    const size_t n = std::min(x.size(), y.size()); double mx = 0, my = 0;
    for (size_t i = 0; i < n; ++i) { mx += x[i]; my += y[i]; } mx /= n; my /= n;
    double sxy = 0, sxx = 0;
    for (size_t i = 0; i < n; ++i) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) * (x[i] - mx); }
    return sxx > 0 ? sxy / sxx : 0.0;
}
static vd slice(const vd& v, double a, double b) {
    const size_t i0 = clampi((long long)(a * FS), v.size()), i1 = clampi((long long)(b * FS), v.size());
    return vd(v.begin() + i0, v.begin() + std::max(i0, i1));
}
static vd envDb(const vd& y, double win) {
    const int W = std::max(1, (int)(win * FS)); const size_t n = y.size();
    vd c(n + 1, 0.0); for (size_t i = 0; i < n; ++i) c[i + 1] = c[i] + y[i] * y[i];
    vd e(n);
    for (size_t i = 0; i < n; ++i) {
        const long long a = (long long)i - (W - 1) / 2, b = a + W;
        const size_t ia = clampi(a, n), ib = clampi(b, n);
        e[i] = 10 * std::log10((c[ib] - c[ia]) / W + 1e-20);
    }
    return e;
}

static const int NCOL = 34;
static double colBand(int i) { return 125.0 * std::pow(2.0, i / 6.0); }
static const double GROUPS[8][2] = {{125, 220}, {220, 350}, {350, 560}, {560, 900}, {900, 1400}, {1400, 2200}, {2200, 3500}, {3500, 5700}};
static const char* GNAMES[8] = {"160", "250", "400", "630", "1k", "1k6", "2k5", "4k"};
struct Band { const char* name; double lo, hi; };
static const Band DEC_BANDS[4] = {{"250-600", 250, 600}, {"600-1.5k", 600, 1500}, {"1.5-3k", 1500, 3000}, {"3-6k", 3000, 6000}};
static const Band SW_BANDS[4] = {{"300-600", 300, 600}, {"600-1.5k", 600, 1500}, {"1.5-3k", 1500, 3000}, {"3-6k", 3000, 6000}};
static const Band SLAP_BANDS[4] = {{"low", 60, 600}, {"mid", 600, 1500}, {"hi", 1500, 4000}, {"top", 4000, 8000}};
static const double WINDOWS[3][2] = {{0.2, 0.8}, {0.8, 2.0}, {2.0, 4.5}};
static const char* WNAMES[3] = {"early", "mid", "late"};
static const int NRESP = 13;
static const double EDGES[NRESP][2] = {{125, 180}, {180, 250}, {250, 355}, {355, 500}, {500, 710}, {710, 1000}, {1000, 1400}, {1400, 2000}, {2000, 2800}, {2800, 4000}, {4000, 5600}, {5600, 8000}, {8000, 11000}};
static const char* RNAMES[NRESP] = {"150", "210", "300", "420", "600", "840", "1k2", "1k7", "2k4", "3k4", "4k8", "6k7", "9k5"};
static const double RWIN[3][2] = {{0.1, 0.5}, {0.5, 1.5}, {1.5, 3.0}};
static const char* RWNAMES[3] = {"early", "body", "late"};

static vd colour(const vd& mid, double se, double t0, double t1) {
    const vd seg = slice(mid, se + t0, se + t1);
    const int N = 8192; const vd P = welch(seg.data(), seg.size(), N, N / 4);
    vd out(NCOL);
    for (int i = 0; i < NCOL; ++i) {
        const double fc = colBand(i), flo = fc / std::pow(2.0, 1 / 12.0), fhi = fc * std::pow(2.0, 1 / 12.0);
        double s = 0; int k = 0;
        for (int j = 0; j <= N / 2; ++j) { const double f = (double)j * FS / N; if (f >= flo && f <= fhi) { s += P[j]; ++k; } }
        out[i] = db(k ? s / k : 0);
    }
    return out;
}
static void normalise(vd& c) { const double m = meanv(c); for (auto& v : c) v -= m; }
static vd grouped(const vd& d) {
    vd g(8);
    for (int k = 0; k < 8; ++k) { double s = 0; int n = 0; for (int i = 0; i < NCOL; ++i) { const double f = colBand(i); if (f >= GROUPS[k][0] && f < GROUPS[k][1]) { s += d[i]; ++n; } } g[k] = n ? s / n : 0; }
    return g;
}
static double colRms(const vd& d) { double s = 0; for (int i = 0; i < NCOL - 2; ++i) s += d[i] * d[i]; return std::sqrt(s / (NCOL - 2)); }

static vd respBands(const Wav& w, double a, double b) {
    const vd l = slice(w.L, a, b), r = slice(w.R, a, b);
    const int N = 4096; const vd P = welch(l.data(), l.size(), N, N / 2, r.data());
    vd out(NRESP);
    for (int i = 0; i < NRESP; ++i) {
        double s = 0; int k = 0;
        for (int j = 0; j <= N / 2; ++j) { const double f = (double)j * FS / N; if (f >= EDGES[i][0] && f < EDGES[i][1]) { s += P[j]; ++k; } }
        out[i] = db(k ? s / k : 0);
    }
    return out;
}
static vd roughness(const Wav& w, double se) {
    const vd l = slice(w.L, se + 0.5, se + 2.5), r = slice(w.R, se + 0.5, se + 2.5);
    const int N = 16384; const vd P = welch(l.data(), l.size(), N, N / 4, r.data());
    vd Ldb(P.size()); for (size_t i = 0; i < P.size(); ++i) Ldb[i] = db(P[i]);
    const double regions[3][2] = {{500, 1500}, {1500, 4000}, {4000, 8000}};
    vd out;
    for (int rg = 0; rg < 3; ++rg) {
        vd fine;
        for (double c = regions[rg][0]; c < regions[rg][1]; c *= std::pow(2.0, 1 / 48.0)) {
            const double flo = c / std::pow(2.0, 1 / 96.0), fhi = c * std::pow(2.0, 1 / 96.0); double s = 0; int k = 0;
            for (size_t j = 0; j < Ldb.size(); ++j) { const double f = (double)j * FS / N; if (f >= flo && f < fhi) { s += Ldb[j]; ++k; } }
            fine.push_back(k ? s / k : (fine.empty() ? 0 : fine.back()));
        }
        vd dev(fine.size());
        for (size_t i = 0; i < fine.size(); ++i) {
            const size_t a = i >= 4 ? i - 4 : 0, b = std::min(fine.size(), i + 5); double s = 0;
            for (size_t j = a; j < b; ++j) s += fine[j]; dev[i] = fine[i] - s / (b - a);
        }
        const double m = meanv(dev); double v = 0; for (double x : dev) v += (x - m) * (x - m);
        out.push_back(std::sqrt(v / std::max<size_t>(1, dev.size())));
    }
    return out;
}

struct Swing { double rmsDb, rate, line; };
static Swing swingOf(const vd& band, double se) {
    const vd y = slice(band, se + 0.4, se + 2.9);
    const size_t n = y.size(), N = pow2at(n);
    std::vector<cd> a(N, cd(0)); for (size_t i = 0; i < n; ++i) a[i] = cd(y[i], 0);
    fft(a, false);
    for (size_t k = 1; k < N / 2; ++k) a[k] *= 2.0;
    for (size_t k = N / 2 + 1; k < N; ++k) a[k] = cd(0);
    fft(a, true);
    vd env(n); for (size_t i = 0; i < n; ++i) env[i] = std::abs(a[i]);
    env = lowpass2(env, 120.0, FS);
    vd e(n), t(n);
    for (size_t i = 0; i < n; ++i) { e[i] = 20 * std::log10(std::max(env[i], 1e-9)); t[i] = (double)i; }
    const double k = slope(t, e), m = meanv(e), mt = meanv(t);
    for (size_t i = 0; i < n; ++i) e[i] -= m + k * (t[i] - mt);
    vd d; for (size_t i = 0; i < n; i += 48) d.push_back(e[i]);
    const double fsd = FS / 48.0;
    const vd mband = bandpass2(d, 18.0, 30.0, fsd);
    const size_t M = d.size(), NF = pow2at(M);
    std::vector<cd> s(NF, cd(0)); const vd h = hann((int)M);
    for (size_t i = 0; i < M; ++i) s[i] = cd(d[i] * h[i], 0);
    fft(s, false);
    double pk = 0, pkf = 0, lineMax = 0; vd med;
    for (size_t j = 0; j <= NF / 2; ++j) {
        const double f = (double)j * fsd / NF, mag = std::abs(s[j]);
        if (f >= 5 && f <= 40 && mag > pk) { pk = mag; pkf = f; }
        if (f >= 22.5 && f <= 25.5 && mag > lineMax) lineMax = mag;
        if (f >= 10 && f <= 40) med.push_back(mag);
    }
    Swing sw; sw.rmsDb = rms(mband); sw.rate = pkf;
    sw.line = 20 * std::log10(lineMax / (percentile(med, 50) + 1e-12));
    return sw;
}

static vd autocorr(const vd& y, size_t k) {
    const size_t N = pow2at(2 * y.size());
    std::vector<cd> a(N, cd(0)); for (size_t i = 0; i < y.size(); ++i) a[i] = cd(y[i], 0);
    fft(a, false); for (auto& v : a) v = cd(std::norm(v), 0); fft(a, true);
    vd r(k); for (size_t i = 0; i < k; ++i) r[i] = a[i].real(); return r;
}
static vd whiten(const vd& y, int p = 64) {
    vd r = autocorr(y, p + 2); r[0] *= 1.0 + 1e-6;
    vd a(p + 1, 0.0), tmp(p + 1); a[0] = 1.0; double err = r[0];
    for (int i = 1; i <= p; ++i) {
        double acc = r[i]; for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
        const double kk = -acc / err;
        tmp = a; for (int j = 1; j < i; ++j) a[j] = tmp[j] + kk * tmp[i - j];
        a[i] = kk; err *= (1.0 - kk * kk);
    }
    vd out(y.size());
    for (size_t n = 0; n < y.size(); ++n) { double s = y[n]; for (int j = 1; j <= p && j <= (int)n; ++j) s += a[j] * y[n - j]; out[n] = s; }
    return out;
}
struct Rep { double floorv, mainv, lag, spur, slap, slaplag; vd curve; };
static Rep repOf(const Wav& w, double se) {
    const size_t p = clampi((long long)((se + 0.2) * FS), w.L.size()), q = std::min(p + (size_t)(3.5 * FS), w.L.size());
    vd y(q - p); for (size_t i = 0; i < y.size(); ++i) y[i] = w.L[p + i] + w.R[p + i];
    const double r0 = rms(y) + 1e-20; for (auto& v : y) v /= r0;
    y = whiten(y);
    const size_t K = (size_t)(0.75 * FS);
    vd r = autocorr(y, K); const double n0 = r[0]; for (auto& v : r) v = std::fabs(v / n0);
    const vd k = hann(97); double ks = 0; for (double v : k) ks += v;
    vd s(K);
    for (size_t i = 0; i < K; ++i) { double acc = 0; for (int j = 0; j < 97; ++j) { const long long idx = (long long)i + j - 48; if (idx >= 0 && idx < (long long)K) acc += r[idx] * k[j]; } s[i] = acc / ks; }
    auto at = [&](double t) { return (size_t)(t * FS); };
    Rep rp; rp.curve.assign(s.begin() + at(0.03), s.begin() + at(0.72));
    rp.floorv = percentile(rp.curve, 80) * 1000.0;
    auto maxIn = [&](double a, double b, double& lag) { double m = -1; size_t im = at(a); for (size_t i = at(a); i < at(b); ++i) if (s[i] > m) { m = s[i]; im = i; } lag = im * 1000.0 / FS; return m; };
    const double fl = rp.floorv / 1000.0;
    rp.mainv = maxIn(0.25, 0.40, rp.lag) / fl; double dummy; rp.spur = maxIn(0.06, 0.25, dummy) / fl; rp.slap = maxIn(0.44, 0.53, rp.slaplag) / fl;
    return rp;
}
struct Peak { double lag, x; };
static std::vector<Peak> peaksOf(const Rep& rp) {
    const vd& c = rp.curve; const double fl = rp.floorv / 1000.0;
    std::vector<size_t> idx;
    for (size_t i = 1; i + 1 < c.size(); ++i) if (c[i] > c[i - 1] && c[i] >= c[i + 1] && c[i] > 1.8 * fl) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return c[a] > c[b]; });
    std::vector<Peak> out;
    for (size_t i : idx) {
        const double lag = 30.0 + i * 1000.0 / FS; bool ok = true;
        for (const auto& o : out) if (std::fabs(lag - o.lag) <= 12.0) ok = false;
        if (ok) out.push_back({lag, c[i] / fl});
        if (out.size() >= 7) break;
    }
    std::sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.lag < b.lag; });
    return out;
}

struct SlapB { double sideDb, lDb, corr, lagMs; };
static SlapB slapBand(const vd& v, const vd& sS, const vd& sL) {
    auto dotAt = [&](const vd& sig, size_t lag, double& saa, double& svv) {
        const size_t n = std::min(v.size(), sig.size() > lag ? sig.size() - lag : 0); double sav = 0; saa = 0; svv = 0;
        for (size_t i = 0; i < n; ++i) { sav += sig[lag + i] * v[i]; saa += sig[lag + i] * sig[lag + i]; svv += v[i] * v[i]; }
        return sav;
    };
    double best = 0, bestAbs = -1; size_t bestLag = 0;
    for (size_t lag = (size_t)(0.455 * FS); lag < (size_t)(0.515 * FS); lag += 3) {
        double saa, svv; const double sav = dotAt(sS, lag, saa, svv);
        const double c = sav / (std::sqrt(saa * svv) + 1e-20);
        if (std::fabs(c) > bestAbs) { bestAbs = std::fabs(c); best = c; bestLag = lag; }
    }
    double saa, svv;
    const double savS = dotAt(sS, bestLag, saa, svv), gs = savS / (svv + 1e-20);
    const double savL = dotAt(sL, bestLag, saa, svv), gl = savL / (svv + 1e-20);
    return {20 * std::log10(std::fabs(gs) + 1e-9), 20 * std::log10(std::fabs(gl) + 1e-9), best, bestLag * 1000.0 / FS};
}

struct Profile {
    vd col, col3[3], resp[3], respFloor, rough, dec, dec8, width, env, tail, t60;
    Swing swing[4]; Rep rep; SlapB slap[4];
    double wet, wet2, wetLR, floorOk;
};
static bool gFast = false;
static Profile profileOf(const Wav& w, double se) {
    Profile p; const size_t n = w.L.size();
    vd mid(n); for (size_t i = 0; i < n; ++i) mid[i] = 0.5 * (w.L[i] + w.R[i]);
    const double sp = lv(mid, se - 0.6, se - 0.05);
    p.col = colour(mid, se, 0.3, 2.5);
    for (int k = 0; k < 3; ++k) p.col3[k] = colour(mid, se, WINDOWS[k][0], WINDOWS[k][1]);
    const vd bv = respBands(w, se - 0.7, se - 0.05);
    for (int k = 0; k < 3; ++k) { p.resp[k] = respBands(w, se + RWIN[k][0], se + RWIN[k][1]); for (int i = 0; i < NRESP; ++i) p.resp[k][i] -= bv[i]; }
    {
        const vd hf = bp(mid, 1500, 6000); const vd e = envDb(hf, 0.01); double mx = -1e9; for (double v : e) mx = std::max(mx, v);
        size_t s0 = 0; for (size_t i = 0; i < e.size(); ++i) if (e[i] > mx - 30) { s0 = i; break; }
        const double t0 = (double)s0 / FS; p.floorOk = t0 > 0.45 ? 1.0 : 0.0;
        p.respFloor.assign(NRESP, -99.0);
        if (p.floorOk > 0) { p.respFloor = respBands(w, 0.05, t0 - 0.05); for (int i = 0; i < NRESP; ++i) p.respFloor[i] -= bv[i]; }
    }
    p.rough = roughness(w, se);
    vd pw(n); for (size_t i = 0; i < n; ++i) pw[i] = w.L[i] * w.L[i] + w.R[i] * w.R[i];
    auto pwLv = [&](double a, double b) { const size_t i0 = clampi((long long)(a * FS), n), i1 = clampi((long long)(b * FS), n); double s = 0; for (size_t i = i0; i < i1; ++i) s += pw[i]; return 10 * std::log10(s / std::max<size_t>(1, i1 - i0) + 1e-20); };
    const double spLR = pwLv(se - 0.6, se - 0.05);
    p.wet = lv(mid, se + 0.4, se + 1.0) - sp; p.wet2 = lv(mid, se + 1.5, se + 2.5) - sp; p.wetLR = pwLv(se + 0.4, se + 1.0) - spLR;
    auto slopeOf = [&](const vd& y) {
        const vd e = envDb(y, 0.05); const size_t a = clampi((long long)((se + 0.5) * FS), n), b = clampi((long long)((se + 3.5) * FS), n);
        vd t(b - a), v(b - a); for (size_t i = a; i < b; ++i) { t[i - a] = (double)i / FS; v[i - a] = e[i]; } return slope(t, v);
    };
    p.dec.push_back(slopeOf(mid));
    for (int k = 0; k < 4; ++k) p.dec.push_back(slopeOf(bp(mid, DEC_BANDS[k].lo, DEC_BANDS[k].hi)));
    for (int g = 0; g < 8; ++g) p.dec8.push_back(slopeOf(bp(mid, GROUPS[g][0], GROUPS[g][1])));
    {
        vd m2(n); for (size_t i = 0; i < n; ++i) m2[i] = w.L[i] + w.R[i];
        const double end = std::min(se + 4.2, (double)n / FS - 0.2);
        for (int k = 0; k < 4; ++k) {
            const vd y = bp(m2, DEC_BANDS[k].lo, DEC_BANDS[k].hi); vd ts, ls;
            for (double x = se + 0.4; x + 0.1 <= end; x += 0.1) { ts.push_back(x); ls.push_back(lv(y, x, x + 0.1)); }
            const double kk = slope(ts, ls); p.t60.push_back(kk < 0 ? 60.0 / -kk : 99.0);
        }
    }
    { const double ref = pwLv(se - 0.5, se); for (int i = 0; i < 9; ++i) p.tail.push_back(pwLv(se + 0.5 * i, se + 0.5 * i + 0.5) - ref); }
    for (int i = 0; i < 16; ++i) p.env.push_back(lv(mid, se + 0.05 * i, se + 0.05 * i + 0.05) - sp);
    p.width.assign(4, 0.0);
    for (int k = 0; k < 4; ++k) { p.swing[k] = Swing{0, 0, 0}; p.slap[k] = SlapB{0, 0, 0, 0}; }
    p.rep = Rep{0, 0, 0, 0, 0, 0, vd(1, 0.0)};
    if (gFast) return p;
    for (int k = 0; k < 4; ++k) p.swing[k] = swingOf(bp(mid, SW_BANDS[k].lo, SW_BANDS[k].hi), se);
    p.rep = repOf(w, se);
    {
        vd v0(mid.begin(), mid.begin() + clampi((long long)(se * FS), n)), side(n);
        for (size_t i = 0; i < n; ++i) side[i] = w.L[i] - w.R[i];
        for (int k = 0; k < 4; ++k) p.slap[k] = slapBand(bp(v0, SLAP_BANDS[k].lo, SLAP_BANDS[k].hi), bp(side, SLAP_BANDS[k].lo, SLAP_BANDS[k].hi), bp(w.L, SLAP_BANDS[k].lo, SLAP_BANDS[k].hi));
    }
    { const vd l = slice(w.L, se + 0.4, se + 2.4), r = slice(w.R, se + 0.4, se + 2.4); for (int k = 0; k < 4; ++k) p.width[k] = pearson(bp(l, DEC_BANDS[k].lo, DEC_BANDS[k].hi), bp(r, DEC_BANDS[k].lo, DEC_BANDS[k].hi)); }
    return p;
}

static double startOf(const vd& mid);

static int lagMode(const char* cardPath, const char* wetPath) {
    Wav c; if (!readWav(cardPath, c) || c.fs != FS) { printf("cannot read %s\n", cardPath); return 1; }
    const size_t n = c.L.size(); vd cm(n), cs(n);
    for (size_t i = 0; i < n; ++i) { cm[i] = 0.5 * (c.L[i] + c.R[i]); cs[i] = c.L[i] - c.R[i]; }
    const double t0 = startOf(cm);
    printf("== %s  first consonant at %.3f s\n", cardPath, t0);
    auto cellDb = [&](const vd& y, double a, double b) { return lv(y, a, b); };
    const double ref = lv(cm, t0 + 0.1, t0 + 0.4);
    printf("   t ms        "); for (int k = -15; k < 25; ++k) printf("%5d", k * 10); printf("\n");
    printf("   voice (mid) "); for (int k = -15; k < 25; ++k) printf("%5.0f", cellDb(cm, t0 + k * 0.01, t0 + (k + 1) * 0.01) - ref); printf("\n");
    printf("   card room   "); for (int k = -15; k < 25; ++k) printf("%5.0f", cellDb(cs, t0 + k * 0.01, t0 + (k + 1) * 0.01) + 3.0 - ref); printf("\n");
    Wav w; bool haveWet = wetPath && readWav(wetPath, w) && w.fs == FS;
    vd wp;
    if (haveWet) {
        const size_t m = std::min(w.L.size(), n); wp.assign(m, 0.0);
        for (size_t i = 0; i < m; ++i) wp[i] = std::sqrt(w.L[i] * w.L[i] + w.R[i] * w.R[i]);
        printf("   ours room   "); for (int k = -15; k < 25; ++k) printf("%5.0f", cellDb(wp, t0 + k * 0.01, t0 + (k + 1) * 0.01) - ref); printf("\n");
    }
    auto xcorr = [&](const vd& room, const vd& voice, double a, double b, const char* name) {
        const size_t i0 = clampi((long long)(a * FS), room.size()), i1 = clampi((long long)(b * FS), room.size());
        printf("   %s corr vs lag ms (room leads when negative), %.1f-%.1f s:\n     ", name, a, b);
        double best = 0, bestLag = 0;
        for (int lagMs = -60; lagMs <= 60; lagMs += 4) {
            const long long lag = (long long)(lagMs * FS / 1000); double sr = 0, sv = 0, srv = 0;
            for (size_t i = i0; i < i1; ++i) { const long long j = (long long)i - lag; if (j < 0 || j >= (long long)voice.size()) continue; sr += room[i] * room[i]; sv += voice[j] * voice[j]; srv += room[i] * voice[j]; }
            const double cc = srv / (std::sqrt(sr * sv) + 1e-20);
            printf("%+d:%.2f ", lagMs, cc);
            if (std::fabs(cc) > std::fabs(best)) { best = cc; bestLag = lagMs; }
        }
        printf("\n     strongest at %+.0f ms (%.2f)\n", bestLag, best);
    };
    xcorr(cs, cm, t0 - 0.05, t0 + 0.5, "card side/mid");
    xcorr(cs, cm, t0 - 0.05, t0 + 2.0, "card side/mid");
    if (haveWet) {
        const size_t m = w.L.size(); vd ws(m); for (size_t i = 0; i < m; ++i) ws[i] = w.L[i] - w.R[i];
        xcorr(w.L, cm, t0 - 0.05, t0 + 0.5, "ours wetL/mid"); xcorr(ws, cm, t0 - 0.05, t0 + 0.5, "ours side/mid");
    }
    return 0;
}

static int diffMode(const char* a, const char* b) {
    Wav x, y; if (!readWav(a, x) || !readWav(b, y)) { printf("cannot read %s or %s\n", a, b); return 1; }
    const size_t n = std::min(x.L.size(), y.L.size()); long long mx = 0, nz = 0; double s = 0;
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 2; ++c) {
        const double d = ((c ? x.R[i] : x.L[i]) - (c ? y.R[i] : y.L[i])) * 32768.0; const long long q = (long long)std::llround(std::fabs(d));
        if (q > mx) mx = q; if (q) ++nz; s += d * d;
    }
    printf("%s vs %s: %zu / %zu samples, max |diff| %lld LSB, rms diff %.4f LSB, %lld samples differ%s\n", a, b, x.L.size(), y.L.size(), mx, std::sqrt(s / std::max<size_t>(1, 2 * n)), nz, (mx == 0 && x.L.size() == y.L.size()) ? " - IDENTICAL" : "");
    return 0;
}

static double startOf(const vd& mid) {
    const vd e = envDb(bp(mid, 1500, 6000), 0.01); double mx = -1e9; for (double v : e) mx = std::max(mx, v);
    for (size_t i = 0; i < e.size(); ++i) if (e[i] > mx - 30) return (double)i / FS; return 0.0;
}
static double maxCorr(const vd& room, const vd& voice, size_t a, size_t b) {
    double best = 0;
    for (size_t lag = 0; lag < (size_t)(0.08 * FS); lag += 12) {
        if (a < lag) break; double sr = 0, sv = 0, srv = 0;
        for (size_t i = a; i < b && i < room.size(); ++i) { const double r = room[i], v = voice[i - lag]; sr += r * r; sv += v * v; srv += r * v; }
        best = std::max(best, std::fabs(srv / (std::sqrt(sr * sv) + 1e-20)));
    }
    return best;
}
struct Onset { bool ok = false; double t0 = 0, rmsCells = 0; vd cardCells, oursCells; double fc1, fc2, fo1[3], fo2[3]; };
static Onset onsetOf(const Wav& c, const Wav& wet) {
    Onset o; const size_t n = c.L.size();
    vd cm(n), cs(n); for (size_t i = 0; i < n; ++i) { cm[i] = 0.5 * (c.L[i] + c.R[i]); cs[i] = c.L[i] - c.R[i]; }
    o.t0 = startOf(cm);
    auto cellsOf = [&](const vd& pw, double ref) { vd out; for (int k = 0; k < 22; ++k) { const size_t i0 = clampi((long long)((o.t0 + k * 0.02) * FS), pw.size()), i1 = clampi((long long)((o.t0 + (k + 1) * 0.02) * FS), pw.size()); double s = 0; for (size_t i = i0; i < i1; ++i) s += pw[i]; out.push_back(10 * std::log10(s / std::max<size_t>(1, i1 - i0) + 1e-20) - ref); } return out; };
    vd cm2(n); for (size_t i = 0; i < n; ++i) cm2[i] = cm[i] * cm[i];
    double ref; { const size_t i0 = (size_t)((o.t0 + 0.1) * FS), i1 = (size_t)((o.t0 + 0.4) * FS); double s = 0; for (size_t i = i0; i < i1 && i < n; ++i) s += cm2[i]; ref = 10 * std::log10(s / std::max<size_t>(1, i1 - i0) + 1e-20); }
    vd cs2(n); for (size_t i = 0; i < n; ++i) cs2[i] = 2 * cs[i] * cs[i];
    o.cardCells = cellsOf(cs2, ref);
    const size_t m = std::min(wet.L.size(), n); vd wp(m); for (size_t i = 0; i < m; ++i) wp[i] = wet.L[i] * wet.L[i] + wet.R[i] * wet.R[i];
    o.oursCells = cellsOf(wp, ref);
    double s = 0; for (int k = 1; k < 15; ++k) { const double d = o.oursCells[k] - o.cardCells[k]; s += d * d; } o.rmsCells = std::sqrt(s / 14);
    const size_t a1 = (size_t)((o.t0 + 0.1) * FS), b1 = (size_t)((o.t0 + 0.5) * FS), b2 = (size_t)((o.t0 + 2.0) * FS);
    o.fc1 = maxCorr(cs, cm, a1, b1); o.fc2 = maxCorr(cs, cm, a1, b2);
    vd ws(m); for (size_t i = 0; i < m; ++i) ws[i] = wet.L[i] - wet.R[i];
    const vd* sig[3] = {&wet.L, &wet.R, &ws};
    for (int k = 0; k < 3; ++k) { o.fo1[k] = maxCorr(*sig[k], cm, a1, std::min(b1, m)); o.fo2[k] = maxCorr(*sig[k], cm, a1, std::min(b2, m)); }
    o.ok = true; return o;
}

struct Row {
    double wet, colrms, dec, floorv, mainv, spur, slap, structv, slapgain, width;
    vd col, col3[3], col3rms, resp[3], swing, line, bdec, dec8c, dec8o, tail, t60c, t60o, roughc, rougho;
    double onsetRms = -1, fus[4] = {0, 0, 0, 0};
};
static std::string baseName(const std::string& s) { size_t p = s.find_last_of("/\\"); std::string b = p == std::string::npos ? s : s.substr(p + 1); if (b.size() > 4 && b.substr(b.size() - 4) == ".wav") b.resize(b.size() - 4); return b; }

static Row compare(const std::string& card, const std::string& ours, double se, double se2, bool quiet) {
    Wav c, o;
    if (!readWav(card.c_str(), c) || c.fs != FS) { printf("cannot read %s (16-bit 48 kHz WAV expected)\n", card.c_str()); exit(1); }
    if (!readWav(ours.c_str(), o) || o.fs != FS) { printf("cannot read %s (16-bit 48 kHz WAV expected)\n", ours.c_str()); exit(1); }
    const Profile pc = profileOf(c, se), po = profileOf(o, se2);
    Row r;
    vd dc = po.col, cc = pc.col; normalise(dc); normalise(cc); for (int i = 0; i < NCOL; ++i) dc[i] -= cc[i];
    r.col = grouped(dc); r.colrms = colRms(dc);
    for (int k = 0; k < 3; ++k) { vd a = po.col3[k], b = pc.col3[k]; normalise(a); normalise(b); for (int i = 0; i < NCOL; ++i) a[i] -= b[i]; r.col3[k] = grouped(a); r.col3rms.push_back(colRms(a)); }
    for (int k = 0; k < 3; ++k) { r.resp[k].resize(NRESP); for (int i = 0; i < NRESP; ++i) r.resp[k][i] = po.resp[k][i] - pc.resp[k][i]; }
    r.wet = po.wet - pc.wet; r.dec = po.dec[0] - pc.dec[0];
    r.floorv = po.rep.floorv - pc.rep.floorv; r.mainv = po.rep.mainv - pc.rep.mainv; r.spur = po.rep.spur - pc.rep.spur; r.slap = po.rep.slap - pc.rep.slap;
    r.structv = pearson(pc.rep.curve, po.rep.curve);
    r.slapgain = ((po.slap[0].sideDb - pc.slap[0].sideDb) + (po.slap[1].sideDb - pc.slap[1].sideDb) + (po.slap[2].sideDb - pc.slap[2].sideDb)) / 3;
    r.width = 0; for (int k = 0; k < 4; ++k) r.width += (po.width[k] - pc.width[k]) / 4;
    for (int k = 0; k < 4; ++k) { r.swing.push_back(po.swing[k].rmsDb - pc.swing[k].rmsDb); r.line.push_back(po.swing[k].line - pc.swing[k].line); r.bdec.push_back(po.dec[k + 1] - pc.dec[k + 1]); }
    for (int i = 0; i < 9; ++i) r.tail.push_back(po.tail[i] - pc.tail[i]);
    r.t60c = pc.t60; r.t60o = po.t60; r.roughc = pc.rough; r.rougho = po.rough; r.dec8c = pc.dec8; r.dec8o = po.dec8;
    Onset on; { std::string wp = ours; if (wp.size() > 4) { wp.insert(wp.size() - 4, "_wet"); Wav w; if (readWav(wp.c_str(), w) && w.fs == FS) on = onsetOf(c, w); } }
    if (on.ok) { r.onsetRms = on.rmsCells; r.fus[0] = on.fc1; r.fus[1] = on.fc2; r.fus[2] = on.fo1[2]; r.fus[3] = on.fo2[2]; }

    char line[2048]; int L = 0;
    L += snprintf(line + L, sizeof line - L, "wet %+5.1f/%+5.1f  col rms %4.1f  [", po.wet, pc.wet, r.colrms);
    for (int k = 0; k < 8; ++k) L += snprintf(line + L, sizeof line - L, "%s%s %+4.1f", k ? " " : "", GNAMES[k], r.col[k]);
    L += snprintf(line + L, sizeof line - L, "]  dec %+4.1f/%+4.1f  rep floor %4.1f/%4.1f main %4.1fx/%4.1fx @%3.0f/%3.0f spur %3.1f/%3.1f slap %3.1f/%3.1f struct %.2f",
                  po.dec[0], pc.dec[0], po.rep.floorv, pc.rep.floorv, po.rep.mainv, pc.rep.mainv, po.rep.lag, pc.rep.lag, po.rep.spur, pc.rep.spur, po.rep.slap, pc.rep.slap, r.structv);
    if (quiet) { printf("  %-10s %s\n", baseName(card).c_str(), line); return r; }
    printf("== %s vs %s (speech end %.2f)\n  %s\n", card.c_str(), ours.c_str(), se, line);
    for (int k = 0; k < 3; ++k) {
        printf("  colour %-5s %.1f-%.1fs (ours-card dB, rms %4.1f): ", WNAMES[k], WINDOWS[k][0], WINDOWS[k][1], r.col3rms[k]);
        for (int g = 0; g < 8; ++g) printf("%s %+4.1f ", GNAMES[g], r.col3[k][g]); printf("\n");
    }
    printf("  response re voice, both channels (dB)   "); for (int i = 0; i < NRESP; ++i) printf("%6s", RNAMES[i]); printf("\n");
    for (int k = 0; k < 3; ++k) {
        printf("     %-5s card  ", RWNAMES[k]); for (int i = 0; i < NRESP; ++i) printf("%+6.1f", pc.resp[k][i]); printf("\n");
        printf("     %-5s ours  ", RWNAMES[k]); for (int i = 0; i < NRESP; ++i) printf("%+6.1f", po.resp[k][i]); printf("\n");
        printf("     %-5s DIFF  ", RWNAMES[k]); for (int i = 0; i < NRESP; ++i) printf("%+6.1f", r.resp[k][i]); printf("\n");
    }
    if (pc.floorOk > 0) { printf("     floor card  "); for (int i = 0; i < NRESP; ++i) printf("%+6.1f", pc.respFloor[i]); printf("   (before the voice, re the voice)\n"); }
    if (po.floorOk > 0) { printf("     floor ours  "); for (int i = 0; i < NRESP; ++i) printf("%+6.1f", po.respFloor[i]); printf("\n"); }
    printf("  roughness dB (500-1.5k / 1.5-4k / 4-8k): card %.2f %.2f %.2f   ours %.2f %.2f %.2f\n", pc.rough[0], pc.rough[1], pc.rough[2], po.rough[0], po.rough[1], po.rough[2]);
    printf("  swing @~24Hz dB rms ours/card (rate): "); for (int k = 0; k < 4; ++k) printf("%s %.2f/%.2f (%.0f/%.0f)  ", SW_BANDS[k].name, po.swing[k].rmsDb, pc.swing[k].rmsDb, po.swing[k].rate, pc.swing[k].rate); printf("\n");
    printf("  24 Hz line over floor dB ours/card:   "); for (int k = 0; k < 4; ++k) printf("%s %+.1f/%+.1f  ", SW_BANDS[k].name, po.swing[k].line, pc.swing[k].line); printf("\n");
    printf("  per-band decay dB/s ours/card: "); for (int k = 0; k < 4; ++k) printf("%s %+4.1f/%+4.1f  ", DEC_BANDS[k].name, po.dec[k + 1], pc.dec[k + 1]); printf("\n");
    printf("  T60 s ours/card:               "); for (int k = 0; k < 4; ++k) printf("%s %4.1f/%4.1f  ", DEC_BANDS[k].name, po.t60[k], pc.t60[k]); printf("\n");
    printf("  decay dB/s per colour group ours/card: "); for (int g = 0; g < 8; ++g) printf("%s %+4.1f/%+4.1f  ", GNAMES[g], po.dec8[g], pc.dec8[g]); printf("\n");
    printf("  tail dB re voice, 0.5 s cells  card:"); for (int i = 0; i < 9; ++i) printf("%6.1f", pc.tail[i]); printf("\n");
    printf("                                 ours:"); for (int i = 0; i < 9; ++i) printf("%6.1f", po.tail[i]); printf("\n");
    printf("  width ours/card: "); for (int k = 0; k < 4; ++k) printf("%s %+.2f/%+.2f  ", DEC_BANDS[k].name, po.width[k], pc.width[k]); printf("\n");
    printf("  slap 487ms  (side dB, L dB, side corr, lag ms) ours | card:\n");
    for (int k = 0; k < 4; ++k) printf("     %-4s %+6.1f %+6.1f %+.2f @%5.1f | %+6.1f %+6.1f %+.2f @%5.1f\n", SLAP_BANDS[k].name, po.slap[k].sideDb, po.slap[k].lDb, po.slap[k].corr, po.slap[k].lagMs, pc.slap[k].sideDb, pc.slap[k].lDb, pc.slap[k].corr, pc.slap[k].lagMs);
    printf("  wet 1.5-2.5s after: ours %+5.1f card %+5.1f    wet(L+R power) 0.4-1.0s: ours %+5.1f card %+5.1f\n", po.wet2, pc.wet2, po.wetLR, pc.wetLR);
    printf("  autocorr peaks (lag ms x floor)  card: "); for (const auto& pk : peaksOf(pc.rep)) printf("%3.0f:%.1f  ", pk.lag, pk.x); printf("\n");
    printf("                                   ours: "); for (const auto& pk : peaksOf(po.rep)) printf("%3.0f:%.1f  ", pk.lag, pk.x); printf("\n");
    printf("  early tail envelope dB re speech, 50 ms cells 0..0.75 s card: "); for (int i = 0; i < 16; ++i) printf("%+5.1f ", pc.env[i]); printf("\n");
    printf("                                                        ours: "); for (int i = 0; i < 16; ++i) printf("%+5.1f ", po.env[i]); printf("\n");
    if (on.ok) {
        printf("  onset: room power dB re the card's voice, 20 ms cells from %.3f s\n     card room ", on.t0); for (int k = 0; k < 22; ++k) printf("%5.0f", on.cardCells[k]); printf("\n     ours room ");
        for (int k = 0; k < 22; ++k) printf("%5.0f", on.oursCells[k]); printf("\n     DIFF      "); for (int k = 0; k < 22; ++k) printf("%+5.0f", on.oursCells[k] - on.cardCells[k]); printf("   rms20-300 %.1f\n", on.rmsCells);
        printf("  fusion (room-vs-voice max corr, first 0.4 s / first 1.9 s): card side %.2f / %.2f   ours L %.2f / %.2f  R %.2f / %.2f  side %.2f / %.2f\n", on.fc1, on.fc2, on.fo1[0], on.fo2[0], on.fo1[1], on.fo2[1], on.fo1[2], on.fo2[2]);
    }
    return r;
}

int main(int argc, char** argv) {
    bool quiet = false; std::vector<Row> rows;
    if (argc == 4 && !strcmp(argv[1], "-diff")) return diffMode(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "-lag")) return lagMode(argv[2], argc > 3 ? argv[3] : nullptr);
    for (int i = 1; i < argc; ++i) { if (!strcmp(argv[i], "-q")) quiet = true; if (!strcmp(argv[i], "-fast")) gFast = true; }
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        std::string a = argv[i]; std::vector<std::string> parts; size_t p = 0;
        while (p <= a.size()) { size_t q = a.find(':', p); while (q != std::string::npos && q == p + 1 && isalpha((unsigned char)a[p]) && (q + 1 < a.size()) && (a[q + 1] == '\\' || a[q + 1] == '/')) q = a.find(':', q + 1); parts.push_back(a.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
        if (parts.size() < 3) { printf("usage: %s [-q] card.wav:ours.wav:speechEnd[:speechEnd2] ...\n", argv[0]); return 1; }
        const double se = atof(parts[2].c_str()), se2 = parts.size() > 3 ? atof(parts[3].c_str()) : se;
        rows.push_back(compare(parts[0], parts[1], se, se2, quiet));
    }
    if (rows.empty()) { printf("usage: %s [-q] card.wav:ours.wav:speechEnd[:speechEnd2] ...\n", argv[0]); return 1; }
    if (rows.size() > 1) {
        const double n = (double)rows.size();
        auto m = [&](double Row::*f) { double s = 0; for (const auto& r : rows) s += r.*f; return s / n; };
        printf("  MEAN(ours-card): wet %+5.2f dB  colour rms %4.2f  [", m(&Row::wet), m(&Row::colrms));
        for (int k = 0; k < 8; ++k) { double s = 0; for (const auto& r : rows) s += r.col[k]; printf("%s%s %+4.1f", k ? " " : "", GNAMES[k], s / n); }
        printf("]  dec %+5.2f dB/s  rep floor %+5.2f main %+5.2fx spur %+5.2fx slap %+5.2fx  struct %.3f  slapgain %+5.1f dB  width %+.3f\n",
               m(&Row::dec), m(&Row::floorv), m(&Row::mainv), m(&Row::spur), m(&Row::slap), m(&Row::structv), m(&Row::slapgain), m(&Row::width));
        for (int k = 0; k < 3; ++k) {
            double rs = 0; for (const auto& r : rows) rs += r.col3rms[k];
            printf("  MEAN colour %-5s rms %4.2f: ", WNAMES[k], rs / n);
            for (int g = 0; g < 8; ++g) { double s = 0; for (const auto& r : rows) s += r.col3[k][g]; printf("%s %+4.1f ", GNAMES[g], s / n); } printf("\n");
        }
        printf("  MEAN response DIFF ours-card (dB)        "); for (int i = 0; i < NRESP; ++i) printf("%6s", RNAMES[i]); printf("\n");
        for (int k = 0; k < 3; ++k) { printf("     %-5s       ", RWNAMES[k]); for (int i = 0; i < NRESP; ++i) { double s = 0; for (const auto& r : rows) s += r.resp[k][i]; printf("%+6.1f", s / n); } printf("\n"); }
        printf("  MEAN swing diff dB: "); for (int k = 0; k < 4; ++k) { double s = 0; for (const auto& r : rows) s += r.swing[k]; printf("%s %+.2f  ", SW_BANDS[k].name, s / n); }
        printf("  MEAN 24 Hz line diff dB: "); for (int k = 0; k < 4; ++k) { double s = 0; for (const auto& r : rows) s += r.line[k]; printf("%s %+.1f  ", SW_BANDS[k].name, s / n); } printf("\n");
        printf("  MEAN band decay diff dB/s: "); for (int k = 0; k < 4; ++k) { double s = 0; for (const auto& r : rows) s += r.bdec[k]; printf("%s %+.2f  ", DEC_BANDS[k].name, s / n); }
        printf("  MEAN T60 card/ours: "); for (int k = 0; k < 4; ++k) { double sc = 0, so = 0; for (const auto& r : rows) { sc += r.t60c[k]; so += r.t60o[k]; } printf("%s %4.1f/%4.1f  ", DEC_BANDS[k].name, sc / n, so / n); } printf("\n");
        printf("  MEAN decay dB/s per group card/ours: "); for (int g = 0; g < 8; ++g) { double sc = 0, so = 0; for (const auto& r : rows) { sc += r.dec8c[g]; so += r.dec8o[g]; } printf("%s %+.2f/%+.2f  ", GNAMES[g], sc / n, so / n); } printf("\n");
        printf("  MEAN tail diff dB (0.5 s cells): "); for (int i = 0; i < 9; ++i) { double s = 0; for (const auto& r : rows) s += r.tail[i]; printf("%+5.1f ", s / n); }
        printf("  MEAN roughness card/ours: "); for (int k = 0; k < 3; ++k) { double sc = 0, so = 0; for (const auto& r : rows) { sc += r.roughc[k]; so += r.rougho[k]; } printf("%.2f/%.2f ", sc / n, so / n); } printf("\n");
        int no = 0; double so = 0, f[4] = {0, 0, 0, 0};
        for (const auto& r : rows) if (r.onsetRms >= 0) { ++no; so += r.onsetRms; for (int k = 0; k < 4; ++k) f[k] += r.fus[k]; }
        if (no) printf("  MEAN onset rms20-300 %.1f dB   fusion card %.2f / %.2f   ours side %.2f / %.2f   (%d takes with _wet)\n", so / no, f[0] / no, f[1] / no, f[2] / no, f[3] / no, no);
    }
    return 0;
}
