#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <objidl.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
using std::max; using std::min;
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "ole32.lib")

#include "../../src/licensing/license.hpp"
#include "embedded/embedded_key.hpp"

static const COLORREF C_GOLD  = RGB(177, 144, 223);
static const COLORREF C_TEXT  = RGB(234, 228, 241);
static const COLORREF C_DIM   = RGB(166, 154, 182);
static const COLORREF C_PANEL = RGB(36, 29, 45);
static const COLORREF C_BACK  = RGB(25, 20, 31);
static const COLORREF C_EDGE  = RGB(64, 50, 82);
static const COLORREF C_NAVY  = RGB(77, 52, 111);
static const COLORREF C_OFF   = RGB(105, 94, 120);
static const COLORREF C_WARN  = RGB(236, 96, 104);
static const COLORREF C_GOOD  = RGB(110, 196, 140);

static const int WIN_W = 900, WIN_H = 620;
static const int TITLE_H = 34, STATUS_H = 34;

static const RECT R_CLOSE = {WIN_W - 34, 5, WIN_W - 8, 29};
static const RECT R_MIN   = {WIN_W - 64, 5, WIN_W - 38, 29};

static const RECT R_LEFT  = {12, TITLE_H + 12, 452, WIN_H - STATUS_H - 12};
static const RECT R_RIGHT = {464, TITLE_H + 12, WIN_W - 12, WIN_H - STATUS_H - 12};

static const RECT R_NAMEBOX = {24, 104, 440, 132};
static const RECT R_IDBOX   = {24, 164, 440, 192};
static const RECT R_D7   = {24, 224, 94, 250};
static const RECT R_D30  = {102, 224, 178, 250};
static const RECT R_D365 = {186, 224, 256, 250};
static const RECT R_LIFE = {264, 224, 344, 250};
static const RECT R_CUST = {352, 224, 426, 250};
static const RECT R_DAYS = {64, 262, 124, 286};
static const RECT R_HRS  = {192, 262, 252, 286};
static const RECT R_MINS = {332, 262, 392, 286};
static const RECT R_GEN  = {24, 302, 196, 336};
static const RECT R_KEYBOX = {24, 370, 440, 470};
static const RECT R_COPY = {24, 482, 164, 514};
static const RECT R_HIST = {476, 100, 876, 494};
static const RECT R_ADD7   = {476, 506, 604, 538};
static const RECT R_ADD30  = {612, 506, 740, 538};
static const RECT R_ADDX   = {748, 506, 876, 538};

enum { ID_NAME = 101, ID_ID, ID_DAYS, ID_HRS, ID_MINS, ID_KEY, ID_HIST };
enum Stage { NOT_AUTHORISED, READY };
enum Dur { D7, D30, D365, LIFE, CUSTOM };

static const unsigned LIFE_MARK = 0xFFFFFFFFu;

struct Rec {
    std::wstring name;
    std::string machine;
    std::string key;
    unsigned issued = 0;
    unsigned expiry = 0;
};

static Stage stage = NOT_AUTHORISED;
static Dur dur = D30;
static HWND hName, hId, hDays, hHrs, hMins, hKey, hHist;
static HFONT fntUI, fntBold, fntTitle, fntMono;
static RECT hotRect = {0, 0, 0, 0};
static bool mouseTracked = false;
static std::wstring msg;
static bool msgBad = false;
static RECT rCredit = {0, 0, 0, 0};
static int aboutResult = 0, aboutHot = 0;
static bool aboutMouse = false;
static RECT aboutLink = {0, 0, 0, 0};
static const wchar_t* ABOLHB_SITE = L"https://abolhb.com";
static std::vector<Rec> hist;
static int histSel = -1;
static ULONG_PTR gdipTok = 0;
static HBITMAP logoBmp = nullptr;
static int logoW = 0, logoH = 0;
static HBRUSH brBack = nullptr;

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string a(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &a[0], n, nullptr, nullptr);
    return a;
}

static std::string hwndNarrow(HWND h) {
    wchar_t buf[600] = L"";
    GetWindowTextW(h, buf, 600);
    std::string a = narrow(buf);
    SecureZeroMemory(buf, sizeof(buf));
    return a;
}

static std::wstring hwndWide(HWND h) {
    wchar_t buf[600] = L"";
    GetWindowTextW(h, buf, 600);
    return buf;
}

static int hwndInt(HWND h) {
    wchar_t buf[32] = L"";
    GetWindowTextW(h, buf, 32);
    return _wtoi(buf);
}

typedef HRESULT (WINAPI *PFN_SWT)(HWND, LPCWSTR, LPCWSTR);
static void noTheme(HWND h) {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryW(L"uxtheme.dll");
    if (ux) {
        auto f = reinterpret_cast<PFN_SWT>(reinterpret_cast<void*>(GetProcAddress(ux, "SetWindowTheme")));
        if (f) f(h, L"", L"");
    }
}

static std::wstring exeDir() {
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    wchar_t* sl = wcsrchr(p, L'\\');
    if (sl) *sl = 0;
    return p;
}

static std::wstring histPath() { return exeDir() + L"\\octopus_history.txt"; }

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

static void histSave() {
    FILE* f = _wfopen(histPath().c_str(), L"wb");
    if (!f) return;
    fprintf(f, "# OctopusEAX keygen history\n");
    for (const Rec& r : hist) {
        fprintf(f, "%s\t%s\t0\t%u\t%u\t0\t%s\n",
                esc(narrow(r.name)).c_str(), esc(r.machine).c_str(),
                r.issued, r.expiry, esc(r.key).c_str());
    }
    fclose(f);
}

static void histLoad() {
    hist.clear();
    FILE* f = _wfopen(histPath().c_str(), L"rb");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        Rec r;
        std::string parts[7];
        size_t start = 0, i = 0;
        for (; i < 6; ++i) {
            size_t t = s.find('\t', start);
            if (t == std::string::npos) break;
            parts[i] = s.substr(start, t - start);
            start = t + 1;
        }
        if (i < 6) continue;
        parts[6] = s.substr(start);
        r.name = widen(parts[0]);
        r.machine = parts[1];
        r.issued = (unsigned)strtoul(parts[3].c_str(), nullptr, 10);
        r.expiry = (unsigned)strtoul(parts[4].c_str(), nullptr, 10);
        r.key = parts[6];
        hist.push_back(r);
    }
    fclose(f);
}

static void histFill() {
    if (!hHist) return;
    SendMessageW(hHist, LB_RESETCONTENT, 0, 0);
    for (const Rec& r : hist) {
        std::wstring row = r.name.empty() ? L"No name" : r.name;
        SendMessageW(hHist, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
    if (histSel >= 0 && histSel < (int)hist.size())
        SendMessageW(hHist, LB_SETCURSEL, histSel, 0);
}

static void copyToClipboard(HWND h, const std::wstring& s) {
    if (!OpenClipboard(h)) return;
    EmptyClipboard();
    size_t cb = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, cb);
    if (g) {
        void* p = GlobalLock(g);
        if (p) {
            memcpy(p, s.c_str(), cb);
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        } else GlobalFree(g);
    }
    CloseClipboard();
}

static void applyStage() {
    int gen = (stage == READY) ? SW_SHOW : SW_HIDE;
    ShowWindow(hName, gen);
    ShowWindow(hId, gen);
    ShowWindow(hKey, gen);
    ShowWindow(hHist, gen);
    ShowWindow(hDays, gen);
    ShowWindow(hHrs, gen);
    ShowWindow(hMins, gen);
}

static COLORREF mixc(COLORREF a, COLORREF b, double t) {
    auto ch = [t](int x, int y) { return (int)(x + (y - x) * t + 0.5); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

static Gdiplus::Color gpColor(COLORREF c, int a = 255) {
    return Gdiplus::Color((BYTE)(a < 0 ? 0 : (a > 255 ? 255 : a)), GetRValue(c), GetGValue(c), GetBValue(c));
}

static void roundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    const float d = r * 2.0f;
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

static void panel(HDC dc, RECT r, COLORREF fill, COLORREF edge) {
    HBRUSH b = CreateSolidBrush(fill);
    FillRect(dc, &r, b);
    DeleteObject(b);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
}

static void label(HDC dc, int x, int y, const wchar_t* s, COLORREF c, HFONT f) {
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, f);
    TextOutW(dc, x, y, s, (int)wcslen(s));
    SelectObject(dc, of);
}

static bool isHot(const RECT& r) { return EqualRect(&r, &hotRect) != 0; }

enum Icon { ICO_NONE = -1, ICO_CLOSE, ICO_MIN, ICO_KEY, ICO_COPY, ICO_PLUS };

static void drawIcon(HDC dc, float cx, float cy, int icon, COLORREF c, COLORREF bg) {
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush br(gpColor(c));
    Gdiplus::SolidBrush hole(gpColor(bg));
    Gdiplus::Pen pen(gpColor(c), 1.6f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::Pen bold(gpColor(c), 2.0f);
    bold.SetStartCap(Gdiplus::LineCapRound);
    bold.SetEndCap(Gdiplus::LineCapRound);
    switch (icon) {
    case ICO_CLOSE:
        gr.DrawLine(&bold, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f);
        gr.DrawLine(&bold, cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f);
        break;
    case ICO_MIN:
        gr.DrawLine(&bold, cx - 5.0f, cy + 3.5f, cx + 5.0f, cy + 3.5f);
        break;
    case ICO_KEY:
        gr.DrawEllipse(&bold, cx - 7.0f, cy - 3.5f, 7.0f, 7.0f);
        gr.DrawLine(&bold, cx, cy, cx + 7.0f, cy);
        gr.DrawLine(&bold, cx + 4.0f, cy, cx + 4.0f, cy + 3.0f);
        gr.DrawLine(&bold, cx + 6.5f, cy, cx + 6.5f, cy + 2.5f);
        break;
    case ICO_COPY: {
        Gdiplus::GraphicsPath back, front;
        roundPath(back, cx - 6.5f, cy - 6.5f, 9.0f, 10.0f, 1.5f);
        roundPath(front, cx - 2.5f, cy - 3.5f, 9.0f, 10.0f, 1.5f);
        gr.DrawPath(&pen, &back);
        gr.FillPath(&hole, &front);
        gr.DrawPath(&pen, &front);
        break;
    }
    case ICO_PLUS:
        gr.DrawLine(&bold, cx - 5.0f, cy, cx + 5.0f, cy);
        gr.DrawLine(&bold, cx, cy - 5.0f, cx, cy + 5.0f);
        break;
    default:
        break;
    }
}

static void buttonFace(HDC dc, const RECT& r, int icon, const wchar_t* text, COLORREF fill, COLORREF edge, COLORREF fg) {
    panel(dc, r, fill, edge);
    int tw = 0, th = 0;
    HGDIOBJ of = SelectObject(dc, fntBold);
    if (text && text[0]) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
        tw = sz.cx;
        th = sz.cy;
    }
    const int iw = icon != ICO_NONE ? 16 : 0, gap = (iw && tw) ? 6 : 0;
    const int x0 = (r.left + r.right - (iw + gap + tw)) / 2;
    const int cy = (r.top + r.bottom) / 2;
    if (icon != ICO_NONE) drawIcon(dc, x0 + 8.0f, (float)cy, icon, fg, fill);
    if (tw) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        TextOutW(dc, x0 + iw + gap, cy - th / 2, text, (int)wcslen(text));
    }
    SelectObject(dc, of);
}

static void button(HDC dc, const RECT& r, int icon, const wchar_t* text, bool enabled = true, bool lit = false) {
    const bool on = enabled && (isHot(r) || lit);
    buttonFace(dc, r, icon, text, !enabled ? C_PANEL : (on ? C_NAVY : C_BACK),
               !enabled ? C_EDGE : (on ? C_GOLD : C_EDGE), !enabled ? C_OFF : (on ? C_GOLD : C_TEXT));
}

static void primaryButton(HDC dc, const RECT& r, int icon, const wchar_t* text) {
    const COLORREF fill = isHot(r) ? mixc(C_GOLD, C_TEXT, 0.25) : C_GOLD;
    buttonFace(dc, r, icon, text, fill, fill, C_BACK);
}

static void pill(HDC dc, int x, int y, const wchar_t* text, COLORREF c) {
    SIZE sz{};
    HGDIOBJ of = SelectObject(dc, fntBold);
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SelectObject(dc, of);
    {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        roundPath(path, (float)x, (float)y, (float)sz.cx + 16.0f, 18.0f, 9.0f);
        Gdiplus::SolidBrush fill(gpColor(mixc(C_BACK, c, 0.22)));
        gr.FillPath(&fill, &path);
    }
    label(dc, x + 8, y + 2, text, c, fntBold);
}

static void loadLogo() {
    const HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC hr = FindResourceW(mod, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10));
    if (!hr) return;
    HGLOBAL hg = LoadResource(mod, hr);
    if (!hg) return;
    const DWORD n = SizeofResource(mod, hr);
    const void* p = LockResource(hg);
    if (!p || !n) return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!mem) return;
    void* dst = GlobalLock(mem);
    if (!dst) { GlobalFree(mem); return; }
    memcpy(dst, p, n);
    GlobalUnlock(mem);
    IStream* st = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &st)) || !st) { GlobalFree(mem); return; }
    {
        Gdiplus::Bitmap src(st);
        const int w = (int)src.GetWidth(), h = (int)src.GetHeight();
        Gdiplus::BitmapData bd{};
        Gdiplus::Rect rr(0, 0, w, h);
        if (src.GetLastStatus() == Gdiplus::Ok && w > 0 && h > 0 &&
            src.LockBits(&rr, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP hb = bd.Scan0 ? CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
            if (hb && bits) {
                for (int y = 0; y < h; ++y) {
                    const unsigned char* s = (const unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
                    unsigned char* d = (unsigned char*)bits + (size_t)y * w * 4;
                    for (int x = 0; x < w; ++x, s += 4, d += 4) {
                        const int a = s[3];
                        d[0] = (unsigned char)((int)((C_GOLD >> 16) & 0xFF) * a / 255);
                        d[1] = (unsigned char)((int)((C_GOLD >> 8) & 0xFF) * a / 255);
                        d[2] = (unsigned char)((int)(C_GOLD & 0xFF) * a / 255);
                        d[3] = (unsigned char)a;
                    }
                }
                logoBmp = hb;
                logoW = w;
                logoH = h;
            } else if (hb) {
                DeleteObject(hb);
            }
            src.UnlockBits(&bd);
        }
    }
    st->Release();
}

static void drawLogo(HDC dc, int x, int y) {
    if (!logoBmp) return;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, logoBmp);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, x, y, logoW, logoH, mem, 0, 0, logoW, logoH, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

static std::wstring leftText(const Rec& r) {
    if (r.expiry == 0) return L"lifetime";
    return lic::membershipLeft(r.key);
}

static bool unlockHere(unsigned char out[embedded::KEY_N]) {
    const lic::MachineIds ids = lic::knownIds(false);
    for (const std::string& id : ids.id)
        if (!id.empty() && embedded::signingKey(id, out)) return true;
    return false;
}

static bool signKey(const std::string& pretty, unsigned expiry, unsigned flags, std::string& out, std::wstring& err) {
    unsigned char digest[32];
    if (!lic::sha256(lic::signedLine(pretty, expiry, flags), digest)) {
        err = L"Couldn't make the key.";
        return false;
    }
    unsigned char sig[64];
    unsigned char priv[embedded::KEY_N];
    if (!unlockHere(priv)) {
        err = L"This key generator doesn't work on this PC.";
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) {
        SecureZeroMemory(priv, sizeof(priv));
        err = L"Couldn't sign the key.";
        return false;
    }
    BCRYPT_KEY_HANDLE k = nullptr;
    ULONG n = 0;
    bool ok = false;
    if (!BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &k, priv,
                             (ULONG)sizeof(priv), 0)) {
        ok = !BCryptSignHash(k, nullptr, digest, 32, sig, 64, &n, 0) && n == 64;
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    SecureZeroMemory(priv, sizeof(priv));
    if (!ok) {
        err = L"Couldn't sign the key.";
        return false;
    }
    out = lic::encodeKey(expiry, flags, sig);
    return true;
}

static bool expired(const Rec& r) {
    return r.expiry != 0 && (long long)r.expiry <= (long long)time(nullptr);
}

static unsigned remainingOf(const Rec& r) {
    if (r.expiry == 0) return LIFE_MARK;
    const long long left = (long long)r.expiry - (long long)time(nullptr);
    return left > 0 ? (unsigned)left : 0;
}

static void drawRefusedScreen(HDC dc) {
    const RECT card = {170, 110, WIN_W - 170, 470};
    panel(dc, card, C_PANEL, C_EDGE);
    const int x = card.left + 32;
    label(dc, x, card.top + 30, L"This key generator doesn't work on this PC", C_GOLD, fntTitle);
    label(dc, x, card.top + 72, L"It only opens on the PCs it was made for.", C_DIM, fntUI);
    label(dc, x, card.top + 120, L"This PC's number", C_GOLD, fntBold);
    RECT idb = {x, card.top + 142, card.right - 32, card.top + 180};
    panel(dc, idb, C_BACK, C_EDGE);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    HGDIOBJ of = SelectObject(dc, fntTitle);
    RECT t = idb;
    t.left += 14;
    DrawTextW(dc, widen(lic::machineId()).c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of);
    label(dc, x, card.top + 196, L"Send this number to get a copy that works here.", C_DIM, fntUI);
}

static void drawIssueScreen(HDC dc) {
    panel(dc, R_LEFT, C_PANEL, C_EDGE);
    label(dc, 24, TITLE_H + 22, L"New key", C_GOLD, fntBold);

    label(dc, 24, R_NAMEBOX.top - 20, L"Customer name", C_DIM, fntUI);
    {
        RECT hint = {200, R_NAMEBOX.top - 20, R_NAMEBOX.right, R_NAMEBOX.top - 4};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_OFF);
        HGDIOBJ of = SelectObject(dc, fntUI);
        DrawTextW(dc, L"only saved on this PC", -1, &hint, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    panel(dc, R_NAMEBOX, C_BACK, C_EDGE);
    label(dc, 24, R_IDBOX.top - 20, L"Machine number", C_DIM, fntUI);
    panel(dc, R_IDBOX, C_BACK, C_EDGE);

    label(dc, 24, R_D7.top - 20, L"Membership", C_DIM, fntUI);
    button(dc, R_D7,   ICO_NONE, L"7 days",   true, dur == D7);
    button(dc, R_D30,  ICO_NONE, L"30 days",  true, dur == D30);
    button(dc, R_D365, ICO_NONE, L"1 year",   true, dur == D365);
    button(dc, R_LIFE, ICO_NONE, L"Lifetime", true, dur == LIFE);
    button(dc, R_CUST, ICO_NONE, L"Custom",   true, dur == CUSTOM);
    label(dc, 24, R_DAYS.top + 4, L"Days", C_DIM, fntUI);
    panel(dc, R_DAYS, C_BACK, C_EDGE);
    label(dc, 144, R_HRS.top + 4, L"Hours", C_DIM, fntUI);
    panel(dc, R_HRS, C_BACK, C_EDGE);
    label(dc, 270, R_MINS.top + 4, L"Minutes", C_DIM, fntUI);
    panel(dc, R_MINS, C_BACK, C_EDGE);

    primaryButton(dc, R_GEN, ICO_KEY, L"Generate key");
    label(dc, 24, R_KEYBOX.top - 20, L"Key", C_DIM, fntUI);
    panel(dc, R_KEYBOX, C_BACK, C_EDGE);
    button(dc, R_COPY, ICO_COPY, L"Copy key");
    if (!msg.empty()) {
        RECT mr = {24, R_COPY.bottom + 12, R_LEFT.right - 12, R_LEFT.bottom - 8};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, msgBad ? C_WARN : C_GOLD);
        HGDIOBJ of = SelectObject(dc, fntBold);
        DrawTextW(dc, msg.c_str(), -1, &mr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, of);
    }

    panel(dc, R_RIGHT, C_PANEL, C_EDGE);
    label(dc, R_RIGHT.left + 12, TITLE_H + 22, L"Customers", C_GOLD, fntBold);
    label(dc, R_RIGHT.left + 12, TITLE_H + 42, L"Pick one to give them more time.", C_DIM, fntUI);
    panel(dc, R_HIST, C_BACK, C_EDGE);
    const bool has = histSel >= 0 && histSel < (int)hist.size();
    button(dc, R_ADD7,   ICO_PLUS,  L"7 days",   has);
    button(dc, R_ADD30,  ICO_PLUS,  L"30 days",  has);
    button(dc, R_ADDX,   ICO_PLUS,  L"Custom",   has);
    label(dc, R_RIGHT.left + 12, R_ADDX.bottom + 8, L"The new key is copied for you.", C_OFF, fntUI);
}

static void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    HDC dc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = rc.right;
    bi.bmiHeader.biHeight = -rc.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return;
    }
    HGDIOBJ ob = SelectObject(dc, bmp);

    HBRUSH bg = CreateSolidBrush(C_BACK);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    RECT tr = {0, 0, rc.right, TITLE_H};
    HBRUSH tb = CreateSolidBrush(C_PANEL);
    FillRect(dc, &tr, tb);
    DeleteObject(tb);
    HPEN gp = CreatePen(PS_SOLID, 1, mixc(C_EDGE, C_GOLD, 0.6));
    HGDIOBJ opn = SelectObject(dc, gp);
    MoveToEx(dc, 0, TITLE_H - 1, nullptr);
    LineTo(dc, rc.right, TITLE_H - 1);
    SelectObject(dc, opn);
    DeleteObject(gp);
    drawLogo(dc, 10, (TITLE_H - logoH) / 2);
    const int tx = 10 + (logoW ? logoW + 8 : 0);
    label(dc, tx, 7, L"OctopusEAX", C_GOLD, fntTitle);
    label(dc, tx + 118, 12, L"Key generator", C_DIM, fntUI);

    button(dc, R_MIN, ICO_MIN, nullptr);
    if (isHot(R_CLOSE)) {
        const COLORREF red = RGB(200, 58, 72);
        panel(dc, R_CLOSE, red, red);
        drawIcon(dc, (R_CLOSE.left + R_CLOSE.right) / 2.0f, (R_CLOSE.top + R_CLOSE.bottom) / 2.0f, ICO_CLOSE, RGB(255, 255, 255), red);
    } else {
        button(dc, R_CLOSE, ICO_CLOSE, nullptr);
    }

    if (stage == READY) drawIssueScreen(dc);
    else                drawRefusedScreen(dc);

    RECT sb = {0, rc.bottom - STATUS_H, rc.right, rc.bottom};
    HBRUSH sbb = CreateSolidBrush(C_PANEL);
    FillRect(dc, &sb, sbb);
    DeleteObject(sbb);
    HPEN sp = CreatePen(PS_SOLID, 1, C_EDGE);
    HGDIOBJ osp = SelectObject(dc, sp);
    MoveToEx(dc, 0, rc.bottom - STATUS_H, nullptr);
    LineTo(dc, rc.right, rc.bottom - STATUS_H);
    SelectObject(dc, osp);
    DeleteObject(sp);
    const wchar_t* foot = stage == READY ? (embedded::unrestricted() ? L"Works on any PC" : L"Locked to this PC")
                                         : L"Not allowed on this PC";
    label(dc, 16, rc.bottom - 24, foot, C_DIM, fntUI);
    {
        const std::wstring cr = std::wstring(L"\xA9 ") + L"Copyright";
        SIZE sz{};
        HGDIOBJ o2 = SelectObject(dc, fntUI);
        GetTextExtentPoint32W(dc, cr.c_str(), (int)cr.size(), &sz);
        SelectObject(dc, o2);
        const int cx = rc.right - sz.cx - 16, cy = rc.bottom - 24;
        rCredit = {cx - 4, cy - 3, cx + sz.cx + 4, cy + sz.cy + 3};
        label(dc, cx, cy, cr.c_str(), isHot(rCredit) ? C_GOLD : C_DIM, fntUI);
    }

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static RECT aboutOk(const RECT& rc) { return {rc.right - 92, rc.bottom - 40, rc.right - 16, rc.bottom - 14}; }

static LRESULT CALLBACK aboutProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        panel(dc, rc, C_PANEL, C_GOLD);
        drawLogo(dc, 20, 18);
        label(dc, 20 + (logoW ? logoW + 10 : 0), 16, L"OctopusEAX", C_GOLD, fntTitle);
        label(dc, 20, 56, L"Contributes on OctopusEAX :", C_GOLD, fntBold);
        {
            const wchar_t* name = L"ABOLHB";
            label(dc, 20, 80, name, aboutHot == 2 ? C_GOLD : C_TEXT, fntBold);
            SIZE sz{};
            HGDIOBJ of = SelectObject(dc, fntBold);
            GetTextExtentPoint32W(dc, name, (int)wcslen(name), &sz);
            SelectObject(dc, of);
            aboutLink = {18, 78, 22 + sz.cx, 82 + sz.cy};
            if (aboutHot == 2) {
                HPEN pen = CreatePen(PS_SOLID, 1, C_GOLD);
                HGDIOBJ op = SelectObject(dc, pen);
                MoveToEx(dc, 20, 80 + sz.cy, nullptr);
                LineTo(dc, 20 + sz.cx, 80 + sz.cy);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
        }
        label(dc, 20, 100, L"MMLo7", C_TEXT, fntBold);
        label(dc, 20, 136, L"Special thanks to :", C_GOLD, fntBold);
        label(dc, 20, 160, L"FREEMASONRY", C_TEXT, fntBold);
        label(dc, 20, 180, L"SALEMLY", C_TEXT, fntBold);
        const COLORREF face = aboutHot == 1 ? mixc(C_GOLD, C_TEXT, 0.25) : C_GOLD;
        buttonFace(dc, aboutOk(rc), ICO_NONE, L"OK", face, face, C_BACK);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!aboutMouse) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            aboutMouse = true;
        }
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        RECT rc;
        GetClientRect(h, &rc);
        const RECT done = aboutOk(rc);
        const int hot = PtInRect(&done, p) ? 1 : (PtInRect(&aboutLink, p) ? 2 : 0);
        if (hot != aboutHot) {
            aboutHot = hot;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        aboutMouse = false;
        if (aboutHot) {
            aboutHot = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && aboutHot) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        RECT rc;
        GetClientRect(h, &rc);
        const RECT done = aboutOk(rc);
        if (PtInRect(&done, p)) aboutResult = 1;
        else if (PtInRect(&aboutLink, p)) ShellExecuteW(nullptr, L"open", ABOLHB_SITE, nullptr, nullptr, SW_SHOWNORMAL);
        else if (p.y < 44) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE || w == VK_RETURN) aboutResult = 1;
        return 0;
    case WM_CLOSE: aboutResult = 1; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void showAbout(HWND owner) {
    aboutResult = 0;
    aboutHot = 0;
    aboutMouse = false;
    SetRectEmpty(&aboutLink);
    RECT o; GetWindowRect(owner, &o);
    const int W = 360, H = 270;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"OctopusKgAbout", L"OctopusEAX", WS_POPUP,
                             (o.left + o.right - W) / 2, (o.top + o.bottom - H) / 2, W, H,
                             owner, nullptr, nullptr, nullptr);
    if (!h) return;
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    MSG ev;
    while (aboutResult == 0 && GetMessageW(&ev, nullptr, 0, 0)) {
        TranslateMessage(&ev); DispatchMessageW(&ev);
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(h);
    SetForegroundWindow(owner);
}

static void say(HWND h, const wchar_t* text, bool bad) {
    msg = text;
    msgBad = bad;
    InvalidateRect(h, nullptr, FALSE);
    UpdateWindow(h);
}

static bool readInputs(HWND hwnd, std::wstring& name, std::string& pretty, unsigned& expiry) {
    name = hwndWide(hName);
    while (!name.empty() && name.back() == L' ') name.pop_back();
    if (name.empty()) {
        say(hwnd, L"Enter the customer's name.", true);
        return false;
    }
    std::string clean = lic::b32normalise(hwndNarrow(hId));
    if (clean.size() != 16) {
        say(hwnd, L"A machine number has 16 characters, like ABCD-EFGH-IJKL-MNOP.", true);
        return false;
    }
    pretty = lic::group(clean, 4);
    if (dur == LIFE) {
        expiry = 0;
        return true;
    }
    unsigned sec = 0;
    if (dur == D7) sec = 7u * 86400u;
    else if (dur == D30) sec = 30u * 86400u;
    else if (dur == D365) sec = 365u * 86400u;
    else {
        int d = hwndInt(hDays), hr = hwndInt(hHrs), mn = hwndInt(hMins);
        if (d < 0) d = 0;
        if (hr < 0) hr = 0;
        if (mn < 0) mn = 0;
        if (d == 0 && hr == 0 && mn == 0) {
            say(hwnd, L"Custom needs some days, hours or minutes.", true);
            return false;
        }
        sec = (unsigned)d * 86400u + (unsigned)hr * 3600u + (unsigned)mn * 60u;
    }
    expiry = (unsigned)time(nullptr) + sec;
    return true;
}

static int findMachine(const std::string& pretty) {
    const std::string n = lic::b32normalise(pretty);
    for (int i = 0; i < (int)hist.size(); ++i)
        if (lic::b32normalise(hist[i].machine) == n) return i;
    return -1;
}

static void upsert(const Rec& r) {
    int i = findMachine(r.machine);
    if (i < 0) {
        hist.insert(hist.begin(), r);
        histSel = 0;
    } else {
        hist[i] = r;
        histSel = i;
    }
    histSave();
    histFill();
}

static bool writeKey(HWND hwnd, const std::string& pretty, unsigned expiry, unsigned flags, Rec& r, const wchar_t* okMsg) {
    std::string key;
    std::wstring err;
    if (!signKey(pretty, expiry, flags, key, err)) {
        say(hwnd, err.c_str(), true);
        return false;
    }
    r.machine = pretty;
    r.key = key;
    r.issued = (unsigned)time(nullptr);
    r.expiry = expiry;
    SetWindowTextW(hId, widen(pretty).c_str());
    SetWindowTextW(hKey, widen(key).c_str());
    copyToClipboard(hwnd, widen(key));
    upsert(r);
    {
        lic::writeGate(pretty, key);
        HWND app = FindWindowW(L"OctopusEAXWnd", nullptr);
        if (app) {
            COPYDATASTRUCT cds{};
            cds.dwData = 0x4F435458;
            cds.cbData = (DWORD)key.size();
            cds.lpData = (PVOID)key.data();
            SendMessageW(app, WM_COPYDATA, 0, (LPARAM)&cds);
        }
    }
    say(hwnd, okMsg, false);
    return true;
}

static void doGenerate(HWND hwnd) {
    if (stage != READY) return;
    std::wstring name;
    std::string pretty;
    unsigned expiry = 0;
    if (!readInputs(hwnd, name, pretty, expiry)) {
        SetWindowTextW(hKey, L"");
        return;
    }
    Rec r;
    r.name = name;
    writeKey(hwnd, pretty, expiry, 0, r, L"Key copied. Send it to the customer.");
}

static unsigned extraSeconds() {
    int d = hwndInt(hDays), hr = hwndInt(hHrs), mn = hwndInt(hMins);
    if (d < 0) d = 0;
    if (hr < 0) hr = 0;
    if (mn < 0) mn = 0;
    return (unsigned)d * 86400u + (unsigned)hr * 3600u + (unsigned)mn * 60u;
}

static void doAdd(HWND hwnd, unsigned extra, const wchar_t* okMsg) {
    if (histSel < 0 || histSel >= (int)hist.size()) return;
    Rec r = hist[histSel];
    unsigned left = remainingOf(r);
    if (left == LIFE_MARK) {
        writeKey(hwnd, r.machine, 0, 0, r, L"Already lifetime. A fresh key was copied.");
        return;
    }
    unsigned expiry = (unsigned)time(nullptr) + left + extra;
    writeKey(hwnd, r.machine, expiry, 0, r, okMsg);
}

static void doAddCustom(HWND hwnd) {
    unsigned extra = extraSeconds();
    if (extra == 0) {
        say(hwnd, L"Set the days, hours or minutes on the left first.", true);
        return;
    }
    doAdd(hwnd, extra, L"Key with the extra time copied.");
}

static void pickHist(HWND hwnd) {
    histSel = (int)SendMessageW(hHist, LB_GETCURSEL, 0, 0);
    if (histSel < 0 || histSel >= (int)hist.size()) return;
    const Rec& r = hist[histSel];
    SetWindowTextW(hName, r.name.c_str());
    SetWindowTextW(hId, widen(r.machine).c_str());
    SetWindowTextW(hKey, widen(r.key).c_str());
    InvalidateRect(hwnd, nullptr, FALSE);
}

static std::vector<const RECT*> hoverables() {
    std::vector<const RECT*> v = {&R_CLOSE, &R_MIN, &rCredit};
    if (stage != READY) return v;
    v.insert(v.end(), {&R_D7, &R_D30, &R_D365, &R_LIFE, &R_CUST, &R_GEN, &R_COPY});
    const bool has = histSel >= 0 && histSel < (int)hist.size();
    if (has) v.insert(v.end(), {&R_ADD7, &R_ADD30, &R_ADDX});
    return v;
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT mi = (LPMEASUREITEMSTRUCT)l;
        if (mi->CtlID == ID_HIST) { mi->itemHeight = 48; return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT d = (LPDRAWITEMSTRUCT)l;
        if (d->CtlID != ID_HIST || d->itemID == (UINT)-1) return 0;
        const bool sel = (d->itemState & ODS_SELECTED) != 0;
        HBRUSH b = CreateSolidBrush(sel ? C_NAVY : C_BACK);
        FillRect(d->hDC, &d->rcItem, b);
        DeleteObject(b);
        if ((int)d->itemID >= (int)hist.size()) return TRUE;
        const Rec& r = hist[d->itemID];
        const COLORREF stc = expired(r) ? C_WARN : C_GOOD;
        const wchar_t* stt = expired(r) ? L"Expired" : L"Active";
        SIZE pz{};
        HGDIOBJ of = SelectObject(d->hDC, fntBold);
        GetTextExtentPoint32W(d->hDC, stt, (int)wcslen(stt), &pz);
        SelectObject(d->hDC, of);
        const int pillX = d->rcItem.right - 12 - (pz.cx + 16);
        pill(d->hDC, pillX, d->rcItem.top + 6, stt, stc);
        RECT t = d->rcItem;
        t.left += 12; t.top += 6; t.right = pillX - 8;
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, sel ? C_GOLD : C_TEXT);
        of = SelectObject(d->hDC, fntBold);
        DrawTextW(d->hDC, r.name.empty() ? L"No name" : r.name.c_str(), -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(d->hDC, of);
        t.top += 20;
        t.right = d->rcItem.right - 12;
        const std::wstring left = leftText(r);
        const std::wstring sub = left.empty() ? widen(r.machine) : widen(r.machine) + L"    " + left;
        SetTextColor(d->hDC, C_DIM);
        of = SelectObject(d->hDC, fntUI);
        DrawTextW(d->hDC, sub.c_str(), -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(d->hDC, of);
        HPEN sep = CreatePen(PS_SOLID, 1, C_PANEL);
        HGDIOBJ osep = SelectObject(d->hDC, sep);
        MoveToEx(d->hDC, d->rcItem.left + 8, d->rcItem.bottom - 1, nullptr);
        LineTo(d->hDC, d->rcItem.right - 8, d->rcItem.bottom - 1);
        SelectObject(d->hDC, osep);
        DeleteObject(sep);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)w, C_BACK);
        SetTextColor((HDC)w, C_TEXT);
        return (LRESULT)brBack;
    case WM_LBUTTONDOWN: {
        POINT p = {(LONG)LOWORD(l), (LONG)HIWORD(l)};
        if (PtInRect(&R_CLOSE, p)) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (PtInRect(&R_MIN, p)) { ShowWindow(h, SW_MINIMIZE); return 0; }
        if (PtInRect(&rCredit, p)) { SetRectEmpty(&hotRect); showAbout(h); return 0; }
        if (stage == READY) {
            auto setDur = [&](Dur d) {
                dur = d;
                applyStage();
                InvalidateRect(h, nullptr, FALSE);
            };
            if (PtInRect(&R_D7, p)) { setDur(D7); return 0; }
            if (PtInRect(&R_D30, p)) { setDur(D30); return 0; }
            if (PtInRect(&R_D365, p)) { setDur(D365); return 0; }
            if (PtInRect(&R_LIFE, p)) { setDur(LIFE); return 0; }
            if (PtInRect(&R_CUST, p)) { setDur(CUSTOM); return 0; }
            if (PtInRect(&R_GEN, p)) { doGenerate(h); return 0; }
            if (PtInRect(&R_COPY, p)) {
                wchar_t buf[900] = L"";
                GetWindowTextW(hKey, buf, 900);
                if (buf[0]) {
                    copyToClipboard(h, buf);
                    say(h, L"Key copied.", false);
                }
                return 0;
            }
            const bool has = histSel >= 0 && histSel < (int)hist.size();
            if (has && PtInRect(&R_ADD7, p)) { doAdd(h, 7u * 86400u, L"Key with 7 more days copied."); return 0; }
            if (has && PtInRect(&R_ADD30, p)) { doAdd(h, 30u * 86400u, L"Key with 30 more days copied."); return 0; }
            if (has && PtInRect(&R_ADDX, p)) { doAddCustom(h); return 0; }
        }
        if (p.y < TITLE_H) {
            ReleaseCapture();
            SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = {(LONG)LOWORD(l), (LONG)HIWORD(l)};
        if (!mouseTracked) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            mouseTracked = TrackMouseEvent(&tme) != FALSE;
        }
        RECT now = {0, 0, 0, 0};
        for (const RECT* r : hoverables())
            if (PtInRect(r, p)) { now = *r; break; }
        if (!EqualRect(&now, &hotRect)) {
            hotRect = now;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        mouseTracked = false;
        if (!IsRectEmpty(&hotRect)) { SetRectEmpty(&hotRect); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && !IsRectEmpty(&hotRect)) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (HIWORD(w) == EN_CHANGE && !msg.empty()) {
            msg.clear();
            InvalidateRect(h, nullptr, FALSE);
        }
        if (LOWORD(w) == ID_HIST && HIWORD(w) == LBN_SELCHANGE) pickHist(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(_In_ HINSTANCE hi, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nShow) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gdipIn;
    Gdiplus::GdiplusStartup(&gdipTok, &gdipIn, nullptr);

    fntUI = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                        CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fntBold = CreateFontW(-12, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fntTitle = CreateFontW(-19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fntMono = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    brBack = CreateSolidBrush(C_BACK);
    loadLogo();

    unsigned char probe[embedded::KEY_N];
    stage = unlockHere(probe) ? READY : NOT_AUTHORISED;
    SecureZeroMemory(probe, sizeof(probe));

    histLoad();
    HICON icon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON smallIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"OctopusKeygenWnd";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;
    wc.hIcon = icon;
    RegisterClassW(&wc);
    WNDCLASSW ac{};
    ac.lpfnWndProc = aboutProc;
    ac.hInstance = hi;
    ac.lpszClassName = L"OctopusKgAbout";
    ac.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&ac);

    RECT wa = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int sx = wa.left + (wa.right - wa.left - WIN_W) / 2;
    int sy = wa.top + (wa.bottom - wa.top - WIN_H) / 2;
    if (sx < wa.left) sx = wa.left;
    if (sy < wa.top) sy = wa.top;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"OctopusKeygenWnd",
                                L"OctopusEAX Key Generator", WS_POPUP | WS_CLIPCHILDREN,
                                sx, sy, WIN_W, WIN_H, nullptr, nullptr, hi, nullptr);
    SetWindowPos(hwnd, nullptr, sx, sy, WIN_W, WIN_H, SWP_NOZORDER);
    if (icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)(smallIcon ? smallIcon : icon));
    }

    auto edit = [&](DWORD extra, const RECT& box, int h, int id, HFONT f) {
        HWND c = CreateWindowW(L"EDIT", L"", WS_CHILD | extra, box.left + 8,
                               box.top + (box.bottom - box.top - h) / 2,
                               box.right - box.left - 16, h, hwnd,
                               (HMENU)(INT_PTR)id, hi, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        noTheme(c);
        return c;
    };
    hName = edit(ES_AUTOHSCROLL, R_NAMEBOX, 20, ID_NAME, fntUI);
    hId = edit(ES_AUTOHSCROLL, R_IDBOX, 20, ID_ID, fntMono);
    hDays = edit(ES_AUTOHSCROLL | ES_NUMBER, R_DAYS, 18, ID_DAYS, fntUI);
    hHrs  = edit(ES_AUTOHSCROLL | ES_NUMBER, R_HRS, 18, ID_HRS, fntUI);
    hMins = edit(ES_AUTOHSCROLL | ES_NUMBER, R_MINS, 18, ID_MINS, fntUI);
    SendMessageW(hName, EM_SETCUEBANNER, TRUE, (LPARAM)L"Who is this key for?");
    SendMessageW(hId, EM_SETCUEBANNER, TRUE, (LPARAM)L"ABCD-EFGH-IJKL-MNOP");
    SetWindowTextW(hDays, L"0");
    SetWindowTextW(hHrs, L"0");
    SetWindowTextW(hMins, L"0");
    hKey = CreateWindowW(L"EDIT", L"",
                         WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                         R_KEYBOX.left + 8, R_KEYBOX.top + 8,
                         R_KEYBOX.right - R_KEYBOX.left - 16,
                         R_KEYBOX.bottom - R_KEYBOX.top - 16,
                         hwnd, (HMENU)(INT_PTR)ID_KEY, hi, nullptr);
    SendMessageW(hKey, WM_SETFONT, (WPARAM)fntUI, TRUE);
    noTheme(hKey);
    hHist = CreateWindowW(L"LISTBOX", L"",
                          WS_CHILD | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                          R_HIST.left + 1, R_HIST.top + 1,
                          R_HIST.right - R_HIST.left - 2,
                          R_HIST.bottom - R_HIST.top - 2,
                          hwnd, (HMENU)(INT_PTR)ID_HIST, hi, nullptr);
    SendMessageW(hHist, WM_SETFONT, (WPARAM)fntUI, TRUE);
    noTheme(hHist);
    histFill();

    applyStage();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring preset;
    for (int i = 1; i < argc; ++i) preset += argv[i];
    if (argv) LocalFree(argv);

    SetFocus(hName);
    ShowWindow(hwnd, nShow);

    if (!preset.empty() && stage == READY) {
        SetWindowTextW(hId, preset.c_str());
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN) {
            HWND f = GetFocus();
            if (stage == READY && (f == hId || f == hName || f == hDays || f == hHrs || f == hMins)) {
                doGenerate(hwnd);
                continue;
            }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (logoBmp) DeleteObject(logoBmp);
    if (brBack) DeleteObject(brBack);
    if (gdipTok) Gdiplus::GdiplusShutdown(gdipTok);
    return 0;
}
