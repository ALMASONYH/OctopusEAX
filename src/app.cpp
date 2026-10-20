#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <objidl.h>
#include <mmreg.h>
#include <xmmintrin.h>
#include <pmmintrin.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <avrt.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <aclapi.h>
#include <sddl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
using std::max; using std::min;

#include "audio/render_feed.hpp"
#include "cable/cable_package.hpp"
#include "convolution/convolution_reverb.hpp"
#include "equalizer/bass_boost.hpp"
#include "equalizer/eq_presets.hpp"
#include "equalizer/graphic_equalizer.hpp"
#include "isolation/expander_gate.hpp"
#include "isolation/voice_isolator.hpp"
#include "mixing/amount_law.hpp"
#include "mixing/echo_tap.hpp"
#include "mixing/mic_drive.hpp"
#include "mixing/mic_tone.hpp"
#include "mixing/out_chain.hpp"
#include "mixing/send_level.hpp"
#include "mixing/soft_clip.hpp"
#include "mixing/step_back.hpp"
#include "mixing/subsonic.hpp"
#include "mixing/wet_peak.hpp"
#include "playback/hear_tap.hpp"
#include "playback/player.hpp"
#include "playback/program_tap.hpp"
#include "playback/recorder.hpp"
#include "playback/sound_board.hpp"
#include "playback/sound_edit.hpp"
#include "playback/sound_save.hpp"
#include "presets/card_presets.hpp"
#include "reverb/card_core.hpp"
#include "reverb/reverb_engine.hpp"
#include "ui/font_file.hpp"
#include "voice/voice_fx.hpp"
#include "voice/song_changer.hpp"
#include "voice/tempo.hpp"
#include "voice/glitch_pad.hpp"
#include "voice/voice_shaper.hpp"
#include "voice/voice_tone.hpp"
using namespace mix;

static clip::Player gClip;
static clip::ProgramTap gApp;
static clip::SoundBoard gBoard;
static clip::Recorder gRec, gRecHear, gBackVoice, gBackHear;
static clip::HearTap gHearTap;
static std::atomic<int> gMediaWork{0};

struct MediaWork {
    MediaWork() { ++gMediaWork; }
    ~MediaWork() { --gMediaWork; }
    MediaWork(const MediaWork&) = delete;
    MediaWork& operator=(const MediaWork&) = delete;
};

static bool mediaSettle(ULONGLONG ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (gMediaWork.load() > 0 && GetTickCount64() < until) Sleep(20);
    return gMediaWork.load() == 0;
}
static clip::SoundBoard gTakeMic, gTakeEar;
static std::atomic<bool> takeHeld{false};
static std::vector<clip::Program> uiApps;
static std::wstring appNote;
static HWND hApp = nullptr;
static HWND hEqBox = nullptr;
static HWND hMaskBox = nullptr, hOwnBox = nullptr, hGlitchBox = nullptr;
static HWND hTip = nullptr;
static void tipsColour();
static std::vector<std::wstring> uiEqNames;
static bool eqOnPreset();
static void maskLabel(int i, wchar_t* txt);

static const int DEFAULT_PRESET = HAUNTED_CAVERN_INDEX;
static const int FS = 48000;
static const float CASCADE_GAIN = AmountLaw::CASCADE * powf(10.0f, (float)WetPeak::Tune().trimDb / 20.0f);

struct Dev { std::wstring name, id, desc, iface; };

static std::vector<Dev> listDevices(EDataFlow flow,
                                   DWORD states = DEVICE_STATE_ACTIVE) {
    std::vector<Dev> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
        return out;
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, states, &coll))) {
        UINT n = 0; coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d))) continue;
            LPWSTR id = nullptr; d->GetId(&id);
            IPropertyStore* ps = nullptr;
            std::wstring name = L"?", desc, iface;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                auto text = [&](const PROPERTYKEY& key, std::wstring& dst) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt == VT_LPWSTR) dst = pv.pwszVal;
                    PropVariantClear(&pv);
                };
                text(PKEY_Device_FriendlyName, name);
                text(PKEY_Device_DeviceDesc, desc);
                text(PKEY_DeviceInterface_FriendlyName, iface);
                ps->Release();
            }
            if (id) { out.push_back({name, id, desc, iface}); CoTaskMemFree(id); }
            d->Release();
        }
        coll->Release();
    }
    en->Release();
    return out;
}

static std::wstring defaultDeviceId(EDataFlow flow, ERole role = eConsole) {
    std::wstring res;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
        return res;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, role, &d))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) { res = id; CoTaskMemFree(id); }
        d->Release();
    }
    en->Release();
    return res;
}

static bool nameContains(const std::wstring& hay, const wchar_t* needle) {
    std::wstring a = hay, b = needle;
    for (auto& c : a) c = towlower(c);
    for (auto& c : b) c = towlower(c);
    return a.find(b) != std::wstring::npos;
}

static bool enablePrivilege(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

static bool grantAdminsOnKey(const std::wstring& objPath) {
    enablePrivilege(L"SeTakeOwnershipPrivilege");
    enablePrivilege(L"SeRestorePrivilege");
    PSID admins = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-32-544", &admins)) return false;
    std::vector<wchar_t> path(objPath.begin(), objPath.end()); path.push_back(0);
    bool ok = SetNamedSecurityInfoW(path.data(), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION,
                                    admins, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    if (ok) {
        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = KEY_ALL_ACCESS;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        ea.Trustee.ptstrName = (LPWSTR)admins;
        PACL oldDacl = nullptr, newDacl = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (GetNamedSecurityInfoW(path.data(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, &oldDacl, nullptr, &sd) == ERROR_SUCCESS) {
            if (SetEntriesInAclW(1, &ea, oldDacl, &newDacl) == ERROR_SUCCESS) {
                ok = SetNamedSecurityInfoW(path.data(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                                           nullptr, nullptr, newDacl, nullptr) == ERROR_SUCCESS;
                LocalFree(newDacl);
            } else ok = false;
            LocalFree(sd);
        } else ok = false;
    }
    LocalFree(admins);
    return ok;
}

static const wchar_t* FRIENDLY_NAME_VALUE = L"{a45c254e-df1c-4efd-8020-67d146a850e0},2";

static bool writeEndpointName(const std::wstring& key, const wchar_t* newName) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(hk, FRIENDLY_NAME_VALUE, 0, REG_SZ, (const BYTE*)newName,
                             (DWORD)((wcslen(newName) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}

static bool renameEndpoint(const std::wstring& mmid, const wchar_t* newName) {
    size_t p = mmid.rfind(L'{');
    if (p == std::wstring::npos) return false;
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\" +
                       mmid.substr(p) + L"\\Properties";
    if (writeEndpointName(key, newName)) return true;
    if (!grantAdminsOnKey(L"MACHINE\\" + key)) return false;
    return writeEndpointName(key, newName);
}

enum { ON_VOICE = 1, ON_CLIP = 2, ON_PROGRAM = 4, ON_ALL = 7 };
enum { STAGE_MISSING, STAGE_RESTART, STAGE_READY, STAGE_PROBLEM };
static int echoFromRoute(int route) { return route == 0 ? ON_VOICE | ON_CLIP : route == 1 ? ON_CLIP | ON_PROGRAM : ON_ALL; }

static const int AMOUNT_DEFAULT = 9, MIC_DEFAULT = 100, ISO_DEFAULT = 60, CLIP_GAIN_DEFAULT = 84, APP_GAIN_DEFAULT = 80;
static const int GATE_DEFAULT_DB = -170, GATE_OLD_LEVEL = 60, GATE_MIN_DB = -660, VOCAL_DEFAULT_LEVEL = iso::GATE_LEVEL_NORMAL;
static const int GATE_DEFAULT_TONE = 120, GATE_DEFAULT_OUT = -50, GATE_TONE_RANGE = 120;
static const int TONE_RANGE = 240, TONE_SAVED_ZERO = 120, TONE_PAD_RANGE = 100;
static const int ISO_VOCAL = iso::NUM_MODES, ISO_MODES = iso::NUM_MODES + 1;
static const int ISO_ORDER[ISO_MODES] = {ISO_VOCAL, 0, 1, 2, 3};

struct Shared {
    std::atomic<int> micLevel{MIC_DEFAULT};
    std::atomic<int> amount{AMOUNT_DEFAULT};

    std::atomic<int> preset{DEFAULT_PRESET};
    std::atomic<bool> enabled{true};
    std::atomic<int>  echoOn{7};
    std::atomic<int>  changeOn{1};
    std::atomic<bool> monitor{true};
    std::atomic<bool> rawMic{false};
    std::atomic<bool> cardMic{true};
    std::atomic<bool> cmss{false};
    std::atomic<bool> wide{false};
    std::atomic<int>  eqDb[eqb::EQ_BANDS]{};
    std::atomic<int>  eqLevel{0};
    std::atomic<bool> eqOn{true};
    std::atomic<int>  eqPreset{0};
    std::atomic<int>  eqVersion{0};
    std::atomic<bool> isolate{false};
    std::atomic<int>  isoMode{ISO_VOCAL};
    std::atomic<int>  isoLevel{ISO_DEFAULT};
    std::atomic<int>  gateLevel{VOCAL_DEFAULT_LEVEL};
    std::atomic<int>  gateLo{GATE_DEFAULT_TONE};
    std::atomic<int>  gateMid{GATE_DEFAULT_TONE};
    std::atomic<int>  gateHi{GATE_DEFAULT_TONE};
    std::atomic<int>  gateOut{GATE_DEFAULT_OUT};
    std::atomic<bool> toneOn{true};
    std::atomic<int>  toneLo{0};
    std::atomic<int>  toneMid{0};
    std::atomic<int>  toneHi{0};
    std::atomic<int>  tonePadX{0};
    std::atomic<int>  tonePadY{0};
    std::atomic<int>  toneBaseLo{0};
    std::atomic<int>  toneBaseMid{0};
    std::atomic<int>  toneBaseHi{0};
    std::atomic<int>  toneVersion{0};
    std::atomic<int>  clipGain{CLIP_GAIN_DEFAULT};
    std::atomic<bool> clipHear{true};
    std::atomic<bool> clipToMic{true};
    std::atomic<bool> appHear{false};
    std::atomic<bool> appOnly{false};
    std::atomic<int>  pitchHz{0};
    std::atomic<int>  voiceSize{100};
    std::atomic<int>  voiceHz{0};
    std::atomic<int>  voiceMacro{0};
    std::atomic<int>  voiceRatio{1000};
    std::atomic<int>  voiceMask{voice::MASK_NEUTRAL};
    std::atomic<int>  voiceTargetHz{0};
    std::atomic<int>  voiceSpeed{100};
    std::atomic<bool> voiceKeepPitch{true};
    std::atomic<int>  voiceLagMs{0};
    std::atomic<int>  clipSpeed{100};
    std::atomic<bool> clipKeepPitch{true};
    std::atomic<bool> clipFollow{true};
    std::atomic<int>  appGain{APP_GAIN_DEFAULT};
    std::atomic<int>  meterPeak{0};
    std::atomic<bool> glitchHold{false};
    std::atomic<bool> glitchKeyHeld{false};
    std::atomic<bool> glitchArmed{true};
    std::atomic<int>  glitchKey{0};
    std::atomic<int>  glitchStyle{voice::GLITCH_REPEAT};
    std::atomic<bool> reopenMic{false};
    std::atomic<int>  isoVersion{0};
    std::atomic<bool> restart{true};
    std::atomic<bool> quit{false};
    std::mutex devMx;
    std::wstring micId, spkId;
    std::atomic<bool> running{false};
    std::atomic<bool> statusWarn{false};
    std::atomic<bool> needCable{false};
    std::atomic<bool> askCable{false};
    std::atomic<int>  cableState{0};
    std::atomic<int>  cableStage{0};
    wchar_t status[256] = L"";
    wchar_t cable[256] = L"";
    wchar_t cableName[256] = L"";
};
static Shared g;

static voice::ToneGains toneGains() {
    voice::ToneGains t;
    if (!g.toneOn.load()) return t;
    t.loDb = g.toneLo.load() / 10.0;
    t.midDb = g.toneMid.load() / 10.0;
    t.hiDb = g.toneHi.load() / 10.0;
    return t;
}

static int vocalLevelOfThreshold(int threshold) {
    threshold = threshold < GATE_MIN_DB ? GATE_MIN_DB : (threshold > 0 ? 0 : threshold);
    if (threshold <= GATE_DEFAULT_DB) return (int)std::lround((threshold - GATE_MIN_DB) * (double)VOCAL_DEFAULT_LEVEL / (GATE_DEFAULT_DB - GATE_MIN_DB));
    return (int)std::lround(VOCAL_DEFAULT_LEVEL + (threshold - GATE_DEFAULT_DB) * (100.0 - VOCAL_DEFAULT_LEVEL) / -GATE_DEFAULT_DB);
}
static int vocalLevelOfOld(int old) {
    old = old < 0 ? 0 : (old > 100 ? 100 : old);
    if (old <= GATE_OLD_LEVEL) return (int)std::lround(old * (double)VOCAL_DEFAULT_LEVEL / GATE_OLD_LEVEL);
    return (int)std::lround(VOCAL_DEFAULT_LEVEL + (old - GATE_OLD_LEVEL) * (100.0 - VOCAL_DEFAULT_LEVEL) / (100 - GATE_OLD_LEVEL));
}
static iso::GateShape gateShape() {
    iso::GateShape s;
    const int level = g.gateLevel.load();
    s.thresholdDb = iso::gateLevelThresholdDb(level);
    s.slope = iso::gateLevelSlope(level);
    s.loDb = g.gateLo.load() / 10.0;
    s.midDb = g.gateMid.load() / 10.0;
    s.hiDb = g.gateHi.load() / 10.0;
    s.outDb = g.gateOut.load() / 10.0;
    return s;
}
static bool vocalOn() { return g.isolate.load() && g.isoMode.load() == ISO_VOCAL; }
static std::atomic<int>& isoFader() { return g.isoMode.load() == ISO_VOCAL ? g.gateLevel : g.isoLevel; }
static const wchar_t* isoModeName(int m) { return m == ISO_VOCAL ? L"Vocal" : iso::MODE_NAME[m < 0 ? 0 : (m >= iso::NUM_MODES ? iso::NUM_MODES - 1 : m)]; }

static const wchar_t* CABLE_SERVICE =
    L"SYSTEM\\CurrentControlSet\\Services\\VBAudioVACMME";

static bool driverPresent() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CABLE_SERVICE, 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS)
        return false;
    RegCloseKey(hk);
    return true;
}

static bool cableAdapter(const Dev& d) {
    return nameContains(d.iface, L"VB-Audio Virtual Cable");
}

static bool cableCapture(const Dev& d) {
    if (cableAdapter(d) || nameContains(d.name, L"CABLE Output")) return true;
    return d.iface.empty() && nameContains(d.name, L"What U Hear");
}

static bool cableRender(const Dev& d) {
    return (cableAdapter(d) && !nameContains(d.name, L"16ch")) || nameContains(d.name, L"CABLE Input");
}

static Dev findCableRender(DWORD states) {
    Dev best;
    for (auto& d : listDevices(eRender, states)) {
        if (!cableRender(d)) continue;
        if (nameContains(d.name, L"CABLE Input")) return d;
        if (best.id.empty()) best = d;
    }
    return best;
}

static std::wstring findParkRender(bool silentOk) {
    const std::wstring def = defaultDeviceId(eRender);
    const std::wstring cable = findCableRender(DEVICE_STATE_ACTIVE).id;
    std::wstring spk;
    { std::lock_guard<std::mutex> lk(g.devMx); spk = g.spkId; }
    std::wstring best;
    int bestScore = 0;
    for (const Dev& d : listDevices(eRender)) {
        if (d.id.empty() || d.id == def || d.id == cable || d.id == spk) continue;
        if (cableAdapter(d) || cableRender(d) || nameContains(d.name, L"CABLE") || nameContains(d.name, L"What U Hear")) continue;
        if (nameContains(d.name, L"Voicemeeter Input") && !nameContains(d.name, L"Aux") && !nameContains(d.name, L"VAIO3"))
            continue;
        int sc = 0;
        if (nameContains(d.name, L"Aux Input") || nameContains(d.name, L"Voicemeeter Aux")) sc = 100;
        else if (nameContains(d.name, L"VAIO3")) sc = 90;
        else if (silentOk && nameContains(d.name, L"Steam Streaming Speakers")) sc = 50;
        else continue;
        if (sc > bestScore) { bestScore = sc; best = d.id; }
    }
    return best;
}

static std::wstring renderName(const std::wstring& id) {
    if (id.empty()) return L"";
    for (const Dev& d : listDevices(eRender))
        if (d.id == id) return d.name;
    return L"";
}

static bool voicemeeterRender(const Dev& d) {
    return nameContains(d.name, L"Voicemeeter") || nameContains(d.iface, L"VB-Audio VoiceMeeter")
        || nameContains(d.name, L"VAIO");
}

static bool idIsVoicemeeter(const std::wstring& id) {
    if (id.empty()) return false;
    for (const Dev& d : listDevices(eRender))
        if (d.id == id) return voicemeeterRender(d);
    return false;
}

static std::wstring findAppHearRender() {
    const std::wstring park = findParkRender(false);
    const std::wstring cable = findCableRender(DEVICE_STATE_ACTIVE).id;
    const std::wstring def = defaultDeviceId(eRender);
    std::wstring spk;
    { std::lock_guard<std::mutex> lk(g.devMx); spk = g.spkId; }
    std::wstring best;
    int bestScore = 0;
    for (const Dev& d : listDevices(eRender)) {
        if (d.id.empty() || d.id == park || d.id == cable || d.id == spk || d.id == def) continue;
        if (cableRender(d) || nameContains(d.name, L"CABLE Input") || nameContains(d.name, L"What U Hear")) continue;
        if (nameContains(d.name, L"Voicemeeter Input") && !nameContains(d.name, L"Aux") && !nameContains(d.name, L"VAIO3"))
            continue;
        int sc = 0;
        if (nameContains(d.name, L"VAIO3")) sc = 100;
        else if (nameContains(d.name, L"Aux Input") || nameContains(d.name, L"Voicemeeter Aux")) continue;
        else if (nameContains(d.iface, L"VB-Audio VoiceMeeter") || nameContains(d.name, L"VAIO")) sc = 50;
        else continue;
        if (sc > bestScore) { bestScore = sc; best = d.id; }
    }
    return best;
}

static bool appToMonitor() { return !g.appHear.load() || g.appOnly.load(); }

static std::wstring appParkName;

static void appPick(const clip::Program& p) {
    const std::wstring park = findParkRender(appToMonitor());
    appParkName = renderName(park);
    gApp.start(p, FS, park);
}

static void appRepark() {
    if (!gApp.chosen()) return;
    if (findParkRender(appToMonitor()) != gApp.stealTo) appPick(clip::Program{gApp.pid, gApp.name});
}

static std::wstring monitorRenderId(const std::wstring& spkId) {
    const bool hearApp = gApp.chosen() && appToMonitor();
    const bool self = g.monitor.load() || g.clipHear.load() || takeHeld.load();
    if (hearApp && idIsVoicemeeter(spkId)) {
        if (self) return spkId;
        return findAppHearRender();
    }
    if (self || hearApp) return spkId;
    return {};
}

static void setStatus(const wchar_t* s, bool warn) {
    {
        std::lock_guard<std::mutex> lk(g.devMx);
        wcsncpy_s(g.cable, s, 255);
    }
    g.statusWarn = warn;
}

static bool cableDisabled() {
    const DWORD off = DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED;
    for (auto& d : listDevices(eCapture, off))
        if (cableCapture(d)) return true;
    return !findCableRender(off).id.empty();
}

static int renameTries = 0;

static bool isElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te{}; DWORD cb = sizeof te;
    const bool ok = GetTokenInformation(tok, TokenElevation, &te, sizeof te, &cb) && te.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

static bool cableReady() {
    for (auto& d : listDevices(eCapture)) {
        if (!cableCapture(d)) continue;
        if (!nameContains(d.name, L"What U Hear") && nameContains(d.name, L"CABLE Output") &&
            isElevated() && renameTries < 20 && !renameEndpoint(d.id, L"What U Hear"))
            ++renameTries;
        std::lock_guard<std::mutex> lk(g.devMx);
        wcsncpy_s(g.cableName, d.name.c_str(), 255);
        return true;
    }
    std::lock_guard<std::mutex> lk(g.devMx);
    g.cableName[0] = 0;
    return false;
}

static std::wstring exeFolder() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

static const wchar_t* INSTALL_FLAG = L"--install-cable";
static const UINT WM_CABLE_READY = WM_APP + 21;
enum { CABLE_IDLE, CABLE_FETCH, CABLE_UNPACK, CABLE_SETUP, CABLE_FAILED };
static std::atomic<int> gCableJob{CABLE_IDLE};
static std::atomic<bool> gCableRetry{false};

static std::wstring cableSetup() { return cable::setupIn(cable::folder()); }

static int elevatedInstall(const std::wstring& setup) {
    if (!cable::isSetupName(setup) || !cable::exists(setup)) return 2;
    const HANDLE held = CreateFileW(setup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (held == INVALID_HANDLE_VALUE) return 2;
    if (!cable::signedFile(setup, held)) {
        CloseHandle(held);
        return 2;
    }
    const std::wstring dir = setup.substr(0, setup.find_last_of(L"\\/"));
    SHELLEXECUTEINFOW se{};
    se.cbSize = sizeof(se);
    se.fMask = SEE_MASK_NOCLOSEPROCESS;
    se.lpVerb = L"open";
    se.lpFile = setup.c_str();
    se.lpDirectory = dir.c_str();
    se.nShow = SW_SHOWNORMAL;
    const bool started = ShellExecuteExW(&se) && se.hProcess;
    CloseHandle(held);
    if (!started) return 1;
    WaitForSingleObject(se.hProcess, 30 * 60 * 1000);
    CloseHandle(se.hProcess);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (int i = 0; i < 200 && !cableReady(); ++i) Sleep(100);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

static void startCableDownload(HWND owner) {
    int idle = CABLE_IDLE, failed = CABLE_FAILED;
    if (!gCableJob.compare_exchange_strong(idle, CABLE_FETCH) && !gCableJob.compare_exchange_strong(failed, CABLE_FETCH)) return;
    std::thread([owner] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const std::wstring dir = cable::folder();
        cable::makeFolder(dir);
        std::wstring setup = cable::setupIn(dir);
        if (setup.empty()) {
            const std::wstring page = dir + L"\\vb-cable.htm", zip = dir + L"\\VBCABLE_Driver_Pack.zip";
            const std::vector<std::wstring> urls = cable::packUrls(cable::fetch(cable::PAGE_URL, page) ? page : L"");
            DeleteFileW(page.c_str());
            bool got = false;
            for (const std::wstring& u : urls)
                if (cable::fetch(u, zip) && cable::sizeOf(zip) > 100000) { got = true; break; }
            if (got) {
                gCableJob = CABLE_UNPACK;
                cable::unzip(zip, dir);
                setup = cable::setupIn(dir);
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
        if (setup.empty()) { gCableJob = CABLE_FAILED; return; }
        gCableJob = CABLE_SETUP;
        PostMessageW(owner, WM_CABLE_READY, 0, 0);
    }).detach();
}

static void runCableSetup(HWND owner) {
    const std::wstring setup = cableSetup();
    if (setup.empty()) { gCableJob = CABLE_FAILED; return; }
    if (isElevated()) {
        std::thread([setup] { elevatedInstall(setup); gCableJob = CABLE_IDLE; g.restart = true; }).detach();
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring args = std::wstring(INSTALL_FLAG) + L" \"" + setup + L"\"";
    SHELLEXECUTEINFOW se{};
    se.cbSize = sizeof(se);
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    se.hwnd = owner;
    se.lpVerb = L"runas";
    se.lpFile = exe;
    se.lpParameters = args.c_str();
    se.nShow = SW_HIDE;
    if (!ShellExecuteExW(&se) || !se.hProcess) { gCableJob = CABLE_IDLE; return; }
    HANDLE proc = se.hProcess;
    std::thread([proc] {
        WaitForSingleObject(proc, INFINITE);
        CloseHandle(proc);
        gCableJob = CABLE_IDLE;
        g.restart = true;
    }).detach();
}

static void openCableFolder() {
    ShellExecuteW(nullptr, L"open", cable::folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void provisionCable() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int busyTicks = 0;
    for (;;) {
        const int job = gCableJob.load();
        if (job == CABLE_FETCH || job == CABLE_UNPACK) {
            setStatus(L"Downloading VB-CABLE...", true);
            g.needCable = true;
        } else if (job == CABLE_SETUP) {
            setStatus(L"Installing VB-CABLE. Finish the installer window, then restart your PC.", true);
            g.needCable = true;
        } else if (cableReady()) {
            const int state = g.cableState.load();
            const bool playback = !findCableRender(DEVICE_STATE_ACTIVE).id.empty();
            g.needCable = false;
            if (!playback) {
                const bool off = !findCableRender(DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED).id.empty();
                if (off)
                    { setStatus(L"CABLE Input is turned off. Turn it back on in Windows Sound settings, Playback tab.", true); g.cableStage = STAGE_PROBLEM; }
                else {
                    setStatus(L"VB-CABLE didn't install completely. Click here to install it again.", true); g.cableStage = STAGE_MISSING;
                    g.needCable = true;
                    g.askCable = true;
                }
                busyTicks = 0;
            } else if (state == 3) {
                setStatus(L"Another app is holding CABLE Input (Voicemeeter or OBS?). Close it or change its output.", true); g.cableStage = STAGE_PROBLEM;
                if (++busyTicks >= 5) { busyTicks = 0; gCableRetry = true; }
            } else {
                setStatus(L"Virtual mic ready", false); g.cableStage = STAGE_READY;
                busyTicks = 0;
                if (state == 2 && g.running.load()) g.restart = true;
            }
        } else if (cableDisabled()) {
            setStatus(L"The virtual mic is turned off. Turn on CABLE Output and CABLE Input in Windows Sound settings.", true); g.cableStage = STAGE_PROBLEM;
            g.needCable = false;
        } else if (driverPresent()) {
            setStatus(L"Restart your PC to finish installing VB-CABLE.", true); g.cableStage = STAGE_RESTART;
            g.needCable = false;
        } else {
            setStatus(L"No virtual mic yet. Click here to install VB-CABLE.", true); g.cableStage = STAGE_MISSING;
            g.needCable = true;
            g.askCable = true;
        }

        for (int i = 0; i < 30 && !g.quit.load(); ++i) Sleep(100);
        if (g.quit.load()) break;
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

static inline float micGainFor(int v) {
    return v <= 0 ? 0.0f : powf((float)v / 100.0f, 1.3010f);
}

static bool chordDown(int packed) {
    const int vk = packed & 0xFFFF, mods = packed >> 16;
    auto down = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
    if (!vk || !down(vk)) return false;
    if ((mods & MOD_CONTROL) && !down(VK_CONTROL)) return false;
    if ((mods & MOD_ALT) && !down(VK_MENU)) return false;
    if ((mods & MOD_SHIFT) && !down(VK_SHIFT)) return false;
    if ((mods & MOD_WIN) && !down(VK_LWIN) && !down(VK_RWIN)) return false;
    return true;
}

static int clipRate() {
    const int own = g.clipSpeed.load();
    if (!g.clipFollow.load() || !(g.changeOn.load() & ON_VOICE)) return own;
    return std::clamp(own * g.voiceSpeed.load() / 100, 25, 400);
}

static void flushDenormals() {
    _mm_setcsr(_mm_getcsr() | 0x8040);
}

static HANDLE gCapEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

static const int RENDER_BUFFER_MS = 80;
static const int CABLE_FILL_MS = 25;
static const int CABLE_SLACK_MS = 15;
static const int MONITOR_FILL_MS = 10;
static const int MONITOR_SLACK_MS = 4;
static const int CAPTURE_BUFFER_MS = 100;

static void floatStereo(WAVEFORMATEX& fmt) {
    fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = FS;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 8;
    fmt.nAvgBytesPerSec = FS * 8;
}

static bool mixIsFloatStereo48(const WAVEFORMATEX* f) {
    if (!f || f->nChannels != 2 || f->nSamplesPerSec != (DWORD)FS || f->wBitsPerSample != 32)
        return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE || f->cbSize < 22) return false;
    const auto* e = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    static const GUID kIeeeFloat =
        {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return IsEqualGUID(e->SubFormat, kIeeeFloat);
}

static void applyClientProps(IAudioClient* ac, bool raw) {
    IAudioClient2* ac2 = nullptr;
    if (FAILED(ac->QueryInterface(__uuidof(IAudioClient2), (void**)&ac2))) return;
    AudioClientProperties props{};
    props.cbSize = sizeof(props);
    props.bIsOffload = FALSE;
    props.eCategory = AudioCategory_Other;
    props.Options = raw ? AUDCLNT_STREAMOPTIONS_RAW : (AUDCLNT_STREAMOPTIONS)0;
    ac2->SetClientProperties(&props);
    ac2->Release();
}

static bool initLowLatency(IAudioClient* ac, DWORD flags, HANDLE evt) {
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(ac->GetMixFormat(&mix)) || !mix) return false;
    const bool okFmt = mixIsFloatStereo48(mix);
    IAudioClient3* ac3 = nullptr;
    bool ok = false;
    if (okFmt && SUCCEEDED(ac->QueryInterface(__uuidof(IAudioClient3), (void**)&ac3))) {
        UINT32 defP = 0, fundP = 0, minP = 0, maxP = 0;
        if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(mix, &defP, &fundP, &minP, &maxP)) && minP) {
            if (SUCCEEDED(ac3->InitializeSharedAudioStream(flags, minP, mix, nullptr))) {
                ok = !evt || SUCCEEDED(ac->SetEventHandle(evt));
            }
        }
        ac3->Release();
    }
    CoTaskMemFree(mix);
    return ok;
}

static IAudioClient* tryOpenStream(IMMDevice* dev, bool capture, bool raw, UINT32& bufFrames) {
    IAudioClient* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)))
        return nullptr;
    applyClientProps(ac, raw);
    HANDLE evt = capture ? gCapEvt : nullptr;
    if (capture && raw) {
        if (initLowLatency(ac, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, evt)) {
            ac->GetBufferSize(&bufFrames);
            return ac;
        }
        ac->Release();
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)))
            return nullptr;
        applyClientProps(ac, raw);
    }

    WAVEFORMATEX fmt;
    floatStereo(fmt);
    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    if (capture) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    const int ms = capture ? CAPTURE_BUFFER_MS : RENDER_BUFFER_MS;
    if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, (REFERENCE_TIME)ms * 10000, 0, &fmt, nullptr))) {
        ac->Release();
        return nullptr;
    }
    if (evt && FAILED(ac->SetEventHandle(evt))) {
        ac->Release();
        return nullptr;
    }
    ac->GetBufferSize(&bufFrames);
    return ac;
}

static IAudioClient* openStream(IMMDevice* dev, bool capture, bool raw, UINT32& bufFrames) {
    IAudioClient* ac = tryOpenStream(dev, capture, raw, bufFrames);
    if (ac) return ac;
    if (raw) return tryOpenStream(dev, capture, false, bufFrames);
    return nullptr;
}

static IAudioClient* openCapture(IMMDevice* dev, UINT32& bufFrames) {
    return openStream(dev, true, g.rawMic.load(), bufFrames);
}

static bool openFeed(IMMDevice* dev, IAudioClient*& ac, wasapi::RenderFeed& feed, bool quick) {
    UINT32 frames = 0;
    ac = openStream(dev, false, quick, frames);
    if (!ac) return false;
    const UINT32 fill = (UINT32)(FS * (quick ? MONITOR_FILL_MS : CABLE_FILL_MS) / 1000);
    const UINT32 slack = (UINT32)(FS * (quick ? MONITOR_SLACK_MS : CABLE_SLACK_MS) / 1000);
    if (feed.attach(ac, frames, fill, slack)) return true;
    ac->Release();
    ac = nullptr;
    return false;
}

struct MicSwap {
    std::thread worker;
    std::atomic<bool> done{false};
    bool busy = false;
    IAudioClient* fresh = nullptr;

    void begin(IMMDevice* dev) {
        busy = true;
        done = false;
        worker = std::thread([this, dev] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            UINT32 frames = 0;
            fresh = openCapture(dev, frames);
            if (SUCCEEDED(com)) CoUninitialize();
            done = true;
        });
    }
    bool ready() const { return busy && done.load(); }
    IAudioClient* collect() {
        worker.join();
        busy = false;
        IAudioClient* c = fresh;
        fresh = nullptr;
        return c;
    }
    void abandon() {
        if (worker.joinable()) worker.join();
        busy = false;
        if (fresh) { fresh->Release(); fresh = nullptr; }
    }
};

struct InputTap {
    DWORD lastInput = 0;
    POINT lastPos{};
    int lastButtons = 0;
    bool primed = false;

    bool poll() {
        LASTINPUTINFO li{sizeof(li), 0};
        if (!GetLastInputInfo(&li)) return false;
        static const int BUTTONS[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        int buttons = 0;
        for (int i = 0; i < 5; ++i)
            if (GetAsyncKeyState(BUTTONS[i]) & 0x8000) buttons |= 1 << i;
        POINT pos{};
        const bool havePos = GetCursorPos(&pos) != FALSE;
        const bool moved = havePos && (pos.x != lastPos.x || pos.y != lastPos.y);
        const bool fresh = primed && (buttons != lastButtons || (li.dwTime != lastInput && !moved));
        lastInput = li.dwTime;
        if (havePos) lastPos = pos;
        lastButtons = buttons;
        primed = true;
        return fresh;
    }
};

static const ULONGLONG ROOMS_KEEP_MS = 2000;
static const UINT32 MOST_LOST_FRAMES = FS * RENDER_BUFFER_MS / 1000;
static const int MIC_POS_TRUST = 8;
static conv::Reverb gConv;
static std::atomic<bool> gConvOn{false};

struct RoomBus {
    eax::EAXReverb* rv = nullptr;
    int preset = -1;
    Subsonic subL, subR;
    WetPeak peak;
    EchoTap slap;
    eax::CardCore core;
    StepBack back;
    bool slapLive = false, coreLive = false;
    float dryNow = 1.0f, dryWant = 1.0f;
    int64_t tail = 0;
    std::vector<float> sendL, sendR, wl, wr, tap, coreIn, coreOut;

    RoomBus() : core(FS, HC_CORE) {
        subL.prepare(FS);
        subR.prepare(FS);
        peak.prepare(FS);
        slap.prepare(FS);
        back.prepare(FS, HC_CORE.decayTime);
    }
    ~RoomBus() { delete rv; }
    RoomBus(const RoomBus&) = delete;
    RoomBus& operator=(const RoomBus&) = delete;

    void forget() {
        delete rv;
        rv = nullptr;
        preset = -1;
        slapLive = coreLive = false;
        tail = 0;
        back.reset();
    }

    void rest(int frames) {
        coreLive = false;
        back.idle(frames);
    }

    void run(int curPreset, bool coreOn, int amt, float cutWant, const float* cL, const float* cR, bool clipIn, float sendClip,
             const float* aL, const float* aR, float sendApp, UINT32 frames) {
        if (!rv || preset != curPreset) {
            delete rv;
            rv = new eax::EAXReverb(FS, CARD_PRESETS[curPreset].p);
            preset = curPreset;
        }
        sendL.resize(frames); sendR.resize(frames); wl.resize(frames); wr.resize(frames); tap.resize(frames);
        for (UINT32 i = 0; i < frames; ++i) {
            const float clipL = clipIn ? cL[i] : 0.0f, clipR = clipIn ? cR[i] : 0.0f;
            sendL[i] = subL.process(clipL * sendClip + aL[i] * sendApp);
            sendR[i] = subR.process(clipR * sendClip + aR[i] * sendApp);
            tap[i] = 0.5f * ((sendClip > 0.f ? clipL + clipR : 0.f) + (sendApp > 0.f ? aL[i] + aR[i] : 0.f));
        }
        const float gRoom = sendClip > sendApp ? sendClip : sendApp;
        if (preset == HAUNTED_CAVERN_INDEX) {
            rv->setCascade(gRoom * CASCADE_GAIN);
            rv->setCentre(AmountLaw::centre(amt));
        }
        rv->processStereo(sendL.data(), sendR.data(), wl.data(), wr.data(), (int)frames);
        if (curPreset == HAUNTED_CAVERN_INDEX)
            for (UINT32 i = 0; i < frames; ++i) peak.process(wl[i], wr[i]);
        if (curPreset == HAUNTED_CAVERN_INDEX) {
            if (!slapLive) { slap.clear(); slapLive = true; }
            slap.setDelay(AmountLaw::slapDelay(FS, amt));
            slap.process(tap.data(), (sendClip > 0.f || sendApp > 0.f) ? AmountLaw::slap(amt) : 0.f, wl.data(), wr.data(), (int)frames);
        } else slapLive = false;
        coreOut.resize(frames);
        if (coreOn) {
            if (!coreLive) { core.clear(); coreLive = true; }
            const float gCore = gRoom <= 0.f ? 0.0f : AmountLaw::core(amt);
            coreIn.resize(frames);
            for (UINT32 i = 0; i < frames; ++i) coreIn[i] = tap[i] * gCore;
            core.process(coreIn.data(), coreOut.data(), (int)frames);
            for (UINT32 i = 0; i < frames; ++i) { wl[i] += coreOut[i]; wr[i] += coreOut[i]; }
        } else coreLive = false;
        dryWant = powf(10.0f, -back.cutDb(cutWant, tap.data(), (int)frames) / 20.0f);
    }

    float roomL(UINT32 i, bool coreOn, bool wide) const { return wide ? wl[i] : (coreOn ? coreOut[i] : wl[i]); }
    float roomR(UINT32 i, bool coreOn, bool wide) const { return wide ? wr[i] : (coreOn ? coreOut[i] : wr[i]); }
};

static void audioThread() {
    flushDenormals();
    DWORD mmIndex = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIndex);
    if (mmTask) AvSetMmThreadPriority(mmTask, AVRT_PRIORITY_CRITICAL);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    gConvOn = gConv.load(exeFolder() + L"\\haunted_cavern_ir.wav", FS);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
        en = nullptr;

    int curPreset = g.preset.load();
    int curAmount = g.amount.load();

    auto buildEngine = [](int preset) { return new eax::EAXReverb(FS, CARD_PRESETS[preset].p); };
    auto buildLoudStage = []() {
        eax::Params p = CARD_PRESETS[HAUNTED_CAVERN_INDEX].p;
        p.decayTime *= HC_LOUD_DECAY / HC_CORE.decayTime;
        return new eax::EAXReverb(FS, p);
    };
    eax::EAXReverb* rv = buildEngine(curPreset);
    eax::EAXReverb* rvLoud = nullptr;

    std::vector<float> mono, send, wl, wr, dl, dr, kl, kr, inter, heard, micIn;
    MicDrive drive;
    float dryNow = 1.0f;
    StepBack voiceBack;
    voiceBack.prepare(FS, HC_CORE.decayTime);
    Subsonic subsonic;
    SendLevel sendLevel;
    sendLevel.prepare(FS);
    MicTone micTone;
    micTone.prepare(FS);
    WetPeak wetPeak;
    iso::VoiceIsolator isolator;
    isolator.prepare(FS);
    iso::ExpanderGate presetGate;
    presetGate.prepare(FS);
    voice::VoiceTone voiceTone;
    voiceTone.prepare(FS);
    InputTap inputTap;
    voice::VoiceShaper shaper;
    const auto voicefxBox = std::make_unique<voice::VoiceFx>();
    voice::VoiceFx& voicefx = *voicefxBox;
    voice::GlitchPad glitchPad;
    std::vector<voice::SongChanger> changers(2);
    voice::SongChanger& clipChanger = changers[0];
    voice::SongChanger& appChanger = changers[1];
    bool clipChanging = false, appChanging = false, songBusWas = false;
    voice::VoiceSpeed voicePace;
    voice::ClipSpeed clipPace;
    unsigned clipCutSeen = 0;
    voicePace.prepare(FS);
    clipPace.prepare(FS);
    gBoard.prepare(FS);
    gTakeMic.prepare(FS);
    gTakeEar.prepare(FS);
    shaper.prepare(FS);
    voicefx.prepare(FS);
    glitchPad.prepare(FS, 1);
    clipChanger.prepare(FS);
    appChanger.prepare(FS);
    bool shaperWas = false;
    int64_t shaperIdle = 0;
    auto songMask = [](voice::SongChanger& ch, bool& live, bool want, std::vector<float>& l, std::vector<float>& r, int n) {
        if (!want && !live) { ch.listen(l.data(), r.data(), n); return; }
        if (!live) { ch.start(); live = true; }
        ch.set(g.pitchHz.load(), g.voiceRatio.load(), g.voiceMask.load());
        ch.hold(want && (g.glitchHold.load() || g.glitchKeyHeld.load()), g.glitchStyle.load());
        ch.process(l.data(), r.data(), n, want);
        if (ch.idle()) live = false;
    };

    const auto rooms = std::make_unique<RoomBus[]>(2);
    RoomBus& pub = rooms[0];
    RoomBus& priv = rooms[1];
    SoftClip sumCab, sumMon;
    EchoTap slap;
    bool slapLive = false;
    std::vector<float> cL, cR, interMon, tapIn, aL, aR;
    std::vector<float> coreIn, coreOut, keep, loudIn, loudOut, loudL, loudR;
    std::vector<float> takeL, takeR, hearL, hearR;
    int64_t songTail = 0, appTail = 0;
    bool isoWas = false, vocalWas = false;
    int isoSeen = -1, vocalSeen = -1, toneSeen = -1;
    float isoGain = 0.0f, toneGain = 0.0f;
    OutChain voiceMon, voiceCab, progMon, progCab;
    OutChain* const chains[] = {&voiceMon, &voiceCab, &progMon, &progCab};
    for (OutChain* c : chains) c->prepare(FS);
    eqb::BassBoost voiceBass, progBass, appBass;
    for (eqb::BassBoost* b : {&voiceBass, &progBass, &appBass}) b->prepare(FS);
    eax::CoreRoom loudRoom = HC_CORE;
    loudRoom.decayTime = HC_LOUD_DECAY;
    eax::CardCore core(FS, HC_CORE), coreLoud(FS, loudRoom);
    int64_t loudStageTail = 0;
    bool coreLive = false, drainIsCore = false;
    int64_t coreTail = 0;
    int eqSeen = -1;
    subsonic.prepare(FS);
    wetPeak.prepare(FS);
    slap.prepare(FS);
    eax::EAXReverb* drainRv = nullptr;
    int64_t drainLeft = 0;
    bool measuredWas = false;
    ULONGLONG lastBlockAt = 0;
    MicSwap micSwap;

    auto forgetRooms = [&]() {
        delete rv;
        rv = buildEngine(curPreset);
        delete rvLoud;
        rvLoud = nullptr;
        delete drainRv;
        drainRv = nullptr;
        drainLeft = 0;
        measuredWas = false;
        pub.forget();
        priv.forget();
        coreLive = slapLive = false;
        coreTail = loudStageTail = songTail = appTail = 0;
        isoWas = shaperWas = clipChanging = appChanging = songBusWas = false;
        voicePace.reset();
        clipPace.reset();
        g.voiceLagMs = 0;
        voiceBack.reset();
    };

    while (!g.quit.load()) {
        if (!g.restart.exchange(false)) { Sleep(50); continue; }

        std::wstring micId, spkId;
        { std::lock_guard<std::mutex> lk(g.devMx); micId = g.micId; spkId = g.spkId; }

        const Dev cable = findCableRender(DEVICE_STATE_ACTIVE);

        IMMDevice *dMic = nullptr, *dSpk = nullptr, *dCab = nullptr;
        IAudioClient *acMic = nullptr, *acSpk = nullptr, *acCab = nullptr;
        IAudioCaptureClient* cap = nullptr;
        wasapi::RenderFeed feedCab, feedSpk;
        UINT32 bfMic = 0;
        std::wstring monOpenId;
        bool monOpenQuick = false;

        bool ok = false;
        do {
            if (micId.empty() || !en) break;
            if (FAILED(en->GetDevice(micId.c_str(), &dMic))) break;
            acMic = openCapture(dMic, bfMic);
            if (!acMic) break;
            if (FAILED(acMic->GetService(__uuidof(IAudioCaptureClient),
                                         (void**)&cap))) break;
            if (!cable.id.empty() && SUCCEEDED(en->GetDevice(cable.id.c_str(), &dCab)))
                openFeed(dCab, acCab, feedCab, false);
            const std::wstring monId = monitorRenderId(spkId);
            const bool quickMon = g.monitor.load() || g.clipHear.load();
            if (!monId.empty() && SUCCEEDED(en->GetDevice(monId.c_str(), &dSpk)) && openFeed(dSpk, acSpk, feedSpk, quickMon)) {
                monOpenId = monId;
                monOpenQuick = quickMon;
            }
            ok = true;
        } while (false);

        bool retryLater = !ok;
        if (!ok) {
            {
                std::lock_guard<std::mutex> lk(g.devMx);
                wcscpy_s(g.status, L"Can't open this microphone. Pick another one.");
            }
            g.running = false;
        } else {
            acMic->Start();
            if (acCab) acCab->Start();
            if (acSpk) acSpk->Start();
            g.cableState = feedCab.live() ? 1 : (cable.id.empty() ? 2 : 3);
            {
                std::lock_guard<std::mutex> lk(g.devMx);
                g.status[0] = 0;
            }
            g.running = true;
            g.reopenMic = false;
            gApp.resync();
            if (lastBlockAt && GetTickCount64() - lastBlockAt > ROOMS_KEEP_MS) forgetRooms();


            ULONGLONG nextMonTry = 0;
            int monFails = 0;
            std::wstring monId;
            int monKey = -1;
            UINT64 micNextPos = 0;
            int micPosRun = 0;
            while (!g.quit.load() && !g.restart.load()) {
                int key = 0;
                if (g.monitor.load()) key |= 1;
                if (g.clipHear.load()) key |= 2;
                if (gApp.chosen() && appToMonitor()) key |= 4;
                if (takeHeld.load()) key |= 8;
                if (key != monKey) {
                    monKey = key;
                    monId = key ? monitorRenderId(spkId) : std::wstring();
                    monFails = 0;
                    nextMonTry = 0;
                    if (feedSpk.live() && !monId.empty() && (monId != monOpenId || ((key & 3) != 0) != monOpenQuick)) {
                        feedSpk.release();
                        if (acSpk) { acSpk->Stop(); acSpk->Release(); acSpk = nullptr; }
                        if (dSpk) { dSpk->Release(); dSpk = nullptr; }
                    }
                }
                const bool wantMon = !monId.empty();
                const bool quickMon = (key & 3) != 0;
                if (wantMon && !feedSpk.live()) {
                    const ULONGLONG now = GetTickCount64();
                    if (now >= nextMonTry) {
                        if (dSpk) { dSpk->Release(); dSpk = nullptr; }
                        if (en) en->GetDevice(monId.c_str(), &dSpk);
                        if (dSpk && openFeed(dSpk, acSpk, feedSpk, quickMon) && acSpk) {
                            acSpk->Start();
                            monOpenId = monId;
                            monOpenQuick = quickMon;
                            monFails = 0;
                        } else {
                            nextMonTry = now + (2000ULL << std::min(monFails, 4));
                            ++monFails;
                        }
                    }
                }
                if (!feedCab.live() && !cable.id.empty() && gCableRetry.exchange(false)) {
                    if (!dCab) en->GetDevice(cable.id.c_str(), &dCab);
                    if (dCab && openFeed(dCab, acCab, feedCab, false) && acCab) acCab->Start();
                    g.cableState = feedCab.live() ? 1 : 3;
                }
                if (!wantMon && feedSpk.live()) {
                    feedSpk.release();
                    if (acSpk) { acSpk->Stop(); acSpk->Release(); acSpk = nullptr; }
                    if (dSpk) { dSpk->Release(); dSpk = nullptr; }
                }
                if (!micSwap.busy && g.reopenMic.exchange(false)) micSwap.begin(dMic);
                if (micSwap.ready()) {
                    IAudioClient* next = micSwap.collect();
                    IAudioCaptureClient* nextCap = nullptr;
                    if (next && SUCCEEDED(next->GetService(__uuidof(IAudioCaptureClient), (void**)&nextCap)) && SUCCEEDED(next->Start())) {
                        cap->Release();
                        acMic->Stop();
                        acMic->Release();
                        acMic = next;
                        cap = nextCap;
                        next->GetBufferSize(&bfMic);
                        micNextPos = 0;
                        micPosRun = 0;
                    } else {
                        if (nextCap) nextCap->Release();
                        if (next) next->Release();
                    }
                }
                curAmount = g.amount.load();
                if (g.preset.load() != curPreset) {
                    const bool fromCore = curPreset == HAUNTED_CAVERN_INDEX && !gConvOn.load();
                    curPreset = g.preset.load();

                    delete drainRv;
                    drainRv = rv;
                    drainIsCore = fromCore;
                    drainLeft = (int64_t)(rv->decayTime() * FS) + FS / 2;
                    rv = buildEngine(curPreset);
                }
                UINT32 pk = 0;
                if (FAILED(cap->GetNextPacketSize(&pk))) break;
                if (pk == 0) { WaitForSingleObject(gCapEvt, 20); continue; }
                BYTE* data = nullptr;
                UINT32 got = 0;
                DWORD flags = 0;
                UINT64 devPos = 0;
                if (FAILED(cap->GetBuffer(&data, &got, &flags, &devPos, nullptr))) break;
                const bool packetSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                UINT32 lost = 0;
                if (devPos == micNextPos) micPosRun = std::min(micPosRun + 1, MIC_POS_TRUST);
                else if (micPosRun >= MIC_POS_TRUST && bfMic < MOST_LOST_FRAMES && devPos > micNextPos && devPos - micNextPos <= MOST_LOST_FRAMES) lost = (UINT32)(devPos - micNextPos);
                else micPosRun = 0;
                micNextPos = devPos + got;
                if (packetSilent) micIn.assign((size_t)got * 2, 0.0f);
                else micIn.assign((const float*)data, (const float*)data + (size_t)got * 2);
                cap->ReleaseBuffer(got);
                bool feedLost = false;
                for (int pass = lost ? 0 : 1; pass < 2; ++pass) {
                    const UINT32 frames = pass == 0 ? lost : got;
                    const bool silent = pass == 0 || packetSilent;
                    const float* f = micIn.data();
                    mono.resize(frames); wl.resize(frames); wr.resize(frames);
                    inter.resize(frames * 2);
                    const float micGain = micGainFor(g.micLevel.load());
                    if (eqSeen != g.eqVersion.load()) {
                        eqSeen = g.eqVersion.load();
                        int b[eqb::EQ_BANDS];
                        for (int k = 0; k < eqb::EQ_BANDS; ++k) b[k] = g.eqDb[k].load();
                        for (OutChain* c : chains) c->eq.set(b, g.eqLevel.load());
                    }

                    const double boostDb = eqb::BassBoost::boostFor(micGain > 0.0f ? 20.0 * log10((double)micGain) : 0.0);
                    for (eqb::BassBoost* b : {&voiceBass, &progBass, &appBass}) b->set(boostDb);
                    for (UINT32 i = 0; i < frames; ++i) {
                        float v = silent ? 0.0f : 0.5f * (f[2 * i] + f[2 * i + 1]);
                        if (!(v > -4.0f && v < 4.0f)) v = 0.0f;
                        mono[i] = v * micGain;
                    }
                    const bool vocal = vocalOn();
                    if (vocal) {
                        if (vocalSeen != g.isoVersion.load()) {
                            vocalSeen = g.isoVersion.load();
                            presetGate.configure(gateShape());
                        }
                        if (!vocalWas) { presetGate.reset(); vocalWas = true; }
                        presetGate.process(mono.data(), (int)frames);
                    } else vocalWas = false;
                    if (g.isolate.load() && !vocal) {
                        if (!isoWas) { isolator.reset(); isoWas = true; isoGain = micGain; }
                        if (isoSeen != g.isoVersion.load()) {
                            isoSeen = g.isoVersion.load();
                            isolator.configure(g.isoMode.load(), g.isoLevel.load());
                        }
                        if (micGain > 0.0f && micGain != isoGain) {
                            if (isoGain > 0.0f) isolator.shiftLevel(20.0 * log10((double)micGain / isoGain));
                            isoGain = micGain;
                        }
                        if (pass == 1 && inputTap.poll()) isolator.inputHint();
                        isolator.process(mono.data(), (int)frames);
                    } else isoWas = false;
                    if (toneSeen != g.toneVersion.load()) {
                        toneSeen = g.toneVersion.load();
                        voiceTone.set(toneGains());
                    }
                    if (micGain > 0.0f && micGain != toneGain) {
                        if (toneGain > 0.0f) voiceTone.shiftLevel(20.0 * log10((double)micGain / toneGain));
                        toneGain = micGain;
                    }
                    voiceTone.process(mono.data(), (int)frames);

                    const bool voiceChange = (g.changeOn.load() & ON_VOICE) != 0;
                    voicePace.process(mono.data(), (int)frames, voiceChange ? g.voiceSpeed.load() : 100, g.voiceKeepPitch.load());
                    g.voiceLagMs = (int)(voicePace.lagSeconds() * 1000.0 + 0.5);
                    const int mi = g.voiceMask.load();
                    const bool masked = mi >= 0 && mi < voice::NUM_MASKS;
                    shaper.set(g.pitchHz.load(), g.voiceSize.load(), g.voiceRatio.load() / 1000.0);
                    shaper.anchor(masked ? voice::MASKS[mi].anchor : voice::Anchor{});
                    const bool shapeWanted = voiceChange && !shaper.neutral();
                    if (shapeWanted) shaperIdle = 0;
                    else if (shaperWas) shaperIdle += frames;
                    if (shapeWanted || (voiceChange && shaperWas && shaperIdle < (int64_t)FS)) {
                        if (!shaperWas) { shaper.reset(); shaperWas = true; }
                        shaper.process(mono.data(), (int)frames);
                        g.voiceHz = (int)(shaper.voiceHz() + 0.5);
                        g.voiceTargetHz = (int)(shaper.targetHz() + 0.5);
                    } else if (shaperWas) {
                        shaperWas = false;
                        g.voiceHz = 0;
                    }
                    if (voiceChange && masked && voice::MASKS[mi].fx != voice::FX_NONE) {
                        voicefx.configure(voice::MASKS[mi].fx, voice::MASKS[mi].a, voice::MASKS[mi].b);
                        voicefx.process(mono.data(), (int)frames);
                    } else voicefx.configure(voice::FX_NONE, 0.0, 0.0);
                    glitchPad.set(g.glitchStyle.load());
                    if (pass == 1) {
                        const bool keyDown = chordDown(g.glitchKey.load());
                        if (!keyDown) g.glitchArmed = true;
                        g.glitchKeyHeld = keyDown && g.glitchArmed.load();
                    }
                    glitchPad.hold(voiceChange && (g.glitchHold.load() || g.glitchKeyHeld.load()));
                    glitchPad.process(mono.data(), nullptr, (int)frames);

                    micTone.on = g.cardMic.load() && !g.rawMic.load();
                    for (UINT32 i = 0; i < frames; ++i) mono[i] = micTone.process(mono[i]);

                    drive.set(g.micLevel.load());
                    const float gDrive = drive.gain();
                    for (UINT32 i = 0; i < frames; ++i) mono[i] = voiceBass.run(mono[i] * gDrive);

                    const int amt = curAmount;
                    const int echoTo = g.echoOn.load();
                    const bool fxAll = !g.enabled.load() || amt <= 0;
                    const bool effectOff = fxAll || !(echoTo & ON_VOICE);
                    const bool appFxOff = fxAll || !(echoTo & ON_PROGRAM);
                    const bool clipFxOff = fxAll || !(echoTo & ON_CLIP);

                    const bool measured = gConvOn.load() && curPreset == HAUNTED_CAVERN_INDEX;
                    if (measured && !measuredWas) gConv.clear();
                    measuredWas = measured;
                    const float wetAmt = measured ? powf((float)amt / 100.0f, AmountLaw::WET_EXP)
                                                  : AmountLaw::send((float)amt / 100.0f);
                    const float gSend = effectOff ? 0.0f : wetAmt;
                    const float gSendApp = appFxOff ? 0.0f : wetAmt;
                    const float gSendClip = clipFxOff ? 0.0f : wetAmt;
                    if (curPreset == HAUNTED_CAVERN_INDEX && rv) { rv->setCascade(gSend * CASCADE_GAIN); rv->setCentre(AmountLaw::centre(amt)); }
                    const bool coreOn = !measured && curPreset == HAUNTED_CAVERN_INDEX;
                    send.resize(frames); tapIn.resize(frames); keep.resize(frames);
                    for (UINT32 i = 0; i < frames; ++i) {
                        const float level = sendLevel.process(mono[i]);
                        keep[i] = coreOn ? level : 1.0f;
                        tapIn[i] = subsonic.process(mono[i]);
                        send[i] = tapIn[i] * keep[i] * gSend;
                    }
                    if (measured)
                        gConv.process(send.data(), wl.data(), wr.data(), (int)frames);
                    else
                        rv->process(send.data(), wl.data(), wr.data(), (int)frames);

                    const bool wide = g.wide.load();
                    if (coreOn) loudStageTail = (int64_t)(HC_LOUD_DECAY * FS);
                    else if (loudStageTail > 0) loudStageTail -= frames;
                    if (wide && (coreOn || loudStageTail > 0)) {
                        if (!rvLoud) rvLoud = buildLoudStage();
                        rvLoud->setCascade(gSend * CASCADE_GAIN);
                        rvLoud->setCentre(AmountLaw::centre(amt));
                        loudIn.resize(frames); loudL.resize(frames); loudR.resize(frames);
                        for (UINT32 i = 0; i < frames; ++i) loudIn[i] = coreOn ? tapIn[i] * (1.0f - keep[i]) * gSend : 0.0f;
                        rvLoud->process(loudIn.data(), loudL.data(), loudR.data(), (int)frames);
                        for (UINT32 i = 0; i < frames; ++i) { wl[i] += loudL[i]; wr[i] += loudR[i]; }
                    } else if (rvLoud) {
                        delete rvLoud;
                        rvLoud = nullptr;
                    }

                    kl.resize(frames); kr.resize(frames);
                    if (coreOn) {
                        std::fill(kl.begin(), kl.end(), 0.0f);
                        std::fill(kr.begin(), kr.end(), 0.0f);
                    } else {
                        std::copy(wl.begin(), wl.end(), kl.begin());
                        std::copy(wr.begin(), wr.end(), kr.begin());
                    }
                    if (drainRv) {
                        std::fill(send.begin(), send.end(), 0.0f);
                        dl.resize(frames); dr.resize(frames);
                        drainRv->process(send.data(), dl.data(), dr.data(),
                                         (int)frames);
                        for (UINT32 i = 0; i < frames; ++i) {
                            wl[i] += dl[i];
                            wr[i] += dr[i];
                            if (!drainIsCore) { kl[i] += dl[i]; kr[i] += dr[i]; }
                        }
                        drainLeft -= frames;
                        if (drainLeft <= 0) { delete drainRv; drainRv = nullptr; }
                    }

                    if (coreOn) coreTail = (int64_t)(HC_CORE.decayTime * FS);
                    else if (coreTail > 0) coreTail -= frames;
                    if (coreOn || coreTail > 0) {
                        if (!coreLive) { core.clear(); coreLoud.clear(); coreLive = true; }
                        const float gCore = (coreOn && !effectOff) ? AmountLaw::core(amt) : 0.0f;
                        coreIn.resize(frames); coreOut.resize(frames); loudIn.resize(frames); loudOut.resize(frames);
                        for (UINT32 i = 0; i < frames; ++i) { coreIn[i] = tapIn[i] * keep[i] * gCore; loudIn[i] = tapIn[i] * (1.0f - keep[i]) * gCore; }
                        core.process(coreIn.data(), coreOut.data(), (int)frames);
                        coreLoud.process(loudIn.data(), loudOut.data(), (int)frames);
                        for (UINT32 i = 0; i < frames; ++i) coreOut[i] += loudOut[i];
                        for (UINT32 i = 0; i < frames; ++i) { kl[i] += coreOut[i]; kr[i] += coreOut[i]; }
                    } else coreLive = false;

                    if (!measured && curPreset == HAUNTED_CAVERN_INDEX)
                        for (UINT32 i = 0; i < frames; ++i) wetPeak.process(wl[i], wr[i]);

                    const float gSlap = effectOff ? 0.0f : AmountLaw::slap(amt);
                    if (!measured && curPreset == HAUNTED_CAVERN_INDEX) {
                        if (!slapLive) { slap.clear(); slapLive = true; }
                        slap.setDelay(AmountLaw::slapDelay(FS, amt));
                        slap.process(tapIn.data(), gSlap, wl.data(), wr.data(), (int)frames);
                    } else slapLive = false;
                    if (coreLive)
                        for (UINT32 i = 0; i < frames; ++i) { wl[i] += coreOut[i]; wr[i] += coreOut[i]; }

                    const bool eqOn = g.eqOn.load(), cmssOn = g.cmss.load();
                    const float cutWant = (!effectOff && curPreset == HAUNTED_CAVERN_INDEX) ? AmountLaw::dryCutDb(amt) : 0.0f;
                    const float cutWantApp = ((!appFxOff || !clipFxOff) && curPreset == HAUNTED_CAVERN_INDEX) ? AmountLaw::dryCutDb(amt) : 0.0f;
                    const float dryWant = powf(10.0f, -voiceBack.cutDb(cutWant, tapIn.data(), (int)frames) / 20.0f);
                    heard.resize(frames * 2);
                    for (UINT32 i = 0; i < frames; ++i) {
                        dryNow += 0.002f * (dryWant - dryNow);
                        const float dry = mono[i] * dryNow;
                        float l = drive.process(dry + (wide ? wl[i] : kl[i]));
                        float r = drive.process(dry + (wide ? wr[i] : kr[i]));
                        float cl = drive.process(dry + kl[i]);
                        float cr = drive.process(dry + kr[i]);
                        voiceMon.process(l, r, eqOn, cmssOn);
                        heard[2 * i] = l;
                        heard[2 * i + 1] = r;
                        voiceCab.process(cl, cr, eqOn, cmssOn);
                        inter[2 * i] = cl;
                        inter[2 * i + 1] = cr;
                    }
                    gRec.feed(inter.data(), frames);
                    gBackVoice.feed(inter.data(), frames);

                    interMon.resize(frames * 2);
                    const bool songOn = gClip.active() || gBoard.active(), appOn = gApp.active();
                    const int64_t roomTail = pub.rv ? (int64_t)(pub.rv->decayTime() * FS) + FS / 2 : 0;
                    if (songOn) songTail = roomTail; else if (songTail > 0) songTail -= frames;
                    if (appOn) appTail = roomTail; else if (appTail > 0) appTail -= frames;
                    const bool songLive = songOn || songTail > 0, appLive = appOn || appTail > 0;

                    if (songLive || appLive) {
                        if (!songBusWas) {
                            clipChanger.reset();
                            appChanger.reset();
                            clipChanging = appChanging = false;
                            songBusWas = true;
                        }
                        cL.resize(frames); cR.resize(frames); aL.resize(frames); aR.resize(frames);
                        if (gClip.cuts.load() != clipCutSeen) { clipCutSeen = gClip.cuts.load(); clipPace.reset(); }
                        if (gClip.isPaused()) {
                            std::fill(cL.begin(), cL.end(), 0.0f);
                            std::fill(cR.begin(), cR.end(), 0.0f);
                        } else {
                            clipPace.render(cL.data(), cR.data(), (int)frames, clipRate(), g.clipKeepPitch.load(),
                                            [](float* l, float* r, int n) { gClip.pull(l, r, n); });
                        }
                        gBoard.mix(cL.data(), cR.data(), (int)frames);
                        if (appOn) gApp.pull(aL.data(), aR.data(), (int)frames);
                        else { std::fill(aL.begin(), aL.end(), 0.0f); std::fill(aR.begin(), aR.end(), 0.0f); }
                        const float cg = micGainFor(g.clipGain.load());
                        const int appV = g.appGain.load();
                        const bool hearApp = appToMonitor(), appOnly = g.appOnly.load(), appDryMon = hearApp && gApp.parking.load();
                        const float agCab = micGainFor(appV);

                        const bool toMic = g.clipToMic.load(), clipHear = g.clipHear.load(), self = g.monitor.load();
                        const bool clipMine = !toMic && clipHear, appMine = appOnly && hearApp;
                        const bool roomToCable = (toMic && songLive) || (appLive && !appOnly);
                        const bool roomToMon = (clipHear && toMic && songLive) || (hearApp && appLive && !appOnly);

                        for (UINT32 i = 0; i < frames; ++i) {
                            cL[i] *= cg; cR[i] *= cg;
                            aL[i] *= agCab; aR[i] *= agCab;
                            progBass.process(cL[i], cR[i]);
                            appBass.process(aL[i], aR[i]);
                        }
                        {
                            const int to = g.changeOn.load();
                            songMask(clipChanger, clipChanging, (to & ON_CLIP) != 0 && songLive, cL, cR, (int)frames);
                            songMask(appChanger, appChanging, (to & ON_PROGRAM) != 0 && appLive, aL, aR, (int)frames);
                        }
                        pub.run(curPreset, coreOn, amt, cutWantApp, cL.data(), cR.data(), toMic, gSendClip,
                                aL.data(), aR.data(), appOnly ? 0.0f : gSendApp, frames);
                        if ((clipMine && songOn) || (appMine && appOn)) priv.tail = roomTail;
                        else if (priv.tail > 0) priv.tail -= frames;
                        const bool privLive = ((clipMine && songOn) || (appMine && appOn)) || priv.tail > 0;
                        if (privLive)
                            priv.run(curPreset, coreOn, amt, cutWantApp, cL.data(), cR.data(), clipMine, clipMine ? gSendClip : 0.0f,
                                     aL.data(), aR.data(), appMine ? gSendApp : 0.0f, frames);
                        else
                            priv.rest((int)frames);
                        for (UINT32 i = 0; i < frames; ++i) {
                            pub.dryNow += 0.002f * (pub.dryWant - pub.dryNow);
                            if (privLive) priv.dryNow += 0.002f * (priv.dryWant - priv.dryNow);
                            const float songDry = clipMine ? priv.dryNow : pub.dryNow, appDry = appMine ? priv.dryNow : pub.dryNow;
                            const float songL = cL[i] * songDry, songR = cR[i] * songDry;
                            const float appL = aL[i] * appDry, appR = aR[i] * appDry;
                            float l = (clipHear ? songL : 0.0f) + (appDryMon ? appL : 0.0f) + (roomToMon ? pub.roomL(i, coreOn, wide) : 0.0f);
                            float r = (clipHear ? songR : 0.0f) + (appDryMon ? appR : 0.0f) + (roomToMon ? pub.roomR(i, coreOn, wide) : 0.0f);
                            if (privLive) {
                                l += priv.roomL(i, coreOn, wide);
                                r += priv.roomR(i, coreOn, wide);
                            }
                            progMon.process(l, r, eqOn, cmssOn);
                            float ml = (self ? heard[2 * i] : 0.0f) + l;
                            float mr = (self ? heard[2 * i + 1] : 0.0f) + r;
                            sumMon.process(ml, mr);
                            interMon[2 * i] = ml; interMon[2 * i + 1] = mr;
                            float pl = (toMic ? songL : 0.0f) + (appOnly ? 0.0f : appL) + (roomToCable ? pub.roomL(i, coreOn, false) : 0.0f);
                            float pr = (toMic ? songR : 0.0f) + (appOnly ? 0.0f : appR) + (roomToCable ? pub.roomR(i, coreOn, false) : 0.0f);
                            progCab.process(pl, pr, eqOn, cmssOn);
                            float xl = inter[2 * i] + pl, xr = inter[2 * i + 1] + pr;
                            sumCab.process(xl, xr);
                            inter[2 * i] = xl; inter[2 * i + 1] = xr;
                        }
                    } else {
                        songBusWas = false;
                        pub.rest((int)frames);
                        priv.rest((int)frames);
                        const bool self = g.monitor.load();
                        for (UINT32 i = 0; i < frames * 2; ++i) interMon[i] = self ? heard[i] : 0.f;
                    }
                    if (gTakeMic.active() || gTakeEar.active()) {
                        takeL.assign(frames, 0.0f); takeR.assign(frames, 0.0f);
                        hearL.assign(frames, 0.0f); hearR.assign(frames, 0.0f);
                        gTakeMic.mix(takeL.data(), takeR.data(), (int)frames);
                        gTakeEar.mix(hearL.data(), hearR.data(), (int)frames);
                        for (UINT32 i = 0; i < frames; ++i) {
                            float xl = inter[2 * i] + takeL[i], xr = inter[2 * i + 1] + takeR[i];
                            float ml = interMon[2 * i] + takeL[i] + hearL[i], mr = interMon[2 * i + 1] + takeR[i] + hearR[i];
                            sumCab.process(xl, xr);
                            sumMon.process(ml, mr);
                            inter[2 * i] = xl; inter[2 * i + 1] = xr;
                            interMon[2 * i] = ml; interMon[2 * i + 1] = mr;
                        }
                    }
                    {
                        float loud = 0.0f;
                        for (UINT32 i = 0; i < frames * 2; ++i) { const float a = fabsf(inter[i]); if (a > loud) loud = a; }
                        const int q = (int)(loud * 10000.0f);
                        int seen = g.meterPeak.load();
                        while (q > seen && !g.meterPeak.compare_exchange_weak(seen, q)) {}
                    }
                    if (feedCab.live() && !feedCab.push(inter.data(), frames)) { feedLost = true; break; }
                    if (feedSpk.live() && !feedSpk.push(interMon.data(), frames)) {
                        feedSpk.release();
                        if (acSpk) { acSpk->Stop(); acSpk->Release(); acSpk = nullptr; }
                        if (dSpk) { dSpk->Release(); dSpk = nullptr; }
                        nextMonTry = GetTickCount64() + 2000;
                    }
                    lastBlockAt = GetTickCount64();
                }
                if (feedLost) break;
            }
            if (!g.quit.load() && !g.restart.load()) retryLater = true;
            g.running = false;
            g.cableState = 0;
        }
        micSwap.abandon();
        if (cap) cap->Release();
        feedSpk.release();
        feedCab.release();
        if (acMic) { acMic->Stop(); acMic->Release(); }
        if (acSpk) { acSpk->Stop(); acSpk->Release(); }
        if (acCab) { acCab->Stop(); acCab->Release(); }
        if (dMic) dMic->Release();
        if (dSpk) dSpk->Release();
        if (dCab) dCab->Release();
        if (retryLater) {
            for (int i = 0; i < 10 && !g.quit.load(); ++i) Sleep(100);
            g.restart = true;
        }
    }
    delete rv;
    delete rvLoud;
    delete drainRv;
    if (en) en->Release();
    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
    if (SUCCEEDED(com)) CoUninitialize();
}

static const int COL_W = 566;
static const int CX    = COL_W - 12;
static const int WIN_W = COL_W + CX, WIN_H = 756;
static const int TITLE_H = 34;
static const int STATUS_H = 34;

static float uiScale = 1.0f;
static int uiZoom = 100, uiDpi = 96, takeFormat = 0;
enum { FROM_VOICE = 1, FROM_HEAR = 2 };
static const int KEEP_STEPS[] = {0, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 240, 300, 360, 420, 480, 540, 600};
static const int NUM_KEEP_STEPS = (int)(sizeof KEEP_STEPS / sizeof KEEP_STEPS[0]), KEEP_DEFAULT_SEC = 60;
static int recFrom = FROM_VOICE, keepSec = KEEP_DEFAULT_SEC;
static bool lastQuiet = true;
static const int ZOOM_MIN = 50, ZOOM_MAX = 300;

static int sc(int v) { return (int)lroundf(v * uiScale); }
static int unsc(int v) { return (int)floorf((v + 0.5f) / uiScale); }
static RECT scRect(const RECT& r) { return {sc(r.left), sc(r.top), sc(r.right), sc(r.bottom)}; }
static RECT designRect(int w, int h) { return {0, 0, (int)lroundf(w / uiScale), (int)lroundf(h / uiScale)}; }

static void scaleDc(HDC dc) {
    SetGraphicsMode(dc, GM_ADVANCED);
    XFORM x = {uiScale, 0.0f, 0.0f, uiScale, 0.0f, 0.0f};
    SetWorldTransform(dc, &x);
}

static void flatDc(HDC dc) { ModifyWorldTransform(dc, nullptr, MWT_IDENTITY); }

static HPEN linePen(COLORREF c) { return CreatePen(PS_SOLID, uiScale == floorf(uiScale) ? 1 : 0, c); }


struct PixelSnap {
    HDC dc;
    XFORM keep{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    bool read = false;

    explicit PixelSnap(HDC d) : dc(d) {
        read = GetWorldTransform(dc, &keep) != FALSE;
        if (!read) keep = XFORM{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        flatDc(dc);
    }
    ~PixelSnap() {
        if (read) SetWorldTransform(dc, &keep);
    }
    PixelSnap(const PixelSnap&) = delete;
    PixelSnap& operator=(const PixelSnap&) = delete;

    LONG x(float v) const { return lroundf(v * keep.eM11 + keep.eDx); }
    LONG y(float v) const { return lroundf(v * keep.eM22 + keep.eDy); }
    RECT map(const RECT& r) const { return {x((float)r.left), y((float)r.top), x((float)r.right), y((float)r.bottom)}; }
    int line() const { return keep.eM11 >= 1.0f && keep.eM11 == floorf(keep.eM11) ? (int)keep.eM11 : 1; }
    RECT inside(const RECT& r) const {
        RECT d = map(r);
        InflateRect(&d, -line(), -line());
        return d;
    }
};

static void edgeLines(HDC dc, const RECT& d, int t, COLORREF edge) {
    HBRUSH b = CreateSolidBrush(edge);
    const RECT sides[4] = {{d.left, d.top, d.right, d.top + t}, {d.left, d.bottom - t, d.right, d.bottom},
                           {d.left, d.top, d.left + t, d.bottom}, {d.right - t, d.top, d.right, d.bottom}};
    for (const RECT& side : sides) FillRect(dc, &side, b);
    DeleteObject(b);
}

static bool mouseMsg(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL; }

static LPARAM designPt(LPARAM l) {
    const int x = unsc((short)LOWORD(l)), y = unsc((short)HIWORD(l));
    return MAKELPARAM((WORD)(short)x, (WORD)(short)y);
}

static bool modalNext(MSG& msg) {
    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got > 0) return true;
    if (got == 0) PostQuitMessage((int)msg.wParam);
    return false;
}

static POINT wheelPoint(HWND h, LPARAM l) {
    POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
    ScreenToClient(h, &p);
    p.x = unsc(p.x);
    p.y = unsc(p.y);
    return p;
}

static double fadeBegin(HWND h, UINT m, WPARAM w, LPARAM l);
static void fadeEnd(double outer);
static void fadeApply(HWND h, HDC dc, const RECT& paint);
static void fadeForget(HWND h);
static void fadeDrop(HWND h);
static bool hoverWatch(HWND h, UINT m, WPARAM w);
static void hoverFade(HWND h);

template <LRESULT (CALLBACK *Proc)(HWND, UINT, WPARAM, LPARAM)>
static LRESULT CALLBACK scaledProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCDESTROY) fadeForget(h);
    const double outer = fadeBegin(h, m, w, l);
    const bool watch = hoverWatch(h, m, w);
    const LRESULT r = Proc(h, m, w, mouseMsg(m) ? designPt(l) : l);
    fadeEnd(outer);
    if (watch) hoverFade(h);
    return r;
}

static void invalidateDesign(HWND h, const RECT& r) {
    if (IsRectEmpty(&r)) return;
    RECT d = scRect(r);
    InflateRect(&d, 2, 2);
    InvalidateRect(h, &d, FALSE);
}

static int dpiOfMonitor(HMONITOR mon) {
    typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) fn = (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor");
    }
    UINT dx = 96, dy = 96;
    if (fn && mon && SUCCEEDED(fn(mon, 0, &dx, &dy)) && dx >= 48) return (int)dx;
    HDC screen = GetDC(nullptr);
    const int d = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return d >= 48 ? d : 96;
}

static RECT workAreaOf(HMONITOR mon) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    RECT wa = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    return wa;
}

static float fitScale(float wanted, const RECT& wa) {
    const float fit = std::min((wa.right - wa.left - 8) / (float)WIN_W, (wa.bottom - wa.top - 8) / (float)WIN_H);
    const float s = std::min(wanted, fit);
    return s < ZOOM_MIN / 100.0f ? ZOOM_MIN / 100.0f : (s > ZOOM_MAX / 100.0f ? ZOOM_MAX / 100.0f : s);
}

static float zoomScale() { return uiDpi / 96.0f * uiZoom / 100.0f; }

static void paintBox(HWND h, void (*paintFn)(HDC, const RECT&));
static void boxTitle(HDC dc, const RECT& rc, const wchar_t* title, const RECT& close, bool closeHot);
static void boxText(HDC dc, const RECT& r, const wchar_t* s, COLORREF c, HFONT f, UINT align);
static void printBox(HWND h, HDC dc, void (*paintFn)(HDC, const RECT&));
static const int STRIP_H = 30;

struct Theme { int hue, sat, depth; };
struct Look {
    COLORREF accent, text, dim, panel, back, edge, hover, off;
    int clear, blur, darken;
    Theme theme;
};
struct ThemeChip { const wchar_t* name; int hue, sat; };
static const ThemeChip THEME_CHIPS[] = {
    {L"Purple", 265, 100}, {L"Blue", 218, 100}, {L"Sky", 198, 100}, {L"Teal", 172, 100}, {L"Green", 140, 100},
    {L"Gold", 44, 100}, {L"Orange", 24, 100}, {L"Red", 356, 100}, {L"Pink", 326, 100}, {L"Graphite", 230, 0}};
static const int NUM_THEME_CHIPS = (int)(sizeof(THEME_CHIPS) / sizeof(THEME_CHIPS[0]));
static const Theme PURPLE_THEME = {265, 100, 40};
static const int CLEAR_DEFAULT = 25, BLUR_DEFAULT = 30, DARKEN_DEFAULT = 35;

static COLORREF hslRgb(double h, double s, double l) {
    h = fmod(fmod(h, 360.0) + 360.0, 360.0);
    s = s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
    l = l < 0.0 ? 0.0 : (l > 1.0 ? 1.0 : l);
    const double c = (1.0 - fabs(2.0 * l - 1.0)) * s;
    const double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
    const double m = l - c / 2.0;
    double r = 0.0, gr = 0.0, b = 0.0;
    switch ((int)(h / 60.0)) {
    case 0: r = c; gr = x; break;
    case 1: r = x; gr = c; break;
    case 2: gr = c; b = x; break;
    case 3: gr = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    auto byte = [](double v) { const int o = (int)(v * 255.0 + 0.5); return (BYTE)(o < 0 ? 0 : (o > 255 ? 255 : o)); };
    return RGB(byte(r + m), byte(gr + m), byte(b + m));
}

static void rgbHsl(COLORREF c, double& h, double& s, double& l) {
    const double r = GetRValue(c) / 255.0, gr = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
    const double mx = r > gr ? (r > b ? r : b) : (gr > b ? gr : b);
    const double mn = r < gr ? (r < b ? r : b) : (gr < b ? gr : b);
    const double d = mx - mn;
    l = (mx + mn) / 2.0;
    s = d < 1e-9 ? 0.0 : d / (1.0 - fabs(2.0 * l - 1.0));
    if (d < 1e-9) h = 0.0;
    else if (mx == r) h = 60.0 * fmod((gr - b) / d + 6.0, 6.0);
    else if (mx == gr) h = 60.0 * ((b - r) / d + 2.0);
    else h = 60.0 * ((r - gr) / d + 4.0);
}

static void themeColours(Look& lk, const Theme& t) {
    const double h = t.hue, s = t.sat / 100.0, base = 0.06 + 0.10 * t.depth / 100.0;
    lk.accent = hslRgb(h, 0.55 * s, 0.72);
    lk.text   = hslRgb(h, 0.30 * s, 0.92);
    lk.dim    = hslRgb(h, 0.16 * s, 0.66);
    lk.back   = hslRgb(h, 0.22 * s, base);
    lk.panel  = hslRgb(h, 0.22 * s, base + 0.045);
    lk.edge   = hslRgb(h, 0.24 * s, base + 0.16);
    lk.hover  = hslRgb(h, 0.36 * s, base + 0.22);
    lk.off    = hslRgb(h, 0.12 * s, 0.42);
    lk.theme  = t;
}

static Theme themeFromColour(COLORREF c, int depth) {
    double h, s, l;
    rgbHsl(c, h, s, l);
    const int sat = (int)(s / 0.55 * 100.0 + 0.5);
    return Theme{(int)(h + 0.5) % 360, sat > 100 ? 100 : sat, depth};
}

static Look defaultLook() {
    Look lk{};
    lk.clear = CLEAR_DEFAULT; lk.blur = BLUR_DEFAULT; lk.darken = DARKEN_DEFAULT;
    themeColours(lk, PURPLE_THEME);
    return lk;
}

static Look look = defaultLook();
static COLORREF C_NAVY, C_GOLD, C_TEXT, C_DIM, C_PANEL, C_BACK, C_EDGE, C_OFF, C_KNOB;

static COLORREF& lookSlot(Look& lk, int i) {
    COLORREF* s[8] = {&lk.accent, &lk.text, &lk.dim, &lk.panel, &lk.back, &lk.edge, &lk.hover, &lk.off};
    return *s[i];
}
static COLORREF& lookSlot(int i) { return lookSlot(look, i); }
static int lookKeyIndex(const std::string& k) {
    static const char* KEYS[8] = {"accent", "text", "dim", "panel", "back", "edge", "hover", "off"};
    for (int i = 0; i < 8; ++i)
        if (k == KEYS[i]) return i;
    return -1;
}

static ULONG_PTR gdipTok = 0;
static std::wstring wallPath;
static HBITMAP wallBmp = nullptr;
static int wallW = 0, wallH = 0;
static int uiAlpha = 100;
static HWND hwndMain = nullptr;
static Gdiplus::Bitmap* logoSrc = nullptr;
static HBITMAP logoBmp = nullptr;
static int logoW = 0, logoH = 0;
static Gdiplus::Bitmap* artSrc = nullptr;
static HBITMAP artBmp = nullptr;
static int artW = 0, artH = 0;
static HBRUSH brLookBack = nullptr, brLookPanel = nullptr;
static void tintLogo(COLORREF c);
static void buildArt();

static COLORREF shade(COLORREF c, double k) {
    auto ch = [k](int v) { const int o = (int)(v * k + 0.5); return o < 0 ? 0 : (o > 255 ? 255 : o); };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

static COLORREF mixc(COLORREF a, COLORREF b, double t) {
    auto ch = [t](int x, int y) { return (int)(x + (y - x) * t + 0.5); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

static void lookBrushes() {
    if (brLookBack) { DeleteObject(brLookBack); brLookBack = nullptr; }
    if (brLookPanel) { DeleteObject(brLookPanel); brLookPanel = nullptr; }
    brLookBack = CreateSolidBrush(C_BACK);
    brLookPanel = CreateSolidBrush(C_PANEL);
}

static void applyLook() {
    C_GOLD = look.accent; C_TEXT = look.text; C_DIM = look.dim; C_PANEL = look.panel;
    C_BACK = look.back; C_EDGE = look.edge; C_NAVY = look.hover; C_OFF = look.off;
    C_KNOB = shade(look.accent, 0.5);
    lookBrushes();
    tintLogo(look.accent);
    buildArt();
    tipsColour();
}

static void dropBackground() {
    if (wallBmp) { DeleteObject(wallBmp); wallBmp = nullptr; }
    wallW = wallH = 0;
}

static void blurPass(const uint32_t* in, uint32_t* out, int w, int h, int r, bool across) {
    const int n = across ? w : h, lines = across ? h : w;
    const int step = across ? 1 : w, lineStep = across ? w : 1;
    for (int L = 0; L < lines; ++L) {
        const uint32_t* s = in + (size_t)L * lineStep;
        uint32_t* d = out + (size_t)L * lineStep;
        auto px = [&](int i) { i = i < 0 ? 0 : (i >= n ? n - 1 : i); return s[(size_t)i * step]; };
        int sb = 0, sg = 0, sr = 0;
        const int cnt = 2 * r + 1;
        for (int i = -r; i <= r; ++i) { const uint32_t p = px(i); sb += p & 255; sg += (p >> 8) & 255; sr += (p >> 16) & 255; }
        for (int i = 0; i < n; ++i) {
            d[(size_t)i * step] = (uint32_t)(sr / cnt) << 16 | (uint32_t)(sg / cnt) << 8 | (uint32_t)(sb / cnt);
            const uint32_t add = px(i + r + 1), sub = px(i - r);
            sb += (int)(add & 255) - (int)(sub & 255);
            sg += (int)((add >> 8) & 255) - (int)((sub >> 8) & 255);
            sr += (int)((add >> 16) & 255) - (int)((sub >> 16) & 255);
        }
    }
}

static void uprightPhoto(Gdiplus::Bitmap& img) {
    const UINT size = img.GetPropertyItemSize(PropertyTagOrientation);
    if (size < sizeof(Gdiplus::PropertyItem) + sizeof(USHORT)) return;
    std::vector<BYTE> buf(size);
    auto* item = (Gdiplus::PropertyItem*)buf.data();
    if (img.GetPropertyItem(PropertyTagOrientation, size, item) != Gdiplus::Ok || item->type != PropertyTagTypeShort || !item->value) return;
    static const Gdiplus::RotateFlipType turn[9] = {
        Gdiplus::RotateNoneFlipNone, Gdiplus::RotateNoneFlipNone, Gdiplus::RotateNoneFlipX, Gdiplus::Rotate180FlipNone,
        Gdiplus::Rotate180FlipX, Gdiplus::Rotate90FlipX, Gdiplus::Rotate90FlipNone, Gdiplus::Rotate270FlipX, Gdiplus::Rotate270FlipNone};
    const USHORT o = *(const USHORT*)item->value;
    if (o >= 2 && o <= 8) img.RotateFlip(turn[o]);
}

static void buildBackground() {
    dropBackground();
    if (wallPath.empty()) return;
    Gdiplus::Bitmap src(wallPath.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok || src.GetWidth() == 0 || src.GetHeight() == 0) return;
    uprightPhoto(src);
    const int bw = sc(WIN_W), bh = sc(WIN_H);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bw; bi.bmiHeader.biHeight = -bh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) { if (dib) DeleteObject(dib); return; }
    HDC mdc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mdc, dib);
    {
        Gdiplus::Graphics gr(mdc);
        gr.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const double sw = src.GetWidth(), sh = src.GetHeight();
        const double k = max(bw / sw, bh / sh);
        const int dw = (int)(sw * k + 0.5), dh = (int)(sh * k + 0.5);
        gr.DrawImage(&src, (bw - dw) / 2, (bh - dh) / 2, dw, dh);
    }
    SelectObject(mdc, old);
    DeleteDC(mdc);
    GdiFlush();
    uint32_t* px = (uint32_t*)bits;
    const int radius = sc(look.blur * 40 / 100);
    if (radius > 0) {
        std::vector<uint32_t> tmp((size_t)bw * bh);
        for (int pass = 0; pass < 3; ++pass) {
            blurPass(px, tmp.data(), bw, bh, radius, true);
            blurPass(tmp.data(), px, bw, bh, radius, false);
        }
    }
    if (look.darken > 0) {
        const uint32_t keep = (uint32_t)(100 - look.darken);
        for (size_t i = 0; i < (size_t)bw * bh; ++i) {
            const uint32_t p = px[i];
            px[i] = ((p >> 16 & 255) * keep / 100) << 16 | ((p >> 8 & 255) * keep / 100) << 8 | ((p & 255) * keep / 100);
        }
    }
    wallBmp = dib;
    wallW = bw;
    wallH = bh;
}

static void clearWallpaper() {
    dropBackground();
    wallPath.clear();
}

static bool loadWallpaper(const wchar_t* path) {
    if (!path || !path[0]) { clearWallpaper(); return false; }
    const std::wstring before = wallPath;
    wallPath = path;
    buildBackground();
    if (wallBmp) return true;
    wallPath = before;
    buildBackground();
    return false;
}

static Gdiplus::Bitmap* pngResource(int id) {
    const HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC hr = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!hr) return nullptr;
    HGLOBAL hg = LoadResource(mod, hr);
    if (!hg) return nullptr;
    const DWORD n = SizeofResource(mod, hr);
    const void* p = LockResource(hg);
    if (!p || !n) return nullptr;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!mem) return nullptr;
    void* dst = GlobalLock(mem);
    if (!dst) { GlobalFree(mem); return nullptr; }
    memcpy(dst, p, n);
    GlobalUnlock(mem);
    IStream* st = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &st)) || !st) { GlobalFree(mem); return nullptr; }
    Gdiplus::Bitmap* decoded = nullptr;
    {
        Gdiplus::Bitmap src(st);
        const int w = (int)src.GetWidth(), h = (int)src.GetHeight();
        if (src.GetLastStatus() == Gdiplus::Ok && w > 0 && h > 0) {
            decoded = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
            if (decoded->GetLastStatus() == Gdiplus::Ok) {
                Gdiplus::Graphics gr(decoded);
                gr.DrawImage(&src, 0, 0, w, h);
            } else {
                delete decoded;
                decoded = nullptr;
            }
        }
    }
    st->Release();
    return decoded;
}

static void loadLogoSrc() {
    if (!logoSrc) logoSrc = pngResource(201);
}

static const int ART_H = 330;

static void buildArt() {
    if (!artSrc) artSrc = pngResource(202);
    if (artBmp) { DeleteObject(artBmp); artBmp = nullptr; artW = artH = 0; }
    if (!artSrc) return;
    const int sw = (int)artSrc->GetWidth(), sh = (int)artSrc->GetHeight();
    if (sw < 1 || sh < 1) return;
    const int th = sc(ART_H), tw = max(1, sw * th / sh);
    Gdiplus::Bitmap conv(tw, th, PixelFormat32bppARGB);
    if (conv.GetLastStatus() != Gdiplus::Ok) return;
    {
        Gdiplus::Graphics gr(&conv);
        gr.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        gr.Clear(Gdiplus::Color(0, 0, 0, 0));
        gr.DrawImage(artSrc, 0, 0, tw, th);
    }
    Gdiplus::BitmapData bd{};
    Gdiplus::Rect rr(0, 0, tw, th);
    if (conv.LockBits(&rr, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return;
    if (!bd.Scan0) { conv.UnlockBits(&bd); return; }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) { conv.UnlockBits(&bd); if (hb) DeleteObject(hb); return; }
    const double hue = look.theme.hue, satK = look.theme.sat / 100.0;
    for (int y = 0; y < th; ++y) {
        const unsigned char* s = (const unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
        unsigned char* d = (unsigned char*)bits + (size_t)y * tw * 4;
        for (int x = 0; x < tw; ++x, s += 4, d += 4) {
            const int a = s[3];
            if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            double h, sat, l;
            rgbHsl(RGB(s[2], s[1], s[0]), h, sat, l);
            const COLORREF c = hslRgb(hue, sat * satK, l);
            d[0] = (unsigned char)(GetBValue(c) * a / 255);
            d[1] = (unsigned char)(GetGValue(c) * a / 255);
            d[2] = (unsigned char)(GetRValue(c) * a / 255);
            d[3] = (unsigned char)a;
        }
    }
    conv.UnlockBits(&bd);
    artBmp = hb;
    artW = max(1, sw * ART_H / sh);
    artH = ART_H;
}

static HBITMAP backBuffer(int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
}

static void drawBitmapAlpha(HDC dc, HBITMAP bmp, int x, int y, int w, int h) {
    if (!bmp) return;
    BITMAP info{};
    GetObjectW(bmp, sizeof info, &info);
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, x, y, w, h, mem, 0, 0, info.bmWidth, info.bmHeight, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

static Gdiplus::Color gpColor(COLORREF c, int a = 255) {
    return Gdiplus::Color((BYTE)(a < 0 ? 0 : (a > 255 ? 255 : a)), GetRValue(c), GetGValue(c), GetBValue(c));
}

static void drawGlow(HDC dc, float cx, float cy, float rx, float ry, COLORREF c, int a) {
    const RECT reach = {(int)(cx - rx) - 1, (int)(cy - ry) - 1, (int)(cx + rx) + 2, (int)(cy + ry) + 2};
    if (!RectVisible(dc, &reach)) return;
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    path.AddEllipse(cx - rx, cy - ry, 2.0f * rx, 2.0f * ry);
    Gdiplus::PathGradientBrush br(&path);
    br.SetCenterColor(gpColor(c, a));
    Gdiplus::Color edge = gpColor(c, 0);
    INT count = 1;
    br.SetSurroundColors(&edge, &count);
    gr.FillPath(&br, &path);
}

static void tintLogo(COLORREF c) {
    loadLogoSrc();
    if (!logoSrc) return;
    const int th0 = (int)logoSrc->GetHeight();
    const int dh = th0 > 0 && th0 < 26 ? th0 : 26;
    const int sw = (int)logoSrc->GetWidth(), sh = (int)logoSrc->GetHeight();
    if (sw < 1 || sh < 1) return;
    const int th = sc(dh);
    int tw = sw * th / sh;
    if (tw < 1) tw = 1;
    Gdiplus::Bitmap conv(tw, th, PixelFormat32bppARGB);
    if (conv.GetLastStatus() != Gdiplus::Ok) return;
    Gdiplus::Graphics gfx(&conv);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
    gfx.DrawImage(logoSrc, 0, 0, tw, th);
    Gdiplus::BitmapData bd{};
    Gdiplus::Rect rr(0, 0, tw, th);
    if (conv.LockBits(&rr, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return;
    if (!bd.Scan0) { conv.UnlockBits(&bd); return; }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) { conv.UnlockBits(&bd); if (hb) DeleteObject(hb); return; }
    const int cr = GetRValue(c), cg = GetGValue(c), cbv = GetBValue(c);
    for (int y = 0; y < th; ++y) {
        unsigned char* s = (unsigned char*)bd.Scan0 + y * bd.Stride;
        unsigned char* d = (unsigned char*)bits + y * tw * 4;
        for (int x = 0; x < tw; ++x) {
            const int a = s[3];
            const int lum = s[0] > s[1] ? (s[0] > s[2] ? s[0] : s[2]) : (s[1] > s[2] ? s[1] : s[2]);
            d[0] = (unsigned char)(cbv * lum / 255 * a / 255);
            d[1] = (unsigned char)(cg * lum / 255 * a / 255);
            d[2] = (unsigned char)(cr * lum / 255 * a / 255);
            d[3] = (unsigned char)a;
            s += 4; d += 4;
        }
    }
    conv.UnlockBits(&bd);
    if (logoBmp) DeleteObject(logoBmp);
    logoBmp = hb;
    logoW = max(1, sw * dh / sh);
    logoH = dh;
}

static void drawLogo(HDC dc, int x, int y) {
    drawBitmapAlpha(dc, logoBmp, x, y + (TITLE_H - logoH) / 2, logoW, logoH);
}

enum { FX_NONE, FX_OPEN, FX_CLOSE, FX_HIDE, FX_DOCK, FX_UNDOCK };
struct WindowFx {
    int kind = FX_NONE;
    double start = 0.0, ms = 0.0;
    bool faded = false, resizing = false;
};
static WindowFx winFx;

static BYTE windowAlphaByte() {
    int a = uiAlpha;
    if (a < 0) a = 0;
    if (a > 100) a = 100;
    const int vis = a < 12 ? 12 : a;
    return (BYTE)(vis * 255 / 100);
}

static void applyWindowAlpha(HWND h) {
    if (!h || (h == hwndMain && (winFx.faded || winFx.resizing || winFx.kind != FX_NONE))) return;
    SetLayeredWindowAttributes(h, 0, windowAlphaByte(), LWA_ALPHA);
}

static void blendRect(HDC dc, const RECT& r, COLORREF c, BYTE a) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 1 || h < 1 || a == 0 || !RectVisible(dc, &r)) return;
    if (a >= 250) {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
        return;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 1;
    bi.bmiHeader.biHeight = 1;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP hb = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb) { DeleteDC(mem); return; }
    unsigned char* px = (unsigned char*)bits;
    px[0] = (unsigned char)(GetBValue(c) * a / 255);
    px[1] = (unsigned char)(GetGValue(c) * a / 255);
    px[2] = (unsigned char)(GetRValue(c) * a / 255);
    px[3] = a;
    HGDIOBJ old = SelectObject(mem, hb);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, r.left, r.top, w, h, mem, 0, 0, 1, 1, bf);
    SelectObject(mem, old);
    DeleteObject(hb);
    DeleteDC(mem);
}

static int alphaFor(COLORREF fill) {
    if (!wallBmp) return 255;
    const int a = 255 - 255 * look.clear / 100;
    if (fill == C_PANEL) return a;
    if (fill == C_BACK) return a + (255 - a) * 2 / 3;
    return 255;
}

static void drawWallpaper(HDC dc, const RECT& dst, int srcY) {
    if (!wallBmp) return;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, wallBmp);
    const RECT d = scRect(dst);
    const int w = d.right - d.left, h = d.bottom - d.top, fullW = sc(WIN_W), fullH = sc(WIN_H);
    flatDc(dc);
    if (wallW == fullW && wallH == fullH) {
        BitBlt(dc, d.left, d.top, w, h, mem, d.left, sc(srcY), SRCCOPY);
    } else {
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, d.left, d.top, w, h, mem, d.left * wallW / fullW, sc(srcY) * wallH / fullH, w * wallW / fullW, h * wallH / fullH, SRCCOPY);
    }
    scaleDc(dc);
    SelectObject(mem, old);
    DeleteDC(mem);
}

static bool pickWallpaper(HWND owner) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Background picture";
    if (!GetOpenFileNameW(&ofn)) return false;
    return loadWallpaper(file);
}

static void refreshLook(HWND h) {
    if (h) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static bool pickColour(HWND owner, COLORREF& c);
static void pickLook(HWND owner);
static void setUiFace(const std::wstring& face);
static void setUiType(const std::wstring& face, bool bold, int size);
static std::wstring dataDir();
static void centredText(HDC dc, const RECT& r, const wchar_t* s, COLORREF c, HFONT f);
static void toneSettle();
static void openToneBox(HWND owner);
static HWND hTone = nullptr;
static void showAbout(HWND owner);

enum { ID_MIC = 101, ID_SPK, ID_PRESET };

static const RECT R_SLIDER = {332, 212, 536, 234};
static const RECT R_MIC    = {332, 288, 536, 310};
static const int  MIC_MAX  = 1000;
static const int  APP_MAX  = 1000;
static const int  CLIP_MAX = 1000;
static int clipLevelFromOld(int old) {
    return old < 0 ? CLIP_GAIN_DEFAULT : (int)std::lround(100.0 * std::pow(old / 100.0, 1.0 / 1.3010));
}
static const RECT R_CLOSE  = {WIN_W - 34, 5, WIN_W - 8, 29};
static const RECT R_MIN    = {WIN_W - 64, 5, WIN_W - 38, 29};
static const RECT R_GEAR   = {WIN_W - 94, 5, WIN_W - 68, 29};
static const RECT R_GRIP   = {WIN_W - 16, WIN_H - 16, WIN_W - 2, WIN_H - 2};

static const int PRESET_X   = 24;
static const int PRESET_Y   = 208;
static const int PRESET_LW  = 270;
static const int PRESET_H   = 220;
static const int PRESET_SBX = PRESET_X + PRESET_LW + 4;
static const int PRESET_SBW = 12;
static const RECT R_SB_UP    = {PRESET_SBX, PRESET_Y, PRESET_SBX + PRESET_SBW, PRESET_Y + PRESET_SBW};
static const RECT R_SB_DOWN  = {PRESET_SBX, PRESET_Y + PRESET_H - PRESET_SBW, PRESET_SBX + PRESET_SBW, PRESET_Y + PRESET_H};
static const RECT R_SB_TRACK = {PRESET_SBX, PRESET_Y + PRESET_SBW, PRESET_SBX + PRESET_SBW, PRESET_Y + PRESET_H - PRESET_SBW};
static const RECT R_SB_ALL   = {PRESET_SBX, PRESET_Y, PRESET_SBX + PRESET_SBW, PRESET_Y + PRESET_H};

static std::vector<Dev> uiMics, uiSpks;
static HWND hMic, hSpk, hPreset;
static HFONT fntUI, fntBold, fntTitle, fntCtl, fntCtlBold, fntCtlTitle;

static const wchar_t* const BASE_FACE = L"Segoe UI";
static const wchar_t* const UI_FACES[] = {L"Segoe UI", L"Bahnschrift", L"Calibri", L"Candara", L"Corbel", L"Trebuchet MS", L"Tahoma", L"Arial",
                                          L"Century Gothic", L"Cambria", L"Constantia", L"Palatino Linotype", L"Sitka Text", L"Consolas",
                                          L"Comic Sans MS", L"Segoe Print"};
static const struct { int em, weight; } UI_TYPE[3] = {{12, FW_NORMAL}, {12, FW_BOLD}, {19, FW_BOLD}};
static const int TEXT_SIZE_MIN = 11, TEXT_SIZE_BASE = 12, TEXT_SIZE_MAX = 13;
static std::wstring uiFace = BASE_FACE;
static bool uiBold = false;
static int uiSize = TEXT_SIZE_BASE;
static const int NO_PLACE = -99999;
static bool uiAnim = true, uiTop = false, uiPlace = true, uiHints = true;
static int winX = NO_PLACE, winY = NO_PLACE;

static void applyTopmost(HWND h) {
    if (h) SetWindowPos(h, uiTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void boxShow(HWND h) {
    if (uiAnim) AnimateWindow(h, 160, AW_BLEND);
    else ShowWindow(h, SW_SHOW);
}

static void boxClose(HWND h) {
    if (!h || !IsWindow(h)) return;
    if (uiAnim && IsWindowVisible(h)) AnimateWindow(h, 130, AW_HIDE | AW_BLEND);
    DestroyWindow(h);
}

static const UINT FX_TIMER = 41;
static const double FX_OPEN_MS = 450.0, FX_CLOSE_MS = 300.0, FX_HIDE_MS = 260.0, FX_DOCK_MS = 280.0, FX_UNDOCK_MS = 320.0;

struct DockImage {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;

    bool make(int width, int height) {
        if (dc && w == width && h == height) return true;
        drop();
        if (width < 1 || height < 1) return false;
        dc = CreateCompatibleDC(nullptr);
        bmp = dc ? backBuffer(width, height) : nullptr;
        if (!bmp) {
            drop();
            return false;
        }
        old = SelectObject(dc, bmp);
        w = width;
        h = height;
        return true;
    }

    void drop() {
        if (dc && old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        dc = nullptr;
        bmp = nullptr;
        old = nullptr;
        w = h = 0;
    }
};

struct Dock {
    HWND ghost = nullptr;
    DockImage shot, frame;
    RECT from{}, to{}, at{};
    double fadeFrom = 1.0, fadeTo = 0.0, fade = 1.0;
    POINT spot{};
    bool spotKnown = false;
};
static Dock dock;

static double fxNow() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static double easeInOut(double k) { return k * k * (3.0 - 2.0 * k); }

static void composeWait() {
    typedef HRESULT (WINAPI *TimingFn)(HWND, DWM_TIMING_INFO*);
    static TimingFn timing = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) timing = (TimingFn)GetProcAddress(dwm, "DwmGetCompositionTimingInfo");
    }
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof ti;
    if (!timing || FAILED(timing(nullptr, &ti)) || ti.qpcRefreshPeriod == 0) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const ULONGLONG t = (ULONGLONG)now.QuadPart, period = ti.qpcRefreshPeriod;
    const ULONGLONG next = ti.qpcVBlank > t ? ti.qpcVBlank : ti.qpcVBlank + ((t - ti.qpcVBlank) / period + 1) * period;
    const double ms = (double)(next - t) * 1000.0 / (double)freq.QuadPart;
    if (ms >= 0.5 && ms < 40.0) Sleep((DWORD)std::lround(ms));
}

static RECT taskbarRect(HMONITOR mon) {
    RECT bar{};
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    const bool primary = tray && GetWindowRect(tray, &bar);
    if (primary && MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST) == mon) return bar;
    for (HWND s = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); s; s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) {
        RECT r{};
        if (MonitorFromWindow(s, MONITOR_DEFAULTTONEAREST) == mon && GetWindowRect(s, &r)) return r;
    }
    if (primary) return bar;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    return {mi.rcMonitor.left, mi.rcMonitor.bottom - 48, mi.rcMonitor.right, mi.rcMonitor.bottom};
}

static RECT dockSpot(HWND h, const RECT& home) {
    const RECT bar = taskbarRect(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
    POINT cur{};
    if (GetCursorPos(&cur) && PtInRect(&bar, cur)) {
        dock.spot = cur;
        dock.spotKnown = true;
    }
    POINT at = dock.spot;
    if (!dock.spotKnown || !PtInRect(&bar, at)) {
        const bool across = bar.right - bar.left >= bar.bottom - bar.top;
        const LONG cx = (home.left + home.right) / 2, cy = (home.top + home.bottom) / 2;
        at.x = across ? std::max(bar.left, std::min(cx, bar.right - 1)) : (bar.left + bar.right) / 2;
        at.y = across ? (bar.top + bar.bottom) / 2 : std::max(bar.top, std::min(cy, bar.bottom - 1));
    }
    const LONG w = std::max(2L, (home.right - home.left) / 8), hh = std::max(2L, (home.bottom - home.top) / 8);
    return {at.x - w / 2, at.y - hh / 2, at.x - w / 2 + w, at.y - hh / 2 + hh};
}

static void dockDrop() {
    if (dock.ghost) DestroyWindow(dock.ghost);
    dock.ghost = nullptr;
    dock.shot.drop();
    dock.frame.drop();
}

static HWND makeGhost(HWND owner, const wchar_t* cls, WNDPROC proc, DWORD exStyle) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    if (!GetClassInfoW(inst, cls, &wc)) {
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
    }
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | exStyle,
                           cls, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr, inst, nullptr);
}

static bool dockGhost() {
    if (!dock.ghost) dock.ghost = makeGhost(nullptr, L"OctopusDock", DefWindowProcW, WS_EX_TRANSPARENT | WS_EX_TOPMOST);
    return dock.ghost != nullptr;
}

static bool dockShoot(HWND h, const RECT& wr) {
    const int w = wr.right - wr.left, hh = wr.bottom - wr.top;
    return dock.shot.make(w, hh) && dock.frame.make(w, hh) && PrintWindow(h, dock.shot.dc, PW_RENDERFULLCONTENT);
}

static void dockDraw(double k) {
    const bool away = winFx.kind == FX_DOCK;
    const double move = away ? k * k : 1.0 - (1.0 - k) * (1.0 - k) * (1.0 - k);
    const double show = away ? std::pow(k, 2.2) : 1.0 - std::pow(1.0 - k, 2.2);
    auto mix = [move](LONG a, LONG b) { return (LONG)std::lround(a + (b - a) * move); };
    dock.at = {mix(dock.from.left, dock.to.left), mix(dock.from.top, dock.to.top), mix(dock.from.right, dock.to.right), mix(dock.from.bottom, dock.to.bottom)};
    dock.fade = dock.fadeFrom + (dock.fadeTo - dock.fadeFrom) * show;
    const int w = std::max(1, std::min(dock.frame.w, (int)(dock.at.right - dock.at.left)));
    const int hh = std::max(1, std::min(dock.frame.h, (int)(dock.at.bottom - dock.at.top)));
    SetStretchBltMode(dock.frame.dc, HALFTONE);
    SetBrushOrgEx(dock.frame.dc, 0, 0, nullptr);
    StretchBlt(dock.frame.dc, 0, 0, w, hh, dock.shot.dc, 0, 0, dock.shot.w, dock.shot.h, SRCCOPY);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)std::lround(windowAlphaByte() * std::min(1.0, std::max(0.0, dock.fade))), 0};
    POINT pos = {dock.at.left, dock.at.top}, src = {0, 0};
    SIZE size = {w, hh};
    UpdateLayeredWindow(dock.ghost, nullptr, &pos, &size, dock.frame.dc, &src, 0, &bf, ULW_ALPHA);
}

static void dockRun(HWND h, int kind, double ms) {
    winFx.kind = kind;
    winFx.start = fxNow();
    winFx.ms = ms * std::max(0.35, std::fabs(dock.fadeTo - dock.fadeFrom));
    dockDraw(0.0);
    ShowWindow(dock.ghost, SW_SHOWNOACTIVATE);
    if (kind == FX_DOCK) SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    composeWait();
    SetTimer(h, FX_TIMER, 8, nullptr);
}

static void dockAway(HWND h, const RECT& home) {
    dock.from = dock.at;
    dock.fadeFrom = dock.fade;
    dock.to = dockSpot(h, home);
    dock.fadeTo = 0.0;
    dockRun(h, FX_DOCK, FX_DOCK_MS);
}

static bool dockDown(HWND h) {
    RECT wr;
    GetWindowRect(h, &wr);
    if (winFx.kind != FX_UNDOCK || !dock.ghost) {
        KillTimer(h, FX_TIMER);
        winFx.kind = FX_NONE;
        if (!dockShoot(h, wr) || !dockGhost()) {
            dockDrop();
            return false;
        }
        BYTE now = 0;
        DWORD flags = 0;
        COLORREF key = 0;
        if (!GetLayeredWindowAttributes(h, &key, &now, &flags) || !(flags & LWA_ALPHA)) now = windowAlphaByte();
        dock.at = wr;
        dock.fade = std::min(1.0, now / (double)windowAlphaByte());
    }
    dockAway(h, wr);
    winFx.faded = true;
    ShowWindow(h, SW_MINIMIZE);
    return true;
}

static bool dockUp(HWND h) {
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    RECT wr;
    GetWindowRect(h, &wr);
    if (winFx.kind != FX_DOCK || !dock.ghost) {
        KillTimer(h, FX_TIMER);
        winFx.kind = FX_NONE;
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (!dockShoot(h, wr) || !dockGhost()) {
            dockDrop();
            return false;
        }
        dock.at = dockSpot(h, wr);
        dock.fade = 0.0;
    }
    dock.from = dock.at;
    dock.fadeFrom = dock.fade;
    dock.to = wr;
    dock.fadeTo = 1.0;
    dockRun(h, FX_UNDOCK, FX_UNDOCK_MS);
    return true;
}

static void fxFrame(HWND h) {
    const double k = std::min(1.0, std::max(0.0, (fxNow() - winFx.start) / winFx.ms));
    if (winFx.kind == FX_DOCK || winFx.kind == FX_UNDOCK) {
        dockDraw(k);
        composeWait();
        if (k < 1.0) return;
        KillTimer(h, FX_TIMER);
        const int kind = winFx.kind;
        winFx.kind = FX_NONE;
        if (kind == FX_UNDOCK) {
            applyWindowAlpha(h);
            ShowWindow(dock.ghost, SW_HIDE);
            composeWait();
        }
        dockDrop();
        return;
    }
    const double shown = winFx.kind == FX_OPEN ? easeInOut(k) : 1.0 - easeInOut(k);
    SetLayeredWindowAttributes(h, 0, (BYTE)std::lround(windowAlphaByte() * shown), LWA_ALPHA);
    if (k < 1.0) return;
    KillTimer(h, FX_TIMER);
    const int kind = winFx.kind;
    winFx.kind = FX_NONE;
    if (kind == FX_CLOSE) {
        DestroyWindow(h);
    } else if (kind == FX_HIDE) {
        winFx.faded = true;
        ShowWindow(h, SW_MINIMIZE);
    } else {
        applyWindowAlpha(h);
    }
}

static void fxStart(HWND h, int kind) {
    winFx.kind = kind;
    winFx.start = fxNow();
    winFx.ms = kind == FX_OPEN ? FX_OPEN_MS : (kind == FX_CLOSE ? FX_CLOSE_MS : FX_HIDE_MS);
    SetTimer(h, FX_TIMER, 8, nullptr);
    fxFrame(h);
}

static void windowOpen(HWND h, int show) {
    if (!uiAnim || show == SW_HIDE || show == SW_MINIMIZE || show == SW_SHOWMINIMIZED || show == SW_SHOWMINNOACTIVE) {
        ShowWindow(h, show);
        return;
    }
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    ShowWindow(h, show);
    UpdateWindow(h);
    fxStart(h, FX_OPEN);
}
static int typeEm[3] = {12, 12, 19}, typeLift[3] = {0, 0, 0}, titleShrink = 0;

struct OwnFont {
    std::wstring face, file;
};
static std::vector<OwnFont> ownFonts;

static HFONT faceFont(int height, int weight, const wchar_t* face) {
    return CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static bool faceInstalled(const wchar_t* face) {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT f = faceFont(-12, FW_NORMAL, face);
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t got[LF_FACESIZE] = L"";
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return _wcsicmp(got, face) == 0;
}

static std::unordered_set<std::wstring> gdiFamilies() {
    std::unordered_set<std::wstring> out;
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW* f, const TEXTMETRICW*, DWORD, LPARAM l) -> int {
        if (f->lfFaceName[0] != L'@') ((std::unordered_set<std::wstring>*)l)->insert(f->lfFaceName);
        return 1;
    }, (LPARAM)&out, 0);
    ReleaseDC(nullptr, dc);
    return out;
}

static std::vector<std::wstring> loadFontFile(const std::wstring& path) {
    std::vector<std::wstring> faces;
    if (!AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr)) return faces;
    for (const std::wstring& f : fontfile::families(path))
        if (f.size() < LF_FACESIZE && faceInstalled(f.c_str())) faces.push_back(f);
    if (!faces.empty()) return faces;
    RemoveFontResourceExW(path.c_str(), FR_PRIVATE, nullptr);
    const std::unordered_set<std::wstring> before = gdiFamilies();
    if (!AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr)) return faces;
    std::wstring shortest;
    for (const std::wstring& f : gdiFamilies())
        if (!before.count(f) && (shortest.empty() || f.size() < shortest.size() || (f.size() == shortest.size() && f < shortest))) shortest = f;
    if (shortest.empty()) RemoveFontResourceExW(path.c_str(), FR_PRIVATE, nullptr);
    else faces.push_back(shortest);
    return faces;
}

static bool fontFileName(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = name.substr(dot + 1);
    return _wcsicmp(ext.c_str(), L"ttf") == 0 || _wcsicmp(ext.c_str(), L"otf") == 0 || _wcsicmp(ext.c_str(), L"ttc") == 0;
}

static const wchar_t* const FONT_GONE = L".removed";

static std::wstring fontsDir() {
    const std::wstring dir = dataDir() + L"\\fonts";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static const std::wstring* faceIn(const std::vector<std::wstring>& list, const std::wstring& name) {
    for (const std::wstring& f : list)
        if (_wcsicmp(f.c_str(), name.c_str()) == 0) return &f;
    return nullptr;
}

static std::vector<std::wstring>& uiFaces() {
    static std::vector<std::wstring> list = [] {
        std::vector<std::wstring> v;
        for (const wchar_t* f : UI_FACES)
            if (faceInstalled(f)) v.push_back(f);
        if (v.empty()) v.push_back(BASE_FACE);
        const std::wstring dir = fontsDir();
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return v;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !fontFileName(fd.cFileName)) continue;
            const std::wstring file = dir + L"\\" + fd.cFileName;
            if (GetFileAttributesW((file + FONT_GONE).c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (DeleteFileW(file.c_str())) DeleteFileW((file + FONT_GONE).c_str());
                continue;
            }
            for (const std::wstring& face : loadFontFile(file))
                if (!faceIn(v, face)) {
                    v.push_back(face);
                    ownFonts.push_back({face, file});
                }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return v;
    }();
    return list;
}

static const std::wstring* knownFace(const std::wstring& name) { return faceIn(uiFaces(), name); }

static const OwnFont* ownFont(const std::wstring& face) {
    for (const OwnFont& f : ownFonts)
        if (_wcsicmp(f.face.c_str(), face.c_str()) == 0) return &f;
    return nullptr;
}

struct TextSize { int width, ascent, xHeight; };

static TextSize textSize(HDC dc, int em, int weight, const wchar_t* face, const wchar_t* sample) {
    HFONT f = faceFont(-em, weight, face);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, sample, (int)wcslen(sample), &sz);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    GLYPHMETRICS gm{};
    const MAT2 one = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    GetGlyphOutlineW(dc, L'x', GGO_METRICS, &gm, 0, nullptr, &one);
    SelectObject(dc, old);
    DeleteObject(f);
    return {sz.cx, tm.tmAscent, (int)gm.gmBlackBoxY};
}

static int typeWeight(int k) { return k == 0 && uiBold ? FW_BOLD : UI_TYPE[k].weight; }

static void fitType() {
    static const wchar_t* const SAMPLE = L"Microphone level  Enable audio effects  Hear myself  Haunted Cavern  0123456789  Save as  Your sounds  -12dB";
    HDC dc = CreateCompatibleDC(nullptr);
    const bool base = _wcsicmp(uiFace.c_str(), BASE_FACE) == 0;
    for (int k = 0; k < 3; ++k) {
        const int want = (int)std::lround(UI_TYPE[k].em * uiSize / (double)TEXT_SIZE_BASE), weight = typeWeight(k);
        const TextSize design = textSize(dc, UI_TYPE[k].em, UI_TYPE[k].weight, BASE_FACE, SAMPLE);
        const TextSize ref = textSize(dc, want, weight, BASE_FACE, SAMPLE);
        int em = want;
        TextSize got = ref;
        if (!base) {
            const int least = (want * 7 + 9) / 10;
            for (em = want + 1;; --em) {
                got = textSize(dc, em, weight, uiFace.c_str(), SAMPLE);
                if (em == least || (got.width <= ref.width && got.xHeight <= ref.xHeight + 1)) break;
            }
        }
        typeEm[k] = em;
        typeLift[k] = (design.ascent - ref.ascent) / 2 + ref.ascent - got.ascent;
    }
    titleShrink = textSize(dc, UI_TYPE[2].em, UI_TYPE[2].weight, BASE_FACE, L"OctopusEAX").width -
                  textSize(dc, typeEm[2], typeWeight(2), uiFace.c_str(), L"OctopusEAX").width;
    DeleteDC(dc);
}

static int liftOf(HFONT f) {
    if (!f) return 0;
    if (f == fntBold || f == fntCtlBold) return typeLift[1];
    if (f == fntTitle || f == fntCtlTitle) return typeLift[2];
    if (f == fntUI || f == fntCtl) return typeLift[0];
    return 0;
}

static HFONT deviceFont(HFONT design) {
    if (design == fntBold) return fntCtlBold;
    if (design == fntTitle) return fntCtlTitle;
    return fntCtl;
}

static int drawText(HDC dc, const wchar_t* s, int n, RECT* r, UINT flags) {
    const int lift = (flags & (DT_VCENTER | DT_BOTTOM | DT_CALCRECT)) ? 0 : sc(liftOf((HFONT)GetCurrentObject(dc, OBJ_FONT)));
    XFORM was = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (!GetWorldTransform(dc, &was) || was.eM11 == 1.0f) {
        RECT moved = *r;
        OffsetRect(&moved, 0, lift);
        return DrawTextExW(dc, (LPWSTR)s, n, lift ? &moved : r, flags, nullptr);
    }
    HGDIOBJ old = SelectObject(dc, deviceFont((HFONT)GetCurrentObject(dc, OBJ_FONT)));
    RECT d = scRect(*r);
    OffsetRect(&d, (int)lroundf(was.eDx), lift + (int)lroundf(was.eDy));
    flatDc(dc);
    const int h = DrawTextExW(dc, (LPWSTR)s, n, &d, flags, nullptr);
    SetWorldTransform(dc, &was);
    SelectObject(dc, old);
    OffsetRect(&d, -(int)lroundf(was.eDx), -(int)lroundf(was.eDy));
    if (flags & DT_CALCRECT) *r = {unsc(d.left), unsc(d.top), (int)ceilf(d.right / uiScale), (int)ceilf(d.bottom / uiScale)};
    return (int)ceilf(h / uiScale);
}
static bool dragSlider = false, dragMic = false, dragClip = false;
static bool dragPitch = false, dragSize = false, dragPos = false, dragVoice = false, dragApp = false;
static bool dragVoiceSpeed = false, dragClipSpeed = false;
static bool holdGlitch = false;
static int  dragPosVal = 0;
static int  sbDragOff  = -1;
static bool posGlide = false;

enum Knob {
    K_AMOUNT, K_MIC, K_ISO, K_CLIP_VOL, K_CLIP_POS, K_CLIP_SPEED, K_APP, K_PITCH, K_SIZE, K_VOICE, K_VSPEED,
    K_EQ, K_EQ_LEVEL = K_EQ + eqb::EQ_BANDS, K_LOOK, K_COUNT = K_LOOK + 5
};
static const UINT GLIDE_TIMER = 3;
static const int GLIDE_MS = 260;
static void glideKnob(HWND owner, int k);
static void glideStop(int k);
static bool glideStep(HWND owner);

static void noTheme(HWND h) { SetWindowTheme(h, L"", L""); }

enum Icon { ICO_NONE = -1, ICO_PLAY, ICO_PAUSE, ICO_STOP, ICO_OPEN, ICO_BACK, ICO_FWD, ICO_PREV, ICO_NEXT,
            ICO_CLOSE, ICO_MIN, ICO_LOOK, ICO_SAVE, ICO_TRASH, ICO_KEY, ICO_RESET, ICO_LOOP, ICO_WARN, ICO_CUT, ICO_REFRESH,
            ICO_PAD, ICO_NOISE_OFF, ICO_POWER, ICO_WAND, ICO_HEADPHONES, ICO_WIDE, ICO_MIC, ICO_CUBE, ICO_PERSON, ICO_NOTE,
            ICO_WINDOW, ICO_MASK, ICO_TUNING, ICO_SEND, ICO_GAUGE, ICO_LOCK, ICO_CHIP, ICO_BARS, ICO_SPEAKER, ICO_BOLT, ICO_ROOM, ICO_STAR, ICO_LINK, ICO_RECORD, ICO_WAVE, ICO_UNDO, ICO_HUSH, ICO_REDO, ICO_PRESETS };

static const COLORREF RECORD_RED = RGB(236, 72, 84);
static void drawIcon(HDC dc, float cx, float cy, int icon, COLORREF c, COLORREF bg);

static int boxIcon(HWND h) {
    if (h == hMic) return ICO_MIC;
    if (h == hSpk) return ICO_SPEAKER;
    if (h == hApp) return ICO_WINDOW;
    if (h == hEqBox) return ICO_PRESETS;
    if (h == hMaskBox) return ICO_MASK;
    if (h == hOwnBox) return ICO_STAR;
    if (h == hGlitchBox) return ICO_BOLT;
    return ICO_NONE;
}

static bool snapPrint(HWND w, DockImage& img);
static bool snapFaceAs(HWND box, DockImage& img, int open);
static bool snapList(HWND list, HWND item, DockImage& img);
static HWND boxOfList(HWND list);
static uint32_t* dibBits(HBITMAP bmp);
static void ghostRun(HWND owner, const RECT& at, DockImage& img, double ms, int peak = -1);
static void listScrolled(HWND owner, HWND list, DockImage& before, int fromTop, int toTop);
static bool fadeAllowed(HWND h);

static const wchar_t* const COMBO_HOT = L"OctopusComboHot";

static void drawDropArrow(HDC dc, float cx, float cy, bool up, COLORREF c) {
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float d = up ? -1.0f : 1.0f;
    const Gdiplus::PointF p[3] = {{cx - 4.2f, cy - 2.1f * d}, {cx + 4.2f, cy - 2.1f * d}, {cx, cy + 2.6f * d}};
    Gdiplus::SolidBrush br(gpColor(c));
    Gdiplus::Pen pen(gpColor(c), 1.6f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    gr.FillPolygon(&br, p, 3);
    gr.DrawPolygon(&pen, p, 3);
}

static void drawTurnArrow(HDC dc, float cx, float cy, double turn, COLORREF c) {
    if (turn <= 0.0 || turn >= 1.0) {
        drawDropArrow(dc, cx, cy, turn >= 1.0, c);
        return;
    }
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    gr.TranslateTransform(cx, cy);
    gr.RotateTransform((Gdiplus::REAL)(180.0 * turn));
    const Gdiplus::PointF p[3] = {{-4.2f, -2.1f}, {4.2f, -2.1f}, {0.0f, 2.6f}};
    Gdiplus::SolidBrush br(gpColor(c));
    Gdiplus::Pen pen(gpColor(c), 1.6f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    gr.FillPolygon(&br, p, 3);
    gr.DrawPolygon(&pen, p, 3);
}

struct FaceAnim {
    HWND box = nullptr;
    double turn = 0.0, glow = 0.0, at = 0.0;
    bool open = false, running = false;
};
static std::vector<FaceAnim> faceAnims;
static const UINT_PTR FACE_TIMER = 0x5A53;
static const double FACE_TURN_MS = 220.0, FACE_GLOW_IN_MS = 110.0, FACE_GLOW_OUT_MS = 180.0;

static double faceGlowWant(const FaceAnim& f) { return f.open || GetPropW(f.box, COMBO_HOT) ? 1.0 : 0.0; }

static FaceAnim& faceOf(HWND box) {
    for (FaceAnim& f : faceAnims)
        if (f.box == box) return f;
    FaceAnim f;
    f.box = box;
    f.open = SendMessageW(box, CB_GETDROPPEDSTATE, 0, 0) != 0;
    f.turn = f.open ? 1.0 : 0.0;
    f.glow = faceGlowWant(f);
    faceAnims.push_back(f);
    return faceAnims.back();
}

static void faceKick(HWND box) {
    FaceAnim& f = faceOf(box);
    if (!uiAnim || !fadeAllowed(GetAncestor(box, GA_ROOT))) {
        f.turn = f.open ? 1.0 : 0.0;
        f.glow = faceGlowWant(f);
        if (f.running) KillTimer(box, FACE_TIMER);
        f.running = false;
        InvalidateRect(box, nullptr, FALSE);
        return;
    }
    if (f.running) return;
    f.running = true;
    f.at = fxNow();
    SetTimer(box, FACE_TIMER, 10, nullptr);
}

static void faceTick(HWND box) {
    FaceAnim& f = faceOf(box);
    const double now = fxNow(), ms = std::min(30.0, std::max(0.0, now - f.at));
    f.at = now;
    const double turnTo = f.open ? 1.0 : 0.0, glowTo = faceGlowWant(f);
    auto step = [ms](double& v, double to, double up, double down) {
        if (v < to) v = std::min(to, v + ms / up);
        else if (v > to) v = std::max(to, v - ms / down);
    };
    step(f.turn, turnTo, FACE_TURN_MS, FACE_TURN_MS);
    step(f.glow, glowTo, FACE_GLOW_IN_MS, FACE_GLOW_OUT_MS);
    InvalidateRect(box, nullptr, FALSE);
    if (f.turn == turnTo && f.glow == glowTo) {
        KillTimer(box, FACE_TIMER);
        f.running = false;
    }
}

static void paintComboFace(HWND h, HDC hdc, int openAs = -1) {
    RECT dev; GetClientRect(h, &dev);
    scaleDc(hdc);
    const RECT rc = designRect(dev.right, dev.bottom);
    const bool hot = GetPropW(h, COMBO_HOT) != nullptr;
    double turn = 0.0, glow = 0.0;
    if (openAs >= 0) {
        turn = openAs ? 1.0 : 0.0;
        glow = openAs || hot ? 1.0 : 0.0;
    } else if (!uiAnim) {
        const bool open = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
        turn = open ? 1.0 : 0.0;
        glow = open || hot ? 1.0 : 0.0;
    } else {
        const FaceAnim& f = faceOf(h);
        turn = easeInOut(f.turn);
        glow = f.glow;
    }

    const int AW = 22;
    float arrowX = rc.right - AW / 2.0f;
    const float arrowY = dev.bottom / 2.0f / uiScale;
    {
        PixelSnap px(hdc);
        const int t = px.line();
        const COLORREF edge = mixc(C_EDGE, mixc(C_EDGE, C_GOLD, 0.55), glow);
        const RECT whole = {0, 0, dev.right, dev.bottom};
        HBRUSH bg = CreateSolidBrush(C_BACK);
        FillRect(hdc, &whole, bg);
        DeleteObject(bg);
        const LONG split = dev.right - (px.x((float)AW) - px.x(0.0f));
        const RECT zone = {split + t, t, dev.right - t, dev.bottom - t};
        HBRUSH nb = CreateSolidBrush(mixc(C_NAVY, mixc(C_NAVY, C_GOLD, 0.22), glow));
        FillRect(hdc, &zone, nb);
        DeleteObject(nb);
        edgeLines(hdc, whole, t, edge);
        const RECT divider = {split, t, split + t, dev.bottom - t};
        HBRUSH db = CreateSolidBrush(edge);
        FillRect(hdc, &divider, db);
        DeleteObject(db);
        arrowX = (zone.left + zone.right) / 2.0f / uiScale;
    }

    drawTurnArrow(hdc, arrowX, arrowY, turn, mixc(C_GOLD, mixc(C_GOLD, C_TEXT, 0.5), glow));

    int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    wchar_t txt[256] = L"";
    if (sel >= 0) {
        if (h == hMic && sel < (int)uiMics.size()) wcsncpy_s(txt, uiMics[sel].name.c_str(), 255);
        else if (h == hSpk && sel < (int)uiSpks.size()) wcsncpy_s(txt, uiSpks[sel].name.c_str(), 255);
        else if (h == hApp) {
            if (sel == 0) wcscpy_s(txt, L"(none)");
            else if (sel - 1 < (int)uiApps.size()) wcsncpy_s(txt, uiApps[sel - 1].name.c_str(), 255);
        }
        else if (h == hEqBox && sel < (int)uiEqNames.size()) {
            wcsncpy_s(txt, uiEqNames[sel].c_str(), 250);
            if (!eqOnPreset()) wcscat_s(txt, L"*");
        }
        else if (h == hMaskBox && sel < voice::NUM_XFI_MASKS) maskLabel(sel, txt);
        else if (h == hOwnBox && sel < voice::NUM_OWN_MASKS) maskLabel(voice::OWN_ORDER[sel], txt);
        else if (h == hGlitchBox && sel < voice::NUM_GLITCH_STYLES) wcsncpy_s(txt, voice::GLITCH_LOOKS[sel].name, 255);
    }
    bool hint = false;
    if (h == hMaskBox && (txt[0] == 0 || wcscmp(txt, L"X-Fi") == 0)) { wcscpy_s(txt, L"X-Fi"); hint = true; }
    if (h == hOwnBox && txt[0] == 0) { wcscpy_s(txt, L"OctopusEAX masks"); hint = true; }
    const int icon = boxIcon(h);
    if (icon != ICO_NONE) drawIcon(hdc, 15.0f, (rc.top + rc.bottom) / 2.0f, icon, hint ? C_DIM : C_GOLD, C_BACK);
    if (txt[0]) {
        RECT tr = {icon != ICO_NONE ? 30 : 8, 0, rc.right - AW - 4, rc.bottom};
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, hint ? C_DIM : C_TEXT);
        HGDIOBJ of = SelectObject(hdc, fntUI);
        drawText(hdc, txt, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(hdc, of);
    }
    flatDc(hdc);
}

static void comboHover(HWND h, bool on) {
    if (on) {
        SetPropW(h, COMBO_HOT, (HANDLE)1);
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
    } else {
        RemovePropW(h, COMBO_HOT);
    }
    faceKick(h);
}

static LRESULT CALLBACK comboSubProc(HWND h, UINT m, WPARAM w, LPARAM l,
                                      UINT_PTR, DWORD_PTR) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bm = mem ? CreateCompatibleBitmap(hdc, rc.right, rc.bottom) : nullptr;
        if (bm) {
            HGDIOBJ ob = SelectObject(mem, bm);
            paintComboFace(h, mem);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bm);
        } else {
            paintComboFace(h, hdc);
        }
        if (mem) DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER && w == FACE_TIMER) {
        faceTick(h);
        return 0;
    }
    if (m == WM_PRINTCLIENT) {
        paintComboFace(h, (HDC)w);
        return 0;
    }
    if (m == WM_MOUSEMOVE && !GetPropW(h, COMBO_HOT)) {
        comboHover(h, true);
    } else if (m == WM_MOUSELEAVE) {
        comboHover(h, false);
    } else if (m == WM_NCDESTROY) {
        RemovePropW(h, COMBO_HOT);
        for (size_t i = 0; i < faceAnims.size(); ++i)
            if (faceAnims[i].box == h) {
                faceAnims.erase(faceAnims.begin() + (std::ptrdiff_t)i);
                break;
            }
    }
    if (m == WM_NCPAINT) return 0;
    return DefSubclassProc(h, m, w, l);
}

static HWND dropListOf(HWND box);

struct ItemGlow {
    int item = -1;
    double level = 0.0;
};

struct ListShine {
    HWND list = nullptr;
    ItemGlow selIn, selOut, overIn, overOut;
    double at = 0.0;
    bool running = false;
};
static std::vector<ListShine> listShines;
static const UINT_PTR SHINE_TIMER = 0x5A51, DROP_TIMER = 0x5A52;
static const double SHINE_IN_MS = 90.0, SHINE_OUT_MS = 180.0, DROP_MS = 210.0;

static ListShine& shineOf(HWND list) {
    for (ListShine& s : listShines)
        if (s.list == list) return s;
    listShines.push_back(ListShine{});
    listShines.back().list = list;
    return listShines.back();
}

static void shineItem(HWND list, int item) {
    RECT r;
    if (item >= 0 && SendMessageW(list, LB_GETITEMRECT, (WPARAM)item, (LPARAM)&r) != LB_ERR) InvalidateRect(list, &r, FALSE);
}

static void shineMove(HWND list, ItemGlow& in, ItemGlow& out, int item) {
    if (item == in.item) return;
    if (item == out.item) {
        std::swap(in, out);
        return;
    }
    if (in.item >= 0 && in.level > 0.0) {
        if (out.item >= 0) shineItem(list, out.item);
        out = in;
    }
    in = {item, 0.0};
}

static void shineKick(ListShine& s) {
    if (s.running) return;
    s.running = true;
    s.at = fxNow();
    SetTimer(s.list, SHINE_TIMER, 10, nullptr);
}

static void shineReset(HWND list, int sel) {
    ListShine& s = shineOf(list);
    s.selIn = {sel, 1.0};
    s.selOut = ItemGlow{};
}

static double shineSel(HWND list, int item, bool sel) {
    if (!uiAnim || !list) return sel ? 1.0 : 0.0;
    ListShine& s = shineOf(list);
    if (sel != (item == s.selIn.item)) {
        shineMove(list, s.selIn, s.selOut, sel ? item : -1);
        shineKick(s);
    }
    return item == s.selIn.item ? s.selIn.level : (item == s.selOut.item ? s.selOut.level : 0.0);
}

static double shineOver(HWND list, int item) {
    if (!list) return 0.0;
    const ListShine& s = shineOf(list);
    return item == s.overIn.item ? s.overIn.level : (item == s.overOut.item ? s.overOut.level : 0.0);
}

static void shineHover(HWND list, int item) {
    ListShine& s = shineOf(list);
    if (item == s.overIn.item) return;
    if (!uiAnim) {
        shineItem(list, s.overIn.item);
        shineItem(list, s.overOut.item);
        s.overIn = {item, 1.0};
        s.overOut = ItemGlow{};
        shineItem(list, item);
        return;
    }
    shineMove(list, s.overIn, s.overOut, item);
    shineKick(s);
}

static void shineTick(HWND list) {
    ListShine& s = shineOf(list);
    const double now = fxNow(), ms = std::max(0.0, now - s.at);
    s.at = now;
    bool moving = false;
    auto rise = [&](ItemGlow& gl) {
        if (gl.item < 0 || gl.level >= 1.0) return;
        gl.level = std::min(1.0, gl.level + ms / SHINE_IN_MS);
        shineItem(list, gl.item);
        moving = true;
    };
    auto fall = [&](ItemGlow& gl) {
        if (gl.item < 0) return;
        gl.level = std::max(0.0, gl.level - ms / SHINE_OUT_MS);
        shineItem(list, gl.item);
        if (gl.level <= 0.0) gl = ItemGlow{};
        moving = true;
    };
    rise(s.selIn);
    fall(s.selOut);
    rise(s.overIn);
    fall(s.overOut);
    if (!moving) {
        KillTimer(list, SHINE_TIMER);
        s.running = false;
    }
}

struct DropOpen {
    HWND list = nullptr, box = nullptr, ghost = nullptr;
    DockImage img;
    double at = 0.0, k = 0.0;
    bool armed = false, running = false, shown = false;
};
static DropOpen dropOpen;

struct RollGhost {
    HWND ghost = nullptr, list = nullptr;
    DockImage img;
    RECT at{};
    int dir = 1;
    double from = 1.0, start = 0.0;
};
static std::vector<RollGhost> rollGhosts;
static UINT_PTR rollTimer = 0;
static const double ROLL_MS = 160.0;

static void dropAlpha(HWND list, BYTE a) {
    SetLayeredWindowAttributes(list, 0, a, LWA_ALPHA);
}

static bool listBar(HWND list) {
    RECT wr, cr;
    POINT o = {0, 0};
    return GetWindowRect(list, &wr) && GetClientRect(list, &cr) && ClientToScreen(list, &o) &&
           (int)cr.right < (int)(wr.right - wr.left) - 2 * (int)(o.x - wr.left);
}

static int dropDir(HWND list, HWND box) {
    RECT lr, br;
    if (!box || !GetWindowRect(list, &lr) || !GetWindowRect(box, &br)) return 1;
    return lr.top >= br.top ? 1 : -1;
}

static void rollPaint(HWND ghost, const DockImage& img, const RECT& at, int dir, double shown, BYTE alpha) {
    const int vh = std::max(1, std::min(img.h, (int)std::lround(shown * img.h)));
    POINT pos = {at.left, dir > 0 ? at.top : at.bottom - vh}, src = {0, dir > 0 ? img.h - vh : 0};
    SIZE size = {img.w, vh};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, 0};
    UpdateLayeredWindow(ghost, nullptr, &pos, &size, img.dc, &src, 0, &bf, ULW_ALPHA);
}

static BYTE rollAlpha(double shown) { return (BYTE)std::lround(255.0 * (0.4 + 0.6 * shown)); }

static void dropStop() {
    DropOpen d = dropOpen;
    dropOpen = DropOpen{};
    if (d.list && IsWindow(d.list)) {
        if (d.running) KillTimer(d.list, DROP_TIMER);
        dropAlpha(d.list, 255);
    }
    if (d.ghost && IsWindow(d.ghost)) DestroyWindow(d.ghost);
    d.img.drop();
}

static void dropPlace(bool step = true) {
    DropOpen& d = dropOpen;
    if (step) {
        const double now = fxNow();
        d.k = std::min(1.0, d.k + std::min(24.0, std::max(0.0, now - d.at)) / DROP_MS);
        d.at = now;
    }
    RECT at;
    if (d.k >= 1.0 || !GetWindowRect(d.list, &at)) {
        dropStop();
        return;
    }
    const int w = (int)(at.right - at.left), h = (int)(at.bottom - at.top);
    const bool bar = listBar(d.list);
    if (!d.img.dc || d.img.w != w || d.img.h != h || !bar) {
        if (bar) UpdateWindow(d.list);
        if (!snapList(d.list, d.box, d.img) || d.img.w != w || d.img.h != h) {
            dropStop();
            return;
        }
    }
    const double shown = 1.0 - (1.0 - d.k) * (1.0 - d.k) * (1.0 - d.k);
    rollPaint(d.ghost, d.img, at, dropDir(d.list, d.box), shown, rollAlpha(shown));
    if (!d.shown) {
        d.shown = true;
        SetWindowPos(d.ghost, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

static void rollEnd(size_t i) {
    RollGhost r = rollGhosts[i];
    rollGhosts.erase(rollGhosts.begin() + (std::ptrdiff_t)i);
    if (IsWindow(r.ghost)) DestroyWindow(r.ghost);
    r.img.drop();
}

static void CALLBACK rollTick(HWND, UINT, UINT_PTR, DWORD) {
    const double now = fxNow();
    for (size_t i = 0; i < rollGhosts.size();) {
        RollGhost& r = rollGhosts[i];
        const double k = (now - r.start) / ROLL_MS;
        if (k >= 1.0 || !IsWindow(r.ghost)) {
            rollEnd(i);
            continue;
        }
        const double shown = r.from * (1.0 - k * k);
        rollPaint(r.ghost, r.img, r.at, r.dir, shown, rollAlpha(shown));
        ++i;
    }
    if (rollGhosts.empty() && rollTimer) {
        KillTimer(nullptr, rollTimer);
        rollTimer = 0;
    }
}

static void dropBegin(HWND list) {
    DropOpen& d = dropOpen;
    if (!d.armed || d.list != list) return;
    d.armed = false;
    if (!fadeAllowed(hwndMain)) {
        dropStop();
        return;
    }
    d.ghost = makeGhost(nullptr, L"OctopusDock", DefWindowProcW, WS_EX_TRANSPARENT | WS_EX_TOPMOST);
    if (!d.ghost) {
        dropStop();
        return;
    }
    for (size_t i = 0; i < rollGhosts.size();)
        if (rollGhosts[i].list == list) rollEnd(i);
        else ++i;
    d.at = fxNow();
    d.k = 0.0;
    d.running = true;
    SetTimer(list, DROP_TIMER, 10, nullptr);
    if (!listBar(list)) dropPlace(false);
}

static void dropArm(HWND box) {
    dropStop();
    const HWND list = dropListOf(box);
    if (!list || !(GetWindowLongPtrW(list, GWL_EXSTYLE) & WS_EX_LAYERED)) return;
    dropOpen.list = list;
    dropOpen.box = box;
    dropOpen.armed = true;
    if (IsWindowVisible(list)) dropBegin(list);
    else if (fadeAllowed(hwndMain)) dropAlpha(list, 1);
}

static void dropPlaced(HWND list, UINT flags) {
    if (flags & SWP_SHOWWINDOW) {
        dropBegin(list);
        return;
    }
    if (dropOpen.list != list || (flags & (SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE))) return;
    if (dropOpen.armed && !IsWindowVisible(list)) ShowWindow(list, SW_SHOWNA);
}

static void dropShown(HWND box) {
    const HWND list = dropListOf(box);
    if (!dropOpen.armed || dropOpen.list != list) return;
    if (IsWindowVisible(list)) dropBegin(list);
    else dropStop();
}

static void dropRollUp(HWND list, HWND box) {
    double from = 1.0;
    if (dropOpen.list == list && dropOpen.running) from = dropOpen.shown ? 1.0 - (1.0 - dropOpen.k) * (1.0 - dropOpen.k) * (1.0 - dropOpen.k) : 0.0;
    if (from < 0.05) return;
    RollGhost r;
    if (!GetWindowRect(list, &r.at) || !snapList(list, box, r.img) || r.img.h != (int)(r.at.bottom - r.at.top)) {
        r.img.drop();
        return;
    }
    r.ghost = makeGhost(nullptr, L"OctopusDock", DefWindowProcW, WS_EX_TRANSPARENT | WS_EX_TOPMOST);
    if (!r.ghost) {
        r.img.drop();
        return;
    }
    r.list = list;
    r.dir = dropDir(list, box);
    r.from = from;
    r.start = fxNow();
    rollPaint(r.ghost, r.img, r.at, r.dir, from, rollAlpha(from));
    SetWindowPos(r.ghost, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    rollGhosts.push_back(r);
    if (!rollTimer) rollTimer = SetTimer(nullptr, 0, 10, rollTick);
}

static LRESULT CALLBACK listFrameProc(HWND h, UINT m, WPARAM w, LPARAM l,
                                      UINT_PTR, DWORD_PTR) {
    if (m == WM_TIMER && w == DROP_TIMER) {
        if (dropOpen.running && dropOpen.list == h) dropPlace();
        else KillTimer(h, DROP_TIMER);
        return 0;
    }
    if (m == WM_TIMER && w == SHINE_TIMER) {
        shineTick(h);
        return 0;
    }
    if (m == WM_WINDOWPOSCHANGING && dropOpen.armed && dropOpen.list == h && !IsWindowVisible(h) &&
        !(((const WINDOWPOS*)l)->flags & (SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE)))
        ((WINDOWPOS*)l)->flags |= SWP_SHOWWINDOW | SWP_NOACTIVATE;
    if (m == WM_WINDOWPOSCHANGING && (((const WINDOWPOS*)l)->flags & SWP_HIDEWINDOW) && IsWindowVisible(h)) {
        if (fadeAllowed(hwndMain)) dropRollUp(h, boxOfList(h));
        if (dropOpen.list == h) dropStop();
    }
    const LRESULT r = DefSubclassProc(h, m, w, l);
    if (m == WM_WINDOWPOSCHANGED) dropPlaced(h, ((const WINDOWPOS*)l)->flags);
    if (m == WM_NCPAINT) {
        HDC dc = GetWindowDC(h);
        RECT rc; GetWindowRect(h, &rc); OffsetRect(&rc, -rc.left, -rc.top);
        HPEN pe = linePen(C_EDGE);
        HGDIOBJ op = SelectObject(dc, pe), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pe);
        ReleaseDC(h, dc);
    }
    return r;
}
static void dressList(HWND combo) {
    COMBOBOXINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetComboBoxInfo(combo, &ci) || !ci.hwndList) return;
    SetWindowTheme(ci.hwndList, L"", L"");
    SetWindowLongPtrW(ci.hwndList, GWL_EXSTYLE, GetWindowLongPtrW(ci.hwndList, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(ci.hwndList, 0, 255, LWA_ALPHA);
    SetWindowSubclass(ci.hwndList, listFrameProc, 0, 0);
}

static const UINT WM_FIT_DROP = WM_APP + 49;
static const UINT WM_FACE_FADE = WM_APP + 51;

struct FaceShot {
    HWND box = nullptr;
    DockImage before;
};
static std::vector<FaceShot> faceShots;

static bool faceText(HWND box, DockImage& img, RECT& at) {
    DockImage whole;
    RECT cr;
    POINT o = {0, 0};
    if (!snapFaceAs(box, whole, 0) || !GetClientRect(box, &cr) || !ClientToScreen(box, &o)) {
        whole.drop();
        return false;
    }
    const int line = uiScale >= 1.0f && uiScale == floorf(uiScale) ? (int)uiScale : 1;
    const int inset = line + 1, w = (int)cr.right - sc(22) - 2 * inset, hh = (int)cr.bottom - 2 * inset;
    const bool ok = w > 0 && hh > 0 && img.make(w, hh);
    if (ok) {
        BitBlt(img.dc, 0, 0, w, hh, whole.dc, inset, inset, SRCCOPY);
        GdiFlush();
        at = {o.x + inset, o.y + inset, o.x + inset + w, o.y + inset + hh};
    }
    whole.drop();
    return ok;
}

static void faceChange(HWND box, HWND owner, bool opening) {
    faceOf(box).open = opening;
    faceKick(box);
    if (!opening) return;
    for (size_t i = 0; i < faceShots.size(); ++i)
        if (faceShots[i].box == box) {
            faceShots[i].before.drop();
            faceShots.erase(faceShots.begin() + (std::ptrdiff_t)i);
            break;
        }
    if (!fadeAllowed(owner)) return;
    FaceShot shot;
    shot.box = box;
    RECT at;
    if (faceText(box, shot.before, at)) faceShots.push_back(shot);
    else shot.before.drop();
}

static void faceFade(HWND owner, HWND box) {
    for (size_t i = 0; i < faceShots.size(); ++i) {
        if (faceShots[i].box != box) continue;
        DockImage before = faceShots[i].before;
        faceShots.erase(faceShots.begin() + (std::ptrdiff_t)i);
        DockImage now;
        RECT at;
        bool differs = false;
        if (fadeAllowed(owner) && faceText(box, now, at) && now.w == before.w && now.h == before.h) {
            const uint32_t* a = dibBits(now.bmp);
            const uint32_t* b = dibBits(before.bmp);
            differs = a && b && memcmp(a, b, (size_t)now.w * now.h * 4) != 0;
        }
        now.drop();
        if (differs) ghostRun(owner, at, before, 160.0);
        else before.drop();
        return;
    }
}

static void nudgeList(HWND list) {
    RECT lr;
    GetWindowRect(list, &lr);
    const int w = (int)(lr.right - lr.left), h = (int)(lr.bottom - lr.top);
    const UINT keep = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(list, nullptr, 0, 0, w, h + 1, keep);
    SetWindowPos(list, nullptr, 0, 0, w, h, keep);
}

static HWND dropListOf(HWND box) {
    COMBOBOXINFO ci{};
    ci.cbSize = sizeof(ci);
    return box && GetComboBoxInfo(box, &ci) ? ci.hwndList : nullptr;
}

static void fitDropList(HWND box) {
    if (!box || !SendMessageW(box, CB_GETDROPPEDSTATE, 0, 0)) return;
    const HWND list = dropListOf(box);
    if (!list) return;
    const int itemH = (int)SendMessageW(list, LB_GETITEMHEIGHT, 0, 0);
    if (itemH <= 0) return;
    const int count = std::max(1, (int)SendMessageW(box, CB_GETCOUNT, 0, 0));
    const int most = (int)SendMessageW(box, CB_GETMINVISIBLE, 0, 0);
    const int rows = box == hMic || box == hSpk || most <= 0 ? count : std::min(count, most);
    RECT lr, cr, inner;
    GetWindowRect(list, &lr);
    GetWindowRect(box, &cr);
    GetClientRect(list, &inner);
    const int frame = (int)(lr.bottom - lr.top) - (int)inner.bottom;
    const HMONITOR mon = MonitorFromWindow(box, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    const bool known = GetMonitorInfoW(mon, &mi) != FALSE;
    const bool onScreen = !known || (lr.top >= mi.rcMonitor.top && lr.bottom <= mi.rcMonitor.bottom);
    if ((int)(lr.bottom - lr.top) == rows * itemH + frame && onScreen) {
        nudgeList(list);
        return;
    }
    const RECT wa = known ? mi.rcWork : workAreaOf(mon);
    const int below = (int)(wa.bottom - cr.bottom), above = (int)(cr.top - wa.top);
    const bool down = rows * itemH + frame <= below || below >= above;
    const int fit = std::max(1, std::min(rows, ((down ? below : above) - frame) / itemH));
    const int h = fit * itemH + frame;
    const int top = down ? (int)cr.bottom : (int)cr.top - h;
    const int w = (int)(lr.right - lr.left);
    SetWindowPos(list, nullptr, lr.left, top, w, h + 1, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(list, nullptr, lr.left, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void primeDropList(HWND box) {
    if (SendMessageW(box, CB_GETDROPPEDSTATE, 0, 0)) {
        fitDropList(box);
        return;
    }
    if (const HWND list = dropListOf(box)) nudgeList(list);
}

static bool roomTracked = false;

static void roomHoverRefresh() {
    POINT c;
    if (!hPreset || !GetCursorPos(&c) || WindowFromPoint(c) != hPreset || !ScreenToClient(hPreset, &c)) return;
    if (!roomTracked) {
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hPreset, 0};
        roomTracked = TrackMouseEvent(&t) != FALSE;
    }
    const LRESULT hit = SendMessageW(hPreset, LB_ITEMFROMPOINT, 0, MAKELPARAM(c.x, c.y));
    shineHover(hPreset, HIWORD(hit) ? -1 : (int)LOWORD(hit));
}

static LRESULT CALLBACK listWheelProc(HWND h, UINT m, WPARAM w, LPARAM l,
                                      UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEWHEEL) return SendMessageW(GetParent(h), WM_MOUSEWHEEL, w, l);
    if (m == WM_TIMER && w == SHINE_TIMER) {
        shineTick(h);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        if (!roomTracked) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            roomTracked = TrackMouseEvent(&t) != FALSE;
        }
        const LRESULT hit = SendMessageW(h, LB_ITEMFROMPOINT, 0, l);
        shineHover(h, HIWORD(hit) ? -1 : (int)LOWORD(hit));
    } else if (m == WM_MOUSELEAVE) {
        roomTracked = false;
        shineHover(h, -1);
    }
    if ((m == WM_LBUTTONDOWN || (m == WM_KEYDOWN && !(l & 0x40000000))) && fadeAllowed(GetAncestor(h, GA_ROOT))) {
        const int top = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0);
        DockImage before;
        if (snapList(h, h, before)) {
            DefSubclassProc(h, WM_SETREDRAW, FALSE, 0);
            const LRESULT r = DefSubclassProc(h, m, w, l);
            DefSubclassProc(h, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(h, nullptr, FALSE);
            const int now = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0);
            if (now != top) {
                UpdateWindow(h);
                listScrolled(GetAncestor(h, GA_ROOT), h, before, top, now);
                roomHoverRefresh();
            } else {
                before.drop();
            }
            return r;
        }
        before.drop();
    }
    return DefSubclassProc(h, m, w, l);
}

static void presetTopTo(HWND h, int nt) {
    const int top = (int)SendMessageW(hPreset, LB_GETTOPINDEX, 0, 0);
    if (nt == top) return;
    DockImage before;
    const bool fade = fadeAllowed(h) && snapList(hPreset, hPreset, before);
    SendMessageW(hPreset, WM_SETREDRAW, FALSE, 0);
    SendMessageW(hPreset, LB_SETTOPINDEX, (WPARAM)nt, 0);
    SendMessageW(hPreset, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hPreset, nullptr, FALSE);
    if (!fade) {
        before.drop();
        roomHoverRefresh();
        return;
    }
    UpdateWindow(hPreset);
    listScrolled(h, hPreset, before, top, (int)SendMessageW(hPreset, LB_GETTOPINDEX, 0, 0));
    roomHoverRefresh();
}

static std::vector<Dev> speakerDevices() {
    std::vector<Dev> out;
    for (auto& d : listDevices(eRender))
        if (!nameContains(d.name, L"CABLE") && !cableAdapter(d))
            out.push_back(d);
    return out;
}

static std::vector<Dev> realMics() {
    std::vector<Dev> all = listDevices(eCapture), mics;
    for (const Dev& d : all)
        if (!cableCapture(d)) mics.push_back(d);
    return mics;
}

static const UINT WM_DEVICES_FOUND = WM_APP + 35;
static const double DEV_SPIN_MS = 650.0;
struct DeviceScan { std::vector<Dev> mics, spks; };
static bool devScanning = false, devScanAsked = false;
static ULONGLONG devSpinAt = 0;

static bool sameDevices(const std::vector<Dev>& a, const std::vector<Dev>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].name != b[i].name) return false;
    return true;
}

static std::wstring devicesAdded(const std::vector<Dev>& was, const std::vector<Dev>& now) {
    std::wstring names;
    for (const Dev& d : now)
        if (std::none_of(was.begin(), was.end(), [&](const Dev& o) { return o.id == d.id; })) names += (names.empty() ? L"" : L", ") + d.name;
    return names;
}

static void deviceBoxSet(bool mics, std::vector<Dev>& fresh) {
    std::vector<Dev>& list = mics ? uiMics : uiSpks;
    if (sameDevices(list, fresh)) return;
    const HWND box = mics ? hMic : hSpk;
    std::wstring keep;
    {
        std::lock_guard<std::mutex> lk(g.devMx);
        keep = mics ? g.micId : g.spkId;
    }
    list.swap(fresh);
    SendMessageW(box, WM_SETREDRAW, FALSE, 0);
    SendMessageW(box, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)list[i].name.c_str());
        if (list[i].id == keep) sel = (int)i;
    }
    SendMessageW(box, CB_SETCURSEL, sel, 0);
    SendMessageW(box, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(box, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    fitDropList(box);
}

static void devicesScan(HWND h, bool asked) {
    if (asked) {
        devScanAsked = true;
        devSpinAt = GetTickCount64();
    }
    if (devScanning) return;
    devScanning = true;
    std::thread([h] {
        const MediaWork busy;
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        auto* found = new DeviceScan{realMics(), speakerDevices()};
        if (com) CoUninitialize();
        if (!PostMessageW(h, WM_DEVICES_FOUND, 0, (LPARAM)found)) delete found;
    }).detach();
}

static float devSpin() {
    if (!uiAnim || !devSpinAt) return 0.0f;
    const double k = (GetTickCount64() - devSpinAt) / DEV_SPIN_MS;
    if (k >= 1.0 && !devScanning) {
        devSpinAt = 0;
        return 0.0f;
    }
    if (k >= 1.0) return (float)std::fmod(360.0 * k, 360.0);
    return (float)(360.0 * (1.0 - (1.0 - k) * (1.0 - k) * (1.0 - k)));
}

static void fillDevices() {
    uiMics = realMics();
    uiSpks = speakerDevices();
    SendMessageW(hMic, CB_RESETCONTENT, 0, 0);
    SendMessageW(hSpk, CB_RESETCONTENT, 0, 0);
    std::wstring defMic = defaultDeviceId(eCapture), defSpk = defaultDeviceId(eRender);
    int selM = -1, selS = 0;
    for (size_t i = 0; i < uiMics.size(); ++i) {
        SendMessageW(hMic, CB_ADDSTRING, 0, (LPARAM)uiMics[i].name.c_str());
        if (uiMics[i].id == defMic || selM < 0) selM = (int)i;
    }
    if (selM < 0) selM = 0;
    for (size_t i = 0; i < uiSpks.size(); ++i) {
        SendMessageW(hSpk, CB_ADDSTRING, 0, (LPARAM)uiSpks[i].name.c_str());
        if (uiSpks[i].id == defSpk) selS = (int)i;
    }
    SendMessageW(hMic, CB_SETCURSEL, selM, 0);
    SendMessageW(hSpk, CB_SETCURSEL, selS, 0);
    std::lock_guard<std::mutex> lk(g.devMx);
    if (!uiMics.empty()) g.micId = uiMics[selM].id;
    if (!uiSpks.empty()) g.spkId = uiSpks[selS].id;
}

static std::wstring appEndNote() {
    switch (gApp.state.load()) {
    case clip::ProgramTap::GONE:
        return gApp.name + L" closed";
    case clip::ProgramTap::TOO_OLD:
        return L"Needs Windows 10 (2004) or newer";
    case clip::ProgramTap::REFUSED: {
        wchar_t t[96];
        swprintf_s(t, 96, L"Windows blocked this app's sound (0x%08lx)", (unsigned long)gApp.refusal.load());
        return t;
    }
    }
    return L"";
}

static void healPrograms() {
    static std::atomic<bool> busy{false};
    const clip::ParkMemory memory = gApp.hijack.memory;
    if (memory.programs().empty() || busy.exchange(true)) return;
    std::vector<std::wstring> skip;
    if (gApp.chosen()) {
        std::vector<DWORD> tree;
        clip::collectTree(gApp.pid, tree);
        for (DWORD p : tree) {
            const std::wstring exe = clip::imagePath(p);
            if (!exe.empty()) skip.push_back(exe);
        }
    }
    std::thread([memory, skip] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        clip::healParked(memory, skip);
        if (SUCCEEDED(com)) CoUninitialize();
        busy = false;
    }).detach();
}

static void fillPrograms() {
    const DWORD keep = gApp.chosen() ? gApp.pid : 0;
    uiApps = clip::listPrograms();
    if (keep && std::none_of(uiApps.begin(), uiApps.end(), [keep](const clip::Program& p) { return p.pid == keep; }))
        uiApps.insert(uiApps.begin(), clip::Program{keep, gApp.name});
    SendMessageW(hApp, CB_RESETCONTENT, 0, 0);
    SendMessageW(hApp, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    int sel = 0;
    for (size_t i = 0; i < uiApps.size(); ++i) {
        SendMessageW(hApp, CB_ADDSTRING, 0, (LPARAM)uiApps[i].name.c_str());
        if (keep && uiApps[i].pid == keep) sel = (int)i + 1;
    }
    SendMessageW(hApp, CB_SETCURSEL, sel, 0);
}

static SIZE boxPx = {0, 0};

static void boxEdge(HDC dc) {
    PixelSnap px(dc);
    edgeLines(dc, RECT{0, 0, boxPx.cx, boxPx.cy}, px.line(), C_GOLD);
}

static int fadeAlpha(double k) {
    const int a = alphaFor(C_BACK);
    return a + (int)lround((255 - a) * k);
}

static void panel(HDC dc, RECT r, COLORREF fill, COLORREF edge, int alpha = -1) {
    PixelSnap px(dc);
    const RECT d = px.map(r);
    blendRect(dc, d, fill, (BYTE)(alpha < 0 ? alphaFor(fill) : alpha));
    edgeLines(dc, d, px.line(), edge);
}

static void label(HDC dc, int x, int y, const wchar_t* s, COLORREF c, HFONT f) {
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, f);
    TextOutW(dc, x, y + liftOf(f), s, (int)wcslen(s));
    SelectObject(dc, of);
}

static void roundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    const float d = r * 2.0f;
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

static void roundBox(Gdiplus::Graphics& gr, float x, float y, float w, float h, float r, COLORREF fill, int alpha, COLORREF edge) {
    Gdiplus::GraphicsPath path;
    roundPath(path, x, y, w, h, r);
    Gdiplus::SolidBrush br(gpColor(fill, alpha));
    gr.FillPath(&br, &path);
    Gdiplus::Pen pen(gpColor(edge), 1.0f);
    gr.DrawPath(&pen, &path);
}

static RECT hotRect = {0, 0, 0, 0};
static bool isHot(const RECT& r) { return EqualRect(&r, &hotRect) != 0; }
static bool zoomDrag = false;

static float iconTurn = 0.0f;

static void drawIcon(HDC dc, float cx, float cy, int icon, COLORREF c, COLORREF bg) {
    const RECT reach = {(int)cx - 12, (int)cy - 12, (int)cx + 13, (int)cy + 13};
    if (!RectVisible(dc, &reach)) return;
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    if (iconTurn != 0.0f) {
        gr.TranslateTransform(cx, cy);
        gr.RotateTransform(iconTurn);
        gr.TranslateTransform(-cx, -cy);
    }
    Gdiplus::SolidBrush br(gpColor(c));
    Gdiplus::SolidBrush hole(gpColor(bg));
    Gdiplus::Pen pen(gpColor(c), 1.6f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::Pen bold(gpColor(c), 2.0f);
    bold.SetLineJoin(Gdiplus::LineJoinRound);
    bold.SetStartCap(Gdiplus::LineCapRound);
    bold.SetEndCap(Gdiplus::LineCapRound);
    switch (icon) {
    case ICO_PLAY: {
        Gdiplus::PointF p[3] = {{cx - 4.0f, cy - 5.5f}, {cx + 5.5f, cy}, {cx - 4.0f, cy + 5.5f}};
        gr.FillPolygon(&br, p, 3);
        break;
    }
    case ICO_PAUSE:
        gr.FillRectangle(&br, cx - 5.0f, cy - 5.5f, 3.5f, 11.0f);
        gr.FillRectangle(&br, cx + 1.5f, cy - 5.5f, 3.5f, 11.0f);
        break;
    case ICO_STOP:
        gr.FillRectangle(&br, cx - 4.5f, cy - 4.5f, 9.0f, 9.0f);
        break;
    case ICO_OPEN: {
        Gdiplus::PointF p[6] = {{cx - 7.0f, cy - 5.0f}, {cx - 2.5f, cy - 5.0f}, {cx - 0.5f, cy - 3.0f},
                                {cx + 7.0f, cy - 3.0f}, {cx + 7.0f, cy + 5.0f}, {cx - 7.0f, cy + 5.0f}};
        gr.DrawPolygon(&pen, p, 6);
        gr.DrawLine(&pen, cx - 7.0f, cy - 0.5f, cx + 7.0f, cy - 0.5f);
        break;
    }
    case ICO_BACK:
    case ICO_FWD: {
        const float k = icon == ICO_BACK ? -1.0f : 1.0f;
        Gdiplus::PointF a[3] = {{cx - 6.0f * k, cy - 5.0f}, {cx, cy}, {cx - 6.0f * k, cy + 5.0f}};
        Gdiplus::PointF b[3] = {{cx, cy - 5.0f}, {cx + 6.0f * k, cy}, {cx, cy + 5.0f}};
        gr.FillPolygon(&br, a, 3);
        gr.FillPolygon(&br, b, 3);
        break;
    }
    case ICO_PREV:
    case ICO_NEXT: {
        const float k = icon == ICO_PREV ? -1.0f : 1.0f;
        Gdiplus::PointF p[3] = {{cx - 2.5f * k, cy - 5.0f}, {cx + 2.5f * k, cy}, {cx - 2.5f * k, cy + 5.0f}};
        gr.DrawLines(&bold, p, 3);
        break;
    }
    case ICO_CLOSE:
        gr.DrawLine(&bold, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f);
        gr.DrawLine(&bold, cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f);
        break;
    case ICO_MIN:
        gr.DrawLine(&bold, cx - 5.0f, cy + 3.5f, cx + 5.0f, cy + 3.5f);
        break;
    case ICO_LOOK: {
        gr.FillEllipse(&br, cx - 7.5f, cy - 6.5f, 15.0f, 13.0f);
        gr.FillEllipse(&hole, cx + 1.5f, cy + 1.0f, 4.0f, 4.0f);
        gr.FillEllipse(&hole, cx - 5.0f, cy - 3.0f, 3.0f, 3.0f);
        gr.FillEllipse(&hole, cx - 1.5f, cy - 5.0f, 3.0f, 3.0f);
        gr.FillEllipse(&hole, cx + 2.5f, cy - 3.5f, 3.0f, 3.0f);
        gr.FillEllipse(&hole, cx - 5.0f, cy + 1.5f, 3.0f, 3.0f);
        break;
    }
    case ICO_SAVE: {
        Gdiplus::GraphicsPath body;
        roundPath(body, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f, 2.0f);
        gr.DrawPath(&pen, &body);
        gr.FillRectangle(&br, cx - 3.0f, cy - 6.0f, 6.0f, 4.0f);
        gr.DrawRectangle(&pen, cx - 3.5f, cy + 1.0f, 7.0f, 5.0f);
        break;
    }
    case ICO_TRASH: {
        gr.DrawLine(&pen, cx - 6.0f, cy - 4.0f, cx + 6.0f, cy - 4.0f);
        gr.DrawLine(&pen, cx - 2.0f, cy - 6.0f, cx + 2.0f, cy - 6.0f);
        Gdiplus::PointF p[4] = {{cx - 4.5f, cy - 4.0f}, {cx - 3.5f, cy + 6.0f}, {cx + 3.5f, cy + 6.0f}, {cx + 4.5f, cy - 4.0f}};
        gr.DrawLines(&pen, p, 4);
        gr.DrawLine(&pen, cx - 1.2f, cy - 1.0f, cx - 1.2f, cy + 3.5f);
        gr.DrawLine(&pen, cx + 1.2f, cy - 1.0f, cx + 1.2f, cy + 3.5f);
        break;
    }
    case ICO_KEY:
        gr.DrawEllipse(&bold, cx - 7.0f, cy - 3.5f, 7.0f, 7.0f);
        gr.DrawLine(&bold, cx, cy, cx + 7.0f, cy);
        gr.DrawLine(&bold, cx + 4.0f, cy, cx + 4.0f, cy + 3.0f);
        gr.DrawLine(&bold, cx + 6.5f, cy, cx + 6.5f, cy + 2.5f);
        break;
    case ICO_RESET:
    case ICO_LOOP: {
        gr.DrawArc(&pen, cx - 5.5f, cy - 5.5f, 11.0f, 11.0f, 300.0f, 300.0f);
        Gdiplus::PointF p[3] = {{cx + 3.5f, cy - 7.5f}, {cx + 6.0f, cy - 3.0f}, {cx + 1.0f, cy - 2.5f}};
        gr.FillPolygon(&br, p, 3);
        break;
    }
    case ICO_WARN: {
        Gdiplus::PointF p[3] = {{cx, cy - 6.0f}, {cx + 6.8f, cy + 5.6f}, {cx - 6.8f, cy + 5.6f}};
        gr.DrawPolygon(&pen, p, 3);
        gr.DrawLine(&pen, cx, cy - 2.2f, cx, cy + 1.2f);
        gr.FillEllipse(&br, cx - 1.1f, cy + 2.6f, 2.2f, 2.2f);
        break;
    }
    case ICO_CUT:
        gr.DrawEllipse(&pen, cx - 6.2f, cy + 1.4f, 4.8f, 4.8f);
        gr.DrawEllipse(&pen, cx + 1.4f, cy + 1.4f, 4.8f, 4.8f);
        gr.DrawLine(&pen, cx - 2.2f, cy + 2.0f, cx + 4.0f, cy - 6.5f);
        gr.DrawLine(&pen, cx + 2.2f, cy + 2.0f, cx - 4.0f, cy - 6.5f);
        break;
    case ICO_REFRESH: {
        const float r = 5.5f;
        gr.DrawArc(&pen, cx - r, cy - r, 2.0f * r, 2.0f * r, 200.0f, 120.0f);
        gr.DrawArc(&pen, cx - r, cy - r, 2.0f * r, 2.0f * r, 20.0f, 120.0f);
        for (const float end : {320.0f, 140.0f}) {
            const float a = end * 3.14159265f / 180.0f;
            const float ex = cx + r * cosf(a), ey = cy + r * sinf(a), tx = -sinf(a), ty = cosf(a), nx = cosf(a), ny = sinf(a);
            Gdiplus::PointF head[3] = {{ex + tx * 3.2f, ey + ty * 3.2f},
                                       {ex + nx * 2.6f - tx * 0.6f, ey + ny * 2.6f - ty * 0.6f},
                                       {ex - nx * 2.6f - tx * 0.6f, ey - ny * 2.6f - ty * 0.6f}};
            gr.FillPolygon(&br, head, 3);
        }
        break;
    }
    case ICO_PAD: {
        Gdiplus::GraphicsPath box;
        roundPath(box, cx - 6.5f, cy - 6.5f, 13.0f, 13.0f, 1.5f);
        gr.DrawPath(&pen, &box);
        Gdiplus::Pen hair(gpColor(c, 150), 1.0f);
        gr.DrawLine(&hair, cx, cy - 5.0f, cx, cy + 5.0f);
        gr.DrawLine(&hair, cx - 5.0f, cy, cx + 5.0f, cy);
        gr.FillEllipse(&br, cx + 0.2f, cy - 4.8f, 4.6f, 4.6f);
        break;
    }
    case ICO_NOISE_OFF: {
        for (float dy = -4.5f; dy <= 4.5f; dy += 4.5f) {
            Gdiplus::GraphicsPath w;
            w.AddBezier(cx - 7.0f, cy + dy, cx - 4.5f, cy + dy - 3.5f, cx - 2.5f, cy + dy + 3.5f, cx, cy + dy);
            w.AddBezier(cx, cy + dy, cx + 2.5f, cy + dy - 3.5f, cx + 4.5f, cy + dy + 3.5f, cx + 7.0f, cy + dy);
            gr.DrawPath(&pen, &w);
        }
        Gdiplus::Pen gap(gpColor(bg), 4.5f);
        gap.SetStartCap(Gdiplus::LineCapRound);
        gap.SetEndCap(Gdiplus::LineCapRound);
        gr.DrawLine(&gap, cx - 5.5f, cy + 6.5f, cx + 5.5f, cy - 6.5f);
        gr.DrawLine(&bold, cx - 5.5f, cy + 6.5f, cx + 5.5f, cy - 6.5f);
        break;
    }
    case ICO_SPEAKER: {
        Gdiplus::PointF body[6] = {{cx - 7.5f, cy - 3.0f}, {cx - 4.0f, cy - 3.0f}, {cx - 0.5f, cy - 6.5f}, {cx - 0.5f, cy + 6.5f}, {cx - 4.0f, cy + 3.0f}, {cx - 7.5f, cy + 3.0f}};
        gr.FillPolygon(&br, body, 6);
        gr.DrawArc(&pen, cx - 2.0f, cy - 3.0f, 6.0f, 6.0f, -50.0f, 100.0f);
        gr.DrawArc(&pen, cx - 2.5f, cy - 6.0f, 12.0f, 12.0f, -50.0f, 100.0f);
        break;
    }
    case ICO_BOLT: {
        Gdiplus::PointF b[7] = {{cx + 1.5f, cy - 7.5f}, {cx - 4.5f, cy + 1.0f}, {cx - 0.5f, cy + 1.0f}, {cx - 2.0f, cy + 7.5f}, {cx + 4.5f, cy - 1.5f}, {cx + 0.5f, cy - 1.5f}, {cx + 1.5f, cy - 7.5f}};
        gr.FillPolygon(&br, b, 7);
        break;
    }
    case ICO_ROOM: {
        Gdiplus::PointF roof[3] = {{cx - 7.5f, cy - 0.5f}, {cx, cy - 7.0f}, {cx + 7.5f, cy - 0.5f}};
        gr.DrawLines(&pen, roof, 3);
        gr.DrawRectangle(&pen, cx - 5.5f, cy - 1.0f, 11.0f, 8.0f);
        gr.FillRectangle(&br, cx - 1.7f, cy + 2.0f, 3.4f, 5.0f);
        break;
    }
    case ICO_STAR: {
        Gdiplus::PointF s[10];
        for (int i = 0; i < 10; ++i) {
            const float a = (float)(-3.14159265 / 2.0 + i * 3.14159265 / 5.0), rr = (i % 2) ? 3.0f : 7.2f;
            s[i] = {cx + rr * cosf(a), cy + rr * sinf(a)};
        }
        gr.FillPolygon(&br, s, 10);
        break;
    }
    case ICO_LINK: {
        Gdiplus::GraphicsPath a, b;
        roundPath(a, cx - 7.5f, cy - 2.5f, 8.5f, 5.0f, 2.5f);
        roundPath(b, cx - 1.0f, cy - 2.5f, 8.5f, 5.0f, 2.5f);
        gr.DrawPath(&pen, &a);
        gr.DrawPath(&pen, &b);
        gr.FillRectangle(&hole, cx - 2.5f, cy - 1.0f, 5.0f, 2.0f);
        gr.DrawLine(&pen, cx - 2.5f, cy, cx + 2.5f, cy);
        break;
    }
    case ICO_POWER:
        gr.DrawArc(&bold, cx - 5.5f, cy - 4.5f, 11.0f, 11.0f, 300.0f, 300.0f);
        gr.DrawLine(&bold, cx, cy - 7.0f, cx, cy - 1.0f);
        break;
    case ICO_WAND: {
        gr.DrawLine(&bold, cx - 6.0f, cy + 6.0f, cx + 1.5f, cy - 1.5f);
        gr.DrawLine(&pen, cx + 4.0f, cy - 7.0f, cx + 4.0f, cy - 1.0f);
        gr.DrawLine(&pen, cx + 1.0f, cy - 4.0f, cx + 7.0f, cy - 4.0f);
        gr.FillEllipse(&br, cx - 1.5f, cy - 7.0f, 2.0f, 2.0f);
        gr.FillEllipse(&br, cx + 5.0f, cy + 1.0f, 2.0f, 2.0f);
        break;
    }
    case ICO_HEADPHONES: {
        gr.DrawArc(&bold, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f, 180.0f, 180.0f);
        Gdiplus::GraphicsPath l, r;
        roundPath(l, cx - 7.5f, cy, 4.0f, 6.0f, 1.5f);
        roundPath(r, cx + 3.5f, cy, 4.0f, 6.0f, 1.5f);
        gr.FillPath(&br, &l);
        gr.FillPath(&br, &r);
        break;
    }
    case ICO_WIDE: {
        gr.DrawLine(&pen, cx - 1.5f, cy, cx + 1.5f, cy);
        Gdiplus::PointF a[3] = {{cx - 3.5f, cy - 4.0f}, {cx - 7.5f, cy}, {cx - 3.5f, cy + 4.0f}};
        Gdiplus::PointF b[3] = {{cx + 3.5f, cy - 4.0f}, {cx + 7.5f, cy}, {cx + 3.5f, cy + 4.0f}};
        gr.DrawLines(&bold, a, 3);
        gr.DrawLines(&bold, b, 3);
        gr.DrawLine(&pen, cx - 7.5f, cy, cx - 4.0f, cy);
        gr.DrawLine(&pen, cx + 4.0f, cy, cx + 7.5f, cy);
        break;
    }
    case ICO_MIC: {
        Gdiplus::GraphicsPath cap;
        roundPath(cap, cx - 2.5f, cy - 7.5f, 5.0f, 9.0f, 2.5f);
        gr.FillPath(&br, &cap);
        gr.DrawArc(&pen, cx - 5.0f, cy - 5.0f, 10.0f, 9.0f, 0.0f, 180.0f);
        gr.DrawLine(&pen, cx, cy + 4.0f, cx, cy + 7.0f);
        gr.DrawLine(&pen, cx - 3.0f, cy + 7.0f, cx + 3.0f, cy + 7.0f);
        break;
    }
    case ICO_CUBE: {
        Gdiplus::PointF h[6] = {{cx, cy - 7.0f}, {cx + 6.0f, cy - 3.5f}, {cx + 6.0f, cy + 3.5f}, {cx, cy + 7.0f}, {cx - 6.0f, cy + 3.5f}, {cx - 6.0f, cy - 3.5f}};
        gr.DrawPolygon(&pen, h, 6);
        gr.DrawLine(&pen, cx, cy, cx, cy + 7.0f);
        gr.DrawLine(&pen, cx, cy, cx + 6.0f, cy - 3.5f);
        gr.DrawLine(&pen, cx, cy, cx - 6.0f, cy - 3.5f);
        break;
    }
    case ICO_PERSON:
        gr.FillEllipse(&br, cx - 2.8f, cy - 7.0f, 5.6f, 5.6f);
        gr.FillPie(&br, cx - 6.0f, cy - 0.5f, 12.0f, 14.0f, 180.0f, 180.0f);
        break;
    case ICO_NOTE: {
        gr.FillEllipse(&br, cx - 5.5f, cy + 1.5f, 6.0f, 4.6f);
        gr.DrawLine(&pen, cx, cy + 3.5f, cx, cy - 7.0f);
        Gdiplus::PointF f[3] = {{cx, cy - 7.0f}, {cx + 5.0f, cy - 5.0f}, {cx + 5.0f, cy - 1.5f}};
        gr.DrawLines(&pen, f, 3);
        break;
    }
    case ICO_WINDOW:
        gr.DrawRectangle(&pen, cx - 6.5f, cy - 5.5f, 13.0f, 11.0f);
        gr.FillRectangle(&br, cx - 6.5f, cy - 5.5f, 13.0f, 3.0f);
        break;
    case ICO_MASK: {
        Gdiplus::GraphicsPath m;
        m.AddBezier(cx - 7.0f, cy - 5.0f, cx - 3.0f, cy - 7.0f, cx + 3.0f, cy - 7.0f, cx + 7.0f, cy - 5.0f);
        m.AddBezier(cx + 7.0f, cy - 5.0f, cx + 7.0f, cy + 4.0f, cx + 3.0f, cy + 7.5f, cx, cy + 7.5f);
        m.AddBezier(cx, cy + 7.5f, cx - 3.0f, cy + 7.5f, cx - 7.0f, cy + 4.0f, cx - 7.0f, cy - 5.0f);
        gr.DrawPath(&pen, &m);
        gr.FillEllipse(&br, cx - 4.5f, cy - 3.0f, 3.2f, 2.2f);
        gr.FillEllipse(&br, cx + 1.3f, cy - 3.0f, 3.2f, 2.2f);
        gr.DrawArc(&pen, cx - 3.0f, cy, 6.0f, 4.0f, 15.0f, 150.0f);
        break;
    }
    case ICO_TUNING:
        gr.DrawLine(&pen, cx - 2.5f, cy - 7.0f, cx - 2.5f, cy - 2.0f);
        gr.DrawLine(&pen, cx + 2.5f, cy - 7.0f, cx + 2.5f, cy - 2.0f);
        gr.DrawArc(&pen, cx - 2.5f, cy - 4.0f, 5.0f, 4.0f, 0.0f, 180.0f);
        gr.DrawLine(&pen, cx, cy, cx, cy + 7.0f);
        gr.DrawLine(&pen, cx - 2.5f, cy + 7.0f, cx + 2.5f, cy + 7.0f);
        break;
    case ICO_SEND: {
        Gdiplus::GraphicsPath cap;
        roundPath(cap, cx + 1.5f, cy - 7.5f, 5.0f, 9.0f, 2.5f);
        gr.FillPath(&br, &cap);
        gr.DrawArc(&pen, cx, cy - 5.0f, 8.0f, 9.0f, 0.0f, 180.0f);
        gr.DrawLine(&pen, cx + 4.0f, cy + 4.0f, cx + 4.0f, cy + 7.0f);
        gr.DrawLine(&pen, cx + 1.5f, cy + 7.0f, cx + 6.5f, cy + 7.0f);
        gr.DrawLine(&pen, cx - 7.5f, cy - 2.5f, cx - 1.5f, cy - 2.5f);
        Gdiplus::PointF head[3] = {{cx - 4.0f, cy - 5.0f}, {cx - 1.5f, cy - 2.5f}, {cx - 4.0f, cy}};
        gr.DrawLines(&pen, head, 3);
        break;
    }
    case ICO_GAUGE:
        gr.DrawArc(&pen, cx - 6.5f, cy - 5.0f, 13.0f, 13.0f, 180.0f, 180.0f);
        gr.DrawLine(&bold, cx, cy + 1.5f, cx + 3.8f, cy - 3.0f);
        gr.FillEllipse(&br, cx - 1.5f, cy, 3.0f, 3.0f);
        gr.DrawLine(&pen, cx - 6.5f, cy + 4.0f, cx + 6.5f, cy + 4.0f);
        break;
    case ICO_LOCK: {
        Gdiplus::GraphicsPath body;
        roundPath(body, cx - 5.0f, cy - 0.5f, 10.0f, 8.0f, 1.5f);
        gr.FillPath(&br, &body);
        gr.DrawArc(&pen, cx - 3.5f, cy - 7.0f, 7.0f, 8.0f, 180.0f, 180.0f);
        gr.FillEllipse(&hole, cx - 1.2f, cy + 2.0f, 2.4f, 2.4f);
        break;
    }
    case ICO_CHIP:
        gr.DrawRectangle(&pen, cx - 4.5f, cy - 4.5f, 9.0f, 9.0f);
        gr.FillRectangle(&br, cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
        for (float d = -3.0f; d <= 3.0f; d += 3.0f) {
            gr.DrawLine(&pen, cx + d, cy - 7.5f, cx + d, cy - 4.5f);
            gr.DrawLine(&pen, cx + d, cy + 4.5f, cx + d, cy + 7.5f);
            gr.DrawLine(&pen, cx - 7.5f, cy + d, cx - 4.5f, cy + d);
            gr.DrawLine(&pen, cx + 4.5f, cy + d, cx + 7.5f, cy + d);
        }
        break;
    case ICO_BARS: {
        const float knob[3] = {-2.5f, 2.0f, -0.5f};
        for (int i = 0; i < 3; ++i) {
            const float x = cx + (i - 1) * 5.0f;
            gr.DrawLine(&pen, x, cy - 6.5f, x, cy + 6.5f);
            gr.FillRectangle(&br, x - 2.0f, cy + knob[i] - 1.5f, 4.0f, 3.0f);
        }
        break;
    }
    case ICO_PRESETS:
        for (int i = 0; i < 3; ++i) {
            const float y = cy + (i - 1) * 5.0f;
            if (i == 1) gr.FillEllipse(&br, cx - 7.0f, y - 2.0f, 4.0f, 4.0f);
            else gr.DrawEllipse(&pen, cx - 6.5f, y - 1.5f, 3.0f, 3.0f);
            gr.DrawLine(i == 1 ? &bold : &pen, cx - 1.0f, y, cx + 7.0f, y);
        }
        break;
    case ICO_RECORD: {
        Gdiplus::SolidBrush red(gpColor(c == C_OFF ? c : RECORD_RED));
        gr.FillEllipse(&red, cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
        break;
    }
    case ICO_WAVE: {
        const float tall[5] = {2.5f, 5.0f, 7.0f, 4.0f, 6.0f};
        for (int i = 0; i < 5; ++i) {
            const float x = cx + (i - 2) * 3.0f;
            gr.DrawLine(&pen, x, cy - tall[i], x, cy + tall[i]);
        }
        break;
    }
    case ICO_UNDO: {
        gr.DrawLine(&pen, cx - 6.0f, cy - 2.5f, cx + 1.0f, cy - 2.5f);
        gr.DrawArc(&pen, cx - 3.0f, cy - 2.5f, 8.0f, 8.0f, 270.0f, 180.0f);
        gr.DrawLine(&pen, cx + 1.0f, cy + 5.5f, cx - 3.0f, cy + 5.5f);
        Gdiplus::PointF head[3] = {{cx - 2.5f, cy - 6.0f}, {cx - 6.0f, cy - 2.5f}, {cx - 2.5f, cy + 1.0f}};
        gr.DrawLines(&pen, head, 3);
        break;
    }
    case ICO_REDO: {
        gr.DrawLine(&pen, cx + 6.0f, cy - 2.5f, cx - 1.0f, cy - 2.5f);
        gr.DrawArc(&pen, cx - 5.0f, cy - 2.5f, 8.0f, 8.0f, 90.0f, 180.0f);
        gr.DrawLine(&pen, cx - 1.0f, cy + 5.5f, cx + 3.0f, cy + 5.5f);
        Gdiplus::PointF head[3] = {{cx + 2.5f, cy - 6.0f}, {cx + 6.0f, cy - 2.5f}, {cx + 2.5f, cy + 1.0f}};
        gr.DrawLines(&pen, head, 3);
        break;
    }    case ICO_HUSH: {
        const float side[2] = {3.5f, 6.5f};
        for (int i = 0; i < 2; ++i) {
            gr.DrawLine(&pen, cx - 7.5f + 2.6f * i, cy - side[i], cx - 7.5f + 2.6f * i, cy + side[i]);
            gr.DrawLine(&pen, cx + 7.5f - 2.6f * i, cy - side[i], cx + 7.5f - 2.6f * i, cy + side[i]);
        }
        Gdiplus::PointF a[3] = {{cx - 2.6f, cy - 2.6f}, {cx - 0.6f, cy}, {cx - 2.6f, cy + 2.6f}};
        Gdiplus::PointF b[3] = {{cx + 2.6f, cy - 2.6f}, {cx + 0.6f, cy}, {cx + 2.6f, cy + 2.6f}};
        gr.DrawLines(&pen, a, 3);
        gr.DrawLines(&pen, b, 3);
        break;
    }
    default:
        break;
    }
}

static void buttonFace(HDC dc, const RECT& r, int icon, const wchar_t* text, COLORREF fill, COLORREF edge, COLORREF fg, int alpha = -1) {
    if (!RectVisible(dc, &r)) return;
    panel(dc, r, fill, edge, alpha);
    int tw = 0, th = 0;
    HGDIOBJ of = SelectObject(dc, fntBold);
    if (text && text[0]) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
        tw = sz.cx;
        th = sz.cy;
    }
    const int iw = icon != ICO_NONE ? 16 : 0, wide = (int)(r.right - r.left);
    const bool tight = tw > wide - 10 - iw - ((iw && tw) ? 6 : 0);
    const int gap = (iw && tw) ? (tight ? 3 : 6) : 0;
    const int room = wide - (tight ? 4 : 10) - iw - gap;
    const bool squeezed = tw > room && room > 0;
    if (squeezed) tw = room;
    const int x0 = (r.left + r.right - (iw + gap + tw)) / 2;
    const int cy = (r.top + r.bottom) / 2;
    if (icon != ICO_NONE) drawIcon(dc, x0 + 8.0f, (float)cy, icon, fg, fill);
    if (tw) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        if (squeezed) {
            RECT tr = {x0 + iw + gap, r.top, x0 + iw + gap + tw, r.bottom};
            drawText(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            TextOutW(dc, x0 + iw + gap, cy - th / 2, text, (int)wcslen(text));
        }
    }
    SelectObject(dc, of);
}

struct HotFade { RECT r; double level; };
static HotFade hotIn = {{0, 0, 0, 0}, 0.0}, hotOut = {{0, 0, 0, 0}, 0.0}, hotPress = {{0, 0, 0, 0}, 0.0};
static const double HOT_IN_MS = 110.0, HOT_OUT_MS = 170.0, TICK_MS = 180.0, PRESS_MS = 260.0;
static bool boxPainting = false;

static void hotPressAt(const RECT& r) {
    if (!uiAnim || IsRectEmpty(&r)) return;
    hotPress = {r, 1.0};
}

static double pressLevelFor(const RECT& r) {
    return !boxPainting && EqualRect(&r, &hotPress.r) ? hotPress.level : 0.0;
}

static void hotMoveTo(const RECT& now) {
    if (EqualRect(&now, &hotOut.r)) {
        const HotFade swap = hotIn;
        hotIn = hotOut;
        hotOut = swap;
    } else {
        hotOut = hotIn;
        hotIn = {now, 0.0};
    }
    if (IsRectEmpty(&hotIn.r)) hotIn.level = 0.0;
}

static void hotLeave(HWND h) {
    if (IsRectEmpty(&hotRect)) return;
    invalidateDesign(h, hotRect);
    SetRectEmpty(&hotRect);
    hotMoveTo(hotRect);
}

static bool hotFadeStep(double ms) {
    bool moving = false;
    if (!IsRectEmpty(&hotIn.r) && hotIn.level < 1.0) { hotIn.level = std::min(1.0, hotIn.level + ms / HOT_IN_MS); moving = true; }
    if (hotOut.level > 0.0) {
        hotOut.level = std::max(0.0, hotOut.level - ms / HOT_OUT_MS);
        if (hotOut.level == 0.0) SetRectEmpty(&hotOut.r);
        moving = true;
    }
    if (hotPress.level > 0.0) {
        hotPress.level = std::max(0.0, hotPress.level - ms / PRESS_MS);
        if (hotPress.level == 0.0) SetRectEmpty(&hotPress.r);
        moving = true;
    }
    return moving;
}

static double hotLevelFor(const RECT& r, bool hot) {
    if (boxPainting || !uiAnim) return hot ? 1.0 : 0.0;
    if (hot) return EqualRect(&r, &hotIn.r) ? hotIn.level : 1.0;
    if (EqualRect(&r, &hotOut.r)) return hotOut.level;
    return 0.0;
}

static void iconButton(HDC dc, const RECT& r, int icon, const wchar_t* text, bool hot, bool enabled = true, bool lit = false) {
    if (!enabled) { buttonFace(dc, r, icon, text, C_PANEL, C_EDGE, C_OFF); return; }
    const double k = lit ? 1.0 : hotLevelFor(r, hot);
    const COLORREF fill = mixc(mixc(C_BACK, C_NAVY, k), C_GOLD, 0.22 * pressLevelFor(r));
    buttonFace(dc, r, icon, text, fill, mixc(C_EDGE, C_GOLD, k), mixc(C_TEXT, C_GOLD, k), fadeAlpha(k));
}

static void primaryButton(HDC dc, const RECT& r, int icon, const wchar_t* text, bool hot, bool enabled = true) {
    if (!enabled) { iconButton(dc, r, icon, text, false, false); return; }
    const double k = hotLevelFor(r, hot);
    const COLORREF fill = mixc(mixc(C_GOLD, mixc(C_GOLD, C_TEXT, 0.25), k), C_TEXT, 0.35 * pressLevelFor(r));
    buttonFace(dc, r, icon, text, fill, fill, C_BACK);
}

static void button(HDC dc, const RECT& r, const wchar_t* text, bool hot, bool enabled = true) {
    iconButton(dc, r, ICO_NONE, text, hot, enabled);
}

struct TickSeen { int x, y; bool on; ULONGLONG at; };
static std::vector<TickSeen> ticks;
static bool ticksLive = false;

static double tickProgress(int x, int y, bool on) {
    const ULONGLONG now = GetTickCount64();
    for (TickSeen& t : ticks) {
        if (t.x != x || t.y != y) continue;
        if (t.on != on) { t.on = on; t.at = ticksLive ? now : 0; }
        if (!t.at) return 1.0;
        const double k = (double)(now - t.at) / TICK_MS;
        return k >= 1.0 ? 1.0 : k;
    }
    ticks.push_back({x, y, on, 0});
    return 1.0;
}

static void ticksRepaint(HWND h) {
    const ULONGLONG now = GetTickCount64();
    for (const TickSeen& t : ticks)
        if (t.at && now - t.at < (ULONGLONG)TICK_MS + 50) invalidateDesign(h, RECT{t.x, t.y, t.x + 16, t.y + 16});
}

static void optionIcon(HDC dc, int x, int y, int icon, bool on) {
    if (icon != ICO_NONE) drawIcon(dc, x + 30.0f, y + 7.5f, icon, on ? C_GOLD : C_DIM, C_PANEL);
}

static int optionText(int icon) { return icon == ICO_NONE ? 22 : 42; }

static void drawCheck(HDC dc, int x, int y, bool on, int icon, const wchar_t* text) {
    const double k = tickProgress(x, y, on);
    const RECT box = {x, y, x + 16, y + 16};
    if (RectVisible(dc, &box)) {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const COLORREF fill = mixc(C_BACK, C_NAVY, on ? k : 1.0 - k);
        roundBox(gr, x + 0.5f, y + 0.5f, 14.0f, 14.0f, 3.0f, fill, fadeAlpha(on ? k : 1.0 - k), on ? C_GOLD : C_EDGE);
        if (on) {
            Gdiplus::Pen pen(gpColor(C_GOLD), 2.0f);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            const Gdiplus::PointF p[3] = {{x + 3.8f, y + 7.8f}, {x + 6.6f, y + 10.6f}, {x + 11.4f, y + 4.4f}};
            if (k >= 1.0) {
                gr.DrawLines(&pen, p, 3);
            } else if (k < 0.4) {
                const float f = (float)(k / 0.4);
                gr.DrawLine(&pen, p[0].X, p[0].Y, p[0].X + (p[1].X - p[0].X) * f, p[0].Y + (p[1].Y - p[0].Y) * f);
            } else {
                const float f = (float)((k - 0.4) / 0.6);
                gr.DrawLine(&pen, p[0].X, p[0].Y, p[1].X, p[1].Y);
                gr.DrawLine(&pen, p[1].X, p[1].Y, p[1].X + (p[2].X - p[1].X) * f, p[1].Y + (p[2].Y - p[1].Y) * f);
            }
        }
    }
    optionIcon(dc, x, y, icon, on);
    label(dc, x + optionText(icon), y - 1, text, on ? C_TEXT : C_DIM, fntUI);
}

static void drawCheck(HDC dc, int x, int y, bool on, const wchar_t* text) {
    drawCheck(dc, x, y, on, ICO_NONE, text);
}

static void drawRadio(HDC dc, int x, int y, bool on, int icon, const wchar_t* text) {
    const double k = tickProgress(x, y, on);
    const RECT box = {x, y, x + 16, y + 16};
    if (RectVisible(dc, &box)) {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const COLORREF fill = mixc(C_BACK, C_NAVY, on ? k : 1.0 - k);
        Gdiplus::SolidBrush ring(gpColor(fill, fadeAlpha(on ? k : 1.0 - k)));
        Gdiplus::Pen edge(gpColor(on ? C_GOLD : C_EDGE), 1.0f);
        gr.FillEllipse(&ring, x + 0.5f, y + 0.5f, 14.0f, 14.0f);
        gr.DrawEllipse(&edge, x + 0.5f, y + 0.5f, 14.0f, 14.0f);
        if (on) {
            const float d = (float)(3.0 + 4.0 * k);
            Gdiplus::SolidBrush dot(gpColor(C_GOLD));
            gr.FillEllipse(&dot, x + 7.5f - d / 2.0f, y + 7.5f - d / 2.0f, d, d);
        }
    }
    optionIcon(dc, x, y, icon, on);
    label(dc, x + optionText(icon), y - 1, text, on ? C_TEXT : C_DIM, fntUI);
}

static void drawFader(HDC dc, const RECT& r, int value, int maxValue) {
    const RECT reach = {r.left, r.top - 4, r.right, r.bottom + 4};
    if (!RectVisible(dc, &reach)) return;
    const int mid = (r.top + r.bottom) / 2;
    const int span = r.right - r.left - 12;
    const int px = r.left + 6 + span * value / (maxValue > 0 ? maxValue : 1);
    {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        roundBox(gr, (float)r.left + 0.5f, mid - 3.0f, (float)(r.right - r.left) - 1.0f, 6.0f, 3.0f, C_BACK, alphaFor(C_BACK), C_EDGE);
        if (px > r.left + 3) {
            Gdiplus::GraphicsPath done;
            roundPath(done, (float)r.left + 1.5f, mid - 2.0f, (float)(px - r.left) - 1.0f, 4.0f, 2.0f);
            Gdiplus::SolidBrush fillBr(gpColor(mixc(C_NAVY, C_GOLD, 0.35)));
            gr.FillPath(&fillBr, &done);
        }
        roundBox(gr, px - 5.5f, mid - 10.0f, 12.0f, 20.0f, 4.0f, C_GOLD, 255, C_KNOB);
        Gdiplus::Pen grip(gpColor(C_KNOB), 1.2f);
        gr.DrawLine(&grip, px + 0.5f, mid - 5.0f, px + 0.5f, mid + 5.0f);
    }
    for (int i = 0; i <= 10; ++i) {
        const int tx = r.left + 6 + span * i / 10;
        HPEN p = linePen((i % 5) != 0 ? C_EDGE : C_DIM);
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, tx, r.bottom + 3, nullptr);
        LineTo(dc, tx, r.bottom + ((i % 5) != 0 ? 6 : 9));
        SelectObject(dc, op);
        DeleteObject(p);
    }
}

static void levelTick(HDC dc, const RECT& r, int maxValue) {
    const int x = r.left + 6 + (r.right - r.left - 12) * 100 / maxValue;
    HPEN p = CreatePen(PS_SOLID, 2, C_GOLD);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x, r.bottom + 2, nullptr);
    LineTo(dc, x, r.bottom + 8);
    SelectObject(dc, op);
    DeleteObject(p);
}

static const int METER_SEGS = 48;
static const float METER_FLOOR_DB = -48.0f, METER_RISE_SEC = 0.012f, METER_FALL_DB = 26.0f, METER_HOLD_SEC = 0.9f, METER_HOLD_FALL_DB = 14.0f;
static const float METER_WARM_AT = 0.44f, METER_HOT_AT = 0.875f;
static float meterDb = METER_FLOOR_DB - 12.0f, meterHoldDb = METER_FLOOR_DB - 12.0f, meterHeldFor = 0.0f;
static ULONGLONG meterLast = 0;

static void meterTick() {
    const ULONGLONG now = GetTickCount64();
    const float dt = meterLast ? std::min(0.1f, (float)(now - meterLast) / 1000.0f) : 0.016f;
    meterLast = now;
    const float peak = (float)g.meterPeak.exchange(0) / 10000.0f;
    const float db = peak > 1e-5f ? 20.0f * log10f(peak) : -100.0f;
    if (db > meterDb) meterDb += (db - meterDb) * (1.0f - expf(-dt / METER_RISE_SEC));
    else meterDb = std::max(db, meterDb - METER_FALL_DB * dt);
    if (meterDb >= meterHoldDb) {
        meterHoldDb = meterDb;
        meterHeldFor = 0.0f;
    } else if ((meterHeldFor += dt) > METER_HOLD_SEC) {
        meterHoldDb = std::max(meterDb, meterHoldDb - METER_HOLD_FALL_DB * dt);
    }
}

static RECT stripRect() {
    return RECT{0, WIN_H - STATUS_H - STRIP_H, WIN_W, WIN_H - STATUS_H};
}

static COLORREF levelColour(int seg) {
    const float t = (seg + 0.5f) / (float)METER_SEGS;
    if (t >= METER_HOT_AT) return RGB(236, 72, 84);
    if (t >= METER_WARM_AT) return mixc(RGB(246, 150, 62), RGB(240, 108, 58), (t - METER_WARM_AT) / (METER_HOT_AT - METER_WARM_AT));
    if (t >= 0.30f) return RGB(238, 196, 76);
    return mixc(shade(C_GOLD, 0.62), C_GOLD, t / 0.30f);
}

static float meterSegs(float db) {
    const float n = (db - METER_FLOOR_DB) / -METER_FLOOR_DB * METER_SEGS;
    return n < 0.0f ? 0.0f : (n > METER_SEGS ? (float)METER_SEGS : n);
}

static void drawLevelStrip(HDC dc, const RECT& s) {
    blendRect(dc, s, C_PANEL, (BYTE)alphaFor(C_PANEL));
    HPEN ep = linePen(C_EDGE);
    HGDIOBJ oep = SelectObject(dc, ep);
    MoveToEx(dc, s.left, s.top, nullptr);
    LineTo(dc, s.right, s.top);
    SelectObject(dc, oep);
    DeleteObject(ep);
    const int midY = (s.top + s.bottom) / 2;
    label(dc, s.left + 16, midY - 8, L"Mic output", C_DIM, fntUI);
    const float lit = meterSegs(meterDb);
    const int full = (int)lit, held = (int)meterSegs(meterHoldDb);
    const float part = lit - full;
    const int x0 = s.left + 96, x1 = s.right - 84;
    const float step = (float)(x1 - x0) / METER_SEGS;
    {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (int i = 0; i < METER_SEGS; ++i) {
            const COLORREF zone = levelColour(i), off = mixc(C_BACK, zone, 0.20);
            double glow = i < full ? 1.0 : (i == full ? part : 0.0);
            if (held > 0 && i == std::min(held, METER_SEGS - 1) && held > full) glow = 1.0;
            Gdiplus::GraphicsPath seg;
            roundPath(seg, x0 + i * step + 1.0f, midY - 7.0f, step - 3.0f, 14.0f, 2.0f);
            Gdiplus::SolidBrush br(gpColor(mixc(off, zone, glow)));
            gr.FillPath(&br, &seg);
        }
    }
    wchar_t t[32];
    if (meterDb <= METER_FLOOR_DB) wcscpy_s(t, L"silent");
    else swprintf_s(t, 32, L"%.0f dB", meterDb > 0.0f ? 0.0f : meterDb);
    RECT tr = {x1 + 8, s.top, s.right - 16, s.bottom};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, lit >= METER_WARM_AT * METER_SEGS ? levelColour(std::max(0, full - 1)) : C_TEXT);
    HGDIOBJ of = SelectObject(dc, fntBold);
    drawText(dc, t, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

static double sbShown = -1.0, sbHeld = 0.0;

static bool presetBarStep(double ms) {
    if (!hPreset) return false;
    const double top = (double)SendMessageW(hPreset, LB_GETTOPINDEX, 0, 0);
    bool moved = false;
    if (sbShown < 0.0 || sbDragOff >= 0 || !uiAnim) {
        moved = sbShown != top;
        sbShown = top;
    } else if (sbShown != top) {
        sbShown += (top - sbShown) * (1.0 - std::exp(-ms / 45.0));
        if (std::fabs(top - sbShown) < 0.01) sbShown = top;
        moved = true;
    }
    const double want = sbDragOff >= 0 ? 1.0 : 0.0;
    if (sbHeld != want) {
        sbHeld = want > sbHeld ? std::min(want, sbHeld + (uiAnim ? ms / 90.0 : 1.0)) : std::max(want, sbHeld - (uiAnim ? ms / 220.0 : 1.0));
        moved = true;
    }
    return moved;
}

static void drawPresetScrollbar(HDC dc) {
    if (!hPreset) return;
    const int total = (int)SendMessageW(hPreset, LB_GETCOUNT, 0, 0);
    const int top = (int)SendMessageW(hPreset, LB_GETTOPINDEX, 0, 0);
    const int ITEM_H = 22;
    const int vis = PRESET_H / ITEM_H;
    const int x = PRESET_SBX, y = PRESET_Y, sw = PRESET_SBW, sh = PRESET_H, AH = sw;
    const bool canUp = top > 0, canDn = total > vis && top + vis < total;

    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    roundBox(gr, x + 0.5f, y + 0.5f, sw - 1.0f, sh - 1.0f, 5.0f, C_BACK, alphaFor(C_BACK), C_EDGE);
    auto chevron = [&](float cy, bool up, bool on) {
        const RECT& part = up ? R_SB_UP : R_SB_DOWN;
        const double hk = on ? hotLevelFor(part, isHot(part)) : 0.0, pad = std::max(0.55 * hk, pressLevelFor(part));
        if (pad > 0.0) {
            const COLORREF fill = mixc(C_BACK, C_NAVY, pad);
            roundBox(gr, x + 2.0f, cy - AH / 2.0f + 2.0f, sw - 4.0f, AH - 4.0f, 3.0f, fill, fadeAlpha(pad), fill);
        }
        Gdiplus::Pen pen(gpColor(mixc(on ? C_DIM : C_EDGE, C_GOLD, hk)), 1.6f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        const float cx = x + sw / 2.0f, k = up ? -1.0f : 1.0f;
        Gdiplus::PointF p[3] = {{cx - 3.0f, cy - 1.5f * k}, {cx, cy + 1.5f * k}, {cx + 3.0f, cy - 1.5f * k}};
        gr.DrawLines(&pen, p, 3);
    };
    chevron(y + AH / 2.0f, true, canUp);
    chevron(y + sh - AH / 2.0f, false, canDn);
    if (total > vis) {
        const int trackH = sh - 2 * AH;
        const int thumbH = max(AH, trackH * vis / total);
        const int maxTop = total - vis;
        const double shown = sbShown < 0.0 || !uiAnim || sbDragOff >= 0 ? (double)top : std::min((double)maxTop, std::max(0.0, sbShown));
        const float thumbY = maxTop <= 0 ? 0.0f : (shown == (double)top ? (float)((long long)top * (trackH - thumbH) / maxTop) : (float)(shown * (trackH - thumbH) / maxTop));
        const double lift = 0.30 + 0.12 * hotLevelFor(R_SB_TRACK, isHot(R_SB_TRACK));
        const double held = uiAnim ? sbHeld : (sbDragOff >= 0 ? 1.0 : 0.0);
        const double shade = lift + (0.55 - lift) * std::max(held, 0.7 * pressLevelFor(R_SB_TRACK));
        roundBox(gr, x + 2.5f, y + AH + thumbY, sw - 5.0f, (float)thumbH, (sw - 5.0f) / 2.0f,
                 mixc(C_NAVY, C_GOLD, shade), 255, C_NAVY);
    }
}

static const wchar_t* BRAND_LABEL = L"Copyright";

static const int  CARD_TOP = 120;

static HWND hClipUrl = nullptr;
static const int ID_CLIPURL = 7301;
static const int ID_APP = 7302;
static RECT rCredit = {0, 0, 0, 0};
static RECT rStatus = {0, 0, 0, 0};
static bool hotStatus = false;

static const RECT R_CARD = {150, CARD_TOP, 970, CARD_TOP + 490};
static const int CARD_X = R_CARD.left + 32, CARD_R = 610, ART_CX = 793;

static void drawCard(HDC dc) {
    panel(dc, R_CARD, C_PANEL, C_EDGE);
    HPEN dp = linePen(C_EDGE);
    HGDIOBJ op = SelectObject(dc, dp);
    MoveToEx(dc, 632, R_CARD.top + 32, nullptr);
    LineTo(dc, 632, R_CARD.bottom - 32);
    SelectObject(dc, op);
    DeleteObject(dp);
    const int artTop = R_CARD.top + 40;
    drawGlow(dc, (float)ART_CX, artTop + ART_H / 2.0f, 150.0f, 196.0f, C_GOLD, 64);
    drawBitmapAlpha(dc, artBmp, ART_CX - artW / 2, artTop, artW, artH);
    auto centred = [&](int y, const wchar_t* s, COLORREF c, HFONT f) {
        RECT r = {640, y, R_CARD.right - 16, y + 26};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c);
        HGDIOBJ of = SelectObject(dc, f);
        drawText(dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    };
    centred(artTop + ART_H + 14, L"OctopusEAX", C_GOLD, fntTitle);
    centred(artTop + ART_H + 40, L"Audio Effects Console", C_DIM, fntUI);
}

static void drawStep(HDC dc, int y, int n, const wchar_t* text) {
    {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush br(gpColor(C_NAVY));
        Gdiplus::Pen pen(gpColor(C_GOLD), 1.0f);
        gr.FillEllipse(&br, (float)CARD_X, (float)y, 19.0f, 19.0f);
        gr.DrawEllipse(&pen, (float)CARD_X, (float)y, 19.0f, 19.0f);
    }
    wchar_t num[4];
    swprintf_s(num, 4, L"%d", n);
    RECT nr = {CARD_X, y, CARD_X + 20, y + 20};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_GOLD);
    HGDIOBJ of = SelectObject(dc, fntBold);
    drawText(dc, num, -1, &nr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    label(dc, CARD_X + 30, y + 2, text, C_TEXT, fntUI);
}

static const wchar_t* CABLE_SITE = L"https://vb-audio.com/Cable/";

static const RECT R_CB_RUN    = {CARD_X, CARD_TOP + 348, CARD_X + 164, CARD_TOP + 384};
static const RECT R_CB_FOLDER = {CARD_X + 174, CARD_TOP + 348, CARD_X + 310, CARD_TOP + 384};
static const RECT R_CB_BACK   = {CARD_X + 320, CARD_TOP + 348, CARD_R, CARD_TOP + 384};
static const RECT R_CB_SITE   = {CARD_X, CARD_TOP + 400, CARD_X + 190, CARD_TOP + 418};

static bool cablePanel = false;
static bool cableAsked = false;

enum { VIEW_INSTALL, VIEW_RESTART, VIEW_READY };

static int cableView() {
    const int job = gCableJob.load();
    if (job == CABLE_FETCH || job == CABLE_UNPACK || job == CABLE_SETUP) return VIEW_INSTALL;
    const int stage = g.cableStage.load();
    if (stage == STAGE_READY || stage == STAGE_PROBLEM) return VIEW_READY;
    if (stage == STAGE_RESTART) return VIEW_RESTART;
    return VIEW_INSTALL;
}

static void restartPc(HWND owner) {
    if (MessageBoxW(owner, L"Restart your PC now? Save anything you have open first.", L"OctopusEAX",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION | SHTDN_REASON_FLAG_PLANNED);
}

static void drawCablePanel(HDC dc, const RECT&) {
    drawCard(dc);
    const int top = R_CARD.top + 30;
    const int view = cableView();
    if (view == VIEW_READY) {
        const bool clean = g.cableStage.load() == STAGE_READY;
        wchar_t name[256];
        {
            std::lock_guard<std::mutex> lk(g.devMx);
            wcsncpy_s(name, g.cableName[0] ? g.cableName : L"What U Hear", 255);
        }
        label(dc, CARD_X, top, clean ? L"The virtual mic is ready" : L"The virtual mic needs a hand", C_GOLD, fntTitle);
        if (clean) {
            label(dc, CARD_X, top + 40, L"In Discord, your games or any other app, pick this microphone:", C_DIM, fntUI);
            label(dc, CARD_X, top + 66, name, C_TEXT, fntBold);
            label(dc, CARD_X, top + 100, L"Everything you hear in OctopusEAX goes out through it.", C_DIM, fntUI);
            drawStep(dc, top + 140, 1, L"Open the voice settings in Discord or your game.");
            drawStep(dc, top + 164, 2, L"Set the input device to the microphone above.");
            drawStep(dc, top + 188, 3, L"Talk, and they hear you with your effects.");
        } else {
            wchar_t line[256];
            {
                std::lock_guard<std::mutex> lk(g.devMx);
                wcsncpy_s(line, g.cable, 255);
            }
            RECT tr = {CARD_X, top + 40, CARD_R, top + 120};
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, C_TEXT);
            HGDIOBJ of = SelectObject(dc, fntUI);
            drawText(dc, line, -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, of);
            iconButton(dc, R_CB_BACK, ICO_NONE, L"Sound settings", isHot(R_CB_BACK));
        }
        primaryButton(dc, R_CB_RUN, ICO_NONE, L"Done", isHot(R_CB_RUN));
        return;
    }
    if (view == VIEW_RESTART) {
        label(dc, CARD_X, top, L"Almost done", C_GOLD, fntTitle);
        label(dc, CARD_X, top + 40, L"VB-CABLE is installed. Windows needs a restart before", C_DIM, fntUI);
        label(dc, CARD_X, top + 58, L"the virtual mic shows up.", C_DIM, fntUI);
        drawStep(dc, top + 98, 1, L"Save anything you have open.");
        drawStep(dc, top + 122, 2, L"Press Restart now.");
        drawStep(dc, top + 146, 3, L"Open OctopusEAX again after Windows starts.");
        primaryButton(dc, R_CB_RUN, ICO_NONE, L"Restart now", isHot(R_CB_RUN));
        iconButton(dc, R_CB_BACK, ICO_PREV, L"Later", isHot(R_CB_BACK));
        return;
    }
    label(dc, CARD_X, top, L"Install the virtual mic", C_GOLD, fntTitle);
    label(dc, CARD_X, top + 40, L"OctopusEAX sends your voice to Discord and games through", C_DIM, fntUI);
    label(dc, CARD_X, top + 58, L"VB-CABLE, a free virtual audio cable by VB-Audio. Press Install", C_DIM, fntUI);
    label(dc, CARD_X, top + 76, L"to download it from vb-audio.com.", C_DIM, fntUI);
    drawStep(dc, top + 116, 1, L"Press Install.");
    drawStep(dc, top + 140, 2, L"Click Yes, then Install Driver.");
    drawStep(dc, top + 164, 3, L"Restart your PC.");

    const int job = gCableJob.load();
    const bool busy = job == CABLE_FETCH || job == CABLE_UNPACK || job == CABLE_SETUP;
    const wchar_t* note = job == CABLE_FETCH  ? L"Downloading VB-CABLE..."
                        : job == CABLE_UNPACK ? L"Unpacking..."
                        : job == CABLE_SETUP  ? L"The VB-CABLE installer is open. Finish it, then restart your PC."
                        : job == CABLE_FAILED ? L"Download failed. Check your connection and try again."
                                              : nullptr;
    if (note) label(dc, CARD_X, R_CB_RUN.top - 30, note, C_GOLD, fntBold);
    primaryButton(dc, R_CB_RUN, ICO_NONE, job == CABLE_FAILED ? L"Try again" : (busy ? L"Working..." : L"Install"),
                  isHot(R_CB_RUN), !busy);
    iconButton(dc, R_CB_FOLDER, ICO_OPEN, L"Open folder", isHot(R_CB_FOLDER), !cableSetup().empty());
    iconButton(dc, R_CB_BACK, ICO_PREV, L"Back", isHot(R_CB_BACK));
    label(dc, R_CB_SITE.left, R_CB_SITE.top, L"vb-audio.com/Cable", mixc(C_GOLD, C_TEXT, hotLevelFor(R_CB_SITE, isHot(R_CB_SITE))), fntBold);
    label(dc, CARD_X, R_CB_SITE.bottom + 10, L"VB-CABLE is donationware made by VB-Audio Software.", C_DIM, fntUI);
}

static const int  EQ_Y   = TITLE_H + 12;
static const int  EQ_H   = 228;
static const int  EQ_FX0 = CX + 76, EQ_FDX = 54;
static const int  EQ_LVX = CX + 484;
static const int  EQ_FT  = EQ_Y + 56, EQ_FB = EQ_Y + 172;
static const RECT R_EQ_ON   = {CX + 180, EQ_Y + 9, CX + 330, EQ_Y + 24};
static const RECT R_EQ_BOX  = {CX + 382, EQ_Y + 5, CX + 482, EQ_Y + 29};
static const RECT R_EQ_SAVE = {CX + 486, EQ_Y + 5, CX + 512, EQ_Y + 29};
static const RECT R_EQ_DROP = {CX + 516, EQ_Y + 5, CX + 542, EQ_Y + 29};
static const RECT R_EQ_DEF  = {CX + 24, EQ_FB + 29, CX + 108, EQ_FB + 51};

static const int  VOICE_Y = TITLE_H + 454;
static const int  VOICE_H = 196;
static const RECT R_PITCH = {114, VOICE_Y + 31, 250, VOICE_Y + 53};
static const RECT R_SIZE  = {352, VOICE_Y + 31, 494, VOICE_Y + 53};
static const RECT R_VOICE = {114, VOICE_Y + 64, 464, VOICE_Y + 86};
static const int  PITCH_SPAN = 150;
static const int  VSIZE_MIN = 60, VSIZE_MAX = 150;
static const RECT R_MASK_BOX = {110, VOICE_Y + 4, 300, VOICE_Y + 28};
static const RECT R_OWN_BOX = {306, VOICE_Y + 4, 480, VOICE_Y + 28};
static const RECT R_MASK_OFF = {486, VOICE_Y + 4, 542, VOICE_Y + 28};
static const RECT R_VSPEED = {114, VOICE_Y + 96, 230, VOICE_Y + 118};
static const RECT R_VKEEP  = {284, VOICE_Y + 100, 364, VOICE_Y + 115};
static const RECT R_VLAG   = {392, VOICE_Y + 98, 542, VOICE_Y + 117};
static const wchar_t* const KEEP_PITCH = L"Keep pitch";
static const RECT R_GLITCH = {110, VOICE_Y + 131, 304, VOICE_Y + 155};
static const RECT R_GLITCH_BOX = {310, VOICE_Y + 131, 420, VOICE_Y + 155};
static const RECT R_GLITCH_KEY = {426, VOICE_Y + 131, 452, VOICE_Y + 155};
static bool glitchListening = false;
static std::wstring glitchNote;
static ULONGLONG glitchNoteAt = 0;
static std::wstring keyName(UINT mods, UINT vk);
static bool plainKey(UINT vk);

static constexpr DWORD chordOf(UINT mods, UINT vk) { return (DWORD)((mods << 16) | vk); }
enum { ACT_LAST, ACT_RECORD, ACT_SEND, ACT_EFFECTS, ACT_MASK, ACT_ECHO, ACT_ISOLATE, ACT_HEAR, ACT_SETUP_NEXT, ACT_SETUP_PREV, NUM_SHORTCUTS };
struct Shortcut {
    const wchar_t* reg;
    const wchar_t* name;
    const wchar_t* what;
};
static const Shortcut SHORTCUTS[NUM_SHORTCUTS] = {
    {L"HotLast", L"Save the last", L"Save what Last keeps to a file"},
    {L"HotRecord", L"Record", L"Start or stop recording"},
    {L"HotSend", L"Send to mic", L"Play the recording on the mic, or stop it"},
    {L"HotEffects", L"Effects", L"Audio effects on or off"},
    {L"HotMask", L"Voice mask", L"Voice mask on your voice, on or off"},
    {L"HotEcho", L"Echo", L"Echo on your voice, on or off"},
    {L"HotIsolation", L"Isolation", L"Noise isolation on or off"},
    {L"HotHear", L"Hear myself", L"Hear myself on or off"},
    {L"HotNextSetup", L"Next setup", L"Switch to the next saved setup"},
    {L"HotPrevSetup", L"Previous setup", L"Switch to the previous saved setup"},
};
static DWORD shortcutKey[NUM_SHORTCUTS];
static int keyListen = -1, hotKeyRow = -1;
static std::wstring keyNote, clipsDir;

static bool keyUsable(DWORD key) {
    const UINT mods = key >> 16, vk = key & 0xFFFF;
    return vk > 0 && vk < 0xFF && (mods & ~(UINT)(MOD_CONTROL | MOD_ALT | MOD_SHIFT)) == 0 && (mods || !plainKey(vk));
}

static std::wstring keyText(DWORD key) { return keyName(key >> 16, key & 0xFFFF); }
static std::wstring keyLabel(int a) { return shortcutKey[a] ? keyText(shortcutKey[a]) : L"Not set"; }

static void keyListenFlip(HWND h, int row);
static void keyListenEnd();
static bool keyCapture(HWND h, const MSG& m);
static void keyClear(int row);
static std::wstring keepText(int sec);
static std::wstring clipsFolder();
static void clipsOpen();
static void clipsPick(HWND owner);
static const int  MASK_Y = VOICE_Y + 168;
static const RECT R_MASK_VOICE = {110, MASK_Y, 250, MASK_Y + 15};
static const RECT R_MASK_CLIP  = {258, MASK_Y, 398, MASK_Y + 15};
static const RECT R_MASK_APP   = {406, MASK_Y, 542, MASK_Y + 15};
static const int ID_MASKBOX = 7304, ID_OWNBOX = 7305, ID_GLITCHBOX = 7306;
static const int SPEED_MIN = 50, SPEED_MAX = 200;

static int speedToFader(int pct) {
    return (int)std::lround(100.0 + 100.0 * std::log2(pct / 100.0));
}
static int faderToSpeed(int v) {
    int pct = (int)std::lround(20.0 * std::pow(2.0, (v - 100) / 100.0)) * 5;
    return pct < SPEED_MIN ? SPEED_MIN : (pct > SPEED_MAX ? SPEED_MAX : pct);
}
static int stepSpeed(int pct, int d) {
    pct += d * 5;
    return pct < SPEED_MIN ? SPEED_MIN : (pct > SPEED_MAX ? SPEED_MAX : pct);
}
static void drawLagNote(HDC dc) {
    const bool on = g.voiceSpeed.load() != 100 && (g.changeOn.load() & ON_VOICE) != 0;
    const int ms = g.voiceLagMs.load();
    wchar_t full[64] = L"", brief[32] = L"";
    if (!on) {
        wcscpy_s(full, L"Speed adds a delay");
        wcscpy_s(brief, L"Adds a delay");
    } else if (ms < 50) {
        wcscpy_s(full, L"Others hear you late");
        wcscpy_s(brief, L"Heard late");
    } else {
        swprintf_s(full, 64, L"Others hear you %d.%d s late", ms / 1000, (ms % 1000) / 100);
        swprintf_s(brief, 32, L"%d.%d s late", ms / 1000, (ms % 1000) / 100);
    }
    const COLORREF c = on ? C_GOLD : C_DIM;
    SIZE keep{}, sz{};
    HGDIOBJ of = SelectObject(dc, fntUI);
    GetTextExtentPoint32W(dc, KEEP_PITCH, (int)wcslen(KEEP_PITCH), &keep);
    const int room = R_VLAG.right - (R_VKEEP.left + optionText(ICO_TUNING) + keep.cx + 23);
    const wchar_t* t = full;
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    if (sz.cx > room) {
        t = brief;
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    }
    SelectObject(dc, of);
    const int x = R_VLAG.right - sz.cx;
    drawIcon(dc, x - 11.0f, (R_VLAG.top + R_VLAG.bottom) / 2.0f, ICO_WARN, c, C_PANEL);
    label(dc, x, R_VLAG.top + 2, t, c, fntUI);
}
static int glitchIndex() {
    const int s = g.glitchStyle.load();
    return s >= 0 && s < voice::NUM_GLITCH_STYLES ? s : voice::GLITCH_REPEAT;
}
static void showGlitch() {
    if (!hGlitchBox) return;
    SendMessageW(hGlitchBox, CB_SETCURSEL, (WPARAM)glitchIndex(), 0);
    InvalidateRect(hGlitchBox, nullptr, TRUE);
}
static int ownSlot(int mask) {
    for (int k = 0; k < voice::NUM_OWN_MASKS; ++k)
        if (voice::OWN_ORDER[k] == mask) return k;
    return -1;
}
static void showMask() {
    const int mi = g.voiceMask.load();
    if (hMaskBox) {
        SendMessageW(hMaskBox, CB_SETCURSEL, mi >= 0 && mi < voice::NUM_XFI_MASKS ? (WPARAM)mi : (WPARAM)-1, 0);
        InvalidateRect(hMaskBox, nullptr, TRUE);
    }
    if (hOwnBox) {
        SendMessageW(hOwnBox, CB_SETCURSEL, (WPARAM)ownSlot(mi), 0);
        InvalidateRect(hOwnBox, nullptr, TRUE);
    }
}
static void fillMasks() {
    if (!hMaskBox || !hOwnBox) return;
    for (HWND b : {hMaskBox, hOwnBox}) SendMessageW(b, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < voice::NUM_XFI_MASKS; ++i) SendMessageW(hMaskBox, CB_ADDSTRING, 0, (LPARAM)voice::MASKS[i].name);
    for (int k = 0; k < voice::NUM_OWN_MASKS; ++k) SendMessageW(hOwnBox, CB_ADDSTRING, 0, (LPARAM)voice::MASKS[voice::OWN_ORDER[k]].name);
    showMask();
}
static void pickMask(int i) {
    if (i < 0 || i >= voice::NUM_MASKS) return;
    g.voiceMask = i;
    g.voiceRatio = voice::MASKS[i].ratio;
    g.voiceSize = voice::MASKS[i].size;
    g.pitchHz = 0;
    g.voiceMacro = 0;
    showMask();
}
static void maskLabel(int i, wchar_t* txt) {
    wcsncpy_s(txt, 256, i == voice::MASK_NEUTRAL ? L"X-Fi" : voice::MASKS[i].name, 250);
    if (g.pitchHz.load() != 0 || g.voiceSize.load() != voice::MASKS[i].size || g.voiceRatio.load() != voice::MASKS[i].ratio)
        wcscat_s(txt, 256, L"*");
}

static const int  CLIP_Y = EQ_Y + EQ_H + 10;
static const int  CLIP_H = 400;
static const RECT R_CLIP_URL  = {CX + 96,  CLIP_Y + 9,  CX + 306, CLIP_Y + 33};
static const RECT R_CLIP_FILE = {CX + 314, CLIP_Y + 9,  CX + 382, CLIP_Y + 33};
static const RECT R_CLIP_PLAY = {CX + 390, CLIP_Y + 9,  CX + 466, CLIP_Y + 33};
static const RECT R_CLIP_STOP = {CX + 474, CLIP_Y + 9,  CX + 542, CLIP_Y + 33};
static const RECT R_CLIP_POS  = {CX + 96,  CLIP_Y + 50, CX + 300, CLIP_Y + 72};
static const RECT R_CLIP_BACK = {CX + 418, CLIP_Y + 50, CX + 476, CLIP_Y + 72};
static const RECT R_CLIP_FWD  = {CX + 484, CLIP_Y + 50, CX + 542, CLIP_Y + 72};
static const RECT R_CLIP_VOL  = {CX + 96,  CLIP_Y + 86, CX + 300, CLIP_Y + 108};
static const RECT R_CLIP_HEAR = {CX + 358, CLIP_Y + 82, CX + 508, CLIP_Y + 97};
static const RECT R_CLIP_MIC  = {CX + 358, CLIP_Y + 104, CX + 542, CLIP_Y + 119};
static const RECT R_CLIP_SPEED = {CX + 96, CLIP_Y + 122, CX + 262, CLIP_Y + 144};
static const RECT R_CLIP_KEEP  = {CX + 318, CLIP_Y + 126, CX + 420, CLIP_Y + 141};
static const RECT R_CLIP_FOLLOW = {CX + 428, CLIP_Y + 126, CX + 542, CLIP_Y + 141};
static const RECT R_APP_BOX   = {CX + 96,  CLIP_Y + 188, CX + COL_W - 24, CLIP_Y + 212};
static const RECT R_APP_VOL   = {CX + 96,  CLIP_Y + 222, CX + 300, CLIP_Y + 244};
static const RECT R_APP_ONLY  = {CX + 358, CLIP_Y + 226, CX + 542, CLIP_Y + 241};
static const RECT R_APP_CARD  = {CX + 24,  CLIP_Y + 274, CX + 236, CLIP_Y + 289};
static const RECT R_APP_SELF  = {CX + 300, CLIP_Y + 274, CX + 542, CLIP_Y + 289};
static const wchar_t* EQ_BAND_NAME[eqb::EQ_BANDS] =
    {L"125", L"250", L"500", L"1k", L"2k", L"4k", L"8k"};
static int dragEq = -1;
static const int ID_EQBOX = 7303;

static int eqFaderX(int k) { return k < eqb::EQ_BANDS ? EQ_FX0 + k * EQ_FDX : EQ_LVX; }

static const wchar_t* EQ_KEY = L"Software\\OctopusEAX\\Equalizer";
static void eqSave() {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, EQ_KEY, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
    auto put = [&](const wchar_t* n, int v) {
        DWORD d = (DWORD)v;
        RegSetValueExW(hk, n, 0, REG_DWORD, (const BYTE*)&d, sizeof d);
    };
    wchar_t n[16];
    for (int k = 0; k < eqb::EQ_BANDS; ++k) {
        swprintf_s(n, 16, L"Band%d", k);
        put(n, g.eqDb[k].load() + eqb::EQ_RANGE);
    }
    put(L"Level", g.eqLevel.load());
    put(L"Enabled", g.eqOn.load() ? 1 : 0);
    put(L"Preset", g.eqPreset.load() + 1);
    RegCloseKey(hk);
}

static const wchar_t* EQ_USER_KEY = L"Software\\OctopusEAX\\Equalizer\\Presets";
struct EqUserPreset { std::wstring name; int band[eqb::EQ_BANDS]; };
static std::vector<EqUserPreset> eqUser;
static void eqUserLoad() {
    eqUser.clear();
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, EQ_USER_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[64] = {}; DWORD nlen = 64, type = 0;
        wchar_t val[128] = {}; DWORD vlen = sizeof val - sizeof(wchar_t);
        const LONG got = RegEnumValueW(hk, i, name, &nlen, nullptr, &type, (BYTE*)val, &vlen);
        if (got == ERROR_MORE_DATA) continue;
        if (got != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        EqUserPreset p{}; p.name = name;
        const wchar_t* s = val;
        for (int k = 0; k < eqb::EQ_BANDS; ++k) {
            int v = _wtoi(s);
            p.band[k] = v < -eqb::EQ_RANGE ? -eqb::EQ_RANGE : (v > eqb::EQ_RANGE ? eqb::EQ_RANGE : v);
            const wchar_t* c = wcschr(s, L',');
            if (!c) break;
            s = c + 1;
        }
        eqUser.push_back(p);
    }
    RegCloseKey(hk);
}
static void eqUserStore(const EqUserPreset& p) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, EQ_USER_KEY, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
    wchar_t val[128] = L"";
    for (int k = 0; k < eqb::EQ_BANDS; ++k) {
        wchar_t t[8]; swprintf_s(t, 8, k ? L",%d" : L"%d", p.band[k]);
        wcscat_s(val, t);
    }
    RegSetValueExW(hk, p.name.c_str(), 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
}
static void eqUserDrop(const std::wstring& name) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, EQ_USER_KEY, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return;
    RegDeleteValueW(hk, name.c_str());
    RegCloseKey(hk);
}
static int eqPresetCount() { return eqb::NUM_EQ_PRESETS + (int)eqUser.size(); }
static const int* eqPresetBands(int i) {
    if (i < 0 || i >= eqPresetCount()) return nullptr;
    return i < eqb::NUM_EQ_PRESETS ? eqb::EQ_PRESETS[i].band : eqUser[i - eqb::NUM_EQ_PRESETS].band;
}
static const wchar_t* eqPresetName(int i) {
    if (i < 0 || i >= eqPresetCount()) return L"Custom";
    return i < eqb::NUM_EQ_PRESETS ? eqb::EQ_PRESETS[i].name : eqUser[i - eqb::NUM_EQ_PRESETS].name.c_str();
}
static bool eqOnPreset() {
    const int* b = eqPresetBands(g.eqPreset.load());
    if (!b) return false;
    for (int k = 0; k < eqb::EQ_BANDS; ++k) if (g.eqDb[k].load() != b[k]) return false;
    return true;
}
static void fillEqPresets() {
    uiEqNames.clear();
    for (int i = 0; i < eqPresetCount(); ++i) uiEqNames.push_back(eqPresetName(i));
    SendMessageW(hEqBox, CB_RESETCONTENT, 0, 0);
    for (const auto& n : uiEqNames) SendMessageW(hEqBox, CB_ADDSTRING, 0, (LPARAM)n.c_str());
    SendMessageW(hEqBox, CB_SETCURSEL, g.eqPreset.load(), 0);
}
static void eqLoad() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, EQ_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return;
    auto get = [&](const wchar_t* n, int lo, int hi, int def) {
        DWORD d = 0, sz = sizeof d, type = 0;
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)&d, &sz) != ERROR_SUCCESS ||
            type != REG_DWORD) return def;
        int v = (int)d;
        return v < lo ? lo : (v > hi ? hi : v);
    };
    wchar_t n[16];
    for (int k = 0; k < eqb::EQ_BANDS; ++k) {
        swprintf_s(n, 16, L"Band%d", k);
        g.eqDb[k] = get(n, 0, 2 * eqb::EQ_RANGE, eqb::EQ_RANGE) - eqb::EQ_RANGE;
    }
    g.eqLevel  = get(L"Level", 0, eqb::EQ_LEVEL_RANGE, 0);
    g.eqOn     = get(L"Enabled", 0, 1, 1) != 0;
    g.eqPreset = get(L"Preset", 1, eqPresetCount(), 1) - 1;
    RegCloseKey(hk);
    g.eqVersion.fetch_add(1);
}
static void eqBump() { g.eqVersion.fetch_add(1); eqSave(); if (hEqBox) InvalidateRect(hEqBox, nullptr, FALSE); }
static void eqApplyPreset(int i) {
    const int* b = eqPresetBands(i);
    if (!b) return;
    for (int k = 0; k < eqb::EQ_BANDS; ++k) g.eqDb[k] = b[k];
    g.eqPreset = i;
    if (hEqBox) SendMessageW(hEqBox, CB_SETCURSEL, i, 0);
    eqBump();
}
static int vFaderValue(int y, int minV, int maxV) {
    const int span = EQ_FB - EQ_FT - 12;
    int v = minV + ((EQ_FB - 6 - y) * (maxV - minV) + span / 2) / std::max(1, span);
    return v < minV ? minV : (v > maxV ? maxV : v);
}
static void setEqFromY(HWND hwnd, int k, int y) {
    std::atomic<int>& slot = k < eqb::EQ_BANDS ? g.eqDb[k] : g.eqLevel;
    const int v = k < eqb::EQ_BANDS ? vFaderValue(y, -eqb::EQ_RANGE, eqb::EQ_RANGE) : -vFaderValue(y, -eqb::EQ_LEVEL_RANGE, 0);
    if (slot.load() == v) return;
    slot = v;
    g.eqVersion.fetch_add(1);
    if (hEqBox) InvalidateRect(hEqBox, nullptr, FALSE);
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void drawVFader(HDC dc, int x, int value, int minV, int maxV, bool bipolar) {
    const int span = EQ_FB - EQ_FT - 12;
    const int py = EQ_FB - 6 - span * (value - minV) / (maxV - minV);
    const int zy = bipolar ? EQ_FB - 6 - span * (0 - minV) / (maxV - minV) : EQ_FT + 6;
    for (int i = 0; i <= 12; ++i) {
        const int ty = EQ_FB - 6 - span * i / 12;
        const bool major = i % 6 == 0;
        HPEN p = linePen(major ? C_DIM : C_EDGE);
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, x - (major ? 12 : 9), ty, nullptr);
        LineTo(dc, x - 5, ty);
        MoveToEx(dc, x + 6, ty, nullptr);
        LineTo(dc, x + (major ? 13 : 10), ty);
        SelectObject(dc, op);
        DeleteObject(p);
    }
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    roundBox(gr, x - 3.0f + 0.5f, (float)EQ_FT, 6.0f, (float)(EQ_FB - EQ_FT), 3.0f, C_BACK, alphaFor(C_BACK), C_EDGE);
    const int top = py < zy ? py : zy, bot = py < zy ? zy : py;
    if (bot - top > 1) {
        Gdiplus::GraphicsPath done;
        roundPath(done, x - 2.0f + 0.5f, (float)top, 4.0f, (float)(bot - top), 2.0f);
        Gdiplus::SolidBrush fillBr(gpColor(mixc(C_NAVY, C_GOLD, 0.35)));
        gr.FillPath(&fillBr, &done);
    }
    if (bipolar) {
        Gdiplus::PointF mark[3] = {{x + 22.0f, zy - 4.0f}, {x + 22.0f, zy + 4.0f}, {x + 17.0f, (float)zy}};
        Gdiplus::SolidBrush mb(gpColor(C_DIM));
        gr.FillPolygon(&mb, mark, 3);
    }
    roundBox(gr, x - 10.0f + 0.5f, py - 5.5f, 20.0f, 12.0f, 4.0f, C_GOLD, 255, C_KNOB);
    Gdiplus::Pen grip(gpColor(C_KNOB), 1.2f);
    gr.DrawLine(&grip, x - 5.0f + 0.5f, py + 0.5f, x + 5.0f + 0.5f, py + 0.5f);
}
static const wchar_t* appRouteNote() {
    const bool aux = nameContains(appParkName, L"Voicemeeter") || nameContains(appParkName, L"VAIO");
    if (aux) return g.appHear.load() ? L"The original plays on Aux, so turn off A1 there. What U Hear gets the processed sound only."
                                     : L"The original stays on Aux. What U Hear follows the level above. Input and A1 are left alone.";
    if (!appParkName.empty()) return L"The original goes to Steam Streaming Speakers, so you hear it once, from the app.";
    if (g.appOnly.load()) return L"The original plays on its own speakers, and only you hear it.";
    if (appToMonitor()) return L"The original plays on its own speakers, the app adds only its echo.";
    return L"The original plays on its own speakers.";
}

static const RECT EQ_BOX = {CX + 12, EQ_Y, CX + COL_W - 12, EQ_Y + EQ_H};
static const RECT VOICE_BOX = {12, VOICE_Y, COL_W - 12, VOICE_Y + VOICE_H};

static void drawEqBands(HDC dc) {
    RECT eq = {CX + 12, EQ_Y, CX + COL_W - 12, EQ_Y + EQ_H};
    panel(dc, eq, C_PANEL, C_EDGE);
    label(dc, CX + 24, EQ_Y + 8, L"Graphic equalizer", C_GOLD, fntBold);
    const bool on = g.eqOn.load();
    drawCheck(dc, CX + 180, EQ_Y + 9, on, ICO_BARS, L"Enable equalizer");
    label(dc, R_EQ_BOX.left - 48, EQ_Y + 8, L"Presets", C_DIM, fntUI);
    const bool own = g.eqPreset.load() >= eqb::NUM_EQ_PRESETS;
    iconButton(dc, R_EQ_SAVE, ICO_SAVE, nullptr, isHot(R_EQ_SAVE));
    iconButton(dc, R_EQ_DROP, ICO_TRASH, nullptr, isHot(R_EQ_DROP), own);
    iconButton(dc, R_EQ_DEF, ICO_RESET, L"Default", isHot(R_EQ_DEF));

    wchar_t v[96];
    const int mic = g.micLevel.load();
    const double bdb = mic > 0 ? eqb::BassBoost::boostFor(20.0 * log10((double)micGainFor(mic))) : 0.0;
    swprintf_s(v, 96, L"Bass boost  %+.0f dB", bdb);
    label(dc, R_EQ_DEF.right + 14, R_EQ_DEF.top + 4, v, on ? C_TEXT : C_DIM, fntUI);

    auto centred = [&](int x, int y, const wchar_t* s, COLORREF c, HFONT f) {
        RECT r = {x - 20, y, x + 20, y + 14};
        SetTextColor(dc, c);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, f);
        drawText(dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(dc, of);
    };
    auto rightEnd = [&](int right, int y, const wchar_t* s) {
        RECT r = {right - 40, y, right, y + 14};
        SetTextColor(dc, C_DIM);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, fntUI);
        drawText(dc, s, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(dc, of);
    };
    rightEnd(EQ_FX0 - 20, EQ_FT - 1, L"12dB");
    rightEnd(EQ_FX0 - 20, (EQ_FT + EQ_FB) / 2 - 7, L"0dB");
    rightEnd(EQ_FX0 - 20, EQ_FB - 13, L"-12dB");
    for (int k = 0; k < eqb::EQ_BANDS; ++k) {
        const int x = eqFaderX(k);
        const int db = g.eqDb[k].load();
        drawVFader(dc, x, db, -eqb::EQ_RANGE, eqb::EQ_RANGE, true);
        swprintf_s(v, 96, db > 0 ? L"+%d" : L"%d", db);
        centred(x, EQ_FT - 24, v, on && db ? C_GOLD : C_DIM, fntUI);
        centred(x, EQ_FB + 12, EQ_BAND_NAME[k], on ? C_TEXT : C_DIM, fntBold);
    }
    drawVFader(dc, EQ_LVX, -g.eqLevel.load(), -eqb::EQ_LEVEL_RANGE, 0, false);
    swprintf_s(v, 96, L"%d", -g.eqLevel.load());
    centred(EQ_LVX, EQ_FT - 24, v, on && g.eqLevel.load() ? C_GOLD : C_DIM, fntUI);
    centred(EQ_LVX, EQ_FB + 12, L"Level", on ? C_TEXT : C_DIM, fntBold);
    label(dc, EQ_LVX + 22, EQ_FT - 1, L"0dB", C_DIM, fntUI);
    label(dc, EQ_LVX + 22, EQ_FB - 13, L"-24dB", C_DIM, fntUI);
}

static void drawEqPanel(HDC dc) {
    if (RectVisible(dc, &EQ_BOX)) drawEqBands(dc);
    if (RectVisible(dc, &VOICE_BOX)) {
        RECT vp = {12, VOICE_Y, COL_W - 12, VOICE_Y + VOICE_H};
        panel(dc, vp, C_PANEL, C_EDGE);
        label(dc, 24, VOICE_Y + 8, L"Voice mask", C_GOLD, fntBold);
        const int ph = g.pitchHz.load(), sz = g.voiceSize.load(), live = g.voiceHz.load();
        const int mi = g.voiceMask.load();
        const bool hasFx = mi >= 0 && mi < voice::NUM_MASKS && voice::MASKS[mi].fx != voice::FX_NONE;
        wchar_t t[64];
        if (ph == 0 && sz == 100 && g.voiceRatio.load() == 1000 && !hasFx && g.voiceSpeed.load() == 100) wcscpy_s(t, L"off");
        else if (live > 0) swprintf_s(t, 64, L"%d \u2192 %d Hz", live, g.voiceTargetHz.load());
        else wcscpy_s(t, L"on");
        {
            RECT st = {460, VOICE_Y + 135, 540, VOICE_Y + 152};
            SetTextColor(dc, C_DIM);
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ of = SelectObject(dc, fntUI);
            drawText(dc, t, -1, &st, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
        {
            const bool held = g.glitchHold.load() || g.glitchKeyHeld.load();
            const voice::GlitchLook& gl = voice::GLITCH_LOOKS[glitchIndex()];
            label(dc, 24, VOICE_Y + 136, L"Glitch", C_DIM, fntUI);
            const int gk = g.glitchKey.load();
            std::wstring text = held ? gl.busy : gl.idle;
            if (glitchListening) text = L"Press a key or mouse button";
            else if (!glitchNote.empty() && GetTickCount64() - glitchNoteAt < 3000) text = glitchNote;
            else if (gk && !held) text += L" (" + keyName((UINT)(gk >> 16), (UINT)(gk & 0xFFFF)) + L")";
            iconButton(dc, R_GLITCH, ICO_LOOP, text.c_str(), isHot(R_GLITCH), true, held || glitchListening);
            iconButton(dc, R_GLITCH_KEY, ICO_KEY, nullptr, isHot(R_GLITCH_KEY), true, glitchListening || gk != 0);
        }
        const bool maskLive = mi != voice::MASK_NEUTRAL || ph != 0 || sz != 100 || g.voiceRatio.load() != 1000 || g.voiceSpeed.load() != 100;
        iconButton(dc, R_MASK_OFF, ICO_RESET, L"Off", isHot(R_MASK_OFF), maskLive);
        label(dc, 24, VOICE_Y + 37, L"Pitch", C_DIM, fntUI);
        drawFader(dc, R_PITCH, ph + PITCH_SPAN, 2 * PITCH_SPAN);
        swprintf_s(t, 64, L"%+d Hz", ph);
        label(dc, R_PITCH.right + 8, VOICE_Y + 37, t, ph ? C_TEXT : C_DIM, fntBold);
        label(dc, 314, VOICE_Y + 37, L"Size", C_DIM, fntUI);
        drawFader(dc, R_SIZE, sz - VSIZE_MIN, VSIZE_MAX - VSIZE_MIN);
        swprintf_s(t, 64, L"%d%%", sz);
        label(dc, R_SIZE.right + 8, VOICE_Y + 37, t, sz != 100 ? C_TEXT : C_DIM, fntBold);
        const int vm = g.voiceMacro.load();
        label(dc, 24, VOICE_Y + 70, L"Voice changer", C_DIM, fntUI);
        drawFader(dc, R_VOICE, vm + 100, 200);
        if (vm == 0) wcscpy_s(t, L"natural");
        else if (vm > 0) swprintf_s(t, 64, L"bigger +%d", vm);
        else swprintf_s(t, 64, L"smaller %d", vm);
        label(dc, R_VOICE.right + 8, VOICE_Y + 70, t, vm ? C_TEXT : C_DIM, fntBold);
        {
            const int sp = g.voiceSpeed.load();
            label(dc, 24, VOICE_Y + 102, L"Speed", C_DIM, fntUI);
            drawFader(dc, R_VSPEED, speedToFader(sp), 200);
            swprintf_s(t, 64, L"%d.%02dx", sp / 100, sp % 100);
            label(dc, R_VSPEED.right + 8, VOICE_Y + 102, t, sp != 100 ? C_TEXT : C_DIM, fntBold);
            drawCheck(dc, R_VKEEP.left, R_VKEEP.top, g.voiceKeepPitch.load(), ICO_TUNING, KEEP_PITCH);
            drawLagNote(dc);
        }
        {
            HPEN sp = linePen(C_EDGE);
            HGDIOBJ osp = SelectObject(dc, sp);
            MoveToEx(dc, 24, MASK_Y - 7, nullptr);
            LineTo(dc, COL_W - 24, MASK_Y - 7);
            SelectObject(dc, osp);
            DeleteObject(sp);
            const int co = g.changeOn.load();
            label(dc, 24, MASK_Y - 1, L"Mask on", C_GOLD, fntBold);
            drawCheck(dc, R_MASK_VOICE.left, MASK_Y, (co & ON_VOICE) != 0, ICO_PERSON, L"My voice");
            drawCheck(dc, R_MASK_CLIP.left, MASK_Y, (co & ON_CLIP) != 0, ICO_NOTE, L"Sound clip");
            drawCheck(dc, R_MASK_APP.left, MASK_Y, (co & ON_PROGRAM) != 0, ICO_WINDOW, L"Program");
        }
    }
    {
        RECT cp = {CX + 12, CLIP_Y, CX + COL_W - 12, CLIP_Y + CLIP_H};
        panel(dc, cp, C_PANEL, C_EDGE);
        const RECT above = {CX + 12, CLIP_Y, CX + COL_W - 12, R_APP_SELF.bottom + 1};
        if (!RectVisible(dc, &above)) return;
        label(dc, CX + 24, CLIP_Y + 13, L"Sound clip", C_GOLD, fntBold);
        panel(dc, R_CLIP_URL, C_BACK, C_EDGE);
        drawIcon(dc, R_CLIP_URL.left + 14.0f, (R_CLIP_URL.top + R_CLIP_URL.bottom) / 2.0f, ICO_LINK, C_DIM, C_BACK);
        const bool playing = gClip.active(), paused = gClip.isPaused();
        iconButton(dc, R_CLIP_FILE, ICO_OPEN, L"Open", isHot(R_CLIP_FILE));
        iconButton(dc, R_CLIP_PLAY, ICO_PLAY, paused ? L"Resume" : L"Play", isHot(R_CLIP_PLAY), true, playing && !paused);
        const bool pausing = playing && !paused;
        iconButton(dc, R_CLIP_STOP, pausing ? ICO_PAUSE : ICO_STOP, pausing ? L"Pause" : L"Stop", isHot(R_CLIP_STOP), playing || paused);
        {
            const int st = gClip.state.load();
            std::wstring m = st == clip::IDLE ? L"Paste a link or open a sound file"
                           : st == clip::DONE ? L"Finished" : gClip.isPaused() ? L"Paused" : gClip.message();
            RECT sr = {CX + 96, CLIP_Y + 34, CX + COL_W - 24, CLIP_Y + 47};
            SetTextColor(dc, st == clip::FAILED ? C_GOLD : C_DIM);
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ of = SelectObject(dc, fntUI);
            drawText(dc, m.c_str(), -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
        {
            const double dur = gClip.durationSec.load(), pos = gClip.positionSec.load();
            const bool canSeek = gClip.seekable();
            const int permille = dragPos || posGlide ? dragPosVal : (dur > 0.0 ? (int)(pos / dur * 1000.0 + 0.5) : 0);
            const double shown = dragPos || posGlide ? dur * dragPosVal / 1000.0 : pos;
            label(dc, CX + 24, CLIP_Y + 55, L"Position", C_DIM, fntUI);
            drawFader(dc, R_CLIP_POS, permille > 1000 ? 1000 : permille, 1000);
            wchar_t tm[48];
            swprintf_s(tm, 48, L"%d:%02d / %d:%02d", (int)shown / 60, (int)shown % 60, (int)dur / 60, (int)dur % 60);
            label(dc, R_CLIP_POS.right + 10, CLIP_Y + 55, tm, canSeek ? C_TEXT : C_DIM, fntBold);
            iconButton(dc, R_CLIP_BACK, ICO_BACK, L"10 s", isHot(R_CLIP_BACK), canSeek);
            iconButton(dc, R_CLIP_FWD,  ICO_FWD, L"10 s", isHot(R_CLIP_FWD), canSeek);
        }
        label(dc, CX + 24, CLIP_Y + 89, L"Volume", C_DIM, fntUI);
        drawFader(dc, R_CLIP_VOL, g.clipGain.load(), CLIP_MAX);
        levelTick(dc, R_CLIP_VOL, CLIP_MAX);
        wchar_t cv[16]; swprintf_s(cv, 16, L"%d", g.clipGain.load());
        label(dc, R_CLIP_VOL.right + 10, CLIP_Y + 89, cv, C_TEXT, fntBold);
        drawCheck(dc, R_CLIP_HEAR.left, R_CLIP_HEAR.top, g.clipHear.load(), ICO_HEADPHONES, L"Hear it myself");
        drawCheck(dc, R_CLIP_MIC.left,  R_CLIP_MIC.top,  g.clipToMic.load(), ICO_SEND, L"Send to the mic");
        {
            const int sp = g.clipSpeed.load(), rate = clipRate();
            label(dc, CX + 24, CLIP_Y + 125, L"Speed", C_DIM, fntUI);
            drawFader(dc, R_CLIP_SPEED, speedToFader(sp), 200);
            swprintf_s(cv, 16, L"%d.%02dx", rate / 100, rate % 100);
            label(dc, R_CLIP_SPEED.right + 10, CLIP_Y + 125, cv, rate != sp ? C_GOLD : (sp != 100 ? C_TEXT : C_DIM), fntBold);
            drawCheck(dc, R_CLIP_KEEP.left, R_CLIP_KEEP.top, g.clipKeepPitch.load(), ICO_TUNING, KEEP_PITCH);
            drawCheck(dc, R_CLIP_FOLLOW.left, R_CLIP_FOLLOW.top, g.clipFollow.load(), ICO_GAUGE, L"Voice speed");
        }
        {
            HPEN dp = linePen(C_EDGE);
            HGDIOBJ op = SelectObject(dc, dp);
            MoveToEx(dc, CX + 24, CLIP_Y + 160, nullptr);
            LineTo(dc, CX + COL_W - 24, CLIP_Y + 160);
            SelectObject(dc, op);
            DeleteObject(dp);
        }
        label(dc, CX + 24, CLIP_Y + 168, L"Program on the mic", C_GOLD, fntBold);
        {
            std::wstring note = appNote.empty() ? std::wstring(L"Pick an app to play through your mic") : appNote;
            COLORREF shade = appNote.empty() ? C_DIM : C_GOLD;
            if (gApp.active()) {
                const bool heard = gApp.heard();
                note = !heard ? L"Waiting for " + gApp.name + L" to play sound" : g.appOnly.load() ? L"Only you hear " + gApp.name : gApp.name + L" is on your mic";
                shade = heard ? C_TEXT : C_DIM;
            } else if (gApp.chosen()) {
                note = L"Opening " + gApp.name + L"...";
                shade = C_DIM;
            }
            RECT nr = {CX + 170, CLIP_Y + 167, CX + COL_W - 24, CLIP_Y + 184};
            SetTextColor(dc, shade);
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ of = SelectObject(dc, fntUI);
            drawText(dc, note.c_str(), -1, &nr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
        label(dc, CX + 24, R_APP_BOX.top + 4, L"Program", C_DIM, fntUI);
        label(dc, CX + 24, R_APP_VOL.top + 3, L"Level", C_DIM, fntUI);
        drawFader(dc, R_APP_VOL, g.appGain.load(), APP_MAX);
        {
            const int span = R_APP_VOL.right - R_APP_VOL.left - 12;
            levelTick(dc, R_APP_VOL, APP_MAX);
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ of = SelectObject(dc, fntUI);
            for (int mark : {100, 500, APP_MAX}) {
                const int mx = R_APP_VOL.left + 6 + span * mark / APP_MAX;
                wchar_t mt[8];
                swprintf_s(mt, 8, L"%d", mark);
                RECT mr = {mx - 30, R_APP_VOL.bottom + 8, mx + 30, R_APP_VOL.bottom + 24};
                SetTextColor(dc, mark == 100 ? C_GOLD : C_DIM);
                drawText(dc, mt, -1, &mr, DT_CENTER | DT_TOP | DT_SINGLELINE);
            }
            SelectObject(dc, of);
            swprintf_s(cv, 16, L"%d", g.appGain.load());
            label(dc, R_APP_VOL.right + 10, R_APP_VOL.top + 3, cv, gApp.active() ? C_TEXT : C_DIM, fntBold);
        }
        drawCheck(dc, R_APP_ONLY.left, R_APP_ONLY.top, g.appOnly.load(), ICO_LOCK, L"Only I hear it");
        drawRadio(dc, R_APP_CARD.left, R_APP_CARD.top,  g.appHear.load(), ICO_CHIP, L"Hear it through the card");
        drawRadio(dc, R_APP_SELF.left, R_APP_SELF.top, !g.appHear.load(), ICO_WINDOW, L"Hear it from the app");
    }
}

static bool startupOn = false;
static const int  ECHO_Y = TITLE_H + 416;
static const RECT R_ECHO_VOICE = {110, ECHO_Y, 250, ECHO_Y + 15};
static const RECT R_ECHO_CLIP  = {258, ECHO_Y, 398, ECHO_Y + 15};
static const RECT R_ECHO_APP   = {406, ECHO_Y, 542, ECHO_Y + 15};
static const RECT R_DEV_REFRESH = {514, TITLE_H + 18, 540, TITLE_H + 42};
static const RECT R_ISOLATE = {110, TITLE_H + 80, 236, TITLE_H + 95};
static const RECT R_STARTUP  = {110, TITLE_H + 108, 420, TITLE_H + 123};
static const RECT R_ISO_PREV = {252, TITLE_H + 77, 272, TITLE_H + 97};
static const RECT R_ISO_NEXT = {336, TITLE_H + 77, 356, TITLE_H + 97};
static const RECT R_ISOLVL   = {372, TITLE_H + 76, 502, TITLE_H + 98};
static bool dragIso = false;
static const wchar_t* SETTINGS_KEY = L"Software\\OctopusEAX\\Settings";
static int presetTop = -1;

static void settingsSave() {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
    auto put = [&](const wchar_t* n, int v) { DWORD d = (DWORD)v; RegSetValueExW(hk, n, 0, REG_DWORD, (const BYTE*)&d, sizeof d); };
    put(L"UiZoom", uiZoom);
    put(L"UiBold", uiBold ? 1 : 0);
    put(L"UiSize", uiSize);
    put(L"UiAnim", uiAnim ? 1 : 0);
    put(L"UiTop", uiTop ? 1 : 0);
    put(L"UiPlace", uiPlace ? 1 : 0);
    put(L"UiHints", uiHints ? 1 : 0);
    for (int a = 0; a < NUM_SHORTCUTS; ++a) put(SHORTCUTS[a].reg, (int)shortcutKey[a]);
    if (winX != NO_PLACE && winY != NO_PLACE) {
        put(L"WinX", winX);
        put(L"WinY", winY);
    }
    put(L"TakeFormat", takeFormat);
    put(L"RecSources", recFrom);
    put(L"KeepLast", keepSec);
    put(L"LastQuiet", lastQuiet ? 1 : 0);
    if (presetTop >= 0) put(L"PresetTop", presetTop);
    put(L"Isolation", g.isolate.load() ? 1 : 0);
    put(L"IsoMode", g.isoMode.load());
    put(L"IsoLevel", g.isoLevel.load());
    put(L"VocalLevel", g.gateLevel.load());
    put(L"GateLo", g.gateLo.load() + GATE_TONE_RANGE);
    put(L"GateMid", g.gateMid.load() + GATE_TONE_RANGE);
    put(L"GateHi", g.gateHi.load() + GATE_TONE_RANGE);
    put(L"GateOut", g.gateOut.load() + GATE_TONE_RANGE);
    put(L"ToneOn", g.toneOn.load() ? 1 : 0);
    put(L"ToneLo", g.toneLo.load() + TONE_SAVED_ZERO);
    put(L"ToneMid", g.toneMid.load() + TONE_SAVED_ZERO);
    put(L"ToneHi", g.toneHi.load() + TONE_SAVED_ZERO);
    put(L"TonePadX", g.tonePadX.load() + TONE_PAD_RANGE);
    put(L"TonePadY", g.tonePadY.load() + TONE_PAD_RANGE);
    put(L"ToneBaseLo", g.toneBaseLo.load() + TONE_SAVED_ZERO);
    put(L"ToneBaseMid", g.toneBaseMid.load() + TONE_SAVED_ZERO);
    put(L"ToneBaseHi", g.toneBaseHi.load() + TONE_SAVED_ZERO);
    put(L"ClipLevel", g.clipGain.load());
    put(L"ClipHear", g.clipHear.load() ? 1 : 0);
    put(L"ClipToMic", g.clipToMic.load() ? 1 : 0);
    put(L"AppHear", g.appHear.load() ? 1 : 0);
    put(L"AppOnly", g.appOnly.load() ? 1 : 0);
    put(L"EchoOn", g.echoOn.load());
    put(L"MaskOn", g.changeOn.load());
    put(L"PitchHz", g.pitchHz.load() + PITCH_SPAN);
    put(L"VoiceSize", g.voiceSize.load());
    put(L"VoiceMacro", g.voiceMacro.load() + 100);
    put(L"VoiceRatio", g.voiceRatio.load());
    put(L"VoiceMask", g.voiceMask.load() + 1);
    put(L"VoiceSpeed", g.voiceSpeed.load());
    put(L"GlitchStyle", g.glitchStyle.load());
    put(L"GlitchKey", g.glitchKey.load());
    put(L"VoiceKeepPitch", g.voiceKeepPitch.load() ? 1 : 0);
    put(L"ClipSpeed", g.clipSpeed.load());
    put(L"ClipKeepPitch", g.clipKeepPitch.load() ? 1 : 0);
    put(L"ClipFollowVoice", g.clipFollow.load() ? 1 : 0);
    put(L"AppGain", g.appGain.load());
    put(L"UiAlpha", uiAlpha);
    put(L"ThemeHue", look.theme.hue); put(L"ThemeSat", look.theme.sat); put(L"ThemeDepth", look.theme.depth);
    put(L"Accent", (int)look.accent); put(L"Text", (int)look.text); put(L"Dim", (int)look.dim); put(L"Panel", (int)look.panel);
    put(L"Back", (int)look.back); put(L"Edge", (int)look.edge); put(L"Hover", (int)look.hover); put(L"Off", (int)look.off);
    put(L"Clear", look.clear); put(L"Blur", look.blur); put(L"Darken", look.darken);
    if (!wallPath.empty())
        RegSetValueExW(hk, L"Wallpaper", 0, REG_SZ, (const BYTE*)wallPath.c_str(), (DWORD)((wallPath.size() + 1) * sizeof(wchar_t)));
    else
        RegSetValueExW(hk, L"Wallpaper", 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
    RegSetValueExW(hk, L"UiFont", 0, REG_SZ, (const BYTE*)uiFace.c_str(), (DWORD)((uiFace.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(hk, L"ClipsFolder", 0, REG_SZ, (const BYTE*)clipsDir.c_str(), (DWORD)((clipsDir.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
}
static bool settingsDirty = false;
static void settingsLater() { settingsDirty = true; }
static void settingsFlush() {
    if (!settingsDirty) return;
    settingsDirty = false;
    settingsSave();
}
static void settingsSoon(HWND h) {
    static ULONGLONG savedAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (GetCapture() == h || now - savedAt < 250) {
        settingsLater();
        return;
    }
    savedAt = now;
    settingsSave();
}
static void settingsLoad() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return;
    auto get = [&](const wchar_t* n, int lo, int hi, int def) {
        DWORD d = 0, sz = sizeof d, type = 0;
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)&d, &sz) != ERROR_SUCCESS || type != REG_DWORD) return def;
        int v = (int)d; return v < lo ? lo : (v > hi ? hi : v);
    };

    uiZoom = get(L"UiZoom", ZOOM_MIN, ZOOM_MAX, 100);
    uiBold = get(L"UiBold", 0, 1, 0) != 0;
    uiSize = get(L"UiSize", TEXT_SIZE_MIN, TEXT_SIZE_MAX, TEXT_SIZE_BASE);
    uiAnim = get(L"UiAnim", 0, 1, 1) != 0;
    uiTop = get(L"UiTop", 0, 1, 0) != 0;
    uiPlace = get(L"UiPlace", 0, 1, 1) != 0;
    uiHints = get(L"UiHints", 0, 1, 1) != 0;
    for (int a = 0; a < NUM_SHORTCUTS; ++a) {
        const DWORD k = (DWORD)get(SHORTCUTS[a].reg, 0, 0x7FFFFFFF, 0);
        shortcutKey[a] = keyUsable(k) ? k : 0;
        for (int b = 0; b < a; ++b)
            if (shortcutKey[a] && shortcutKey[b] == shortcutKey[a]) shortcutKey[a] = 0;
    }
    {
        wchar_t dir[MAX_PATH] = L"";
        DWORD sz = sizeof(dir) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(hk, L"ClipsFolder", nullptr, &type, (BYTE*)dir, &sz) == ERROR_SUCCESS && type == REG_SZ) clipsDir = dir;
    }
    winX = get(L"WinX", -32000, 32000, NO_PLACE);
    winY = get(L"WinY", -32000, 32000, NO_PLACE);
    if (winX == NO_PLACE || winY == NO_PLACE) winX = winY = NO_PLACE;
    takeFormat = get(L"TakeFormat", 0, clip::NUM_SAVE_FORMATS - 1, 0);
    const int oldRecFrom = get(L"RecordFrom", 0, 1, -1);
    recFrom = get(L"RecSources", FROM_VOICE, FROM_VOICE | FROM_HEAR, oldRecFrom == 1 ? FROM_HEAR : FROM_VOICE);
    keepSec = get(L"KeepLast", 0, KEEP_STEPS[NUM_KEEP_STEPS - 1], KEEP_DEFAULT_SEC);
    if (std::find(std::begin(KEEP_STEPS), std::end(KEEP_STEPS), keepSec) == std::end(KEEP_STEPS)) keepSec = KEEP_DEFAULT_SEC;
    lastQuiet = get(L"LastQuiet", 0, 1, 1) != 0;
    presetTop = get(L"PresetTop", 0, NUM_CARD_PRESETS - 1, -1);
    g.isoMode  = get(L"IsoMode", 0, ISO_MODES - 1, ISO_VOCAL);
    g.isoLevel = get(L"IsoLevel", 0, 100, ISO_DEFAULT);
    const int oldGateLevel = get(L"GateLevel", 0, 100, -1), oldGateDb = get(L"GateDb", 0, -GATE_MIN_DB, -1);
    g.gateLevel = get(L"VocalLevel", 0, 100, oldGateLevel >= 0 ? vocalLevelOfOld(oldGateLevel) : (oldGateDb >= 0 ? vocalLevelOfThreshold(oldGateDb + GATE_MIN_DB) : VOCAL_DEFAULT_LEVEL));
    const bool chainWasOn = get(L"MixOn", 0, 1, 0) != 0;
    g.gateLo = get(L"GateLo", 0, 2 * GATE_TONE_RANGE, GATE_DEFAULT_TONE + GATE_TONE_RANGE) - GATE_TONE_RANGE;
    g.gateMid = get(L"GateMid", 0, 2 * GATE_TONE_RANGE, GATE_DEFAULT_TONE + GATE_TONE_RANGE) - GATE_TONE_RANGE;
    g.gateHi = get(L"GateHi", 0, 2 * GATE_TONE_RANGE, GATE_DEFAULT_TONE + GATE_TONE_RANGE) - GATE_TONE_RANGE;
    g.gateOut = get(L"GateOut", 0, 2 * GATE_TONE_RANGE, GATE_DEFAULT_OUT + GATE_TONE_RANGE) - GATE_TONE_RANGE;
    g.toneOn = get(L"ToneOn", 0, 1, 1) != 0;
    g.toneLo = get(L"ToneLo", TONE_SAVED_ZERO - TONE_RANGE, TONE_SAVED_ZERO + TONE_RANGE, TONE_SAVED_ZERO) - TONE_SAVED_ZERO;
    g.toneMid = get(L"ToneMid", TONE_SAVED_ZERO - TONE_RANGE, TONE_SAVED_ZERO + TONE_RANGE, TONE_SAVED_ZERO) - TONE_SAVED_ZERO;
    g.toneHi = get(L"ToneHi", TONE_SAVED_ZERO - TONE_RANGE, TONE_SAVED_ZERO + TONE_RANGE, TONE_SAVED_ZERO) - TONE_SAVED_ZERO;
    g.tonePadX = get(L"TonePadX", 0, 2 * TONE_PAD_RANGE, TONE_PAD_RANGE) - TONE_PAD_RANGE;
    g.tonePadY = get(L"TonePadY", 0, 2 * TONE_PAD_RANGE, TONE_PAD_RANGE) - TONE_PAD_RANGE;
    g.toneBaseLo = get(L"ToneBaseLo", TONE_SAVED_ZERO - TONE_RANGE, TONE_SAVED_ZERO + TONE_RANGE, TONE_SAVED_ZERO) - TONE_SAVED_ZERO;
    g.toneBaseMid = get(L"ToneBaseMid", TONE_SAVED_ZERO - TONE_RANGE, TONE_SAVED_ZERO + TONE_RANGE, TONE_SAVED_ZERO) - TONE_SAVED_ZERO;
    g.toneBaseHi = get(L"ToneBaseHi", TONE_SAVED_ZERO - TONE_RANGE, TONE_SAVED_ZERO + TONE_RANGE, TONE_SAVED_ZERO) - TONE_SAVED_ZERO;
    toneSettle();
    g.toneVersion.fetch_add(1);
    g.clipGain = get(L"ClipLevel", 0, CLIP_MAX, clipLevelFromOld(get(L"ClipGain", 0, 100, -1)));
    g.isolate = get(L"Isolation", 0, 1, 0) != 0;
    if (chainWasOn) {
        g.isolate = true;
        g.isoMode = ISO_VOCAL;
    }
    g.clipHear = get(L"ClipHear", 0, 1, 1) != 0;
    g.clipToMic = get(L"ClipToMic", 0, 1, 1) != 0;
    g.appHear = get(L"AppHear", 0, 1, 0) != 0;
    g.appOnly = get(L"AppOnly", 0, 1, 0) != 0;
    g.echoOn = get(L"EchoOn", 0, ON_ALL, echoFromRoute(get(L"FxRoute", 0, 2, 2)));
    g.changeOn = get(L"MaskOn", 0, ON_ALL, ON_VOICE);
    g.pitchHz   = get(L"PitchHz", 0, 2 * PITCH_SPAN, PITCH_SPAN) - PITCH_SPAN;
    g.voiceSize = get(L"VoiceSize", VSIZE_MIN, VSIZE_MAX, 100);
    g.voiceMacro = get(L"VoiceMacro", 0, 200, 100) - 100;
    g.voiceRatio = get(L"VoiceRatio", 400, 2500, 1000);
    g.voiceMask = get(L"VoiceMask", 0, voice::NUM_MASKS, voice::MASK_NEUTRAL + 1) - 1;
    g.voiceSpeed = get(L"VoiceSpeed", SPEED_MIN, SPEED_MAX, 100);
    g.glitchStyle = get(L"GlitchStyle", 0, voice::NUM_GLITCH_STYLES - 1, voice::GLITCH_REPEAT);
    g.glitchKey = get(L"GlitchKey", 0, 0x0FFFFF, 0);
    g.voiceKeepPitch = get(L"VoiceKeepPitch", 0, 1, 1) != 0;
    g.clipSpeed = get(L"ClipSpeed", SPEED_MIN, SPEED_MAX, 100);
    g.clipKeepPitch = get(L"ClipKeepPitch", 0, 1, 1) != 0;
    g.clipFollow = get(L"ClipFollowVoice", 0, 1, 1) != 0;
    g.appGain    = get(L"AppGain", 0, APP_MAX, APP_GAIN_DEFAULT);
    uiAlpha = get(L"UiAlpha", 0, 100, 100);
    auto has = [&](const wchar_t* n) {
        DWORD type = 0;
        return RegQueryValueExW(hk, n, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS && type == REG_DWORD;
    };
    auto colour = [&](const wchar_t* n, COLORREF def) { return (COLORREF)get(n, 0, 0xFFFFFF, (int)def); };
    Theme theme = PURPLE_THEME;
    if (has(L"ThemeHue"))
        theme = Theme{get(L"ThemeHue", 0, 359, PURPLE_THEME.hue), get(L"ThemeSat", 0, 100, PURPLE_THEME.sat),
                      get(L"ThemeDepth", 0, 100, PURPLE_THEME.depth)};
    else if (has(L"Accent"))
        theme = themeFromColour(colour(L"Accent", 0), PURPLE_THEME.depth);
    else if (has(L"AccentR"))
        theme = themeFromColour(RGB(get(L"AccentR", 0, 255, 0), get(L"AccentG", 0, 255, 0), get(L"AccentB", 0, 255, 0)),
                                PURPLE_THEME.depth);
    themeColours(look, theme);
    if (has(L"ThemeHue")) {
        look.accent = colour(L"Accent", look.accent); look.text = colour(L"Text", look.text);
        look.dim = colour(L"Dim", look.dim); look.panel = colour(L"Panel", look.panel);
        look.back = colour(L"Back", look.back); look.edge = colour(L"Edge", look.edge);
        look.hover = colour(L"Hover", look.hover); look.off = colour(L"Off", look.off);
    }
    look.clear = get(L"Clear", 0, 100, CLEAR_DEFAULT); look.blur = get(L"Blur", 0, 100, BLUR_DEFAULT);
    look.darken = get(L"Darken", 0, 100, DARKEN_DEFAULT);
    {
        wchar_t wp[MAX_PATH] = L"";
        DWORD sz = sizeof(wp) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(hk, L"Wallpaper", nullptr, &type, (BYTE*)wp, &sz) == ERROR_SUCCESS && type == REG_SZ && wp[0])
            wallPath = wp;
    }
    {
        wchar_t face[LF_FACESIZE] = L"";
        DWORD sz = sizeof(face) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(hk, L"UiFont", nullptr, &type, (BYTE*)face, &sz) == ERROR_SUCCESS && type == REG_SZ)
            if (const std::wstring* known = knownFace(face)) uiFace = *known;
    }
    g.isoVersion.fetch_add(1);
    RegCloseKey(hk);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return;
    auto put = [&](const wchar_t* n, int v) { DWORD d = (DWORD)v; RegSetValueExW(hk, n, 0, REG_DWORD, (const BYTE*)&d, sizeof d); };
    put(L"VocalLevel", g.gateLevel.load());
    put(L"GateLo", g.gateLo.load() + GATE_TONE_RANGE);
    put(L"GateMid", g.gateMid.load() + GATE_TONE_RANGE);
    put(L"GateHi", g.gateHi.load() + GATE_TONE_RANGE);
    put(L"GateOut", g.gateOut.load() + GATE_TONE_RANGE);
    if (chainWasOn) {
        put(L"Isolation", 1);
        put(L"IsoMode", ISO_VOCAL);
    }
    for (const wchar_t* gone : {L"MixOn", L"GateOn", L"GateDb", L"GateLevel", L"ToneOut", L"RecordFrom"}) RegDeleteValueW(hk, gone);
    RegCloseKey(hk);
}

static void presetTopKeep() {
    if (!hPreset) return;
    const int top = (int)SendMessageW(hPreset, LB_GETTOPINDEX, 0, 0);
    if (top == presetTop) return;
    presetTop = top;
    settingsSave();
}

static void presetTopPlace() {
    const int most = std::max(0, NUM_CARD_PRESETS - PRESET_H / 22);
    const int top = presetTop >= 0 ? std::min(presetTop, most) : most / 2;
    SendMessageW(hPreset, LB_SETTOPINDEX, top, 0);
    presetTop = -1;
    presetTopKeep();
}

static bool runHidden(std::wstring cmd, DWORD* code) {
    STARTUPINFOW si = {}; si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD ec = 1; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (code) *code = ec;
    return true;
}
static std::wstring schtasksPath() {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(L"\"") + sys + L"\\schtasks.exe\"";
}

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"OctopusEAX";

static bool oldStartupTaskOn(const wchar_t* task) {
    DWORD ec = 1;
    if (!runHidden(schtasksPath() + L" /Query /TN \"" + task + L"\"", &ec)) return false;
    return ec == 0;
}
static const UINT WM_STARTUP_OLD = WM_APP + 41;
static const UINT WM_SETUPS_FOUND = WM_APP + 43;

static bool startupIsOn() {
    HKEY hk;
    bool on = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        DWORD type = 0, cb = 0;
        on = RegQueryValueExW(hk, RUN_VALUE, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS && type == REG_SZ;
        RegCloseKey(hk);
    }
    return on;
}
static void oldStartupTasks(HWND owner, bool drop) {
    std::thread([owner, drop] {
        bool found = false;
        for (const wchar_t* task : {L"OctopusEAX", L"MasonEAX"}) {
            if (!oldStartupTaskOn(task)) continue;
            found = true;
            DWORD ec = 1;
            if (drop) runHidden(schtasksPath() + L" /Delete /TN \"" + task + L"\" /F", &ec);
        }
        if (found && !drop) PostMessageW(owner, WM_STARTUP_OLD, 0, 0);
    }).detach();
}
static bool startupSet(bool on) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return false;
    bool ok;
    if (on) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        ok = RegSetValueExW(hk, RUN_VALUE, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        const LONG rc = RegDeleteValueW(hk, RUN_VALUE);
        ok = rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(hk);
    return ok;
}
static void startupFollow() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return;
    wchar_t cmd[MAX_PATH * 2] = L"";
    DWORD sz = sizeof cmd - sizeof(wchar_t), type = 0;
    const bool on = RegQueryValueExW(hk, RUN_VALUE, nullptr, &type, (BYTE*)cmd, &sz) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(hk);
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (on && _wcsicmp(cmd, (L"\"" + std::wstring(exe) + L"\"").c_str()) != 0) startupSet(true);
}
static bool settingsKept() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return false;
    RegCloseKey(hk);
    return true;
}

static const RECT R_SAVE = {WIN_W - 222, 5, WIN_W - 134, 29};
static ULONGLONG savedAt = 0;
static int faderValue(const RECT& r, int x, int maxValue);

static std::wstring iniPath() { return exeFolder() + L"\\OctopusEAX.ini"; }
static bool settingsFileSave(const std::wstring& path) {
    const std::wstring temp = path + L".part";
    FILE* f = _wfopen(temp.c_str(), L"wb");
    if (!f) return false;
    std::string mic, spk;
    { std::lock_guard<std::mutex> lk(g.devMx); mic = clip::narrow(g.micId); spk = clip::narrow(g.spkId); }
    fprintf(f, "# OctopusEAX settings\n");
    fprintf(f, "mic=%s\nspk=%s\n", mic.c_str(), spk.c_str());
    fprintf(f, "preset=%d\namount=%d\nmic_level=%d\n", g.preset.load(), g.amount.load(), g.micLevel.load());
    fprintf(f, "effects=%d\necho_on=%d\nmask_on=%d\nmonitor=%d\nwide=%d\nraw=%d\ncmss=%d\ncard_mic=%d\n", g.enabled.load() ? 1 : 0, g.echoOn.load(), g.changeOn.load(), g.monitor.load() ? 1 : 0,
            g.wide.load() ? 1 : 0, g.rawMic.load() ? 1 : 0, g.cmss.load() ? 1 : 0, g.cardMic.load() ? 1 : 0);
    fprintf(f, "isolate=%d\niso_mode=%d\niso_level=%d\n", g.isolate.load() ? 1 : 0, g.isoMode.load(), g.isoLevel.load());
    fprintf(f, "vocal_level=%d\ngate_lo=%d\ngate_mid=%d\ngate_hi=%d\ngate_out=%d\n", g.gateLevel.load(), g.gateLo.load(), g.gateMid.load(), g.gateHi.load(), g.gateOut.load());
    fprintf(f, "tone_on=%d\ntone_lo=%d\ntone_mid=%d\ntone_hi=%d\ntone_pad_x=%d\ntone_pad_y=%d\ntone_base_lo=%d\ntone_base_mid=%d\ntone_base_hi=%d\n", g.toneOn.load() ? 1 : 0,
            g.toneLo.load(), g.toneMid.load(), g.toneHi.load(), g.tonePadX.load(), g.tonePadY.load(), g.toneBaseLo.load(), g.toneBaseMid.load(), g.toneBaseHi.load());
    fprintf(f, "clip_level=%d\nclip_hear=%d\nclip_mic=%d\napp_hear=%d\napp_only=%d\n", g.clipGain.load(), g.clipHear.load() ? 1 : 0, g.clipToMic.load() ? 1 : 0, g.appHear.load() ? 1 : 0,
            g.appOnly.load() ? 1 : 0);
    fprintf(f, "pitch_hz=%d\nvoice_size=%d\nvoice_macro=%d\nvoice_ratio=%d\nvoice_mask=%d\napp_gain=%d\n", g.pitchHz.load(), g.voiceSize.load(), g.voiceMacro.load(), g.voiceRatio.load(), g.voiceMask.load(), g.appGain.load());
    fprintf(f, "voice_speed=%d\nvoice_keep_pitch=%d\nclip_speed=%d\nclip_keep_pitch=%d\nclip_follow_voice=%d\n", g.voiceSpeed.load(), g.voiceKeepPitch.load() ? 1 : 0,
            g.clipSpeed.load(), g.clipKeepPitch.load() ? 1 : 0, g.clipFollow.load() ? 1 : 0);
    fprintf(f, "glitch_style=%d\n", g.glitchStyle.load());
    fprintf(f, "theme_hue=%d\ntheme_sat=%d\ntheme_depth=%d\n", look.theme.hue, look.theme.sat, look.theme.depth);
    fprintf(f, "accent=%u\ntext=%u\ndim=%u\npanel=%u\nback=%u\nedge=%u\nhover=%u\noff=%u\n", (unsigned)look.accent, (unsigned)look.text,
            (unsigned)look.dim, (unsigned)look.panel, (unsigned)look.back, (unsigned)look.edge, (unsigned)look.hover, (unsigned)look.off);
    fprintf(f, "wallpaper=%s\nalpha=%d\nclear=%d\nblur=%d\ndarken=%d\n", clip::narrow(wallPath).c_str(), uiAlpha, look.clear, look.blur, look.darken);
    fprintf(f, "font=%s\nfont_bold=%d\nfont_size=%d\n", clip::narrow(uiFace).c_str(), uiBold ? 1 : 0, uiSize);
    fprintf(f, "eq_on=%d\neq_preset=%d\neq_level=%d\neq_bands=", g.eqOn.load() ? 1 : 0, g.eqPreset.load(), g.eqLevel.load());
    for (int k = 0; k < eqb::EQ_BANDS; ++k) fprintf(f, "%s%d", k ? "," : "", g.eqDb[k].load());
    fprintf(f, "\n");
    const bool written = !ferror(f);
    if (fclose(f) != 0 || !written || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}
static void settingsFileLoad(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    char line[2048];
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    bool eqTouched = false;
    std::wstring pendingWall;
    bool hadWall = false;
    std::wstring pendingFace = uiFace;
    bool pendingBold = uiBold;
    int pendingSize = uiSize;
    int fileHue = -1, fileSat = -1, fileDepth = -1;
    COLORREF fileCol[8] = {};
    bool fileHas[8] = {};
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        int iv = atoi(v.c_str());
        if (k == "mic" || k == "spk") {
            std::wstring id = clip::widen(v);
            auto& list = (k == "mic") ? uiMics : uiSpks;
            for (size_t i = 0; i < list.size(); ++i) if (list[i].id == id) {
                SendMessageW(k == "mic" ? hMic : hSpk, CB_SETCURSEL, (WPARAM)i, 0);
                std::lock_guard<std::mutex> lk(g.devMx);
                if (k == "mic") g.micId = id; else g.spkId = id;
            }
        }
        else if (k == "preset") { iv = clampi(iv, 0, NUM_CARD_PRESETS - 1); g.preset = iv; SendMessageW(hPreset, LB_SETCURSEL, iv, 0); }
        else if (k == "amount")    g.amount = clampi(iv, 0, 100);
        else if (k == "mic_level") g.micLevel = clampi(iv, 0, MIC_MAX);
        else if (k == "effects")   g.enabled = iv != 0;
        else if (k == "fx_route")  g.echoOn = echoFromRoute(clampi(iv, 0, 2));
        else if (k == "echo_on")   g.echoOn = clampi(iv, 0, ON_ALL);
        else if (k == "mask_on")   g.changeOn = clampi(iv, 0, ON_ALL);
        else if (k == "monitor")   g.monitor = iv != 0;
        else if (k == "wide")      g.wide = iv != 0;
        else if (k == "isolate")   { g.isolate = iv != 0; g.isoVersion.fetch_add(1); }
        else if (k == "raw")       g.rawMic = iv != 0;
        else if (k == "card_mic")  g.cardMic = iv != 0;
        else if (k == "cmss")      g.cmss = iv != 0;
        else if (k == "iso_mode")  { g.isoMode = clampi(iv, 0, ISO_MODES - 1); g.isoVersion.fetch_add(1); }
        else if (k == "iso_level") { g.isoLevel = clampi(iv, 0, 100); g.isoVersion.fetch_add(1); }
        else if (k == "mix_on" && iv) { g.isolate = true; g.isoMode = ISO_VOCAL; g.isoVersion.fetch_add(1); }
        else if (k == "gate_db")   { g.gateLevel = vocalLevelOfThreshold(iv); g.isoVersion.fetch_add(1); }
        else if (k == "gate_level") { g.gateLevel = vocalLevelOfOld(iv); g.isoVersion.fetch_add(1); }
        else if (k == "vocal_level") { g.gateLevel = clampi(iv, 0, 100); g.isoVersion.fetch_add(1); }
        else if (k == "gate_lo")   { g.gateLo = clampi(iv, -GATE_TONE_RANGE, GATE_TONE_RANGE); g.isoVersion.fetch_add(1); }
        else if (k == "gate_mid")  { g.gateMid = clampi(iv, -GATE_TONE_RANGE, GATE_TONE_RANGE); g.isoVersion.fetch_add(1); }
        else if (k == "gate_hi")   { g.gateHi = clampi(iv, -GATE_TONE_RANGE, GATE_TONE_RANGE); g.isoVersion.fetch_add(1); }
        else if (k == "gate_out")  { g.gateOut = clampi(iv, -GATE_TONE_RANGE, GATE_TONE_RANGE); g.isoVersion.fetch_add(1); }
        else if (k == "tone_on")   { g.toneOn = iv != 0; g.toneVersion.fetch_add(1); }
        else if (k == "tone_lo")   { g.toneLo = clampi(iv, -TONE_RANGE, TONE_RANGE); g.toneVersion.fetch_add(1); }
        else if (k == "tone_mid")  { g.toneMid = clampi(iv, -TONE_RANGE, TONE_RANGE); g.toneVersion.fetch_add(1); }
        else if (k == "tone_hi")   { g.toneHi = clampi(iv, -TONE_RANGE, TONE_RANGE); g.toneVersion.fetch_add(1); }
        else if (k == "tone_pad_x") g.tonePadX = clampi(iv, -TONE_PAD_RANGE, TONE_PAD_RANGE);
        else if (k == "tone_pad_y") g.tonePadY = clampi(iv, -TONE_PAD_RANGE, TONE_PAD_RANGE);
        else if (k == "tone_base_lo")  g.toneBaseLo = clampi(iv, -TONE_RANGE, TONE_RANGE);
        else if (k == "tone_base_mid") g.toneBaseMid = clampi(iv, -TONE_RANGE, TONE_RANGE);
        else if (k == "tone_base_hi")  g.toneBaseHi = clampi(iv, -TONE_RANGE, TONE_RANGE);
        else if (k == "clip_level") g.clipGain = clampi(iv, 0, CLIP_MAX);
        else if (k == "clip_gain") g.clipGain = clipLevelFromOld(clampi(iv, 0, 100));
        else if (k == "clip_hear") g.clipHear = iv != 0;
        else if (k == "clip_mic")  g.clipToMic = iv != 0;
        else if (k == "app_hear")  g.appHear = iv != 0;
        else if (k == "app_only")  g.appOnly = iv != 0;
        else if (k == "pitch_hz")  g.pitchHz = clampi(iv, -PITCH_SPAN, PITCH_SPAN);
        else if (k == "voice_size") g.voiceSize = clampi(iv, VSIZE_MIN, VSIZE_MAX);
        else if (k == "voice_macro") g.voiceMacro = clampi(iv, -100, 100);
        else if (k == "voice_ratio") g.voiceRatio = clampi(iv, 400, 2500);
        else if (k == "voice_mask")  g.voiceMask = clampi(iv, -1, voice::NUM_MASKS - 1);
        else if (k == "app_gain")  g.appGain = clampi(iv, 0, APP_MAX);
        else if (k == "voice_speed") g.voiceSpeed = clampi(iv, SPEED_MIN, SPEED_MAX);
        else if (k == "glitch_style") g.glitchStyle = clampi(iv, 0, voice::NUM_GLITCH_STYLES - 1);
        else if (k == "voice_keep_pitch") g.voiceKeepPitch = iv != 0;
        else if (k == "clip_speed")  g.clipSpeed = clampi(iv, SPEED_MIN, SPEED_MAX);
        else if (k == "clip_keep_pitch") g.clipKeepPitch = iv != 0;
        else if (k == "clip_follow_voice") g.clipFollow = iv != 0;
        else if (k == "theme_hue")   fileHue = clampi(iv, 0, 359);
        else if (k == "theme_sat")   fileSat = clampi(iv, 0, 100);
        else if (k == "theme_depth") fileDepth = clampi(iv, 0, 100);
        else if (lookKeyIndex(k) >= 0) {
            fileCol[lookKeyIndex(k)] = (COLORREF)clampi(iv, 0, 0xFFFFFF);
            fileHas[lookKeyIndex(k)] = true;
        }
        else if (k == "alpha")     uiAlpha = clampi(iv, 0, 100);
        else if (k == "clear")     look.clear = clampi(iv, 0, 100);
        else if (k == "blur")      look.blur = clampi(iv, 0, 100);
        else if (k == "darken")    look.darken = clampi(iv, 0, 100);
        else if (k == "wallpaper") {
            pendingWall = clip::widen(v);
            hadWall = true;
        }
        else if (k == "font")      pendingFace = clip::widen(v);
        else if (k == "font_bold") pendingBold = iv != 0;
        else if (k == "font_size") pendingSize = clampi(iv, TEXT_SIZE_MIN, TEXT_SIZE_MAX);
        else if (k == "eq_on")     { g.eqOn = iv != 0; eqTouched = true; }
        else if (k == "eq_preset") { g.eqPreset = clampi(iv, 0, eqPresetCount() - 1); eqTouched = true; }
        else if (k == "eq_level")  { g.eqLevel = clampi(iv, 0, eqb::EQ_LEVEL_RANGE); eqTouched = true; }
        else if (k == "eq_bands") {
            int band[eqb::EQ_BANDS] = {};
            size_t p = 0;
            for (int b = 0; b < eqb::EQ_BANDS && p <= v.size(); ++b) {
                size_t c = v.find(',', p); std::string t = v.substr(p, c == std::string::npos ? std::string::npos : c - p);
                band[b] = clampi(atoi(t.c_str()), -eqb::EQ_RANGE, eqb::EQ_RANGE);
                if (c == std::string::npos) break;
                p = c + 1;
            }
            for (int b = 0; b < eqb::EQ_BANDS; ++b) g.eqDb[b] = band[b];
            eqTouched = true;
        }
    }
    fclose(f);
    if (hadWall) wallPath = pendingWall;
    if (fileHue >= 0 || fileHas[0]) {
        const Theme t = fileHue >= 0
            ? Theme{fileHue, fileSat >= 0 ? fileSat : PURPLE_THEME.sat, fileDepth >= 0 ? fileDepth : PURPLE_THEME.depth}
            : themeFromColour(fileCol[0], look.theme.depth);
        themeColours(look, t);
        if (fileHue >= 0)
            for (int i = 0; i < 8; ++i)
                if (fileHas[i]) lookSlot(i) = fileCol[i];
    }
    buildBackground();
    applyLook();
    const std::wstring* known = knownFace(pendingFace);
    setUiType(known ? *known : uiFace, pendingBold, pendingSize);
    if (eqTouched) {
        g.eqVersion.fetch_add(1);
        eqSave();
        if (hEqBox) {
            SendMessageW(hEqBox, CB_SETCURSEL, g.eqPreset.load(), 0);
            InvalidateRect(hEqBox, nullptr, FALSE);
        }
    }
    toneSettle();
    g.toneVersion.fetch_add(1);
    if (hTone) InvalidateRect(hTone, nullptr, FALSE);
    showMask();
    showGlitch();
    settingsSave();
    applyWindowAlpha(hwndMain);
}

static std::wstring curSetup;
static const RECT R_SETUP_PREV = {WIN_W - 404, 5, WIN_W - 378, 29};
static const RECT R_BOARD = {WIN_W - 530, 5, WIN_W - 414, 29};
static const RECT R_TONE  = {WIN_W - 656, 5, WIN_W - 540, 29};
static const RECT R_SETUP_NAME = {WIN_W - 374, 5, WIN_W - 256, 29};
static const RECT R_SETUP_NEXT = {WIN_W - 252, 5, WIN_W - 226, 29};
static const RECT R_SETUP_DEL  = {WIN_W - 130, 5, WIN_W - 104, 29};
static std::wstring dataDir() {
    static const std::wstring dir = [] {
        wchar_t app[MAX_PATH] = {0};
        const DWORD n = GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH);
        if (!n || n >= MAX_PATH) return exeFolder();
        const std::wstring mine = std::wstring(app) + L"\\OctopusEAX";
        CreateDirectoryW(mine.c_str(), nullptr);
        return mine;
    }();
    return dir;
}
static std::wstring setupsDir() {
    const std::wstring dir = dataDir() + L"\\setups";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
static std::vector<std::wstring> regList(const wchar_t* name) {
    std::vector<std::wstring> items;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return items;
    std::vector<wchar_t> buf(16384, L'\0');
    DWORD type = 0, size = (DWORD)((buf.size() - 2) * sizeof(wchar_t));
    if (RegQueryValueExW(hk, name, nullptr, &type, (BYTE*)buf.data(), &size) == ERROR_SUCCESS && type == REG_MULTI_SZ)
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) items.push_back(p);
    RegCloseKey(hk);
    return items;
}
static void regListSet(const wchar_t* name, const std::vector<std::wstring>& items) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
    std::wstring list;
    for (const std::wstring& t : items) {
        list += t;
        list.push_back(L'\0');
    }
    list.push_back(L'\0');
    RegSetValueExW(hk, name, 0, REG_MULTI_SZ, (const BYTE*)list.data(), (DWORD)(list.size() * sizeof(wchar_t)));
    RegCloseKey(hk);
}
static void rememberCopyFolder(const std::wstring& folder) {
    if (folder.empty()) return;
    std::vector<std::wstring> all = regList(L"CopyFolders");
    for (const std::wstring& f : all)
        if (_wcsicmp(f.c_str(), folder.c_str()) == 0) return;
    all.push_back(folder);
    if (all.size() > 32) all.erase(all.begin(), all.end() - 32);
    regListSet(L"CopyFolders", all);
}
static std::wstring runFolder() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return L"";
    wchar_t cmd[MAX_PATH * 2] = L"";
    DWORD sz = sizeof cmd - sizeof(wchar_t), type = 0;
    const bool on = RegQueryValueExW(hk, RUN_VALUE, nullptr, &type, (BYTE*)cmd, &sz) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(hk);
    if (!on) return L"";
    std::wstring exe = cmd;
    if (!exe.empty() && exe[0] == L'"') exe = exe.substr(1, exe.find(L'"', 1) == std::wstring::npos ? std::wstring::npos : exe.find(L'"', 1) - 1);
    const size_t slash = exe.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : exe.substr(0, slash);
}
static bool programFile(const wchar_t* name) {
    const size_t n = wcslen(name);
    return n > 4 && _wcsicmp(name + n - 4, L".exe") == 0 && (_wcsnicmp(name, L"OctopusEAX", 10) == 0 || _wcsnicmp(name, L"MasonEAX", 8) == 0);
}
static bool setupFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char head[24] = {};
    const size_t got = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    return got >= 19 && (strncmp(head, "# OctopusEAX settings", 21) == 0 || strncmp(head, "# MasonEAX settings", 19) == 0);
}
static std::vector<std::wstring> setupSearchRoots() {
    std::vector<std::wstring> roots;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &p)) && p) roots.push_back(p);
        CoTaskMemFree(p);
    }
    return roots;
}
static std::vector<std::wstring> scanSetupRoots() {
    struct Place { std::wstring dir; int depth; };
    std::vector<std::wstring> found;
    const ULONGLONG until = GetTickCount64() + 3000;
    for (const std::wstring& root : setupSearchRoots()) {
        std::vector<Place> queue{{root, 3}};
        for (size_t at = 0; at < queue.size() && at < 1500 && GetTickCount64() < until; ++at) {
            const Place here = queue[at];
            WIN32_FIND_DATAW fd;
            const HANDLE h = FindFirstFileExW((here.dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;
            bool program = false, holder = false;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                    if (_wcsicmp(fd.cFileName, L"setups") == 0) holder = true;
                    else if (here.depth > 0) queue.push_back({here.dir + L"\\" + fd.cFileName, here.depth - 1});
                } else if (programFile(fd.cFileName)) {
                    program = true;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            if (program && holder) found.push_back(here.dir + L"\\setups");
        }
    }
    return found;
}
static std::vector<std::wstring> otherSetupDirs;
static void addSetupDirs(const std::vector<std::wstring>& dirs) {
    const std::wstring shared = setupsDir();
    for (const std::wstring& d : dirs) {
        bool seen = _wcsicmp(d.c_str(), shared.c_str()) == 0;
        for (const std::wstring& o : otherSetupDirs) seen = seen || _wcsicmp(o.c_str(), d.c_str()) == 0;
        if (!seen) otherSetupDirs.push_back(d);
    }
}
static void addCopySetupDirs() {
    std::vector<std::wstring> dirs;
    for (const std::wstring& f : regList(L"CopyFolders")) {
        const std::wstring d = f + L"\\setups";
        const DWORD a = GetFileAttributesW(d.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) dirs.push_back(d);
    }
    addSetupDirs(dirs);
}
static std::vector<std::wstring> iniNames(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((dir + L"\\*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        const size_t n = wcslen(fd.cFileName);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n > 4 && _wcsicmp(fd.cFileName + n - 4, L".ini") == 0) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return names;
}
static void copyIfNewer(const std::wstring& from, const std::wstring& to) {
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExW(from.c_str(), GetFileExInfoStandard, &a)) return;
    if (GetFileAttributesExW(to.c_str(), GetFileExInfoStandard, &b) && CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) <= 0) return;
    CopyFileW(from.c_str(), to.c_str(), FALSE);
}
static void syncSetups() {
    const std::wstring shared = setupsDir();
    for (const std::wstring& d : otherSetupDirs)
        for (const std::wstring& n : iniNames(d))
            if (setupFile(d + L"\\" + n)) copyIfNewer(d + L"\\" + n, shared + L"\\" + n);
    const std::vector<std::wstring> mine = iniNames(shared);
    for (const std::wstring& d : otherSetupDirs)
        for (const std::wstring& n : mine) copyIfNewer(shared + L"\\" + n, d + L"\\" + n);
}
static std::wstring setupPath(const std::wstring& name) { return setupsDir() + L"\\" + name + L".ini"; }
static std::wstring cleanName(std::wstring n) {
    std::wstring out;
    for (wchar_t c : n) if (!wcschr(L"\\/:*?\"<>|", c) && c >= 32) out += c;
    while (!out.empty() && out.back() == L' ') out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    if (out.size() > 40) out.resize(40);
    for (const wchar_t* held : {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
                                L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"})
        if (_wcsicmp(out.c_str(), held) == 0) out += L'_';
    return out;
}
static std::vector<std::wstring> setupList() {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((setupsDir() + L"\\*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        std::wstring n = fd.cFileName;
        if (n.size() > 4) names.push_back(n.substr(0, n.size() - 4));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}
static void rememberSetup(const std::wstring& name) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(hk, L"LastSetup", 0, REG_SZ, (const BYTE*)name.c_str(), (DWORD)((name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
}
static std::wstring lastSetup() {
    HKEY hk; wchar_t v[256] = {0}; DWORD cb = sizeof(v) - sizeof(wchar_t), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return L"";
    bool ok = RegQueryValueExW(hk, L"LastSetup", nullptr, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(hk);
    return ok ? std::wstring(v) : L"";
}

static HWND hPromptEdit = nullptr;
static int promptResult = 0;
static const int PR_W = 346, PR_H = 146;
static const RECT R_PR_CLOSE = {PR_W - 34, 5, PR_W - 8, 29}, R_PR_BOX = {24, 58, PR_W - 24, 86};
static const RECT R_PR_OK = {PR_W - 158, 98, PR_W - 96, 124}, R_PR_CANCEL = {PR_W - 86, 98, PR_W - 24, 124};
static bool hotPrOk = false, hotPrCancel = false, hotPrClose = false;
static void paintPrompt(HDC dc, const RECT& rc) {
    boxTitle(dc, rc, L"Setup name", R_PR_CLOSE, hotPrClose);
    panel(dc, RECT{12, 46, rc.right - 12, rc.bottom - 12}, C_PANEL, C_EDGE);
    panel(dc, R_PR_BOX, C_BACK, C_EDGE);
    primaryButton(dc, R_PR_OK, ICO_NONE, L"OK", hotPrOk);
    button(dc, R_PR_CANCEL, L"Cancel", hotPrCancel);
    boxEdge(dc);
}
static LRESULT CALLBACK promptProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        hPromptEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    sc(R_PR_BOX.left + 2), sc(R_PR_BOX.top + 2), sc(R_PR_BOX.right - R_PR_BOX.left - 4), sc(R_PR_BOX.bottom - R_PR_BOX.top - 4), h, (HMENU)(INT_PTR)1, nullptr, nullptr);
        SendMessageW(hPromptEdit, WM_SETFONT, (WPARAM)fntCtl, TRUE);
        SendMessageW(hPromptEdit, EM_SETLIMITTEXT, 40, 0);
        noTheme(hPromptEdit);
        return 0;
    case WM_CTLCOLOREDIT: {
        SetTextColor((HDC)w, C_TEXT); SetBkColor((HDC)w, C_BACK);
        if (!brLookBack) lookBrushes();
        return (LRESULT)brLookBack;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        printBox(h, (HDC)w, paintPrompt);
        return 0;
    case WM_PAINT:
        paintBox(h, paintPrompt);
        return 0;
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        bool a = PtInRect(&R_PR_OK, p) != 0, b = PtInRect(&R_PR_CANCEL, p) != 0, c = PtInRect(&R_PR_CLOSE, p) != 0;
        if (a != hotPrOk || b != hotPrCancel || c != hotPrClose) { hotPrOk = a; hotPrCancel = b; hotPrClose = c; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (PtInRect(&R_PR_OK, p)) promptResult = 1;
        else if (PtInRect(&R_PR_CANCEL, p) || PtInRect(&R_PR_CLOSE, p)) promptResult = 2;
        else if (p.y < TITLE_H) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        return 0;
    }
    case WM_CLOSE: promptResult = 2; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static std::wstring askSetupName(HWND owner, const std::wstring& initial) {
    RECT o; GetWindowRect(owner, &o);
    const int W = PR_W, H = PR_H;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"OctopusPrompt", L"OctopusEAX", WS_POPUP,
                             (o.left + o.right - sc(W)) / 2, (o.top + o.bottom - sc(H)) / 2, sc(W), sc(H),
                             owner, nullptr, nullptr, nullptr);
    if (!h) return L"";
    promptResult = 0; hotPrOk = hotPrCancel = hotPrClose = false;
    SetWindowTextW(hPromptEdit, initial.c_str());
    EnableWindow(owner, FALSE);
    boxShow(h);
    SetFocus(hPromptEdit);
    SendMessageW(hPromptEdit, EM_SETSEL, 0, -1);
    MSG msg;
    while (promptResult == 0 && modalNext(msg)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { promptResult = 1; continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { promptResult = 2; continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    wchar_t buf[64] = {0};
    GetWindowTextW(hPromptEdit, buf, 64);
    EnableWindow(owner, TRUE);
    boxClose(h);
    hPromptEdit = nullptr;
    SetForegroundWindow(owner);
    return promptResult == 1 ? cleanName(buf) : L"";
}

static float colH = 0.1f, colS = 0.83f, colV = 0.94f;
static int colPickResult = 0;
static bool colDragSv = false, colDragHue = false;
static bool hotColOk = false, hotColCancel = false, hotColClose = false;
static const int CP_W = 388, CP_H = 262;
static const RECT R_COL_CLOSE  = {CP_W - 34, 5, CP_W - 8, 29};
static const RECT R_COL_SV     = {24, 58, 240, 238};
static const RECT R_COL_HUE    = {252, 58, 272, 238};
static const RECT R_COL_PREV   = {288, 58, CP_W - 24, 114};
static const RECT R_COL_OK     = {288, 176, CP_W - 24, 202};
static const RECT R_COL_CANCEL = {288, 212, CP_W - 24, 238};

static COLORREF hsvRgb(float h, float s, float v) {
    h = h - floorf(h);
    if (h < 0) h += 1;
    s = s < 0 ? 0 : (s > 1 ? 1 : s);
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    const float c = v * s;
    const float x = c * (1.f - fabsf(fmodf(h * 6.f, 2.f) - 1.f));
    const float m = v - c;
    float r = 0, gv = 0, b = 0;
    const int i = ((int)floorf(h * 6.f) % 6 + 6) % 6;
    if (i == 0) { r = c; gv = x; }
    else if (i == 1) { r = x; gv = c; }
    else if (i == 2) { gv = c; b = x; }
    else if (i == 3) { gv = x; b = c; }
    else if (i == 4) { r = x; b = c; }
    else { r = c; b = x; }
    return RGB((BYTE)((r + m) * 255.f + 0.5f), (BYTE)((gv + m) * 255.f + 0.5f), (BYTE)((b + m) * 255.f + 0.5f));
}

static void rgbHsv(COLORREF c, float& h, float& s, float& v) {
    const float r = GetRValue(c) / 255.f, gv = GetGValue(c) / 255.f, b = GetBValue(c) / 255.f;
    const float mx = max(r, max(gv, b)), mn = min(r, min(gv, b)), d = mx - mn;
    v = mx;
    s = mx > 1e-6f ? d / mx : 0.f;
    if (d < 1e-6f) h = 0.f;
    else if (mx == r) h = fmodf((gv - b) / d + 6.f, 6.f) / 6.f;
    else if (mx == gv) h = ((b - r) / d + 2.f) / 6.f;
    else h = ((r - gv) / d + 4.f) / 6.f;
}

static void colFromPt(POINT p, bool hue) {
    if (hue) {
        const int span = R_COL_HUE.bottom - R_COL_HUE.top;
        colH = span > 0 ? (float)(p.y - R_COL_HUE.top) / (float)span : 0;
        if (colH < 0) colH = 0; if (colH > 1) colH = 1;
    } else {
        const int sw = R_COL_SV.right - R_COL_SV.left, sh = R_COL_SV.bottom - R_COL_SV.top;
        colS = sw > 0 ? (float)(p.x - R_COL_SV.left) / (float)sw : 0;
        colV = sh > 0 ? 1.f - (float)(p.y - R_COL_SV.top) / (float)sh : 1;
        if (colS < 0) colS = 0; if (colS > 1) colS = 1;
        if (colV < 0) colV = 0; if (colV > 1) colV = 1;
    }
}

template <typename Shade>
static void paintField(HDC dc, const RECT& d, Shade shade) {
    const int w = d.right - d.left, h = d.bottom - d.top;
    if (w < 1 || h < 1) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP hb = mem ? CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (hb && bits) {
        uint32_t* px = (uint32_t*)bits;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const COLORREF c = shade(x, y, w, h);
                px[(size_t)y * w + x] = 0xFF000000u | (uint32_t)GetRValue(c) << 16 | (uint32_t)GetGValue(c) << 8 | GetBValue(c);
            }
        HGDIOBJ old = SelectObject(mem, hb);
        BitBlt(dc, d.left, d.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    }
    if (hb) DeleteObject(hb);
    if (mem) DeleteDC(mem);
}

static void drawColField(HDC dc) {
    {
        PixelSnap px(dc);
        const RECT sv = px.map(R_COL_SV), hue = px.map(R_COL_HUE);
        paintField(dc, sv, [](int x, int y, int w, int h) {
            return hsvRgb(colH, w > 1 ? (float)x / (float)(w - 1) : 0.f, h > 1 ? 1.f - (float)y / (float)(h - 1) : 1.f);
        });
        paintField(dc, hue, [](int, int y, int, int h) { return hsvRgb(h > 1 ? (float)y / (float)(h - 1) : 0.f, 1.f, 1.f); });
        edgeLines(dc, sv, px.line(), C_EDGE);
        edgeLines(dc, hue, px.line(), C_EDGE);
    }
    const int w = R_COL_SV.right - R_COL_SV.left;
    const int h = R_COL_SV.bottom - R_COL_SV.top;
    const int cx = R_COL_SV.left + (int)(colS * (w - 1));
    const int cy = R_COL_SV.top + (int)((1.f - colV) * (h - 1));
    HPEN wp = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(dc, wp);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 6, cy - 6, cx + 7, cy + 7);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(wp);

    const int hh = R_COL_HUE.bottom - R_COL_HUE.top;
    const int hy = R_COL_HUE.top + (int)(colH * (hh - 1));
    HPEN gp = CreatePen(PS_SOLID, 2, C_TEXT);
    op = SelectObject(dc, gp);
    MoveToEx(dc, R_COL_HUE.left - 3, hy, nullptr);
    LineTo(dc, R_COL_HUE.right + 3, hy);
    SelectObject(dc, op);
    DeleteObject(gp);
}

static void paintColor(HDC dc, const RECT& rc) {
    boxTitle(dc, rc, L"Color", R_COL_CLOSE, hotColClose);
    panel(dc, RECT{12, 46, rc.right - 12, rc.bottom - 12}, C_PANEL, C_EDGE);
    drawColField(dc);
    panel(dc, R_COL_PREV, hsvRgb(colH, colS, colV), C_EDGE);
    primaryButton(dc, R_COL_OK, ICO_NONE, L"OK", hotColOk);
    button(dc, R_COL_CANCEL, L"Cancel", hotColCancel);
    boxEdge(dc);
}

static LRESULT CALLBACK colorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        printBox(h, (HDC)w, paintColor);
        return 0;
    case WM_PAINT:
        paintBox(h, paintColor);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (PtInRect(&R_COL_SV, p)) { colDragSv = true; SetCapture(h); colFromPt(p, false); InvalidateRect(h, nullptr, FALSE); }
        else if (PtInRect(&R_COL_HUE, p)) { colDragHue = true; SetCapture(h); colFromPt(p, true); InvalidateRect(h, nullptr, FALSE); }
        else if (PtInRect(&R_COL_OK, p)) colPickResult = 1;
        else if (PtInRect(&R_COL_CANCEL, p) || PtInRect(&R_COL_CLOSE, p)) colPickResult = 2;
        else if (p.y < TITLE_H) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (colDragSv) { colFromPt(p, false); InvalidateRect(h, nullptr, FALSE); }
        else if (colDragHue) { colFromPt(p, true); InvalidateRect(h, nullptr, FALSE); }
        bool a = PtInRect(&R_COL_OK, p) != 0, b = PtInRect(&R_COL_CANCEL, p) != 0, c = PtInRect(&R_COL_CLOSE, p) != 0;
        if (a != hotColOk || b != hotColCancel || c != hotColClose) { hotColOk = a; hotColCancel = b; hotColClose = c; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_CAPTURECHANGED:
        colDragSv = colDragHue = false;
        return 0;
    case WM_LBUTTONUP:
        colDragSv = colDragHue = false;
        ReleaseCapture();
        return 0;
    case WM_CLOSE:
        colPickResult = 2;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool pickColour(HWND owner, COLORREF& c) {
    rgbHsv(c, colH, colS, colV);
    colPickResult = 0;
    hotColOk = hotColCancel = hotColClose = false;
    colDragSv = colDragHue = false;
    RECT o; GetWindowRect(owner, &o);
    const int W = CP_W, H = CP_H;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"OctopusColor", L"Color", WS_POPUP,
                             (o.left + o.right - sc(W)) / 2, (o.top + o.bottom - sc(H)) / 2, sc(W), sc(H),
                             owner, nullptr, nullptr, nullptr);
    if (!h) return false;
    EnableWindow(owner, FALSE);
    boxShow(h);
    SetForegroundWindow(h);
    MSG msg;
    while (colPickResult == 0 && modalNext(msg)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { colPickResult = 1; continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { colPickResult = 2; continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    boxClose(h);
    SetForegroundWindow(owner);
    if (colPickResult != 1) return false;
    c = hsvRgb(colH, colS, colV);
    return true;
}

static int lookResult = 0, lookDrag = -1, hotLookBtn = -1, hotSwatch = -1, hotChip = -1;
static HWND hLook = nullptr;
static const int LK_W = 560, LK_H = 672;
static int lookTab = 0;
static ULONGLONG lookTickAt = 0;
static const UINT LOOK_TICK_TIMER = 7;
static const RECT R_LK_CLOSE  = {LK_W - 34, 5, LK_W - 8, 29};
static const RECT R_LK_TAB_LOOK = {12, 46, LK_W / 2 - 3, 72}, R_LK_TAB_BEH = {LK_W / 2 + 3, 46, LK_W - 12, 72};
static const RECT LK_PANELS[4] = {{12, 82, LK_W - 12, 196}, {12, 206, LK_W - 12, 298}, {12, 308, LK_W - 12, 512}, {12, 522, LK_W - 12, 624}};
static const RECT LK_BEH_PANELS[2] = {{12, 82, LK_W - 12, 300}, {12, 310, LK_W - 12, 624}};
static const RECT R_BH_ANIM = {24, 114, LK_W - 24, 130}, R_BH_TOP = {24, 160, LK_W - 24, 176};
static const RECT R_BH_PLACE = {24, 206, LK_W - 24, 222}, R_BH_HINTS = {24, 252, LK_W - 24, 268};
static const int KEY_ROW_Y = 344, KEY_ROW_STEP = 22;
static const RECT R_HK_DIR = {24, 578, LK_W - 196, 602};
static const RECT R_HK_DIR_OPEN = {LK_W - 184, 578, LK_W - 108, 602}, R_HK_DIR_PICK = {LK_W - 100, 578, LK_W - 24, 602};
struct BehaviourOption {
    const RECT* r;
    bool* on;
    int icon;
    const wchar_t* name;
    const wchar_t* note;
};
static const BehaviourOption BEHAVIOUR[4] = {
    {&R_BH_ANIM, &uiAnim, ICO_WAND, L"Animations", L"Windows fade in and out, and sliders glide"},
    {&R_BH_TOP, &uiTop, ICO_WINDOW, L"Always on top", L"Keeps OctopusEAX above games and other windows"},
    {&R_BH_PLACE, &uiPlace, ICO_ROOM, L"Remember where the window was", L"Opens where you last left it"},
    {&R_BH_HINTS, &uiHints, ICO_NOTE, L"Show hints", L"Explains each control when the mouse rests on it"},
};

static RECT keyCap(int row) {
    const int y = KEY_ROW_Y + row * KEY_ROW_STEP;
    return {24, y, 172, y + 19};
}

static std::wstring hotkeyWhat(int a) {
    if (a != ACT_LAST) return SHORTCUTS[a].what;
    return keepSec ? L"Save the last " + keepText(keepSec) + L" to a file" : L"Save what Last keeps to a file (Last is off now)";
}

static int keyRowAt(POINT p) {
    for (int a = 0; a < NUM_SHORTCUTS; ++a) {
        const RECT r = keyCap(a);
        if (PtInRect(&r, p)) return a;
    }
    return -1;
}
static const wchar_t* LOOK_NAMES[8] = {L"Accent", L"Text", L"Faded text", L"Panels", L"Background", L"Edges", L"Highlight", L"Off"};
static const RECT R_LK_CUSTOM = {366, 112, 456, 138};
static const RECT R_LK_TONE   = {174, 154, 414, 176};
static const RECT R_LK_BG     = {24, 338, 154, 364};
static const RECT R_LK_CLR    = {162, 338, 188, 364};
static const RECT R_LK_CLEAR  = {174, 374, 414, 396};
static const RECT R_LK_BLUR   = {174, 406, 414, 428};
static const RECT R_LK_DARK   = {174, 438, 414, 460};
static const RECT R_LK_ALPHA  = {174, 470, 414, 492};
static const RECT R_LK_FONT_PREV = {24, 552, 50, 576};
static const RECT R_LK_FONT   = {56, 552, 304, 576};
static const RECT R_LK_FONT_NEXT = {310, 552, 336, 576};
static const RECT R_LK_FONT_ADD = {348, 552, 450, 576};
static const RECT R_LK_FONT_DROP = {456, 552, 482, 576};
static const RECT R_LK_SIZE_PREV = {68, 588, 94, 612};
static const RECT R_LK_SIZE   = {100, 588, 150, 612};
static const RECT R_LK_SIZE_NEXT = {156, 588, 182, 612};
static const RECT R_LK_BOLD   = {198, 588, 278, 612};
static const RECT R_LK_NOTE   = {294, 588, LK_W - 24, 612};
static const RECT R_LK_RESET  = {12, 634, 162, 660};
static const RECT R_LK_OK     = {LK_W - 92, 634, LK_W - 12, 660};
static const RECT* LK_FADERS[5] = {&R_LK_TONE, &R_LK_CLEAR, &R_LK_BLUR, &R_LK_DARK, &R_LK_ALPHA};
static const wchar_t* LK_FADER_NAMES[5] = {L"Background brightness", L"See-through panels", L"Blur", L"Darken", L"Window opacity"};

static RECT lookChip(int i) {
    const int x = 24 + i * 34;
    return RECT{x, 112, x + 26, 138};
}
static RECT lookSwatch(int i) {
    const int x = 24 + (i % 4) * 124, y = 234 + (i / 4) * 32;
    return RECT{x, y, x + 26, y + 20};
}
static int currentChip() {
    for (int i = 0; i < NUM_THEME_CHIPS; ++i)
        if (THEME_CHIPS[i].hue == look.theme.hue && THEME_CHIPS[i].sat == look.theme.sat) return i;
    return -1;
}
static int lookFaderValue(int k) {
    return k == 0 ? look.theme.depth : k == 1 ? look.clear : k == 2 ? look.blur : k == 3 ? look.darken : uiAlpha;
}
static void lookChanged(HWND h) {
    applyLook();
    settingsSoon(h);
    refreshLook(hwndMain);
    InvalidateRect(h, nullptr, FALSE);
}
static void setLookValue(HWND h, int k, int v) {
    if (v == lookFaderValue(k)) return;
    if (k == 0) {
        Theme t = look.theme;
        t.depth = v;
        themeColours(look, t);
        lookChanged(h);
        return;
    }
    if (k == 1) { look.clear = v; refreshLook(hwndMain); }
    else if (k == 2) look.blur = v;
    else if (k == 3) look.darken = v;
    else { uiAlpha = v; applyWindowAlpha(hwndMain); }
    InvalidateRect(h, nullptr, FALSE);
}
static void setLookFader(HWND h, int k, int x) {
    setLookValue(h, k, faderValue(*LK_FADERS[k], x, 100));
}
static int lookFaderAt(POINT p) {
    for (int k = 0; k < 5; ++k) {
        RECT grab = *LK_FADERS[k];
        InflateRect(&grab, 0, 6);
        if (PtInRect(&grab, p)) return k;
    }
    return -1;
}
static int faceIndex() {
    const std::vector<std::wstring>& faces = uiFaces();
    for (size_t i = 0; i < faces.size(); ++i)
        if (_wcsicmp(faces[i].c_str(), uiFace.c_str()) == 0) return (int)i;
    return 0;
}
static std::wstring lookNote;

static void fontChanged(HWND h, const std::wstring& note) {
    settingsSave();
    lookNote = note;
    InvalidateRect(h, nullptr, FALSE);
}
static void stepFace(HWND h, int d) {
    const std::vector<std::wstring>& faces = uiFaces();
    const int n = (int)faces.size();
    const std::wstring next = faces[(size_t)(((faceIndex() + d) % n + n) % n)];
    setUiFace(next);
    fontChanged(h, L"");
}
static void stepSize(HWND h, int d) {
    setUiType(uiFace, uiBold, uiSize + d);
    fontChanged(h, L"");
}
static void flipBold(HWND h) {
    setUiType(uiFace, !uiBold, uiSize);
    fontChanged(h, L"");
}
static void addOwnFont(HWND h) {
    std::vector<wchar_t> name(MAX_PATH * 2, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = h;
    ofn.lpstrFilter = L"Fonts (*.ttf, *.otf, *.ttc)\0*.ttf;*.otf;*.ttc\0";
    ofn.lpstrFile = name.data();
    ofn.nMaxFile = (DWORD)name.size();
    ofn.lpstrTitle = L"Add a font";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    const std::wstring src = name.data(), leaf = src.substr(src.find_last_of(L"\\/") + 1);
    if (!fontFileName(leaf)) {
        fontChanged(h, L"Pick a .ttf, .otf or .ttc font file");
        return;
    }
    for (const std::wstring& face : fontfile::families(src))
        if (const std::wstring* known = knownFace(face)) {
            const std::wstring pick = *known;
            setUiFace(pick);
            fontChanged(h, pick + L" is already in the list");
            return;
        }
    const std::wstring dir = fontsDir();
    const size_t dot = leaf.find_last_of(L'.');
    std::wstring copy = dir + L"\\" + leaf;
    for (int k = 2; GetFileAttributesW(copy.c_str()) != INVALID_FILE_ATTRIBUTES; ++k)
        copy = dir + L"\\" + leaf.substr(0, dot) + L" (" + std::to_wstring(k) + L")" + leaf.substr(dot);
    if (!CopyFileW(src.c_str(), copy.c_str(), TRUE)) {
        fontChanged(h, L"That font could not be copied");
        return;
    }
    DeleteFileW((copy + FONT_GONE).c_str());
    const std::vector<std::wstring> faces = loadFontFile(copy);
    std::wstring first, known;
    for (const std::wstring& face : faces) {
        if (const std::wstring* k = knownFace(face)) {
            if (known.empty()) known = *k;
            continue;
        }
        uiFaces().push_back(face);
        ownFonts.push_back({face, copy});
        if (first.empty()) first = face;
    }
    if (first.empty()) {
        if (!faces.empty()) RemoveFontResourceExW(copy.c_str(), FR_PRIVATE, nullptr);
        DeleteFileW(copy.c_str());
        if (!known.empty()) setUiFace(known);
        fontChanged(h, known.empty() ? L"Windows can't use that file as a font" : known + L" is already in the list");
        return;
    }
    setUiFace(first);
    fontChanged(h, L"Added " + first);
}
static void dropOwnFont(HWND h) {
    const OwnFont* own = ownFont(uiFace);
    if (!own) return;
    const std::wstring file = own->file, face = own->face;
    std::vector<std::wstring>& list = uiFaces();
    for (auto it = ownFonts.begin(); it != ownFonts.end();) {
        if (_wcsicmp(it->file.c_str(), file.c_str()) != 0) {
            ++it;
            continue;
        }
        const std::wstring gone = it->face;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::wstring& f) { return _wcsicmp(f.c_str(), gone.c_str()) == 0; }), list.end());
        it = ownFonts.erase(it);
    }
    setUiFace(BASE_FACE);
    RemoveFontResourceExW(file.c_str(), FR_PRIVATE, nullptr);
    if (!DeleteFileW(file.c_str())) {
        const HANDLE mark = CreateFileW((file + FONT_GONE).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mark != INVALID_HANDLE_VALUE) CloseHandle(mark);
    }
    fontChanged(h, face + L" removed");
}

static void panelHead(HDC dc, const RECT& p, const wchar_t* head, const wchar_t* note) {
    label(dc, 24, p.top + 8, head, C_GOLD, fntBold);
    HGDIOBJ of = SelectObject(dc, fntBold);
    SIZE sz{};
    GetTextExtentPoint32W(dc, head, (int)wcslen(head), &sz);
    SelectObject(dc, of);
    label(dc, 24 + sz.cx + 10, p.top + 8, note, C_DIM, fntUI);
}

static void paintBehaviour(HDC dc) {
    for (const RECT& p : LK_BEH_PANELS) panel(dc, p, C_PANEL, C_EDGE);
    label(dc, 24, LK_BEH_PANELS[0].top + 8, L"Window", C_GOLD, fntBold);
    panelHead(dc, LK_BEH_PANELS[1], L"Hotkeys",
              keyListen >= 0 ? L"press the keys, Backspace for none, Esc to keep" : L"work in every program, even in games");
    for (const BehaviourOption& o : BEHAVIOUR) {
        drawCheck(dc, o.r->left, o.r->top, *o.on, o.icon, o.name);
        label(dc, o.r->left + optionText(o.icon), o.r->top + 20, o.note, C_DIM, fntUI);
    }
    for (int a = 0; a < NUM_SHORTCUTS; ++a) {
        const RECT cap = keyCap(a);
        const bool listening = keyListen == a, lit = listening || hotKeyRow == a;
        panel(dc, cap, lit ? C_NAVY : C_BACK, lit ? C_GOLD : C_EDGE);
        boxText(dc, cap, listening ? L"Press keys" : keyLabel(a).c_str(), listening ? C_GOLD : (shortcutKey[a] ? C_TEXT : C_DIM), fntBold, DT_CENTER);
        label(dc, cap.right + 12, cap.top + 2, hotkeyWhat(a).c_str(), C_DIM, fntUI);
    }
    {
        const std::wstring where = L"Saved clips go to " + clipsFolder();
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_DIM);
        HGDIOBJ of = SelectObject(dc, fntUI);
        RECT dr = R_HK_DIR;
        drawText(dc, where.c_str(), -1, &dr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    iconButton(dc, R_HK_DIR_OPEN, ICO_OPEN, L"Open", hotLookBtn == 16);
    iconButton(dc, R_HK_DIR_PICK, ICO_NONE, L"Change", hotLookBtn == 17);
    const RECT nr = {R_LK_RESET.left, R_LK_RESET.top, R_LK_OK.left - 12, R_LK_RESET.bottom};
    boxText(dc, nr, keyNote.empty() ? L"Click a key to set it, right-click to clear it" : keyNote.c_str(), keyNote.empty() ? C_DIM : C_TEXT, fntUI, DT_LEFT);
}

static void behaviourFlip(HWND h, int i) {
    bool& on = *BEHAVIOUR[i].on;
    on = !on;
    if (&on == &uiTop) applyTopmost(hwndMain);
    if (&on == &uiHints && hTip) SendMessageW(hTip, TTM_ACTIVATE, uiHints ? TRUE : FALSE, 0);
    if (&on == &uiPlace && uiPlace && hwndMain) {
        RECT wr;
        GetWindowRect(hwndMain, &wr);
        winX = wr.left;
        winY = wr.top;
    }
    settingsSave();
    lookTickAt = GetTickCount64();
    SetTimer(h, LOOK_TICK_TIMER, 16, nullptr);
    InvalidateRect(h, nullptr, FALSE);
}

static void paintLook(HDC dc, const RECT& rc) {
    boxTitle(dc, rc, L"Settings", R_LK_CLOSE, hotLookBtn == 13);
    iconButton(dc, R_LK_TAB_LOOK, ICO_LOOK, L"Look", hotLookBtn == 14, true, lookTab == 0);
    iconButton(dc, R_LK_TAB_BEH, ICO_WAND, L"Behaviour", hotLookBtn == 15, true, lookTab == 1);
    if (lookTab == 1) {
        paintBehaviour(dc);
        primaryButton(dc, R_LK_OK, ICO_NONE, L"Done", hotLookBtn == 3);
        boxEdge(dc);
        return;
    }
    for (const RECT& p : LK_PANELS) panel(dc, p, C_PANEL, C_EDGE);
    label(dc, 24, LK_PANELS[0].top + 8, L"Theme colour", C_GOLD, fntBold);
    label(dc, 128, LK_PANELS[0].top + 8, L"changes the whole app", C_DIM, fntUI);
    const int cur = currentChip();
    {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (int i = 0; i < NUM_THEME_CHIPS; ++i) {
            const RECT r = lookChip(i);
            Look sample{};
            themeColours(sample, Theme{THEME_CHIPS[i].hue, THEME_CHIPS[i].sat, look.theme.depth});
            Gdiplus::SolidBrush back(gpColor(sample.back));
            Gdiplus::SolidBrush dot(gpColor(sample.accent));
            gr.FillEllipse(&back, (float)r.left, (float)r.top, 26.0f, 26.0f);
            gr.FillEllipse(&dot, r.left + 5.0f, r.top + 5.0f, 16.0f, 16.0f);
            Gdiplus::Pen ring(gpColor(i == cur ? C_TEXT : (i == hotChip ? C_DIM : C_EDGE)), i == cur ? 2.0f : 1.0f);
            gr.DrawEllipse(&ring, r.left + 0.5f, r.top + 0.5f, 25.0f, 25.0f);
        }
    }
    iconButton(dc, R_LK_CUSTOM, ICO_LOOK, L"Custom", hotLookBtn == 4, true, cur < 0);
    {
        RECT nr = {R_LK_CUSTOM.right + 8, R_LK_CUSTOM.top, LK_W - 24, R_LK_CUSTOM.bottom};
        const int shown = hotChip >= 0 ? hotChip : cur;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT);
        HGDIOBJ of = SelectObject(dc, fntBold);
        drawText(dc, shown >= 0 ? THEME_CHIPS[shown].name : L"Custom", -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
    label(dc, 24, LK_PANELS[1].top + 8, L"Fine tune", C_GOLD, fntBold);
    label(dc, 100, LK_PANELS[1].top + 8, L"Click to change, right-click to reset", C_DIM, fntUI);
    for (int i = 0; i < 8; ++i) {
        const RECT r = lookSwatch(i);
        panel(dc, r, lookSlot(i), hotSwatch == i ? C_TEXT : C_EDGE);
        label(dc, r.right + 8, r.top + 2, LOOK_NAMES[i], hotSwatch == i ? C_TEXT : C_DIM, fntUI);
    }
    label(dc, 24, LK_PANELS[2].top + 8, L"Background picture", C_GOLD, fntBold);
    iconButton(dc, R_LK_BG, ICO_OPEN, L"Choose", hotLookBtn == 0);
    if (wallBmp) iconButton(dc, R_LK_CLR, ICO_CLOSE, nullptr, hotLookBtn == 1);
    {
        RECT pr = {R_LK_CLR.right + 10, R_LK_BG.top, LK_W - 24, R_LK_BG.bottom};
        SetTextColor(dc, C_DIM);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, fntUI);
        drawText(dc, wallPath.empty() ? L"No picture" : wallPath.c_str(), -1, &pr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    for (int k = 0; k < 5; ++k) {
        const RECT& r = *LK_FADERS[k];
        const bool live = k == 0 || k == 4 || wallBmp;
        label(dc, 24, r.top + 3, LK_FADER_NAMES[k], live ? C_DIM : C_OFF, fntUI);
        drawFader(dc, r, lookFaderValue(k), 100);
        wchar_t t[16];
        swprintf_s(t, 16, L"%d", lookFaderValue(k));
        label(dc, r.right + 12, r.top + 3, t, live ? C_TEXT : C_DIM, fntBold);
    }
    label(dc, 24, LK_PANELS[3].top + 8, L"Font", C_GOLD, fntBold);
    label(dc, 62, LK_PANELS[3].top + 8, L"the lettering in every window", C_DIM, fntUI);
    iconButton(dc, R_LK_FONT_PREV, ICO_PREV, nullptr, hotLookBtn == 5);
    panel(dc, R_LK_FONT, hotLookBtn == 7 ? C_NAVY : C_BACK, hotLookBtn == 7 ? C_GOLD : C_EDGE);
    centredText(dc, R_LK_FONT, uiFace.c_str(), C_TEXT, fntBold);
    iconButton(dc, R_LK_FONT_NEXT, ICO_NEXT, nullptr, hotLookBtn == 6);
    iconButton(dc, R_LK_FONT_ADD, ICO_OPEN, L"Add font", hotLookBtn == 8);
    if (ownFont(uiFace)) iconButton(dc, R_LK_FONT_DROP, ICO_TRASH, nullptr, hotLookBtn == 9);
    label(dc, 24, R_LK_SIZE.top + 4, L"Size", C_DIM, fntUI);
    iconButton(dc, R_LK_SIZE_PREV, ICO_PREV, nullptr, hotLookBtn == 10, uiSize > TEXT_SIZE_MIN);
    panel(dc, R_LK_SIZE, C_BACK, C_EDGE);
    centredText(dc, R_LK_SIZE, std::to_wstring(uiSize).c_str(), C_TEXT, fntBold);
    iconButton(dc, R_LK_SIZE_NEXT, ICO_NEXT, nullptr, hotLookBtn == 11, uiSize < TEXT_SIZE_MAX);
    iconButton(dc, R_LK_BOLD, ICO_NONE, L"Bold", hotLookBtn == 12, true, uiBold);
    {
        wchar_t t[48];
        swprintf_s(t, 48, L"Font %d of %d", faceIndex() + 1, (int)uiFaces().size());
        RECT nr = R_LK_NOTE;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, lookNote.empty() ? C_DIM : C_TEXT);
        HGDIOBJ of = SelectObject(dc, fntUI);
        drawText(dc, lookNote.empty() ? t : lookNote.c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    iconButton(dc, R_LK_RESET, ICO_RESET, L"Reset look", hotLookBtn == 2);
    primaryButton(dc, R_LK_OK, ICO_NONE, L"Done", hotLookBtn == 3);
    boxEdge(dc);
}

static LRESULT CALLBACK lookProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        printBox(h, (HDC)w, paintLook);
        return 0;
    case WM_TIMER:
        if (w == GLIDE_TIMER) glideStep(h);
        if (w == LOOK_TICK_TIMER) {
            if (GetTickCount64() - lookTickAt > 300) KillTimer(h, LOOK_TICK_TIMER);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (lookTab == 1 && keyListen >= 0 && keyRowAt(p) == keyListen) return 0;
        if (lookTab == 0) {
            if (const int k = lookFaderAt(p); k >= 0) {
                glideKnob(h, K_LOOK + k);
                return 0;
            }
            for (const RECT* once : {&R_LK_FONT_ADD, &R_LK_FONT_DROP, &R_LK_BOLD})
                if (PtInRect(once, p)) return 0;
        }
        return lookProc(h, WM_LBUTTONDOWN, w, l);
    }
    case WM_PAINT:
        ticksLive = true;
        paintBox(h, paintLook);
        ticksLive = false;
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        const int row = lookTab == 1 ? keyRowAt(p) : -1;
        if (row >= 0) {
            keyListenFlip(h, row);
            return 0;
        }
        if (keyListen >= 0) {
            keyListenEnd();
            InvalidateRect(h, nullptr, FALSE);
        }
        if (PtInRect(&R_LK_TAB_LOOK, p) || PtInRect(&R_LK_TAB_BEH, p)) {
            lookTab = PtInRect(&R_LK_TAB_BEH, p) ? 1 : 0;
            hotSwatch = hotChip = hotKeyRow = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_LK_OK, p) || PtInRect(&R_LK_CLOSE, p)) { lookResult = 1; return 0; }
        if (lookTab == 1) {
            for (int i = 0; i < 4; ++i)
                if (PtInRect(BEHAVIOUR[i].r, p)) { behaviourFlip(h, i); return 0; }
            if (PtInRect(&R_HK_DIR_OPEN, p)) {
                clipsOpen();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&R_HK_DIR_PICK, p)) {
                clipsPick(h);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (p.y < TITLE_H) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
            return 0;
        }
        for (int i = 0; i < NUM_THEME_CHIPS; ++i) {
            const RECT r = lookChip(i);
            if (PtInRect(&r, p)) {
                themeColours(look, Theme{THEME_CHIPS[i].hue, THEME_CHIPS[i].sat, look.theme.depth});
                lookChanged(h);
                return 0;
            }
        }
        if (PtInRect(&R_LK_CUSTOM, p)) {
            COLORREF c = look.accent;
            if (pickColour(h, c)) {
                themeColours(look, themeFromColour(c, look.theme.depth));
                lookChanged(h);
            }
            return 0;
        }
        for (int i = 0; i < 8; ++i) {
            const RECT r = lookSwatch(i);
            if (PtInRect(&r, p)) {
                if (pickColour(h, lookSlot(i))) lookChanged(h);
                return 0;
            }
        }
        if (PtInRect(&R_LK_BG, p)) {
            if (pickWallpaper(h)) lookChanged(h);
            return 0;
        }
        if (wallBmp && PtInRect(&R_LK_CLR, p)) { clearWallpaper(); lookChanged(h); return 0; }
        if (const int k = lookFaderAt(p); k >= 0) {
            glideStop(K_LOOK + k);
            lookDrag = k;
            SetCapture(h);
            setLookFader(h, k, p.x);
            return 0;
        }
        if (PtInRect(&R_LK_RESET, p)) {
            themeColours(look, PURPLE_THEME);
            look.clear = CLEAR_DEFAULT;
            look.blur = BLUR_DEFAULT;
            look.darken = DARKEN_DEFAULT;
            setUiType(BASE_FACE, false, TEXT_SIZE_BASE);
            lookNote.clear();
            buildBackground();
            lookChanged(h);
            return 0;
        }
        if (PtInRect(&R_LK_FONT_PREV, p)) { stepFace(h, -1); return 0; }
        if (PtInRect(&R_LK_FONT_NEXT, p) || PtInRect(&R_LK_FONT, p)) { stepFace(h, 1); return 0; }
        if (PtInRect(&R_LK_FONT_ADD, p)) { addOwnFont(h); return 0; }
        if (ownFont(uiFace) && PtInRect(&R_LK_FONT_DROP, p)) { dropOwnFont(h); return 0; }
        if (PtInRect(&R_LK_SIZE_PREV, p)) { stepSize(h, -1); return 0; }
        if (PtInRect(&R_LK_SIZE_NEXT, p)) { stepSize(h, 1); return 0; }
        if (PtInRect(&R_LK_BOLD, p)) { flipBold(h); return 0; }
        if (p.y < TITLE_H) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (lookTab == 1) {
            if (const int row = keyRowAt(p); row >= 0) {
                keyListenEnd();
                keyClear(row);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        if (lookTab != 0) return 0;
        for (int i = 0; i < 8; ++i) {
            const RECT r = lookSwatch(i);
            if (PtInRect(&r, p)) {
                Look base = look;
                themeColours(base, look.theme);
                lookSlot(i) = lookSlot(base, i);
                lookChanged(h);
                return 0;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (lookDrag >= 0) { setLookFader(h, lookDrag, p.x); return 0; }
        int sw = -1, bt = -1, ch = -1;
        const int kr = lookTab == 1 ? keyRowAt(p) : -1;
        if (PtInRect(&R_LK_TAB_LOOK, p)) bt = 14;
        else if (PtInRect(&R_LK_TAB_BEH, p)) bt = 15;
        else if (PtInRect(&R_LK_OK, p)) bt = 3;
        else if (PtInRect(&R_LK_CLOSE, p)) bt = 13;
        else if (lookTab != 0) bt = PtInRect(&R_HK_DIR_OPEN, p) ? 16 : (PtInRect(&R_HK_DIR_PICK, p) ? 17 : -1);
        else if (PtInRect(&R_LK_BG, p)) bt = 0;
        else if (wallBmp && PtInRect(&R_LK_CLR, p)) bt = 1;
        else if (PtInRect(&R_LK_RESET, p)) bt = 2;
        else if (PtInRect(&R_LK_CUSTOM, p)) bt = 4;
        else if (PtInRect(&R_LK_FONT_PREV, p)) bt = 5;
        else if (PtInRect(&R_LK_FONT_NEXT, p)) bt = 6;
        else if (PtInRect(&R_LK_FONT, p)) bt = 7;
        else if (PtInRect(&R_LK_FONT_ADD, p)) bt = 8;
        else if (ownFont(uiFace) && PtInRect(&R_LK_FONT_DROP, p)) bt = 9;
        else if (uiSize > TEXT_SIZE_MIN && PtInRect(&R_LK_SIZE_PREV, p)) bt = 10;
        else if (uiSize < TEXT_SIZE_MAX && PtInRect(&R_LK_SIZE_NEXT, p)) bt = 11;
        else if (PtInRect(&R_LK_BOLD, p)) bt = 12;
        if (lookTab == 0) {
            for (int i = 0; i < 8; ++i) { const RECT r = lookSwatch(i); if (PtInRect(&r, p)) sw = i; }
            for (int i = 0; i < NUM_THEME_CHIPS; ++i) { const RECT r = lookChip(i); if (PtInRect(&r, p)) ch = i; }
        }
        if (sw != hotSwatch || bt != hotLookBtn || ch != hotChip || kr != hotKeyRow) {
            hotSwatch = sw;
            hotLookBtn = bt;
            hotChip = ch;
            hotKeyRow = kr;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (lookDrag == 2 || lookDrag == 3) buildBackground();
        if (lookDrag >= 0) {
            lookDrag = -1;
            ReleaseCapture();
            settingsSave();
            refreshLook(hwndMain);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (lookDrag >= 0 && (HWND)l != h) {
            if (lookDrag == 2 || lookDrag == 3) buildBackground();
            lookDrag = -1;
            settingsSave();
            refreshLook(hwndMain);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        const POINT p = wheelPoint(h, l);
        if (lookTab != 0) return 0;
        const RECT row = {R_LK_FONT_PREV.left, R_LK_FONT.top, R_LK_FONT_NEXT.right, R_LK_FONT.bottom};
        const RECT size = {R_LK_SIZE_PREV.left, R_LK_SIZE.top, R_LK_SIZE_NEXT.right, R_LK_SIZE.bottom};
        if (PtInRect(&row, p)) stepFace(h, GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1);
        else if (PtInRect(&size, p)) stepSize(h, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && keyListen >= 0) {
            keyListenEnd();
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_CLOSE:
        lookResult = 1;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void pickLook(HWND owner) {
    lookResult = 0; lookDrag = -1; hotLookBtn = -1; hotSwatch = -1; hotChip = -1; lookTab = 0;
    keyListen = hotKeyRow = -1;
    lookNote.clear();
    keyNote.clear();
    RECT o;
    GetWindowRect(owner, &o);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"OctopusLook", L"Look", WS_POPUP,
                             (o.left + o.right - sc(LK_W)) / 2, (o.top + o.bottom - sc(LK_H)) / 2, sc(LK_W), sc(LK_H),
                             owner, nullptr, nullptr, nullptr);
    if (!h) return;
    hLook = h;
    EnableWindow(owner, FALSE);
    boxShow(h);
    SetForegroundWindow(h);
    MSG msg;
    while (lookResult == 0 && modalNext(msg)) {
        if (keyListen >= 0 && keyCapture(h, msg)) continue;
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN)) { lookResult = 1; continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    keyListenEnd();
    for (int k = K_LOOK; k < K_COUNT; ++k) glideStop(k);
    EnableWindow(owner, TRUE);
    boxClose(h);
    hLook = nullptr;
    SetForegroundWindow(owner);
    settingsSave();
    applyWindowAlpha(owner);
    applyTopmost(owner);
    refreshLook(owner);
}

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void boxTitle(HDC dc, const RECT& rc, const wchar_t* title, const RECT& close, bool closeHot) {
    HBRUSH bg = CreateSolidBrush(C_BACK);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    const RECT strip = {0, 0, rc.right, TITLE_H};
    blendRect(dc, strip, C_PANEL, (BYTE)alphaFor(C_PANEL));
    HPEN line = linePen(mixc(C_EDGE, C_GOLD, 0.6));
    HGDIOBJ oldLine = SelectObject(dc, line);
    MoveToEx(dc, 0, TITLE_H - 1, nullptr);
    LineTo(dc, rc.right, TITLE_H - 1);
    SelectObject(dc, oldLine);
    DeleteObject(line);
    drawLogo(dc, 10, 0);
    label(dc, 10 + (logoW ? logoW + 8 : 0), 7, title, C_GOLD, fntTitle);
    if (closeHot) {
        const COLORREF red = RGB(200, 58, 72);
        panel(dc, close, red, red);
        drawIcon(dc, (close.left + close.right) / 2.0f, (close.top + close.bottom) / 2.0f, ICO_CLOSE, RGB(255, 255, 255), red);
    } else {
        iconButton(dc, close, ICO_CLOSE, nullptr, false);
    }
}
static void boxText(HDC dc, const RECT& r, const wchar_t* s, COLORREF c, HFONT f, UINT align) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    HGDIOBJ of = SelectObject(dc, f);
    RECT rr = r;
    drawText(dc, s, -1, &rr, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
}
struct BoxFrame {
    HWND wnd = nullptr;
    DockImage img;
};
static std::vector<BoxFrame> boxFrames;

static DockImage* boxFrameFor(HWND h, int w, int hh) {
    for (size_t i = 0; i < boxFrames.size();) {
        if (IsWindow(boxFrames[i].wnd)) {
            ++i;
            continue;
        }
        boxFrames[i].img.drop();
        boxFrames.erase(boxFrames.begin() + (std::ptrdiff_t)i);
    }
    for (BoxFrame& b : boxFrames)
        if (b.wnd == h) return b.img.make(w, hh) ? &b.img : nullptr;
    boxFrames.push_back(BoxFrame{h, {}});
    return boxFrames.back().img.make(w, hh) ? &boxFrames.back().img : nullptr;
}

static void paintBox(HWND h, void (*paintFn)(HDC, const RECT&)) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    DockImage* frame = boxFrameFor(h, rc.right, rc.bottom);
    if (frame) {
        HDC dc = frame->dc;
        const RECT& d = ps.rcPaint;
        SelectClipRgn(dc, nullptr);
        IntersectClipRect(dc, d.left, d.top, d.right, d.bottom);
        scaleDc(dc);
        boxPx = {rc.right, rc.bottom};
        boxPainting = true;
        paintFn(dc, designRect(rc.right, rc.bottom));
        boxPainting = false;
        flatDc(dc);
        SelectClipRgn(dc, nullptr);
        fadeApply(h, dc, d);
        BitBlt(wdc, d.left, d.top, d.right - d.left, d.bottom - d.top, dc, d.left, d.top, SRCCOPY);
    } else {
        printBox(h, wdc, paintFn);
    }
    EndPaint(h, &ps);
}

static void printBox(HWND h, HDC dc, void (*paintFn)(HDC, const RECT&)) {
    RECT rc;
    GetClientRect(h, &rc);
    scaleDc(dc);
    boxPx = {rc.right, rc.bottom};
    boxPainting = true;
    paintFn(dc, designRect(rc.right, rc.bottom));
    boxPainting = false;
    flatDc(dc);
}
static void boxTrackLeave(HWND h, bool& tracking) {
    if (tracking) return;
    TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
    TrackMouseEvent(&t);
    tracking = true;
}

static std::atomic<int>& vocalKnob(int k) {
    switch (k) {
    case 0: return g.gateLevel;
    case 1: return g.gateLo;
    case 2: return g.gateMid;
    case 3: return g.gateHi;
    default: return g.gateOut;
    }
}
static int vocalKnobMin(int k) { return k == 0 ? 0 : -GATE_TONE_RANGE; }
static int vocalKnobMax(int k) { return k == 0 ? 100 : GATE_TONE_RANGE; }
static int vocalKnobDefault(int k) { return k == 0 ? VOCAL_DEFAULT_LEVEL : (k == 4 ? GATE_DEFAULT_OUT : GATE_DEFAULT_TONE); }
static void vocalChanged(HWND h) {
    g.isoVersion.fetch_add(1);
    settingsSoon(h);
    if (h) InvalidateRect(h, nullptr, FALSE);
    if (hwndMain) InvalidateRect(hwndMain, nullptr, FALSE);
}
static void setVocalKnob(HWND h, int k, int v) {
    v = clampInt(v, vocalKnobMin(k), vocalKnobMax(k));
    if (vocalKnob(k).load() == v) return;
    vocalKnob(k) = v;
    vocalChanged(h);
}
static void vocalReset(HWND h) {
    for (int k = 0; k < 5; ++k) vocalKnob(k) = vocalKnobDefault(k);
    vocalChanged(h);
}
static int toneHot = -1, toneDrag = -1;
static bool toneMouse = false;
static const int TONE_W = 420, TONE_H = 616, TONE_PAD_INSET = 10;
static const RECT RN_CLOSE = {TONE_W - 34, 5, TONE_W - 8, 29};
static const RECT RN_PANEL = {12, 46, TONE_W - 12, TONE_H - 12};
static const RECT RN_ON    = {24, 84, 124, 99};
static const RECT RN_FLAT  = {TONE_W - 120, 78, TONE_W - 24, 102};
static const RECT RN_PAD   = {90, 114, 330, 354};
static const int RN_VOCAL_Y = 370;
static const RECT RN_THR   = {100, 404, 330, 426};
static const RECT RN_LO    = {100, 436, 330, 458};
static const RECT RN_MID   = {100, 468, 330, 490};
static const RECT RN_HI    = {100, 500, 330, 522};
static const RECT RN_OUT   = {100, 532, 330, 554};
static const RECT RN_RESET = {TONE_W - 196, 566, TONE_W - 24, 592};
static const RECT* VOCAL_FADERS[5] = {&RN_THR, &RN_LO, &RN_MID, &RN_HI, &RN_OUT};
static const wchar_t* VOCAL_NAMES[5] = {L"Strength", L"Lo", L"Mid", L"Hi", L"Out"};
enum { TN_NONE = -1, TN_CLOSE = 100, TN_ON, TN_FLAT, TN_PAD, TN_RESET };

static void setVocalFader(HWND h, int k, int x) {
    int v = vocalKnobMin(k) + faderValue(*VOCAL_FADERS[k], x, vocalKnobMax(k) - vocalKnobMin(k));
    if (k > 0) v = (v + (v >= 0 ? 2 : -2)) / 5 * 5;
    setVocalKnob(h, k, v);
}
static int vocalFaderAt(POINT p) {
    for (int k = 0; k < 5; ++k) {
        RECT grab = *VOCAL_FADERS[k];
        InflateRect(&grab, 8, 6);
        if (PtInRect(&grab, p)) return k;
    }
    return TN_NONE;
}

static std::atomic<int>& toneKnob(int k) { return k == 0 ? g.toneLo : (k == 1 ? g.toneMid : g.toneHi); }
static std::atomic<int>& toneBase(int k) { return k == 0 ? g.toneBaseLo : (k == 1 ? g.toneBaseMid : g.toneBaseHi); }
static int toneTilt(int pad) { return (pad * TONE_RANGE + (pad >= 0 ? TONE_PAD_RANGE / 2 : -TONE_PAD_RANGE / 2)) / TONE_PAD_RANGE; }
static int toneOf(int base, int tilt) { return clampInt(base + tilt, -TONE_RANGE, TONE_RANGE); }
static int toneKnobTilt(int k) {
    switch (k) {
    case 0: return -toneTilt(g.tonePadX.load());
    case 1: return toneTilt(g.tonePadY.load());
    default: return toneTilt(g.tonePadX.load());
    }
}
static bool toneSettled() {
    for (int k = 0; k < 3; ++k)
        if (toneKnob(k).load() != toneOf(toneBase(k).load(), toneKnobTilt(k))) return false;
    return true;
}
static void toneSettle() {
    if (toneSettled()) return;
    for (int k = 0; k < 3; ++k) toneBase(k) = toneKnob(k).load();
    g.tonePadX = 0;
    g.tonePadY = 0;
}
static bool toneShaping() { return g.toneOn.load() && (g.toneLo.load() != 0 || g.toneMid.load() != 0 || g.toneHi.load() != 0); }
static int toneRoom(int fromBase) { return clampInt(fromBase * TONE_PAD_RANGE / TONE_RANGE, 0, TONE_PAD_RANGE); }
static void toneChanged(HWND h) {
    g.toneVersion.fetch_add(1);
    settingsSoon(h);
    if (h) InvalidateRect(h, nullptr, FALSE);
    if (hwndMain) InvalidateRect(hwndMain, nullptr, FALSE);
}
static void setTonePad(HWND h, int x, int y) {
    const int lo = g.toneBaseLo.load(), mid = g.toneBaseMid.load(), hi = g.toneBaseHi.load();
    x = clampInt(x, -std::max(toneRoom(TONE_RANGE - lo), toneRoom(hi + TONE_RANGE)), std::max(toneRoom(TONE_RANGE - hi), toneRoom(lo + TONE_RANGE)));
    y = clampInt(y, -toneRoom(mid + TONE_RANGE), toneRoom(TONE_RANGE - mid));
    if (x == g.tonePadX.load() && y == g.tonePadY.load()) return;
    g.tonePadX = x;
    g.tonePadY = y;
    g.toneLo = toneOf(lo, -toneTilt(x));
    g.toneHi = toneOf(hi, toneTilt(x));
    g.toneMid = toneOf(mid, toneTilt(y));
    toneChanged(h);
}
static POINT tonePadPoint() {
    const int w = RN_PAD.right - RN_PAD.left - 2 * TONE_PAD_INSET, hgt = RN_PAD.bottom - RN_PAD.top - 2 * TONE_PAD_INSET;
    return {RN_PAD.left + TONE_PAD_INSET + (g.tonePadX.load() + TONE_PAD_RANGE) * w / (2 * TONE_PAD_RANGE),
            RN_PAD.top + TONE_PAD_INSET + (TONE_PAD_RANGE - g.tonePadY.load()) * hgt / (2 * TONE_PAD_RANGE)};
}
static void setTonePadFromPoint(HWND h, POINT p) {
    const int w = RN_PAD.right - RN_PAD.left - 2 * TONE_PAD_INSET, hgt = RN_PAD.bottom - RN_PAD.top - 2 * TONE_PAD_INSET;
    const int dx = clampInt(p.x - RN_PAD.left - TONE_PAD_INSET, 0, w), dy = clampInt(p.y - RN_PAD.top - TONE_PAD_INSET, 0, hgt);
    setTonePad(h, (dx * 2 * TONE_PAD_RANGE + w / 2) / w - TONE_PAD_RANGE, TONE_PAD_RANGE - (dy * 2 * TONE_PAD_RANGE + hgt / 2) / hgt);
}
static void toneFlat(HWND h) {
    for (int k = 0; k < 3; ++k) {
        toneBase(k) = 0;
        toneKnob(k) = 0;
    }
    g.tonePadX = 0;
    g.tonePadY = 0;
    toneChanged(h);
}
static int toneHit(POINT p) {
    if (PtInRect(&RN_CLOSE, p)) return TN_CLOSE;
    if (PtInRect(&RN_ON, p)) return TN_ON;
    if (PtInRect(&RN_FLAT, p)) return TN_FLAT;
    if (PtInRect(&RN_PAD, p)) return TN_PAD;
    if (PtInRect(&RN_RESET, p)) return TN_RESET;
    return vocalFaderAt(p);
}
static void paintTonePad(HDC dc, bool live) {
    const RECT& r = RN_PAD;
    const bool held = toneDrag == TN_PAD, hot = held || toneHot == TN_PAD;
    panel(dc, r, C_BACK, hot ? C_GOLD : C_EDGE);
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    {
        PixelSnap px(dc);
        const RECT in = px.inside(r);
        const int t = px.line();
        auto cross = [&](int x, int y, COLORREF c) {
            HBRUSH b = CreateSolidBrush(c);
            const RECT down = {px.x((float)x), in.top, px.x((float)x) + t, in.bottom};
            const RECT across = {in.left, px.y((float)y), in.right, px.y((float)y) + t};
            FillRect(dc, &down, b);
            FillRect(dc, &across, b);
            DeleteObject(b);
        };
        for (int k = 1; k < 4; k += 2) cross(r.left + (r.right - r.left) * k / 4, r.top + (r.bottom - r.top) * k / 4, C_EDGE);
        cross(cx, cy, C_DIM);
    }
    const RECT top = {cx - 50, r.top + 4, cx + 50, r.top + 20}, bottom = {cx - 50, r.bottom - 20, cx + 50, r.bottom - 4};
    const RECT left = {r.left + 6, cy - 20, r.left + 60, cy - 4}, right = {r.right - 60, cy - 20, r.right - 6, cy - 4};
    boxText(dc, top, L"more mid", C_DIM, fntUI, DT_CENTER);
    boxText(dc, bottom, L"less mid", C_DIM, fntUI, DT_CENTER);
    boxText(dc, left, L"deep", C_DIM, fntUI, DT_LEFT);
    boxText(dc, right, L"sharp", C_DIM, fntUI, DT_RIGHT);
    const POINT d = tonePadPoint();
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush halo(gpColor(C_GOLD, held ? 90 : 50));
    gr.FillEllipse(&halo, d.x - 14.0f, d.y - 14.0f, 28.0f, 28.0f);
    Gdiplus::SolidBrush dot(gpColor(live ? C_GOLD : mixc(C_GOLD, C_DIM, 0.5)));
    gr.FillEllipse(&dot, d.x - 7.0f, d.y - 7.0f, 14.0f, 14.0f);
    Gdiplus::Pen edge(gpColor(C_KNOB), 1.2f);
    gr.DrawEllipse(&edge, d.x - 7.0f, d.y - 7.0f, 14.0f, 14.0f);
}
static void paintTone(HDC dc, const RECT& rc) {
    boxTitle(dc, rc, L"Tone", RN_CLOSE, toneHot == TN_CLOSE);
    const bool live = g.toneOn.load();
    panel(dc, RN_PANEL, C_PANEL, C_EDGE);
    label(dc, 24, RN_PANEL.top + 8, L"Tone", C_GOLD, fntBold);
    {
        const RECT hint = {80, RN_PANEL.top + 8, rc.right - 24, RN_PANEL.top + 24};
        boxText(dc, hint, L"Lo 200 Hz, Mid 1 kHz, Hi 3.5 kHz", C_DIM, fntUI, DT_RIGHT);
    }
    drawCheck(dc, RN_ON.left, RN_ON.top, live, L"Tone on");
    iconButton(dc, RN_FLAT, ICO_RESET, L"Flat", toneHot == TN_FLAT);
    paintTonePad(dc, live);
    HPEN sep = linePen(C_EDGE);
    HGDIOBJ oldSep = SelectObject(dc, sep);
    MoveToEx(dc, 24, RN_VOCAL_Y, nullptr);
    LineTo(dc, rc.right - 24, RN_VOCAL_Y);
    SelectObject(dc, oldSep);
    DeleteObject(sep);
    label(dc, 24, RN_VOCAL_Y + 8, L"Vocal", C_GOLD, fntBold);
    {
        const RECT hint = {80, RN_VOCAL_Y + 8, rc.right - 24, RN_VOCAL_Y + 24};
        boxText(dc, hint, vocalOn() ? L"Lo 60 Hz, Mid 650 Hz, Hi 6.3 kHz" : L"Works when Noise isolation is on Vocal", C_DIM, fntUI, DT_RIGHT);
    }
    wchar_t t[32];
    for (int k = 0; k < 5; ++k) {
        const RECT& r = *VOCAL_FADERS[k];
        label(dc, 24, r.top + 3, VOCAL_NAMES[k], C_DIM, fntUI);
        drawFader(dc, r, vocalKnob(k).load() - vocalKnobMin(k), vocalKnobMax(k) - vocalKnobMin(k));
        if (k == 0) swprintf_s(t, 32, L"%d", g.gateLevel.load());
        else swprintf_s(t, 32, L"%+.1f dB", vocalKnob(k).load() / 10.0);
        label(dc, r.right + 8, r.top + 3, t, C_TEXT, fntBold);
    }
    iconButton(dc, RN_RESET, ICO_RESET, L"Back to default", toneHot == TN_RESET);
    boxEdge(dc);
}

static LRESULT CALLBACK toneProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paintBox(h, paintTone);
        return 0;
    case WM_PRINTCLIENT:
        printBox(h, (HDC)w, paintTone);
        return 0;
    case WM_MOUSEMOVE: {
        boxTrackLeave(h, toneMouse);
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (toneDrag == TN_PAD) { setTonePadFromPoint(h, p); return 0; }
        if (toneDrag >= 0 && toneDrag < 5) { setVocalFader(h, toneDrag, p.x); return 0; }
        const int hit = toneHit(p);
        if (hit != toneHot) {
            toneHot = hit;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        toneMouse = false;
        if (toneHot != TN_NONE) {
            toneHot = TN_NONE;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        const int hit = toneHit(p);
        if (hit == TN_PAD) { setTonePad(h, 0, 0); return 0; }
        if (hit >= 0 && hit < 5) { setVocalKnob(h, hit, vocalKnobDefault(hit)); return 0; }
        return toneProc(h, WM_LBUTTONDOWN, w, l);
    }
    case WM_LBUTTONDOWN: {
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        const int hit = toneHit(p);
        switch (hit) {
        case TN_CLOSE: boxClose(h); return 0;
        case TN_ON: g.toneOn = !g.toneOn.load(); toneChanged(h); return 0;
        case TN_FLAT: toneFlat(h); return 0;
        case TN_RESET: vocalReset(h); return 0;
        case TN_PAD:
            toneDrag = TN_PAD;
            SetCapture(h);
            setTonePadFromPoint(h, p);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case TN_NONE:
            if (p.y < TITLE_H) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
            return 0;
        default:
            toneDrag = hit;
            SetCapture(h);
            setVocalFader(h, hit, p.x);
            return 0;
        }
    }
    case WM_LBUTTONUP:
        if (toneDrag != -1) {
            toneDrag = -1;
            ReleaseCapture();
            settingsFlush();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)l != h && toneDrag != -1) {
            toneDrag = -1;
            settingsFlush();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = wheelPoint(h, l);
        const int d = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (d == 0) return 0;
        if (PtInRect(&RN_PAD, p)) setTonePad(h, g.tonePadX.load(), g.tonePadY.load() + d * 5);
        else if (const int k = vocalFaderAt(p); k >= 0) setVocalKnob(h, k, vocalKnob(k).load() + d * (k == 0 ? 1 : 5));
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: boxClose(h); return 0;
        case VK_LEFT: setTonePad(h, g.tonePadX.load() - 2, g.tonePadY.load()); return 0;
        case VK_RIGHT: setTonePad(h, g.tonePadX.load() + 2, g.tonePadY.load()); return 0;
        case VK_UP: setTonePad(h, g.tonePadX.load(), g.tonePadY.load() + 2); return 0;
        case VK_DOWN: setTonePad(h, g.tonePadX.load(), g.tonePadY.load() - 2); return 0;
        }
        break;
    case WM_DESTROY:
        if (toneDrag != -1) { toneDrag = -1; ReleaseCapture(); }
        hTone = nullptr;
        toneMouse = false;
        toneHot = TN_NONE;
        if (hwndMain) InvalidateRect(hwndMain, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void openToneBox(HWND owner) {
    if (hTone) {
        ShowWindow(hTone, SW_SHOW);
        SetForegroundWindow(hTone);
        return;
    }
    RECT o;
    GetWindowRect(owner, &o);
    hTone = CreateWindowExW(WS_EX_TOOLWINDOW, L"OctopusTone", L"Tone", WS_POPUP | WS_CLIPCHILDREN,
                            (o.left + o.right - sc(TONE_W)) / 2, (o.top + o.bottom - sc(TONE_H)) / 2, sc(TONE_W), sc(TONE_H),
                            owner, nullptr, nullptr, nullptr);
    if (!hTone) return;
    boxShow(hTone);
    SetForegroundWindow(hTone);
}

static int aboutResult = 0, aboutHot = 0;
static bool aboutMouse = false;
static RECT aboutLink = {0, 0, 0, 0};
static const wchar_t* ABOLHB_SITE = L"https://abolhb.com";
static const int AB_W = 460, AB_H = 304;
static const RECT R_AB_CLOSE = {AB_W - 34, 5, AB_W - 8, 29};
static RECT aboutOk(const RECT& rc) { return {24, rc.bottom - 50, 100, rc.bottom - 24}; }

static void paintAbout(HDC dc, const RECT& rc) {
    boxTitle(dc, rc, L"OctopusEAX", R_AB_CLOSE, aboutHot == 3);
    panel(dc, RECT{12, 46, rc.right - 12, rc.bottom - 12}, C_PANEL, C_EDGE);
    if (artBmp && artH > 0) {
        const int ah = 210, aw = artW * ah / artH, ax = rc.right - 36 - aw, ay = 58;
        drawGlow(dc, ax + aw / 2.0f, ay + ah / 2.0f, aw * 0.9f, ah * 0.62f, C_GOLD, 56);
        drawBitmapAlpha(dc, artBmp, ax, ay, aw, ah);
    }
    label(dc, 24, 58, L"Contributes on OctopusEAX :", C_GOLD, fntBold);
    {
        const wchar_t* name = L"ABOLHB";
        const COLORREF ink = aboutHot == 2 ? C_GOLD : C_TEXT;
        label(dc, 24, 82, name, ink, fntBold);
        SIZE sz{};
        HGDIOBJ of = SelectObject(dc, fntBold);
        GetTextExtentPoint32W(dc, name, (int)wcslen(name), &sz);
        SelectObject(dc, of);
        aboutLink = {22, 80, 26 + sz.cx, 84 + sz.cy};
        if (aboutHot == 2) {
            HPEN pen = linePen(C_GOLD);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, 24, 82 + sz.cy, nullptr);
            LineTo(dc, 24 + sz.cx, 82 + sz.cy);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
    }
    label(dc, 24, 102, L"MMLo7", C_TEXT, fntBold);
    label(dc, 24, 138, L"Special thanks to :", C_GOLD, fntBold);
    label(dc, 24, 162, L"FREEMASONRY", C_TEXT, fntBold);
    label(dc, 24, 182, L"SALEMLY", C_TEXT, fntBold);
    primaryButton(dc, aboutOk(rc), ICO_NONE, L"OK", aboutHot == 1);
    boxEdge(dc);
}

static LRESULT CALLBACK aboutProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        printBox(h, (HDC)w, paintAbout);
        return 0;
    case WM_PAINT:
        paintBox(h, paintAbout);
        return 0;
    case WM_MOUSEMOVE: {
        if (!aboutMouse) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            aboutMouse = true;
        }
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        RECT dev;
        GetClientRect(h, &dev);
        const RECT done = aboutOk(designRect(dev.right, dev.bottom));
        const int hot = PtInRect(&done, p) ? 1 : (PtInRect(&aboutLink, p) ? 2 : (PtInRect(&R_AB_CLOSE, p) ? 3 : 0));
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
        if (LOWORD(l) == HTCLIENT && (aboutHot == 1 || aboutHot == 2)) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        RECT dev;
        GetClientRect(h, &dev);
        const RECT done = aboutOk(designRect(dev.right, dev.bottom));
        if (PtInRect(&done, p) || PtInRect(&R_AB_CLOSE, p)) aboutResult = 1;
        else if (PtInRect(&aboutLink, p)) ShellExecuteW(nullptr, L"open", ABOLHB_SITE, nullptr, nullptr, SW_SHOWNORMAL);
        else if (p.y < TITLE_H) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
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
    const int W = AB_W, H = AB_H;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"OctopusAbout", L"OctopusEAX", WS_POPUP,
                             (o.left + o.right - sc(W)) / 2, (o.top + o.bottom - sc(H)) / 2, sc(W), sc(H),
                             owner, nullptr, nullptr, nullptr);
    if (!h) return;
    EnableWindow(owner, FALSE);
    boxShow(h);
    SetForegroundWindow(h);
    MSG msg;
    while (aboutResult == 0 && modalNext(msg)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    boxClose(h);
    SetForegroundWindow(owner);
}

static void paintStripOnly(HDC wdc) {
    const RECT strip = stripRect();
    const RECT dev = {sc(strip.left), (LONG)std::ceil(strip.top * uiScale - 0.001f), sc(strip.right), (LONG)std::floor(strip.bottom * uiScale + 0.001f)};
    const int dw = dev.right - dev.left, dh = dev.bottom - dev.top;
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP mb = mdc ? backBuffer(dw, dh) : nullptr;
    if (mdc && mb) {
        HGDIOBJ om = SelectObject(mdc, mb);
        const RECT whole = {0, 0, dw, dh};
        HBRUSH bg = CreateSolidBrush(C_BACK);
        FillRect(mdc, &whole, bg);
        DeleteObject(bg);
        if (wallBmp) {
            HDC mem = CreateCompatibleDC(mdc);
            HGDIOBJ ow = SelectObject(mem, wallBmp);
            const int fullW = sc(WIN_W), fullH = sc(WIN_H);
            if (wallW == fullW && wallH == fullH) {
                BitBlt(mdc, 0, 0, dw, dh, mem, dev.left, dev.top, SRCCOPY);
            } else {
                SetStretchBltMode(mdc, HALFTONE);
                StretchBlt(mdc, 0, 0, dw, dh, mem, dev.left * wallW / fullW, dev.top * wallH / fullH, dw * wallW / fullW, dh * wallH / fullH, SRCCOPY);
            }
            SelectObject(mem, ow);
            DeleteDC(mem);
        }
        SetGraphicsMode(mdc, GM_ADVANCED);
        const XFORM fit = {uiScale, 0.0f, 0.0f, uiScale, -(float)dev.left, -(float)dev.top};
        SetWorldTransform(mdc, &fit);
        drawLevelStrip(mdc, strip);
        flatDc(mdc);
        BitBlt(wdc, dev.left, dev.top, dw, dh, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, om);
    }
    if (mb) DeleteObject(mb);
    if (mdc) DeleteDC(mdc);
}

static void centredText(HDC dc, const RECT& r, const wchar_t* s, COLORREF c, HFONT f) {
    RECT t = r;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    HGDIOBJ of = SelectObject(dc, f);
    drawText(dc, s, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
}

static const int BOARD_PADS = 12, BOARD_COLS = 4, BOARD_W = 560, BOARD_H = 540, BOARD_ROWS = 8, BOARD_ROW_H = 24;
static const int BOARD_PAD_W = 122, BOARD_PAD_H = 50, BOARD_GAP = 8, BOARD_MOST_FILES = 200, HOTKEY_BASE = 0x5100;
static const int HOTKEY_APP = HOTKEY_BASE + 0x40, HOTKEY_PROBE = HOTKEY_BASE + 0x7F;
static const int BOARD_PADS_Y = 46, BOARD_GRID_Y = 78, BOARD_SOUNDS_Y = 266;
static const double BOARD_MOST_SEC = 30.0;
static const UINT WM_BOARD_READY = WM_APP + 31;
static const wchar_t* BOARD_KEY = L"Software\\OctopusEAX\\Soundboard";
static const RECT RB_CLOSE = {BOARD_W - 34, 5, BOARD_W - 8, 29};
static const RECT RB_LIST = {24, BOARD_SOUNDS_Y + 30, 360, BOARD_SOUNDS_Y + 30 + BOARD_ROWS * BOARD_ROW_H};
static const RECT RB_ADD = {372, RB_LIST.top, BOARD_W - 24, RB_LIST.top + 24};
static const RECT RB_REMOVE = {372, RB_LIST.top + 28, BOARD_W - 24, RB_LIST.top + 52};
static const RECT RB_PUT = {372, RB_LIST.top + 56, BOARD_W - 24, RB_LIST.top + 80};
static const RECT RB_TRIM = {372, RB_LIST.top + 84, 452, RB_LIST.top + 108};
static const RECT RB_RENAME = {456, RB_LIST.top + 84, BOARD_W - 24, RB_LIST.top + 108};
static const RECT RB_KEY = {372, RB_LIST.top + 112, BOARD_W - 24, RB_LIST.top + 136};
static const RECT RB_CLEAR = {372, RB_LIST.top + 140, BOARD_W - 24, RB_LIST.top + 164};
static const RECT RB_STOP = {372, RB_LIST.top + 168, 452, RB_LIST.top + 192};
static const RECT RB_STOPKEY = {456, RB_LIST.top + 168, BOARD_W - 24, RB_LIST.top + 192};
static const RECT RB_NOTE = {16, BOARD_H - 30, BOARD_W - 16, BOARD_H - 4};
enum { BH_NONE = -1, BH_CLOSE = 100, BH_ADD, BH_REMOVE, BH_PUT, BH_TRIM, BH_KEY, BH_CLEAR, BH_STOP, BH_RENAME, BH_STOPKEY, BH_SCROLL, BH_ROW = 200 };

struct BoardPad {
    std::wstring path, name;
    std::shared_ptr<const clip::Sound> sound;
    double from = 0.0, to = 0.0;
    double fadeIn = 0.0, fadeOut = 0.0;
    UINT mods = 0, vk = 0;
    bool loading = false;

    bool trimmed() const { return to > from; }
};

struct BoardLoaded {
    int pad = 0;
    std::wstring path, why;
    double from = 0.0, to = 0.0;
    double fadeIn = 0.0, fadeOut = 0.0;
    std::shared_ptr<const clip::Sound> sound;
};

static BoardPad boardPads[BOARD_PADS];
static std::vector<std::wstring> boardFiles;
static HWND hBoard = nullptr;
static int boardPad = 0, boardRow = -1, boardTop = 0, boardHot = BH_NONE;
static bool boardListening = false, boardListenStop = false, boardMouse = false;
static UINT boardStopMods = 0, boardStopVk = 0;
static HWND boardEdit = nullptr;
static int boardEditPad = -1;
static const int ID_BOARDEDIT = 7307;
static unsigned boardLit = 0;
static std::wstring boardNote;

static std::wstring trimClock(double sec, int places = 2) {
    const long long scale = places == 2 ? 100 : (places == 1 ? 10 : 1);
    const long long n = std::llround(std::max(0.0, sec) * scale), whole = n / scale;
    wchar_t t[32];
    if (places == 0) swprintf_s(t, 32, L"%lld:%02lld", whole / 60, whole % 60);
    else swprintf_s(t, 32, places == 2 ? L"%lld:%02lld.%02lld" : L"%lld:%02lld.%lld", whole / 60, whole % 60, n % scale);
    return t;
}

static RECT boardPadRect(int i) {
    const int x = 24 + (i % BOARD_COLS) * (BOARD_PAD_W + BOARD_GAP), y = BOARD_GRID_Y + (i / BOARD_COLS) * (BOARD_PAD_H + BOARD_GAP);
    return {x, y, x + BOARD_PAD_W, y + BOARD_PAD_H};
}

static std::wstring fileTitle(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

static std::wstring padTitle(const BoardPad& bp) {
    return bp.name.empty() ? fileTitle(bp.path) : bp.name;
}

static std::wstring keyName(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";
    if (vk >= VK_F1 && vk <= VK_F24) return s + L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return s + L"Num " + std::to_wstring(vk - VK_NUMPAD0);
    if (vk == VK_MBUTTON) return s + L"Middle mouse";
    if (vk == VK_XBUTTON1) return s + L"Mouse 4";
    if (vk == VK_XBUTTON2) return s + L"Mouse 5";
    LONG code = (LONG)(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
    switch (vk) {
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
    case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        code |= 1 << 24;
        break;
    default:
        break;
    }
    wchar_t name[64] = L"";
    if (GetKeyNameTextW(code, name, 64) > 0) s += name;
    else {
        wchar_t t[32];
        swprintf_s(t, 32, L"Key %u", vk);
        s += t;
    }
    return s;
}

static void boardSave() {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, BOARD_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
    std::wstring multi;
    for (const std::wstring& f : boardFiles) { multi += f; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetValueExW(hk, L"Files", 0, REG_MULTI_SZ, (const BYTE*)multi.c_str(), (DWORD)(multi.size() * sizeof(wchar_t)));
    for (int i = 0; i < BOARD_PADS; ++i) {
        wchar_t n[16];
        swprintf_s(n, 16, L"Pad%d", i);
        const std::wstring& p = boardPads[i].path;
        RegSetValueExW(hk, n, 0, REG_SZ, (const BYTE*)p.c_str(), (DWORD)((p.size() + 1) * sizeof(wchar_t)));
        swprintf_s(n, 16, L"Key%d", i);
        const DWORD k = (boardPads[i].mods << 16) | boardPads[i].vk;
        RegSetValueExW(hk, n, 0, REG_DWORD, (const BYTE*)&k, sizeof k);
        const DWORD fromMs = (DWORD)std::llround(boardPads[i].from * 1000.0), toMs = (DWORD)std::llround(boardPads[i].to * 1000.0);
        swprintf_s(n, 16, L"From%d", i);
        RegSetValueExW(hk, n, 0, REG_DWORD, (const BYTE*)&fromMs, sizeof fromMs);
        swprintf_s(n, 16, L"To%d", i);
        RegSetValueExW(hk, n, 0, REG_DWORD, (const BYTE*)&toMs, sizeof toMs);
        const DWORD inMs = (DWORD)std::llround(boardPads[i].fadeIn * 1000.0), outMs = (DWORD)std::llround(boardPads[i].fadeOut * 1000.0);
        swprintf_s(n, 16, L"FadeIn%d", i);
        RegSetValueExW(hk, n, 0, REG_DWORD, (const BYTE*)&inMs, sizeof inMs);
        swprintf_s(n, 16, L"FadeOut%d", i);
        RegSetValueExW(hk, n, 0, REG_DWORD, (const BYTE*)&outMs, sizeof outMs);
        swprintf_s(n, 16, L"Name%d", i);
        const std::wstring& nm = boardPads[i].name;
        RegSetValueExW(hk, n, 0, REG_SZ, (const BYTE*)nm.c_str(), (DWORD)((nm.size() + 1) * sizeof(wchar_t)));
    }
    const DWORD stop = (boardStopMods << 16) | boardStopVk;
    RegSetValueExW(hk, L"StopKey", 0, REG_DWORD, (const BYTE*)&stop, sizeof stop);
    RegCloseKey(hk);
}

static void boardLoad() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, BOARD_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(hk, L"Files", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ && size > 0) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
        if (RegQueryValueExW(hk, L"Files", nullptr, &type, (BYTE*)buf.data(), &size) == ERROR_SUCCESS) {
            for (const wchar_t* p = buf.data(); *p && boardFiles.size() < (size_t)BOARD_MOST_FILES; p += wcslen(p) + 1) boardFiles.push_back(p);
        }
    }
    for (int i = 0; i < BOARD_PADS; ++i) {
        wchar_t n[16], path[MAX_PATH * 2] = L"";
        swprintf_s(n, 16, L"Pad%d", i);
        DWORD sz = sizeof path - sizeof(wchar_t);
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)path, &sz) == ERROR_SUCCESS && type == REG_SZ) boardPads[i].path = path;
        swprintf_s(n, 16, L"Key%d", i);
        DWORD k = 0;
        sz = sizeof k;
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)&k, &sz) == ERROR_SUCCESS && type == REG_DWORD) {
            boardPads[i].mods = (k >> 16) & 0x0F;
            boardPads[i].vk = k & 0xFF;
        }
        DWORD fromMs = 0, toMs = 0;
        swprintf_s(n, 16, L"From%d", i);
        sz = sizeof fromMs;
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)&fromMs, &sz) != ERROR_SUCCESS || type != REG_DWORD) fromMs = 0;
        swprintf_s(n, 16, L"To%d", i);
        sz = sizeof toMs;
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)&toMs, &sz) != ERROR_SUCCESS || type != REG_DWORD) toMs = 0;
        if (toMs > fromMs && !boardPads[i].path.empty()) {
            boardPads[i].from = fromMs / 1000.0;
            boardPads[i].to = toMs / 1000.0;
        }
        DWORD inMs = 0, outMs = 0;
        swprintf_s(n, 16, L"FadeIn%d", i);
        sz = sizeof inMs;
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)&inMs, &sz) != ERROR_SUCCESS || type != REG_DWORD) inMs = 0;
        swprintf_s(n, 16, L"FadeOut%d", i);
        sz = sizeof outMs;
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)&outMs, &sz) != ERROR_SUCCESS || type != REG_DWORD) outMs = 0;
        if (!boardPads[i].path.empty()) {
            boardPads[i].fadeIn = std::min(inMs, 10000UL) / 1000.0;
            boardPads[i].fadeOut = std::min(outMs, 10000UL) / 1000.0;
        }
        wchar_t name[64] = L"";
        swprintf_s(n, 16, L"Name%d", i);
        sz = sizeof name - sizeof(wchar_t);
        if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)name, &sz) == ERROR_SUCCESS && type == REG_SZ && !boardPads[i].path.empty()) boardPads[i].name = name;
    }
    DWORD stop = 0, ssz = sizeof stop;
    if (RegQueryValueExW(hk, L"StopKey", nullptr, &type, (BYTE*)&stop, &ssz) == ERROR_SUCCESS && type == REG_DWORD) {
        boardStopMods = (stop >> 16) & 0x0F;
        boardStopVk = stop & 0xFF;
    }
    RegCloseKey(hk);
}

static void boardDecode(const std::vector<int>& pads) {
    std::vector<BoardLoaded> jobs;
    for (int pad : pads) {
        BoardPad& bp = boardPads[pad];
        bp.sound.reset();
        bp.loading = !bp.path.empty();
        if (bp.loading) jobs.push_back(BoardLoaded{pad, bp.path, L"", bp.from, bp.to, bp.fadeIn, bp.fadeOut, nullptr});
    }
    if (jobs.empty()) return;
    const HWND owner = hwndMain;
    std::thread([jobs, owner] {
        const MediaWork busy;
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        for (const BoardLoaded& job : jobs) {
            if (g.quit.load()) break;
            BoardLoaded* r = new BoardLoaded(job);
            r->sound = clip::withFades(clip::decodeSound(job.path, FS, BOARD_MOST_SEC, r->why, job.from, job.to, &g.quit), FS, job.fadeIn, job.fadeOut);
            if (!PostMessageW(owner, WM_BOARD_READY, 0, (LPARAM)r)) delete r;
        }
        if (com) CoUninitialize();
    }).detach();
}

static void boardLoaded(BoardLoaded* r) {
    BoardPad& bp = boardPads[r->pad];
    if (bp.path == r->path && bp.from == r->from && bp.to == r->to && bp.fadeIn == r->fadeIn && bp.fadeOut == r->fadeOut) {
        bp.loading = false;
        bp.sound = r->sound;
        if (!r->sound) boardNote = r->why + L": " + fileTitle(r->path);
        else if (r->sound->cut) boardNote = fileTitle(r->path) + L" is long, its first 30 seconds are used. Trim pad picks another part";
    }
    delete r;
    if (hBoard) InvalidateRect(hBoard, nullptr, FALSE);
}

static void boardPlay(int pad) {
    const BoardPad& bp = boardPads[pad];
    if (bp.sound) {
        gBoard.play(pad, bp.sound);
    } else if (bp.loading) {
        boardNote = L"That sound is still loading";
    } else if (!bp.path.empty()) {
        boardNote = L"Reading " + fileTitle(bp.path) + L" again, press the pad once more when it is ready";
        boardDecode({pad});
    }
}

static void hotkeysUnregister() {
    for (int i = 0; i <= BOARD_PADS; ++i) UnregisterHotKey(hwndMain, HOTKEY_BASE + i);
    for (int a = 0; a < NUM_SHORTCUTS; ++a) UnregisterHotKey(hwndMain, HOTKEY_APP + a);
}

static bool hotkeysBusy = false;

static void hotkeysRegister(int fresh = -1, bool quiet = false) {
    hotkeysUnregister();
    hotkeysBusy = false;
    if (boardListening || keyListen >= 0) return;
    for (int a = 0; a < NUM_SHORTCUTS; ++a) {
        const DWORD k = shortcutKey[a];
        if (k && !RegisterHotKey(hwndMain, HOTKEY_APP + a, (k >> 16) | MOD_NOREPEAT, k & 0xFFFF)) hotkeysBusy = true;
    }
    for (int i = 0; i < BOARD_PADS; ++i) {
        BoardPad& bp = boardPads[i];
        if (!bp.vk) continue;
        if (!RegisterHotKey(hwndMain, HOTKEY_BASE + i, bp.mods | MOD_NOREPEAT, bp.vk)) {
            if (!quiet) boardNote = keyName(bp.mods, bp.vk) + L" is used by another program, pick another key";
            if (i == fresh) bp.mods = bp.vk = 0;
            else hotkeysBusy = true;
        }
    }
    if (boardStopVk && !RegisterHotKey(hwndMain, HOTKEY_BASE + BOARD_PADS, boardStopMods | MOD_NOREPEAT, boardStopVk)) {
        if (!quiet) boardNote = keyName(boardStopMods, boardStopVk) + L" is used by another program, pick another key";
        if (fresh == BOARD_PADS) boardStopMods = boardStopVk = 0;
        else hotkeysBusy = true;
    }
}

static bool plainKey(UINT vk) {
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || vk == VK_SPACE || vk == VK_RETURN || vk == VK_TAB ||
           (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102;
}

static UINT heldMods() {
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
    return mods;
}

static std::wstring freeChord(UINT mods, UINT vk, int keep) {
    std::wstring was;
    for (int i = 0; i < BOARD_PADS; ++i)
        if (i != keep && boardPads[i].vk == vk && boardPads[i].mods == mods) {
            boardPads[i].mods = boardPads[i].vk = 0;
            was = L"pad " + std::to_wstring(i + 1);
        }
    if (keep != BOARD_PADS && boardStopVk == vk && boardStopMods == mods) {
        boardStopMods = boardStopVk = 0;
        was = L"Stop key";
    }
    const int gk = g.glitchKey.load();
    if (keep != BOARD_PADS + 1 && gk && (UINT)(gk & 0xFFFF) == vk && (UINT)(gk >> 16) == mods) {
        g.glitchKey = 0;
        settingsSave();
        was = L"glitch";
    }
    for (int a = 0; a < NUM_SHORTCUTS; ++a)
        if (shortcutKey[a] == chordOf(mods, vk)) {
            shortcutKey[a] = 0;
            settingsSave();
            was = std::wstring(SHORTCUTS[a].name) + L" hotkey";
        }
    return was.empty() ? was : L" (taken from the " + was + L")";
}

static std::wstring globalKeyUse(DWORD key) {
    for (int i = 0; i < BOARD_PADS; ++i)
        if (boardPads[i].vk && chordOf(boardPads[i].mods, boardPads[i].vk) == key) return L"soundboard pad " + std::to_wstring(i + 1);
    if (boardStopVk && chordOf(boardStopMods, boardStopVk) == key) return L"the soundboard Stop key";
    const int gk = g.glitchKey.load();
    if (gk && (DWORD)gk == key) return L"the glitch";
    return L"";
}

static void keyListenEnd() {
    if (keyListen < 0) return;
    keyListen = -1;
    hotkeysRegister(-1, true);
}

static void keyListenFlip(HWND h, int row) {
    const bool again = keyListen == row;
    keyListenEnd();
    keyNote.clear();
    if (!again) {
        keyListen = row;
        hotkeysUnregister();
    }
    InvalidateRect(h, nullptr, FALSE);
}

static void keySet(int row, DWORD key) {
    if (!keyUsable(key)) {
        keyNote = L"Hold Ctrl, Alt or Shift with that key";
        return;
    }
    const std::wstring use = globalKeyUse(key);
    if (!use.empty()) {
        keyNote = L"Taken by " + use;
        return;
    }
    if (!RegisterHotKey(hwndMain, HOTKEY_PROBE, (key >> 16) | MOD_NOREPEAT, key & 0xFFFF)) {
        keyNote = keyText(key) + L" is used by another program";
        return;
    }
    UnregisterHotKey(hwndMain, HOTKEY_PROBE);
    std::wstring was;
    for (int a = 0; a < NUM_SHORTCUTS; ++a)
        if (a != row && shortcutKey[a] == key) {
            shortcutKey[a] = 0;
            was = SHORTCUTS[a].name;
        }
    shortcutKey[row] = key;
    keyNote = SHORTCUTS[row].name + std::wstring(L" is now ") + keyText(key) + (was.empty() ? L"" : L", taken from " + was);
    settingsSave();
}

static void keyClear(int row) {
    if (!shortcutKey[row]) return;
    shortcutKey[row] = 0;
    keyNote = SHORTCUTS[row].name + std::wstring(L" has no key now");
    settingsSave();
    hotkeysRegister(-1, true);
}

static bool keyCapture(HWND h, const MSG& m) {
    if (!m.hwnd || GetAncestor(m.hwnd, GA_ROOT) != h) return false;
    switch (m.message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return true;
    default:
        return false;
    }
    const UINT vk = (UINT)m.wParam;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_PROCESSKEY || vk == VK_PACKET) return true;
    const int row = keyListen;
    const UINT mods = heldMods();
    if (vk == VK_ESCAPE && !mods) {
        keyNote.clear();
    } else if (vk == VK_BACK && !mods) {
        shortcutKey[row] = 0;
        keyNote = SHORTCUTS[row].name + std::wstring(L" has no key now");
        settingsSave();
    } else if (mods & MOD_WIN) {
        keyNote = L"Windows key combos belong to Windows";
    } else if (mods == MOD_ALT && vk == VK_F4) {
        keyNote = L"Alt+F4 closes windows, pick another";
    } else {
        keySet(row, chordOf(mods, vk));
    }
    keyListenEnd();
    InvalidateRect(h, nullptr, FALSE);
    return true;
}

static void boardKey(UINT vk) {
    boardListening = false;
    const bool stop = boardListenStop;
    boardListenStop = false;
    BoardPad& bp = boardPads[boardPad];
    int fresh = -1;
    if (vk == VK_ESCAPE) {
        boardNote.clear();
    } else if (vk == VK_BACK || vk == VK_DELETE) {
        if (stop) boardStopMods = boardStopVk = 0;
        else bp.mods = bp.vk = 0;
        boardNote = L"Key removed";
    } else {
        const UINT mods = heldMods();
        fresh = stop ? BOARD_PADS : boardPad;
        if (!mods && plainKey(vk)) {
            boardNote = L"Hold Ctrl, Alt or Shift with a letter or number key";
        } else if (stop) {
            hotkeysUnregister();
            const std::wstring was = freeChord(mods, vk, BOARD_PADS);
            boardStopMods = mods;
            boardStopVk = vk;
            boardNote = keyName(mods, vk) + L" stops every playing sound, even in other programs" + was;
        } else {
            hotkeysUnregister();
            const std::wstring was = freeChord(mods, vk, boardPad);
            bp.mods = mods;
            bp.vk = vk;
            boardNote = keyName(mods, vk) + L" plays this pad, even in other programs" + was;
        }
    }
    hotkeysRegister(fresh);
    boardSave();
}

static void boardPut() {
    if (boardRow < 0 || boardRow >= (int)boardFiles.size()) return;
    BoardPad& bp = boardPads[boardPad];
    bp.path = boardFiles[(size_t)boardRow];
    bp.name.clear();
    bp.from = bp.to = 0.0;
    bp.fadeIn = bp.fadeOut = 0.0;
    boardDecode({boardPad});
    boardNote = fileTitle(boardFiles[(size_t)boardRow]) + L" is on pad " + std::to_wstring(boardPad + 1);
    boardSave();
}

static void boardAdd(HWND owner) {
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Sound files\0*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac;*.mp4;*.webm;*.opus;*.ogg\0All files\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = L"Choose sounds for the soundboard";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> picked;
    const std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) picked.push_back(first);
    for (; *p; p += wcslen(p) + 1) picked.push_back(first + L"\\" + p);
    for (const std::wstring& f : picked) {
        auto same = std::find_if(boardFiles.begin(), boardFiles.end(), [&](const std::wstring& o) { return _wcsicmp(o.c_str(), f.c_str()) == 0; });
        if (same == boardFiles.end()) boardFiles.push_back(f);
        if (boardFiles.size() > (size_t)BOARD_MOST_FILES) boardFiles.erase(boardFiles.begin());
    }
    const std::wstring& last = picked.back();
    for (size_t i = 0; i < boardFiles.size(); ++i)
        if (_wcsicmp(boardFiles[i].c_str(), last.c_str()) == 0) boardRow = (int)i;
    if (boardRow < boardTop) boardTop = boardRow;
    if (boardRow >= boardTop + BOARD_ROWS) boardTop = boardRow - BOARD_ROWS + 1;
    boardNote = L"Pick a pad, then press Put on pad";
    boardSave();
}

static double boardShown = 0.0, boardGlideAt = 0.0;
static int boardDragOff = -1;
static bool boardGliding = false;
static const UINT_PTR BOARD_GLIDE_TIMER = 2;

static bool boardScrolls() { return (int)boardFiles.size() > BOARD_ROWS; }

static RECT boardTrack() { return RECT{RB_LIST.right - 8, RB_LIST.top + 2, RB_LIST.right - 2, RB_LIST.bottom - 2}; }

static RECT boardThumb(double top) {
    const RECT track = boardTrack();
    const int span = (int)(track.bottom - track.top), total = (int)boardFiles.size();
    const int h = std::max(14, span * BOARD_ROWS / std::max(1, total));
    const LONG y = track.top + (LONG)std::lround((span - h) * top / std::max(1, total - BOARD_ROWS));
    return RECT{track.left + 1, y, track.right - 1, y + h};
}

static void boardHoverRefresh(HWND h);

static void boardGlide(HWND h) {
    if (!uiAnim) {
        boardShown = boardTop;
        boardHoverRefresh(h);
        return;
    }
    fadeDrop(h);
    if (!boardGliding) boardGlideAt = fxNow();
    boardGliding = true;
    SetTimer(h, BOARD_GLIDE_TIMER, 10, nullptr);
}

static int boardHit(POINT p);

static void boardHoverRefresh(HWND h) {
    POINT c;
    RECT cl;
    if (!GetCursorPos(&c) || !ScreenToClient(h, &c) || !GetClientRect(h, &cl) || !PtInRect(&cl, c)) return;
    const int hit = boardHit(POINT{unsc(c.x), unsc(c.y)});
    if (hit == boardHot) return;
    const bool watch = hoverWatch(h, WM_MOUSELEAVE, 0);
    boardHot = hit;
    InvalidateRect(h, nullptr, FALSE);
    if (watch) hoverFade(h);
}

static void boardGlideStep(HWND h) {
    const double now = fxNow(), ms = now - boardGlideAt;
    boardGlideAt = now;
    if (boardDragOff >= 0 || !uiAnim || std::fabs(boardTop - boardShown) < 0.01) {
        boardShown = boardTop;
        boardGliding = false;
        KillTimer(h, BOARD_GLIDE_TIMER);
        boardHoverRefresh(h);
    } else {
        boardShown += (boardTop - boardShown) * (1.0 - std::exp(-ms / 45.0));
    }
    invalidateDesign(h, RB_LIST);
}

static int boardRowAt(POINT p) {
    if (!PtInRect(&RB_LIST, p) || p.y <= RB_LIST.top) return -1;
    const double shown = boardGliding ? std::max(0.0, boardShown) : (double)boardTop;
    const int first = (int)shown, lift = (int)std::lround((shown - first) * BOARD_ROW_H);
    const int idx = first + (int)(p.y - RB_LIST.top - 1 + lift) / BOARD_ROW_H;
    return idx >= 0 && idx < (int)boardFiles.size() ? idx : -1;
}

static int boardHit(POINT p) {
    if (PtInRect(&RB_LIST, p) && !boardFiles.empty()) {
        if (boardScrolls() && p.x >= boardTrack().left - 1) return BH_SCROLL;
        const int idx = boardRowAt(p);
        return idx >= 0 ? BH_ROW + idx : BH_NONE;
    }
    for (int i = 0; i < BOARD_PADS; ++i) {
        const RECT r = boardPadRect(i);
        if (PtInRect(&r, p)) return i;
    }
    const RECT* rects[] = {&RB_CLOSE, &RB_ADD, &RB_REMOVE, &RB_PUT, &RB_TRIM, &RB_KEY, &RB_CLEAR, &RB_STOP, &RB_RENAME, &RB_STOPKEY};
    for (int k = 0; k < 10; ++k)
        if (PtInRect(rects[k], p)) return BH_CLOSE + k;
    return BH_NONE;
}

static void paintBoard(HDC dc, const RECT& rc) {
    boxTitle(dc, rc, L"Soundboard", RB_CLOSE, boardHot == BH_CLOSE);

    const RECT pads = {12, BOARD_PADS_Y, rc.right - 12, BOARD_GRID_Y + 3 * BOARD_PAD_H + 2 * BOARD_GAP + 12};
    panel(dc, pads, C_PANEL, C_EDGE);
    label(dc, 24, BOARD_PADS_Y + 8, L"Pads", C_GOLD, fntBold);
    {
        RECT hint = {120, BOARD_PADS_Y + 8, rc.right - 24, BOARD_PADS_Y + 24};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_DIM);
        HGDIOBJ of = SelectObject(dc, fntUI);
        drawText(dc, L"Click a pad to play it. Its key plays it in any program.", -1, &hint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    const unsigned lit = gBoard.playing();
    for (int i = 0; i < BOARD_PADS; ++i) {
        const RECT r = boardPadRect(i);
        const BoardPad& bp = boardPads[i];
        const bool on = (lit >> i) & 1u, sel = i == boardPad, asking = sel && boardListening && !boardListenStop;
        const COLORREF fill = on ? C_NAVY : C_BACK;
        panel(dc, r, fill, on || sel ? C_GOLD : (boardHot == i ? C_DIM : C_EDGE));
        wchar_t num[4];
        swprintf_s(num, 4, L"%d", i + 1);
        label(dc, r.left + 8, r.top + 7, num, C_GOLD, fntBold);
        const std::wstring name = bp.path.empty() ? L"Empty" : padTitle(bp);
        RECT tr = {r.left + 28, r.top + 6, r.right - 8, r.top + 24};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, bp.path.empty() ? C_DIM : (on ? C_GOLD : C_TEXT));
        HGDIOBJ of = SelectObject(dc, bp.path.empty() ? fntUI : fntBold);
        drawText(dc, name.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        const std::wstring sub = asking ? L"Press the keys now" : (bp.loading ? L"Loading..." : (bp.vk ? keyName(bp.mods, bp.vk) : (bp.path.empty() ? L"" : L"No key")));
        RECT kr = {r.left + 8, r.top + 27, r.right - 8, r.bottom - 5};
        SelectObject(dc, fntUI);
        SetTextColor(dc, asking ? C_GOLD : C_DIM);
        drawText(dc, sub.c_str(), -1, &kr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
    }

    const RECT sounds = {12, BOARD_SOUNDS_Y, rc.right - 12, RB_LIST.bottom + 12};
    panel(dc, sounds, C_PANEL, C_EDGE);
    label(dc, 24, BOARD_SOUNDS_Y + 8, L"Your sounds", C_GOLD, fntBold);
    if (!boardFiles.empty()) {
        wchar_t count[32];
        swprintf_s(count, 32, boardFiles.size() == 1 ? L"%d sound" : L"%d sounds", (int)boardFiles.size());
        RECT cr = {RB_LIST.right - 120, BOARD_SOUNDS_Y + 8, RB_LIST.right, BOARD_SOUNDS_Y + 24};
        SetTextColor(dc, C_DIM);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, fntUI);
        drawText(dc, count, -1, &cr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
    panel(dc, RB_LIST, C_BACK, C_EDGE);
    if (boardFiles.empty()) {
        centredText(dc, RB_LIST, L"Press Add sounds to pick sound files", C_DIM, fntUI);
    } else {
        const bool scroll = boardScrolls();
        if (!boardGliding || !scroll) boardShown = boardTop;
        const double shown = std::min((double)std::max(0, (int)boardFiles.size() - BOARD_ROWS), std::max(0.0, boardShown));
        const int first = (int)shown, lift = (int)std::lround((shown - first) * BOARD_ROW_H);
        const int hovered = boardHot >= BH_ROW ? boardHot - BH_ROW : -1;
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, RB_LIST.left + 1, RB_LIST.top + 1, RB_LIST.right - 1, RB_LIST.bottom - 1);
        for (int k = 0; k <= BOARD_ROWS; ++k) {
            const int idx = first + k;
            if (idx >= (int)boardFiles.size()) break;
            const LONG top = RB_LIST.top + 1 + k * BOARD_ROW_H - lift;
            RECT row = {RB_LIST.left + 1, top, RB_LIST.right - (scroll ? 9 : 1), top + BOARD_ROW_H - 1};
            if (idx == boardRow || idx == hovered) {
                PixelSnap px(dc);
                const RECT in = px.inside(RB_LIST);
                const RECT lit = {in.left, std::max(in.top, px.y((float)row.top)), scroll ? px.x((float)row.right) : in.right,
                                  std::min(in.bottom, px.y((float)row.bottom))};
                blendRect(dc, lit, idx == boardRow ? C_NAVY : mixc(C_BACK, C_NAVY, 0.45), 255);
            }
            RECT tx = {row.left + 8, row.top, row.right - 6, row.bottom};
            SetTextColor(dc, idx == boardRow ? C_GOLD : C_TEXT);
            HGDIOBJ of = SelectObject(dc, fntUI);
            drawText(dc, fileTitle(boardFiles[(size_t)idx]).c_str(), -1, &tx, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, of);
        }
        RestoreDC(dc, saved);
        if (scroll) {
            const RECT track = boardTrack();
            blendRect(dc, track, C_PANEL, 255);
            const double shade = boardDragOff >= 0 ? 0.55 : (boardHot == BH_SCROLL ? 0.42 : 0.30);
            const RECT thumb = boardThumb(shown);
            blendRect(dc, thumb, mixc(C_NAVY, C_GOLD, shade), 255);
        }
    }
    const bool row = boardRow >= 0 && boardRow < (int)boardFiles.size();
    iconButton(dc, RB_ADD, ICO_OPEN, L"Add sounds", boardHot == BH_ADD);
    iconButton(dc, RB_REMOVE, ICO_TRASH, L"Remove sound", boardHot == BH_REMOVE, row);
    iconButton(dc, RB_PUT, ICO_PLAY, L"Put on pad", boardHot == BH_PUT, row);
    iconButton(dc, RB_TRIM, ICO_CUT, L"Trim", boardHot == BH_TRIM, !boardPads[boardPad].path.empty(), boardPads[boardPad].trimmed() || boardPads[boardPad].fadeIn > 0.0 || boardPads[boardPad].fadeOut > 0.0);
    button(dc, RB_RENAME, L"Rename", boardHot == BH_RENAME, !boardPads[boardPad].path.empty());
    iconButton(dc, RB_KEY, ICO_KEY, L"Set key", boardHot == BH_KEY, true, boardListening && !boardListenStop);
    iconButton(dc, RB_CLEAR, ICO_RESET, L"Clear pad", boardHot == BH_CLEAR, !boardPads[boardPad].path.empty() || boardPads[boardPad].vk != 0);
    iconButton(dc, RB_STOP, ICO_STOP, L"Stop all", boardHot == BH_STOP);
    {
        const std::wstring sk = boardListening && boardListenStop ? L"Press key" : (boardStopVk ? keyName(boardStopMods, boardStopVk) : L"Stop key");
        iconButton(dc, RB_STOPKEY, ICO_NONE, sk.c_str(), boardHot == BH_STOPKEY, true, boardListening && boardListenStop);
    }

    const RECT status = {0, rc.bottom - STATUS_H, rc.right, rc.bottom};
    blendRect(dc, status, C_PANEL, (BYTE)alphaFor(C_PANEL));
    HPEN edge = linePen(C_EDGE);
    HGDIOBJ oldEdge = SelectObject(dc, edge);
    MoveToEx(dc, 0, status.top, nullptr);
    LineTo(dc, rc.right, status.top);
    SelectObject(dc, oldEdge);
    DeleteObject(edge);
    const std::wstring note = boardNote.empty() ? L"Sounds follow the Sound clip volume, Hear it myself and Send to the mic" : boardNote;
    RECT nr = RB_NOTE;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, boardNote.empty() ? C_DIM : C_TEXT);
    HGDIOBJ of = SelectObject(dc, fntUI);
    drawText(dc, note.c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    boxEdge(dc);
}

static const int TRIM_W = 640, TRIM_H = 394, TRIM_PER_SEC = 200, TRIM_PREVIEW = BOARD_PADS, WAVE_GRAB = 6;
static const double TRIM_LEAST_SEC = 0.1, TRIM_NUDGE_SEC = 0.1, TRIM_VIEW_LEAST_SEC = 1.0, FADE_MOST_SEC = 10.0;

enum { WD_NONE, WD_FROM, WD_TO, WD_PICK, WD_VIEW };
enum { WH_NONE = -1, WH_FROM = 200, WH_TO, WH_WAVE, WH_VIEW };

struct WaveEdit {
    RECT wave, ruler, bar;
    double cap = 0.0;
    bool trimming = true;
    std::shared_ptr<const clip::Outline> shape;
    double from = 0.0, to = 0.0, viewAt = 0.0, viewLen = 1.0, grab = 0.0;
    int drag = WD_NONE, pressX = 0, hot = WH_NONE;
    bool moved = false;
    mutable std::weak_ptr<const clip::Outline> colsOf;
    mutable double colsAt = -1.0, colsLen = -1.0;
    mutable std::vector<float> cols;

    bool ready() const { return shape != nullptr; }
    double length() const { return shape ? shape->seconds : 0.0; }
    double most() const { return cap > 0.0 ? std::min(cap, length()) : length(); }
    double least() const { return std::min(TRIM_LEAST_SEC, length()); }
    double timeAt(int x) const { return viewAt + (x - wave.left - 1) * viewLen / (wave.right - wave.left - 2); }
    double xOf(double t) const { return wave.left + 1 + (t - viewAt) / viewLen * (wave.right - wave.left - 2); }
    double barTime(int x) const { return (x - bar.left - 1) * length() / (bar.right - bar.left - 2); }
    int barX(double t) const { return bar.left + 1 + (int)std::lround(t / length() * (bar.right - bar.left - 2)); }
    bool zoomed() const { return viewLen < length(); }
    bool whole() const { return from <= 0.0 && to >= length(); }
    bool picked() const { return trimming || !whole(); }
};

static void waveClampView(WaveEdit& w) {
    w.viewAt = std::max(0.0, std::min(w.length() - w.viewLen, w.viewAt));
}

static void waveReveal(WaveEdit& w, double t) {
    if (t < w.viewAt || t > w.viewAt + w.viewLen) {
        w.viewAt = t - w.viewLen / 2.0;
        waveClampView(w);
    }
}

static void waveShowAll(WaveEdit& w) {
    w.viewAt = 0.0;
    w.viewLen = w.length();
}

static bool waveSet(WaveEdit& w, bool start, double t) {
    const double len = w.length(), most = w.most(), least = w.least();
    const double keep = std::max(least, std::min(most, w.to - w.from));
    bool pushed = false;
    if (start) {
        w.from = std::max(0.0, std::min(len - least, t));
        if (w.to < w.from + least) w.to = std::min(len, w.from + keep);
        if (w.to > w.from + most) { w.to = w.from + most; pushed = true; }
    } else {
        w.to = std::min(len, std::max(least, t));
        if (w.from > w.to - least) w.from = std::max(0.0, w.to - keep);
        if (w.from < w.to - most) { w.from = w.to - most; pushed = true; }
    }
    return pushed;
}

static void waveZoom(WaveEdit& w, int x, double notches) {
    const double len = w.length();
    if (len <= 0.0) return;
    const double at = w.timeAt(x);
    const double span = std::max(std::min(TRIM_VIEW_LEAST_SEC, len), std::min(len, w.viewLen * std::pow(0.8, notches)));
    w.viewAt = at - (at - w.viewAt) * span / w.viewLen;
    w.viewLen = span;
    waveClampView(w);
}

static int waveHit(const WaveEdit& w, POINT p) {
    if (!w.ready()) return WH_NONE;
    if (PtInRect(&w.wave, p)) {
        if (!w.picked()) return WH_WAVE;
        const double xa = w.xOf(w.from), xb = w.xOf(w.to);
        const double da = std::fabs(p.x - xa), db = std::fabs(p.x - xb);
        if (std::min(da, db) <= WAVE_GRAB) {
            if (da < db) return WH_FROM;
            if (db < da) return WH_TO;
            return p.x < xa ? WH_FROM : WH_TO;
        }
        return WH_WAVE;
    }
    RECT bar = w.bar;
    InflateRect(&bar, 0, 4);
    if (PtInRect(&bar, p) && w.zoomed()) return WH_VIEW;
    return WH_NONE;
}

static void wavePick(WaveEdit& w, int x) {
    const double len = w.length(), most = w.most(), least = w.least();
    const double a = w.grab, t = std::max(0.0, std::min(len, w.timeAt(x)));
    double lo = std::min(a, t), hi = std::max(a, t);
    if (hi - lo > most) {
        if (t > a) hi = a + most;
        else lo = a - most;
    }
    if (hi - lo < least) {
        if (t >= a) {
            hi = std::min(len, lo + least);
            lo = hi - least;
        } else {
            lo = std::max(0.0, hi - least);
            hi = lo + least;
        }
    }
    w.from = lo;
    w.to = hi;
}

static bool waveDown(WaveEdit& w, POINT p) {
    switch (waveHit(w, p)) {
    case WH_FROM:
    case WH_TO:
        w.drag = waveHit(w, p) == WH_FROM ? WD_FROM : WD_TO;
        return true;
    case WH_WAVE:
        w.drag = WD_PICK;
        w.pressX = p.x;
        w.moved = false;
        w.grab = std::max(0.0, std::min(w.length(), w.timeAt(p.x)));
        return true;
    case WH_VIEW: {
        const double t = w.barTime(p.x);
        if (t < w.viewAt || t > w.viewAt + w.viewLen) {
            w.viewAt = t - w.viewLen / 2.0;
            waveClampView(w);
        }
        w.grab = t - w.viewAt;
        w.drag = WD_VIEW;
        return true;
    }
    default:
        return false;
    }
}

static bool waveMove(WaveEdit& w, POINT p, bool* pushed) {
    switch (w.drag) {
    case WD_FROM:
    case WD_TO: {
        const bool hit = waveSet(w, w.drag == WD_FROM, w.timeAt(p.x));
        if (pushed) *pushed = hit;
        return true;
    }
    case WD_PICK:
        if (std::abs(p.x - w.pressX) >= 3) w.moved = true;
        if (w.moved) wavePick(w, p.x);
        return w.moved;
    case WD_VIEW:
        w.viewAt = w.barTime(p.x) - w.grab;
        waveClampView(w);
        return true;
    default:
        return false;
    }
}

static bool waveUp(WaveEdit& w, int x) {
    const bool click = w.drag == WD_PICK && !w.moved;
    if (click && w.trimming) {
        const double t = w.timeAt(x);
        waveSet(w, std::fabs(t - w.from) <= std::fabs(t - w.to), t);
    } else if (click) {
        w.from = 0.0;
        w.to = w.length();
    }
    w.drag = WD_NONE;
    return click;
}

static bool waveEdgeCursor(const WaveEdit& w) {
    return w.drag == WD_FROM || w.drag == WD_TO || (w.drag == WD_NONE && (w.hot == WH_FROM || w.hot == WH_TO));
}

struct WaveMarks {
    const std::vector<size_t>* joins = nullptr;
    double fadeIn = 0.0, fadeOut = 0.0, fadeFrom = 0.0, fadeTo = 0.0, playAt = -1.0;
};

static void drawWaveFades(HDC dc, const WaveEdit& w, const WaveMarks& m, int top, int bottom) {
    double fi = m.fadeIn, fo = m.fadeOut;
    const double span = m.fadeTo - m.fadeFrom;
    if (fi + fo > span && fi + fo > 0.0) {
        const double k = span / (fi + fo);
        fi *= k;
        fo *= k;
    }
    if (fi <= 0.0 && fo <= 0.0) return;
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(gpColor(mixc(C_TEXT, C_GOLD, 0.35), 220), 1.4f);
    const double left = w.wave.left + 1.0, right = w.wave.right - 1.0;
    auto curve = [&](double t0, double t1, bool rising) {
        std::vector<Gdiplus::PointF> pts;
        for (int i = 0; i <= 24; ++i) {
            const double u = i / 24.0, x = std::max(left, std::min(right, w.xOf(t0 + (t1 - t0) * u)));
            const double level = clip::fadeCurve(rising ? u : 1.0 - u);
            pts.push_back(Gdiplus::PointF((float)x, (float)(bottom - level * (bottom - top))));
        }
        gr.DrawLines(&pen, pts.data(), (INT)pts.size());
    };
    if (fi > 0.0) curve(m.fadeFrom, m.fadeFrom + fi, true);
    if (fo > 0.0) curve(m.fadeTo - fo, m.fadeTo, false);
}

static void drawWave(HDC dc, const WaveEdit& w, const WaveMarks& m) {
    const clip::Outline& o = *w.shape;
    const int left = w.wave.left + 1, right = w.wave.right - 1, top = w.wave.top + 1, bottom = w.wave.bottom - 1;
    const int mid = (top + bottom) / 2, half = (bottom - top) / 2 - 4;
    const int xa = std::max(left, std::min(right, (int)std::lround(w.xOf(w.from))));
    const int xb = std::max(left, std::min(right, (int)std::lround(w.xOf(w.to))));
    if (xb > xa) blendRect(dc, RECT{xa, top, xb, bottom}, C_NAVY, 255);
    HPEN axis = linePen(C_EDGE), inside = linePen(C_GOLD), outside = linePen(mixc(C_DIM, C_BACK, 0.45));
    HGDIOBJ old = SelectObject(dc, axis);
    MoveToEx(dc, left, mid, nullptr);
    LineTo(dc, right, mid);
    const float scale = 1.0f / std::max(o.loudest, 0.02f);
    const long long slots = (long long)o.peaks.size();
    const size_t width = (size_t)std::max(0, right - left);
    if (w.colsOf.expired() || w.colsOf.owner_before(w.shape) || w.shape.owner_before(w.colsOf) || w.colsAt != w.viewAt || w.colsLen != w.viewLen ||
        w.cols.size() != width) {
        w.cols.assign(width, -1.0f);
        for (int x = left; x < right; ++x) {
            const long long s0 = std::max(0LL, (long long)std::floor(w.timeAt(x) * o.perSec));
            const long long s1 = std::min(slots, std::max(s0 + 1, (long long)std::ceil(w.timeAt(x + 1) * o.perSec)));
            if (s0 >= slots) break;
            float peak = 0.0f;
            for (long long s = s0; s < s1; ++s) peak = std::max(peak, o.peaks[(size_t)s]);
            w.cols[(size_t)(x - left)] = peak;
        }
        w.colsOf = w.shape;
        w.colsAt = w.viewAt;
        w.colsLen = w.viewLen;
    }
    for (int x = left; x < right; ++x) {
        const float peak = w.cols[(size_t)(x - left)];
        if (peak < 0.0f) break;
        const int h = (int)(std::min(1.0f, peak * scale) * half + 0.5f);
        SelectObject(dc, x >= xa && x < xb ? inside : outside);
        MoveToEx(dc, x, mid - h, nullptr);
        LineTo(dc, x, mid + h + 1);
    }
    SelectObject(dc, old);
    DeleteObject(axis);
    DeleteObject(inside);
    DeleteObject(outside);
    if (m.joins)
        for (size_t at : *m.joins) {
            const int x = (int)std::lround(w.xOf(at / (double)FS));
            if (x > left && x < right) blendRect(dc, RECT{x, top, x + 1, bottom}, mixc(C_DIM, C_BACK, 0.3), 255);
        }
    drawWaveFades(dc, w, m, top + 4, bottom - 4);
    const int tab = std::min(7, (bottom - top) / 6);
    const struct { double t; bool start, hot; } edges[2] = {
        {w.from, true, w.drag == WD_FROM || (w.drag == WD_NONE && w.hot == WH_FROM)},
        {w.to, false, w.drag == WD_TO || (w.drag == WD_NONE && w.hot == WH_TO)}};
    for (const auto& e : edges) {
        const int x = (int)std::lround(w.xOf(e.t));
        if (x < left - 1 || x > right) continue;
        const COLORREF c = e.hot ? mixc(C_GOLD, C_TEXT, 0.45) : C_GOLD;
        blendRect(dc, RECT{x - 1, top, x + 1, bottom}, c, 255);
        blendRect(dc, e.start ? RECT{x - 1, top, x + tab, top + tab} : RECT{x - tab, top, x + 1, top + tab}, c, 255);
        blendRect(dc, e.start ? RECT{x - 1, bottom - tab, x + tab, bottom} : RECT{x - tab, bottom - tab, x + 1, bottom}, c, 255);
    }
    if (m.playAt >= 0.0) {
        const int x = (int)std::lround(w.xOf(m.playAt));
        if (x >= left && x < right) blendRect(dc, RECT{x, top, x + 1, bottom}, C_TEXT, 255);
    }
}

static void drawWaveRuler(HDC dc, const WaveEdit& w) {
    static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
    const double perSec = (w.wave.right - w.wave.left - 2) / w.viewLen;
    double step = steps[15];
    for (double s : steps)
        if (s * perSec >= 70.0) { step = s; break; }
    HPEN tick = linePen(C_EDGE);
    HGDIOBJ old = SelectObject(dc, tick);
    HGDIOBJ of = SelectObject(dc, fntUI);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_DIM);
    for (long long k = (long long)std::ceil(w.viewAt / step - 1e-9); k * step <= w.viewAt + w.viewLen + 1e-9; ++k) {
        const double t = k * step;
        const int x = (int)std::lround(w.xOf(t));
        MoveToEx(dc, x, w.wave.bottom - 5, nullptr);
        LineTo(dc, x, w.wave.bottom - 1);
        const std::wstring text = trimClock(t, step < 1.0 ? 1 : 0);
        SIZE sz{};
        GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
        const int lx = std::max((int)w.ruler.left, std::min((int)w.ruler.right - (int)sz.cx, x - (int)sz.cx / 2));
        TextOutW(dc, lx, w.ruler.top + 1 + liftOf(fntUI), text.c_str(), (int)text.size());
    }
    SelectObject(dc, of);
    SelectObject(dc, old);
    DeleteObject(tick);
}

static void drawWaveBar(HDC dc, const WaveEdit& w) {
    panel(dc, w.bar, C_BACK, C_EDGE);
    const int top = w.bar.top + 1, bottom = w.bar.bottom - 1;
    if (w.zoomed()) {
        const int a = w.barX(w.viewAt), b = std::max(a + 3, w.barX(w.viewAt + w.viewLen));
        PixelSnap px(dc);
        const RECT in = px.inside(w.bar);
        blendRect(dc, RECT{std::max(in.left, px.x((float)a)), in.top, std::min(in.right, px.x((float)b)), in.bottom}, mixc(C_NAVY, C_GOLD, 0.30), 255);
    }
    const int a = w.barX(w.from), b = std::max(a + 2, w.barX(w.to));
    blendRect(dc, RECT{a, top + 2, b, bottom - 2}, C_GOLD, 255);
}

static void valueBox(HDC dc, const RECT& r, const std::wstring& text, bool on, bool hot, bool enabled = true) {
    const double k = enabled ? hotLevelFor(r, hot) : 0.0;
    panel(dc, r, mixc(C_BACK, C_NAVY, k), mixc(C_EDGE, C_GOLD, k), fadeAlpha(k));
    RECT tr = {r.left + 6, r.top, r.right - 4, r.bottom};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, on ? C_TEXT : C_DIM);
    HGDIOBJ of = SelectObject(dc, fntUI);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    const bool fits = sz.cx <= tr.right - tr.left;
    if (!fits) tr = RECT{r.left + 1, r.top, r.right - 1, r.bottom};
    drawText(dc, text.c_str(), -1, &tr, (fits ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

static std::wstring fadeText(double sec) {
    if (sec <= 0.0) return L"Off";
    wchar_t t[16];
    swprintf_s(t, 16, L"%.1f s", sec);
    return t;
}

static std::wstring fadeNote(double fadeIn, double fadeOut) {
    if (fadeIn <= 0.0 && fadeOut <= 0.0) return L"";
    wchar_t t[64];
    if (fadeIn > 0.0 && fadeOut > 0.0) swprintf_s(t, 64, L", fades in %.1f s and out %.1f s", fadeIn, fadeOut);
    else if (fadeIn > 0.0) swprintf_s(t, 64, L", fades in %.1f s", fadeIn);
    else swprintf_s(t, 64, L", fades out %.1f s", fadeOut);
    return t;
}

static double fadeStep(double now, double room, double d) {
    return std::max(0.0, std::min(room, std::round((now + d) * 10.0) / 10.0));
}

static const RECT RT_CLOSE = {TRIM_W - 34, 5, TRIM_W - 8, 29};
static const RECT RT_TOP = {12, 46, TRIM_W - 12, 216};
static const RECT RT_WAVE = {24, 76, TRIM_W - 24, 176};
static const RECT RT_RULER = {24, 178, TRIM_W - 24, 194};
static const RECT RT_VIEW = {24, 198, TRIM_W - 24, 206};
static const RECT RT_LOW = {12, 226, TRIM_W - 12, 348};
static const int RT_COL2 = 262, RT_COL3 = 512;
static const RECT RT_FROM = {80, 240, 160, 264};
static const RECT RT_FROM_BACK = {166, 240, 190, 264};
static const RECT RT_FROM_FWD = {194, 240, 218, 264};
static const RECT RT_TO = {318, 240, 398, 264};
static const RECT RT_TO_BACK = {404, 240, 428, 264};
static const RECT RT_TO_FWD = {432, 240, 456, 264};
static const RECT RT_FIN = {80, 276, 160, 300};
static const RECT RT_FIN_BACK = {166, 276, 190, 300};
static const RECT RT_FIN_FWD = {194, 276, 218, 300};
static const RECT RT_FOUT = {318, 276, 398, 300};
static const RECT RT_FOUT_BACK = {404, 276, 428, 300};
static const RECT RT_FOUT_FWD = {432, 276, 456, 300};
static const RECT RT_PLAY = {24, 312, 124, 336};
static const RECT RT_RESET = {132, 312, 226, 336};
static const RECT RT_CANCEL = {TRIM_W - 212, 312, TRIM_W - 122, 336};
static const RECT RT_SAVE = {TRIM_W - 114, 312, TRIM_W - 24, 336};
static const RECT RT_NOTE = {16, TRIM_H - 30, TRIM_W - 16, TRIM_H - 4};
enum { TH_NONE = -1, TH_CLOSE, TH_FROM_BACK, TH_FROM_FWD, TH_TO_BACK, TH_TO_FWD, TH_FIN_BACK, TH_FIN_FWD, TH_FOUT_BACK, TH_FOUT_FWD, TH_FIN, TH_FOUT,
       TH_PLAY, TH_RESET, TH_CANCEL, TH_SAVE };
static const RECT* const TRIM_BUTTONS[] = {&RT_CLOSE, &RT_FROM_BACK, &RT_FROM_FWD, &RT_TO_BACK, &RT_TO_FWD, &RT_FIN_BACK, &RT_FIN_FWD,
                                           &RT_FOUT_BACK, &RT_FOUT_FWD, &RT_FIN, &RT_FOUT, &RT_PLAY, &RT_RESET, &RT_CANCEL, &RT_SAVE};

struct TrimJob {
    std::atomic<int> permille{0};
    std::atomic<bool> stop{false}, done{false};
    std::shared_ptr<const clip::Outline> outline;
    std::shared_ptr<const clip::Sound> sound;
    std::wstring why;
};

struct TrimState {
    HWND wnd = nullptr, fromBox = nullptr, toBox = nullptr;
    int pad = 0, result = 0, hot = TH_NONE;
    bool mouse = false, playing = false;
    double playFrom = 0.0, fadeIn = 0.0, fadeOut = 0.0;
    ULONGLONG playAt = 0;
    WaveEdit w = {RT_WAVE, RT_RULER, RT_VIEW, BOARD_MOST_SEC};
    std::shared_ptr<TrimJob> load, cue;
    std::wstring path, note;

    double fadeRoom() const { return std::max(0.0, std::min(FADE_MOST_SEC, w.to - w.from)); }
};

static TrimState trim;

static bool trimParse(const std::wstring& text, double& sec) {
    std::wstring s;
    for (wchar_t c : text) {
        if (c >= 0x0660 && c <= 0x0669) c = (wchar_t)(L'0' + (c - 0x0660));
        else if (c >= 0x06F0 && c <= 0x06F9) c = (wchar_t)(L'0' + (c - 0x06F0));
        else if (c == 0x066B || c == L',') c = L'.';
        if (c != L' ') s.push_back(c);
    }
    double total = 0.0;
    size_t start = 0;
    for (int parts = 1; parts <= 3; ++parts) {
        const size_t colon = s.find(L':', start);
        const std::wstring piece = s.substr(start, colon == std::wstring::npos ? std::wstring::npos : colon - start);
        const size_t dots = (size_t)std::count(piece.begin(), piece.end(), L'.');
        if (piece.find_first_of(L"0123456789") == std::wstring::npos || piece.find_first_not_of(L"0123456789.") != std::wstring::npos ||
            dots > 1 || (dots && colon != std::wstring::npos))
            return false;
        total = total * 60.0 + _wtof(piece.c_str());
        if (colon == std::wstring::npos) {
            sec = total;
            return true;
        }
        start = colon + 1;
    }
    return false;
}

static void trimFitFades() {
    trim.fadeIn = std::min(trim.fadeIn, trim.fadeRoom());
    trim.fadeOut = std::min(trim.fadeOut, trim.fadeRoom());
}

static void trimSet(bool start, double t) {
    const bool pushed = waveSet(trim.w, start, t);
    trim.note = pushed ? (start ? L"A part can be up to 30 seconds, so the end moved with it" : L"A part can be up to 30 seconds, so the start moved with it") : L"";
    trimFitFades();
}

static void trimShowTimes() {
    const HWND focus = GetFocus();
    if (trim.fromBox && focus != trim.fromBox) SetWindowTextW(trim.fromBox, trimClock(trim.w.from).c_str());
    if (trim.toBox && focus != trim.toBox) SetWindowTextW(trim.toBox, trimClock(trim.w.to).c_str());
}

static void trimCommit(HWND box) {
    if (!trim.w.ready() || !box || (box != trim.fromBox && box != trim.toBox)) return;
    const bool start = box == trim.fromBox;
    wchar_t buf[32] = L"";
    GetWindowTextW(box, buf, 32);
    double t = 0.0;
    if (trimParse(buf, t)) {
        trimSet(start, t);
        waveReveal(trim.w, start ? trim.w.from : trim.w.to);
    } else {
        trim.note = L"Type the time like 1:05.5 or 65.5";
    }
    SetWindowTextW(trim.fromBox, trimClock(trim.w.from).c_str());
    SetWindowTextW(trim.toBox, trimClock(trim.w.to).c_str());
    InvalidateRect(trim.wnd, nullptr, FALSE);
}

static void trimReset() {
    trim.w.from = 0.0;
    trim.w.to = trim.w.most();
    waveShowAll(trim.w);
    trim.fadeIn = trim.fadeOut = 0.0;
    trim.note.clear();
    trimShowTimes();
}

static std::shared_ptr<TrimJob> trimWork(std::function<void(TrimJob&)> work) {
    auto job = std::make_shared<TrimJob>();
    std::thread([job, work] {
        const MediaWork busy;
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        work(*job);
        if (com) CoUninitialize();
        job->done = true;
    }).detach();
    return job;
}

static void trimStop() {
    if (trim.cue) trim.cue->stop = true;
    trim.cue.reset();
    if (trim.playing) gBoard.stopPad(TRIM_PREVIEW);
    trim.playing = false;
}

static void trimPlay() {
    if (trim.playing || trim.cue) {
        trimStop();
        return;
    }
    trim.playFrom = trim.w.from;
    const std::wstring path = trim.path;
    const double from = trim.w.from, to = trim.w.to, fadeIn = trim.fadeIn, fadeOut = trim.fadeOut;
    trim.cue = trimWork([path, from, to, fadeIn, fadeOut](TrimJob& job) {
        job.sound = clip::withFades(clip::decodeSound(path, FS, BOARD_MOST_SEC, job.why, from, to, &job.stop), FS, fadeIn, fadeOut);
    });
}

static void trimLoaded() {
    const std::shared_ptr<TrimJob> job = trim.load;
    trim.load.reset();
    trim.w.shape = job->outline;
    if (!trim.w.shape) {
        trim.note = job->why + L": " + fileTitle(trim.path);
        return;
    }
    const BoardPad& bp = boardPads[trim.pad];
    waveShowAll(trim.w);
    if (bp.trimmed()) {
        trim.w.to = trim.w.length();
        trimSet(true, bp.from);
        trimSet(false, bp.to);
    } else {
        trim.w.from = 0.0;
        trim.w.to = trim.w.most();
    }
    trim.fadeIn = bp.fadeIn;
    trim.fadeOut = bp.fadeOut;
    trimFitFades();
    trim.note.clear();
    EnableWindow(trim.fromBox, TRUE);
    EnableWindow(trim.toBox, TRUE);
    trimShowTimes();
}

static void trimTick() {
    bool paint = false, wave = false;
    if (trim.load) {
        if (trim.load->done.load()) {
            trimLoaded();
            paint = true;
        } else {
            wave = true;
        }
    }
    if (trim.cue && trim.cue->done.load()) {
        if (trim.cue->sound) {
            gBoard.play(TRIM_PREVIEW, trim.cue->sound);
            trim.playing = true;
            trim.playAt = GetTickCount64();
        } else {
            trim.note = trim.cue->why;
        }
        trim.cue.reset();
        paint = true;
    }
    if (trim.playing) {
        if (!((gBoard.playing() >> TRIM_PREVIEW) & 1u) && GetTickCount64() - trim.playAt > 400) {
            trim.playing = false;
            paint = true;
        } else {
            wave = true;
        }
    }
    if (paint) InvalidateRect(trim.wnd, nullptr, FALSE);
    else if (wave) invalidateDesign(trim.wnd, RT_WAVE);
}

static void trimSave() {
    BoardPad& bp = boardPads[trim.pad];
    const double from = std::llround(trim.w.from * 1000.0) / 1000.0, to = std::llround(trim.w.to * 1000.0) / 1000.0;
    const bool plain = from < 0.0005 && to > trim.w.most() - 0.0005;
    bp.from = plain ? 0.0 : from;
    bp.to = plain ? 0.0 : to;
    bp.fadeIn = std::llround(trim.fadeIn * 1000.0) / 1000.0;
    bp.fadeOut = std::llround(trim.fadeOut * 1000.0) / 1000.0;
    boardDecode({trim.pad});
    boardSave();
    const std::wstring name = L"Pad " + std::to_wstring(trim.pad + 1);
    if (!plain) boardNote = name + L" plays " + trimClock(from) + L" to " + trimClock(to);
    else if (trim.w.length() > BOARD_MOST_SEC) boardNote = name + L" plays the first 30 seconds";
    else boardNote = name + L" plays the whole sound";
    boardNote += fadeNote(bp.fadeIn, bp.fadeOut);
}

static int trimHit(POINT p) {
    for (int k = 0; k < (int)(sizeof TRIM_BUTTONS / sizeof TRIM_BUTTONS[0]); ++k)
        if (PtInRect(TRIM_BUTTONS[k], p)) return TH_CLOSE + k;
    return waveHit(trim.w, p);
}

static void paintTrim(HDC dc, const RECT& rc) {
    boxTitle(dc, rc, L"Trim pad", RT_CLOSE, trim.hot == TH_CLOSE);

    const bool ready = trim.w.ready();
    panel(dc, RT_TOP, C_PANEL, C_EDGE);
    {
        const std::wstring name = L"Pad " + std::to_wstring(trim.pad + 1) + L": " + fileTitle(trim.path);
        RECT nr = {24, RT_TOP.top + 8, RT_COL2 + 142, RT_TOP.top + 24};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_GOLD);
        HGDIOBJ of = SelectObject(dc, fntBold);
        drawText(dc, name.c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (ready) {
            const std::wstring hint = trimClock(trim.w.length(), 1) + L" long, the wheel zooms in";
            RECT hr = {RT_COL2 + 150, RT_TOP.top + 8, RT_TOP.right - 12, RT_TOP.top + 24};
            SelectObject(dc, fntUI);
            SetTextColor(dc, C_DIM);
            drawText(dc, hint.c_str(), -1, &hr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        SelectObject(dc, of);
    }
    panel(dc, RT_WAVE, C_BACK, C_EDGE);
    if (ready) {
        WaveMarks marks;
        marks.fadeIn = trim.fadeIn;
        marks.fadeOut = trim.fadeOut;
        marks.fadeFrom = trim.w.from;
        marks.fadeTo = trim.w.to;
        if (trim.playing) marks.playAt = trim.playFrom + gBoard.position(TRIM_PREVIEW) / (double)FS;
        drawWave(dc, trim.w, marks);
        drawWaveRuler(dc, trim.w);
        drawWaveBar(dc, trim.w);
    } else if (trim.load) {
        wchar_t t[64];
        swprintf_s(t, 64, L"Reading the sound... %d%%", trim.load->permille.load() / 10);
        centredText(dc, RT_WAVE, t, C_DIM, fntUI);
    } else {
        centredText(dc, RT_WAVE, L"This sound can't be read", C_DIM, fntUI);
    }

    panel(dc, RT_LOW, C_PANEL, C_EDGE);
    label(dc, 24, RT_FROM.top + 5, L"Start", C_DIM, fntUI);
    panel(dc, RT_FROM, C_BACK, C_EDGE);
    iconButton(dc, RT_FROM_BACK, ICO_PREV, nullptr, trim.hot == TH_FROM_BACK, ready);
    iconButton(dc, RT_FROM_FWD, ICO_NEXT, nullptr, trim.hot == TH_FROM_FWD, ready);
    label(dc, RT_COL2, RT_TO.top + 5, L"End", C_DIM, fntUI);
    panel(dc, RT_TO, C_BACK, C_EDGE);
    iconButton(dc, RT_TO_BACK, ICO_PREV, nullptr, trim.hot == TH_TO_BACK, ready);
    iconButton(dc, RT_TO_FWD, ICO_NEXT, nullptr, trim.hot == TH_TO_FWD, ready);
    label(dc, RT_COL3, RT_TO.top + 5, L"Length", C_DIM, fntUI);
    if (ready) {
        wchar_t len[32];
        swprintf_s(len, 32, L"%.2f s", std::llround((trim.w.to - trim.w.from) * 100.0) / 100.0);
        label(dc, RT_COL3 + 52, RT_TO.top + 4, len, C_TEXT, fntBold);
    }
    label(dc, 24, RT_FIN.top + 5, L"Fade in", C_DIM, fntUI);
    valueBox(dc, RT_FIN, fadeText(trim.fadeIn), ready && trim.fadeIn > 0.0, ready && trim.hot == TH_FIN);
    iconButton(dc, RT_FIN_BACK, ICO_PREV, nullptr, trim.hot == TH_FIN_BACK, ready);
    iconButton(dc, RT_FIN_FWD, ICO_NEXT, nullptr, trim.hot == TH_FIN_FWD, ready);
    label(dc, RT_COL2, RT_FOUT.top + 5, L"Fade out", C_DIM, fntUI);
    valueBox(dc, RT_FOUT, fadeText(trim.fadeOut), ready && trim.fadeOut > 0.0, ready && trim.hot == TH_FOUT);
    iconButton(dc, RT_FOUT_BACK, ICO_PREV, nullptr, trim.hot == TH_FOUT_BACK, ready);
    iconButton(dc, RT_FOUT_FWD, ICO_NEXT, nullptr, trim.hot == TH_FOUT_FWD, ready);
    const bool busy = trim.cue != nullptr;
    iconButton(dc, RT_PLAY, trim.playing || busy ? ICO_STOP : ICO_PLAY, trim.playing ? L"Stop" : (busy ? L"Loading..." : L"Play part"),
               trim.hot == TH_PLAY, ready, trim.playing);
    iconButton(dc, RT_RESET, ICO_RESET, L"Reset", trim.hot == TH_RESET, ready);
    button(dc, RT_CANCEL, L"Cancel", trim.hot == TH_CANCEL);
    primaryButton(dc, RT_SAVE, ICO_NONE, L"Save", trim.hot == TH_SAVE, ready);

    const RECT status = {0, rc.bottom - STATUS_H, rc.right, rc.bottom};
    blendRect(dc, status, C_PANEL, (BYTE)alphaFor(C_PANEL));
    HPEN edge = linePen(C_EDGE);
    HGDIOBJ oldEdge = SelectObject(dc, edge);
    MoveToEx(dc, 0, status.top, nullptr);
    LineTo(dc, rc.right, status.top);
    SelectObject(dc, oldEdge);
    DeleteObject(edge);
    const std::wstring note = trim.note.empty() ? L"Drag an edge or type a time. The arrows move an edge by 0.1 s, with Shift by 1 s" : trim.note;
    RECT nr = RT_NOTE;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, trim.note.empty() ? C_DIM : C_TEXT);
    HGDIOBJ of = SelectObject(dc, fntUI);
    drawText(dc, note.c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    boxEdge(dc);
}

static void trimAction(int hit) {
    const bool ready = trim.w.ready();
    const double step = GetKeyState(VK_SHIFT) < 0 ? 1.0 : TRIM_NUDGE_SEC;
    switch (hit) {
    case TH_CLOSE:
    case TH_CANCEL:
        trim.result = 2;
        return;
    case TH_SAVE:
        if (ready) trim.result = 1;
        return;
    case TH_PLAY:
        if (ready) trimPlay();
        break;
    case TH_RESET:
        if (ready) trimReset();
        break;
    case TH_FROM_BACK:
    case TH_FROM_FWD:
        if (ready) {
            trimSet(true, trim.w.from + (hit == TH_FROM_FWD ? step : -step));
            waveReveal(trim.w, trim.w.from);
            trimShowTimes();
        }
        break;
    case TH_TO_BACK:
    case TH_TO_FWD:
        if (ready) {
            trimSet(false, trim.w.to + (hit == TH_TO_FWD ? step : -step));
            waveReveal(trim.w, trim.w.to);
            trimShowTimes();
        }
        break;
    case TH_FIN_BACK:
    case TH_FIN_FWD:
        if (ready) trim.fadeIn = fadeStep(trim.fadeIn, trim.fadeRoom(), hit == TH_FIN_FWD ? step : -step);
        break;
    case TH_FOUT_BACK:
    case TH_FOUT_FWD:
        if (ready) trim.fadeOut = fadeStep(trim.fadeOut, trim.fadeRoom(), hit == TH_FOUT_FWD ? step : -step);
        break;
    case TH_FIN:
        if (ready) trim.fadeIn = fadeStep(0.0, trim.fadeRoom(), trim.fadeIn > 0.0 ? 0.0 : 1.0);
        break;
    case TH_FOUT:
        if (ready) trim.fadeOut = fadeStep(0.0, trim.fadeRoom(), trim.fadeOut > 0.0 ? 0.0 : 1.0);
        break;
    default:
        break;
    }
    InvalidateRect(trim.wnd, nullptr, FALSE);
}

static LRESULT CALLBACK trimProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PRINTCLIENT:
        printBox(h, (HDC)w, paintTrim);
        return 0;
    case WM_CREATE: {
        trim.wnd = h;
        HWND* boxes[2] = {&trim.fromBox, &trim.toBox};
        const RECT* places[2] = {&RT_FROM, &RT_TO};
        for (int i = 0; i < 2; ++i) {
            const RECT& r = *places[i];
            *boxes[i] = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | ES_AUTOHSCROLL,
                                      sc(r.left + 5), sc(r.top + 4), sc(r.right - 5) - sc(r.left + 5), sc(r.bottom - 4) - sc(r.top + 4), h, (HMENU)(INT_PTR)(i + 1), nullptr, nullptr);
            SendMessageW(*boxes[i], WM_SETFONT, (WPARAM)fntCtl, TRUE);
            SendMessageW(*boxes[i], EM_SETLIMITTEXT, 16, 0);
            noTheme(*boxes[i]);
        }
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)w, m == WM_CTLCOLOREDIT ? C_TEXT : C_DIM);
        SetBkColor((HDC)w, C_BACK);
        if (!brLookBack) lookBrushes();
        return (LRESULT)brLookBack;
    case WM_COMMAND:
        if (HIWORD(w) == EN_KILLFOCUS) trimCommit((HWND)l);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paintBox(h, paintTrim);
        return 0;
    case WM_TIMER:
        trimTick();
        return 0;
    case WM_MOUSEMOVE: {
        if (!trim.mouse) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            trim.mouse = true;
        }
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (trim.w.drag != WD_NONE) {
            bool pushed = false;
            if (waveMove(trim.w, p, &pushed)) {
                trim.note = pushed ? L"A part can be up to 30 seconds, so the other edge moved with it" : L"";
                trimFitFades();
                trimShowTimes();
            }
        } else {
            const int hit = trimHit(p);
            if (hit == trim.hot) return 0;
            trim.hot = hit;
            trim.w.hot = hit;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        trim.mouse = false;
        trim.hot = trim.w.hot = TH_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if ((HWND)w == h && LOWORD(l) == HTCLIENT && waveEdgeCursor(trim.w)) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        SetFocus(h);
        const int hit = trimHit(p);
        trim.hot = trim.w.hot = hit;
        if (waveDown(trim.w, p)) {
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (hit == TH_NONE) {
            if (p.y < TITLE_H) {
                ReleaseCapture();
                SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }
        trimAction(hit);
        return 0;
    }
    case WM_LBUTTONUP:
        if (trim.w.drag != WD_NONE) {
            if (waveUp(trim.w, (short)LOWORD(l))) {
                trim.note.clear();
                trimFitFades();
            }
            trimShowTimes();
            ReleaseCapture();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        trim.w.drag = WD_NONE;
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = wheelPoint(h, l);
        const double notches = GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA;
        if (!trim.w.ready()) return 0;
        if (PtInRect(&RT_WAVE, p)) waveZoom(trim.w, p.x, notches);
        else if (PtInRect(&RT_FIN, p)) trim.fadeIn = fadeStep(trim.fadeIn, trim.fadeRoom(), notches * TRIM_NUDGE_SEC);
        else if (PtInRect(&RT_FOUT, p)) trim.fadeOut = fadeStep(trim.fadeOut, trim.fadeRoom(), notches * TRIM_NUDGE_SEC);
        else return 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        trim.result = 2;
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        trim.wnd = trim.fromBox = trim.toBox = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void trimPad(HWND owner, int pad) {
    RECT o;
    GetWindowRect(owner, &o);
    trim = TrimState{};
    trim.pad = pad;
    trim.path = boardPads[pad].path;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"OctopusTrim", L"Trim pad", WS_POPUP | WS_CLIPCHILDREN,
                             (o.left + o.right - sc(TRIM_W)) / 2, (o.top + o.bottom - sc(TRIM_H)) / 2, sc(TRIM_W), sc(TRIM_H),
                             owner, nullptr, nullptr, nullptr);
    if (!h) return;
    const std::wstring path = trim.path;
    trim.load = trimWork([path](TrimJob& job) { job.outline = clip::readOutline(path, FS, TRIM_PER_SEC, job.permille, job.stop, job.why); });
    SetTimer(h, 1, 30, nullptr);
    EnableWindow(owner, FALSE);
    const bool mainOff = hwndMain && owner != hwndMain && IsWindowEnabled(hwndMain);
    if (mainOff) EnableWindow(hwndMain, FALSE);
    boxShow(h);
    SetForegroundWindow(h);
    SetFocus(h);
    MSG msg{};
    bool quit = false;
    while (trim.result == 0 && trim.wnd) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            quit = got == 0;
            break;
        }
        if (msg.message == WM_KEYDOWN && GetAncestor(msg.hwnd, GA_ROOT) == trim.wnd) {
            const bool inBox = msg.hwnd == trim.fromBox || msg.hwnd == trim.toBox;
            if (msg.wParam == VK_ESCAPE) {
                trim.result = 2;
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                if (inBox) {
                    trimCommit(msg.hwnd);
                    SendMessageW(msg.hwnd, EM_SETSEL, 0, -1);
                } else if (trim.w.ready()) {
                    trim.result = 1;
                }
                continue;
            }
            if (msg.wParam == VK_SPACE && !inBox && trim.w.ready()) {
                if (msg.lParam & 0x40000000) continue;
                trimPlay();
                InvalidateRect(trim.wnd, nullptr, FALSE);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    const bool save = trim.result == 1 && trim.wnd && trim.w.ready();
    trimStop();
    if (trim.load) trim.load->stop = true;
    if (save) trimSave();
    if (mainOff && IsWindow(hwndMain)) EnableWindow(hwndMain, TRUE);
    EnableWindow(owner, TRUE);
    if (trim.wnd) boxClose(trim.wnd);
    trim = TrimState{};
    if (quit) PostQuitMessage((int)msg.wParam);
    else if (IsWindow(owner)) SetForegroundWindow(owner);
}

static void boardStopListening() {
    if (!boardListening) return;
    boardListening = false;
    boardListenStop = false;
    hotkeysRegister();
}

static void boardRenameEnd(bool keep) {
    if (!boardEdit) return;
    const HWND e = boardEdit;
    boardEdit = nullptr;
    if (keep && boardEditPad >= 0 && boardEditPad < BOARD_PADS && !boardPads[boardEditPad].path.empty()) {
        wchar_t t[64] = L"";
        GetWindowTextW(e, t, 64);
        std::wstring s = t;
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
        BoardPad& bp = boardPads[boardEditPad];
        bp.name = s == fileTitle(bp.path) ? std::wstring() : s;
        boardNote = L"Pad " + std::to_wstring(boardEditPad + 1) + (bp.name.empty() ? L" shows its file name" : L" is called " + bp.name);
        boardSave();
    }
    boardEditPad = -1;
    DestroyWindow(e);
    if (hBoard) {
        if (GetForegroundWindow() == hBoard) SetFocus(hBoard);
        InvalidateRect(hBoard, nullptr, FALSE);
    }
}

static LRESULT CALLBACK boardEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
        boardRenameEnd(w == VK_RETURN);
        return 0;
    }
    if (m == WM_CHAR && (w == L'\r' || w == 27)) return 0;
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DefSubclassProc(h, m, w, l);
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, boardEditProc, 1);
    return DefSubclassProc(h, m, w, l);
}

static void boardRenameStart(HWND h) {
    boardStopListening();
    const BoardPad& bp = boardPads[boardPad];
    if (bp.path.empty()) {
        boardNote = L"Put a sound on the pad first";
        return;
    }
    boardRenameEnd(true);
    const RECT r = boardPadRect(boardPad);
    boardEdit = CreateWindowExW(0, L"EDIT", padTitle(bp).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                sc(r.left + 26), sc(r.top + 5), sc(r.right - 6) - sc(r.left + 26), sc(20), h, (HMENU)(INT_PTR)ID_BOARDEDIT, nullptr, nullptr);
    if (!boardEdit) return;
    boardEditPad = boardPad;
    SendMessageW(boardEdit, WM_SETFONT, (WPARAM)fntCtlBold, TRUE);
    SendMessageW(boardEdit, EM_SETLIMITTEXT, 40, 0);
    SendMessageW(boardEdit, EM_SETSEL, 0, -1);
    SetWindowSubclass(boardEdit, boardEditProc, 1, 0);
    SetFocus(boardEdit);
    boardNote = L"Type a name for pad " + std::to_wstring(boardPad + 1) + L" and press Enter (Esc cancels, empty shows the file name)";
}

static void boardAction(HWND h, int hit) {
    switch (hit) {
    case BH_CLOSE:
        boxClose(h);
        return;
    case BH_ADD:
        boardAdd(h);
        break;
    case BH_REMOVE:
        if (boardRow >= 0 && boardRow < (int)boardFiles.size()) {
            boardFiles.erase(boardFiles.begin() + boardRow);
            if (boardRow >= (int)boardFiles.size()) boardRow = (int)boardFiles.size() - 1;
            boardTop = std::max(0, std::min(boardTop, (int)boardFiles.size() - BOARD_ROWS));
            boardSave();
        }
        break;
    case BH_PUT:
        boardPut();
        break;
    case BH_TRIM:
        boardStopListening();
        if (!boardPads[boardPad].path.empty()) trimPad(h, boardPad);
        break;
    case BH_RENAME:
        boardRenameStart(h);
        break;
    case BH_KEY:
    case BH_STOPKEY: {
        const bool stop = hit == BH_STOPKEY, start = !(boardListening && boardListenStop == stop);
        boardStopListening();
        if (start) {
            boardListening = true;
            boardListenStop = stop;
            hotkeysUnregister();
            boardNote = stop ? L"Press a key or a combination that stops every playing sound (Esc cancels, Backspace removes)"
                             : L"Press a key or a combination for pad " + std::to_wstring(boardPad + 1) + L" (Esc cancels, Backspace removes)";
            SetFocus(h);
        } else {
            boardNote.clear();
        }
        break;
    }
    case BH_CLEAR:
        boardPads[boardPad] = BoardPad{};
        hotkeysUnregister();
        hotkeysRegister();
        boardNote = L"Pad " + std::to_wstring(boardPad + 1) + L" is empty";
        boardSave();
        break;
    case BH_STOP:
        gBoard.stopAll();
        break;
    default:
        if (hit >= 0 && hit < BOARD_PADS) {
            boardStopListening();
            boardPad = hit;
            if (boardPads[hit].path.empty()) boardNote = L"Pick a sound in the list, then press Put on pad";
            else {
                boardNote.clear();
                boardPlay(hit);
            }
        }
        break;
    }
    InvalidateRect(h, nullptr, FALSE);
}

static LRESULT CALLBACK boardProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        printBox(h, (HDC)w, paintBoard);
        return 0;
    case WM_PAINT:
        paintBox(h, paintBoard);
        return 0;
    case WM_TIMER:
        if (w == BOARD_GLIDE_TIMER) {
            boardGlideStep(h);
            return 0;
        }
        if (gBoard.playing() != boardLit) {
            boardLit = gBoard.playing();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (!boardMouse) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            boardMouse = true;
        }
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (boardDragOff >= 0) {
            const RECT track = boardTrack(), thumb = boardThumb(0.0);
            const int room = std::max(1, (int)(track.bottom - track.top) - (int)(thumb.bottom - thumb.top));
            const int most = std::max(0, (int)boardFiles.size() - BOARD_ROWS);
            const int top = std::max(0, std::min(most, (int)std::lround((double)(p.y - boardDragOff - track.top) * most / room)));
            if (top != boardTop) {
                boardTop = top;
                boardShown = top;
                invalidateDesign(h, RB_LIST);
            }
            return 0;
        }
        const int hit = boardHit(p);
        if (hit != boardHot) {
            boardHot = hit;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        boardMouse = false;
        boardHot = BH_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (boardDragOff >= 0) {
            boardDragOff = -1;
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (boardDragOff >= 0) {
            boardDragOff = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        boardRenameEnd(true);
        if (PtInRect(&RB_LIST, p) && boardScrolls() && p.x >= boardTrack().left - 1) {
            const RECT thumb = boardThumb(boardTop);
            if (p.y >= thumb.top && p.y < thumb.bottom) {
                boardDragOff = (int)(p.y - thumb.top);
                SetCapture(h);
            } else {
                const int most = std::max(0, (int)boardFiles.size() - BOARD_ROWS);
                boardTop = std::max(0, std::min(most, boardTop + (p.y < thumb.top ? 1 - BOARD_ROWS : BOARD_ROWS - 1)));
                boardGlide(h);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&RB_LIST, p)) {
            const int idx = boardRowAt(p);
            if (idx >= 0 && idx < (int)boardFiles.size()) {
                boardRow = idx;
                if (m == WM_LBUTTONDBLCLK) boardPut();
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        const int hit = boardHit(p);
        if (m == WM_LBUTTONDBLCLK && !(hit >= 0 && hit < BOARD_PADS)) return 0;
        if (hit == BH_NONE) {
            if (p.y < 34) {
                ReleaseCapture();
                SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }
        boardAction(h, hit);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p = wheelPoint(h, l);
        if (PtInRect(&RB_LIST, p)) {
            const int most = std::max(0, (int)boardFiles.size() - BOARD_ROWS);
            boardTop = std::max(0, std::min(most, boardTop - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA));
            boardGlide(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (boardListening) {
            if (w == VK_SHIFT || w == VK_CONTROL || w == VK_MENU || w == VK_LWIN || w == VK_RWIN) return 0;
            boardKey((UINT)w);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_ESCAPE) { boxClose(h); return 0; }
        break;
    case WM_SYSCHAR:
        if (boardListening) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && boardListening) {
            boardStopListening();
            boardNote.clear();
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == ID_BOARDEDIT && HIWORD(w) == EN_KILLFOCUS) boardRenameEnd(true);
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, C_BACK);
        if (!brLookBack) lookBrushes();
        return (LRESULT)brLookBack;
    case WM_DESTROY:
        KillTimer(h, 1);
        boardEdit = nullptr;
        boardEditPad = -1;
        boardStopListening();
        hBoard = nullptr;
        boardMouse = false;
        boardHot = BH_NONE;
        boardDragOff = -1;
        boardGliding = false;
        boardShown = boardTop;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void openBoard(HWND owner) {
    if (hBoard) {
        ShowWindow(hBoard, SW_SHOW);
        SetForegroundWindow(hBoard);
        return;
    }
    RECT o;
    GetWindowRect(owner, &o);
    hBoard = CreateWindowExW(WS_EX_TOOLWINDOW, L"OctopusBoard", L"Soundboard", WS_POPUP | WS_CLIPCHILDREN,
                             (o.left + o.right - sc(BOARD_W)) / 2, (o.top + o.bottom - sc(BOARD_H)) / 2, sc(BOARD_W), sc(BOARD_H),
                             owner, nullptr, nullptr, nullptr);
    if (!hBoard) return;
    SetTimer(hBoard, 1, 80, nullptr);
    boxShow(hBoard);
    SetForegroundWindow(hBoard);
}

static const int REC_Y = CLIP_Y + 296;
static const RECT R_REC_PANEL = {CX + 12, REC_Y, CX + COL_W - 12, CLIP_Y + CLIP_H};
static const RECT R_REC_FROM_VOICE = {CX + 24, REC_Y + 6, CX + 50, REC_Y + 30};
static const RECT R_REC_FROM_HEAR = {CX + 52, REC_Y + 6, CX + 78, REC_Y + 30};
static const RECT R_REC_GO = {CX + 84, REC_Y + 6, CX + 158, REC_Y + 30};
static const RECT R_REC_TIME = {CX + 162, REC_Y + 6, CX + 206, REC_Y + 30};
static const RECT R_REC_LAST_PREV = {CX + 212, REC_Y + 6, CX + 228, REC_Y + 30};
static const RECT R_REC_LAST = {CX + 230, REC_Y + 6, CX + 328, REC_Y + 30};
static const RECT R_REC_LAST_NEXT = {CX + 330, REC_Y + 6, CX + 346, REC_Y + 30};
static const RECT R_REC_LAST_QUIET = {CX + 350, REC_Y + 6, CX + 376, REC_Y + 30};
static const RECT R_REC_FX = {CX + 388, REC_Y + 6, CX + 414, REC_Y + 30};
static const RECT R_REC_LISTEN = {CX + 420, REC_Y + 6, CX + 446, REC_Y + 30};
static const RECT R_REC_SEND = {CX + 452, REC_Y + 6, CX + 478, REC_Y + 30};
static const RECT R_REC_SAVE = {CX + 484, REC_Y + 6, CX + 510, REC_Y + 30};
static const RECT R_REC_DROP = {CX + 516, REC_Y + 6, CX + 542, REC_Y + 30};
static const RECT R_REC_WAVE = {CX + 24, REC_Y + 36, CX + 542, REC_Y + 64};
static const RECT R_REC_BAR = {CX + 24, REC_Y + 66, CX + 542, REC_Y + 70};
static const RECT R_REC_CUT = {CX + 24, REC_Y + 74, CX + 104, REC_Y + 96};
static const RECT R_REC_KEEP = {CX + 110, REC_Y + 74, CX + 182, REC_Y + 96};
static const RECT R_REC_UNDO = {CX + 188, REC_Y + 74, CX + 214, REC_Y + 96};
static const RECT R_REC_REDO = {CX + 218, REC_Y + 74, CX + 244, REC_Y + 96};
static const RECT R_REC_QUIET = {CX + 248, REC_Y + 74, CX + 274, REC_Y + 96};
static const RECT R_REC_FIN_PREV = {CX + 330, REC_Y + 74, CX + 346, REC_Y + 96};
static const RECT R_REC_FIN = {CX + 348, REC_Y + 74, CX + 386, REC_Y + 96};
static const RECT R_REC_FIN_NEXT = {CX + 388, REC_Y + 74, CX + 404, REC_Y + 96};
static const RECT R_REC_FOUT_PREV = {CX + 468, REC_Y + 74, CX + 484, REC_Y + 96};
static const RECT R_REC_FOUT = {CX + 486, REC_Y + 74, CX + 524, REC_Y + 96};
static const RECT R_REC_FOUT_NEXT = {CX + 526, REC_Y + 74, CX + 542, REC_Y + 96};
static const double RECORD_MOST_SEC = 600.0, RECORD_LEAST_SEC = 0.2, LIVE_VIEW_SEC = 10.0, FX_MOST_SEC = 1800.0;
static const UINT WM_TAKE_SAVED = WM_APP + 33;
enum { TAKE_QUIET, TAKE_EAR, TAKE_MIC };

struct Take {
    WaveEdit w = {R_REC_WAVE, RECT{0, 0, 0, 0}, R_REC_BAR, 0.0, false};
    clip::SoundEdit edit;
    double fadeIn = 0.0, fadeOut = 0.0, playFrom = 0.0;
    int playing = TAKE_QUIET;
    bool working = false;
    std::shared_ptr<const clip::Sound> fxDry, fxWet;
    ULONGLONG playAt = 0, noteAt = 0;
    std::wstring note;

    double fadeRoom() const { return std::max(0.0, std::min(FADE_MOST_SEC, w.length())); }
};

static Take take;
static bool takeSaving = false;
static Take dropped;
static bool droppedHeld = false;
static std::vector<short> recTake;
static std::vector<float> livePeaks;

static size_t recordMost() { return (size_t)(RECORD_MOST_SEC * FS) * 2; }
static size_t takeFrame(double t) { return (size_t)std::llround(std::max(0.0, std::min(t, take.w.length())) * FS); }
static size_t fadeFrames(double sec) { return (size_t)std::llround(std::max(0.0, sec) * FS); }
static bool recVoice() { return (recFrom & FROM_VOICE) != 0; }
static bool recHear() { return (recFrom & FROM_HEAR) != 0; }
static bool recBoth() { return recVoice() && recHear(); }
static clip::Recorder& liveRec() { return recHear() && !recVoice() ? gRecHear : gRec; }
static short mixAdd(short a, short b) { int s = (int)a + (int)b; return (short)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s)); }
static std::vector<short> mixPendV, mixPendH;
static void recMixDrain(bool last) {
    gRec.drain(mixPendV, SIZE_MAX);
    gRecHear.drain(mixPendH, SIZE_MAX);
    const size_t cap = recordMost();
    const size_t n = std::min(mixPendV.size(), mixPendH.size()) & ~(size_t)1;
    for (size_t i = 0; i < n && recTake.size() < cap; ++i) recTake.push_back(mixAdd(mixPendV[i], mixPendH[i]));
    mixPendV.erase(mixPendV.begin(), mixPendV.begin() + n);
    mixPendH.erase(mixPendH.begin(), mixPendH.begin() + n);
    if (last) {
        const std::vector<short>& tail = mixPendV.size() > mixPendH.size() ? mixPendV : mixPendH;
        for (size_t i = 0; i < tail.size() && recTake.size() < cap; ++i) recTake.push_back(tail[i]);
        mixPendV.clear();
        mixPendH.clear();
    }
}
static bool recording() { return gRec.on() || gRecHear.on(); }
static bool takeReady() { return take.w.ready() && !recording() && !take.working; }

static clip::Backlog backlog;
static std::vector<short> backDrain;
static ULONGLONG keepChangedAt = 0;

static size_t keepFrames() { return (size_t)keepSec * FS; }

static int keepIndex() {
    for (int i = 0; i < NUM_KEEP_STEPS; ++i)
        if (KEEP_STEPS[i] >= keepSec) return i;
    return NUM_KEEP_STEPS - 1;
}

static std::wstring keepText(int sec) {
    wchar_t t[32];
    if (sec < 60) swprintf_s(t, 32, L"%d s", sec);
    else if (sec % 60 == 0) swprintf_s(t, 32, L"%d min", sec / 60);
    else swprintf_s(t, 32, L"%d:%02d", sec / 60, sec % 60);
    return t;
}

static std::wstring hearName() {
    std::wstring id;
    {
        std::lock_guard<std::mutex> lk(g.devMx);
        id = g.spkId;
    }
    const std::wstring name = renderName(id);
    return name.empty() ? L"your speakers" : name;
}

static void hearTapSync() {
    const bool want = recHear() && (gRecHear.on() || keepSec > 0);
    if (want && !gHearTap.running()) {
        gHearTap.start(FS, [] {
            std::lock_guard<std::mutex> lk(g.devMx);
            return g.spkId;
        }, &gRecHear, &gBackHear);
    } else if (!want && gHearTap.running()) {
        gHearTap.stop();
    }
}

static void backlogSync() {
    const bool want = keepSec > 0;
    const bool wantV = want && recVoice(), wantH = want && recHear();
    if (wantV && !gBackVoice.on()) gBackVoice.start();
    if (!wantV && gBackVoice.on()) gBackVoice.stop();
    if (wantH && !gBackHear.on()) gBackHear.start();
    if (!wantH && gBackHear.on()) gBackHear.stop();
    if (!want && backlog.capacity()) backlog.resize(0);
}

static std::vector<short> backPendV, backPendH;
static void backlogTick() {
    if (keepSec > 0 && backlog.capacity() != keepFrames() && GetTickCount64() - keepChangedAt >= 400) backlog.resize(keepFrames());
    backDrain.clear();
    if (recBoth()) {
        gBackVoice.drain(backPendV, SIZE_MAX);
        gBackHear.drain(backPendH, SIZE_MAX);
        const size_t n = std::min(backPendV.size(), backPendH.size()) & ~(size_t)1;
        for (size_t i = 0; i < n; ++i) backDrain.push_back(mixAdd(backPendV[i], backPendH[i]));
        backPendV.erase(backPendV.begin(), backPendV.begin() + n);
        backPendH.erase(backPendH.begin(), backPendH.begin() + n);
    } else {
        (recHear() ? gBackHear : gBackVoice).drain(backDrain, SIZE_MAX);
    }
    backlog.push(backDrain.data(), backDrain.size() / 2);
}

static std::wstring timeTitle(const wchar_t* what) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t s[64];
    swprintf_s(s, 64, L"%ls %04u-%02u-%02u %02u-%02u-%02u", what, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

static void takeRefresh(HWND h) { invalidateDesign(h, R_REC_PANEL); }

static const RECT R_STATUS_LINE = {0, WIN_H - STATUS_H, WIN_W, WIN_H};

static bool takeNoteShown() { return !take.note.empty() && (recording() || GetTickCount64() - take.noteAt < 6000); }

static void takeSay(const std::wstring& note) {
    take.note = note;
    take.noteAt = GetTickCount64();
    if (hwndMain) invalidateDesign(hwndMain, R_STATUS_LINE);
}

static void takeStop() {
    gTakeEar.stopPad(0);
    gTakeMic.stopPad(0);
    take.playing = TAKE_QUIET;
}

static void takeFitFades() {
    take.fadeIn = std::min(take.fadeIn, take.fadeRoom());
    take.fadeOut = std::min(take.fadeOut, take.fadeRoom());
}

static void livePeaksGrow() {
    const size_t step = (size_t)(FS / TRIM_PER_SEC), frames = recTake.size() / 2;
    for (size_t at = livePeaks.size() * step; at + step <= frames; at += step) {
        int peak = 0;
        for (size_t i = at; i < at + step; ++i) peak = std::max(peak, std::max(std::abs((int)recTake[2 * i]), std::abs((int)recTake[2 * i + 1])));
        livePeaks.push_back(peak / 32768.0f);
    }
}

static void recordStart(HWND h) {
    takeStop();
    recTake.clear();
    recTake.reserve((size_t)FS * 2 * 30);
    livePeaks.clear();
    mixPendV.clear();
    mixPendH.clear();
    if (recVoice()) gRec.start();
    if (recHear()) gRecHear.start();
    hearTapSync();
    if (recBoth())
        takeSay(L"Recording your voice and what plays on " + hearName() + L" together. Press Stop when you are done");
    else if (recHear())
        takeSay(gHearTap.state() == clip::HearTap::FAILED ? L"Can't listen to " + hearName() + L", pick other speakers"
                                                          : L"Recording what plays on " + hearName() + L". Press Stop when you are done");
    else
        takeSay(g.running.load() ? L"Recording your voice as others hear it. Press Stop when you are done"
                                 : L"Recording, but no sound is coming from the microphone yet");
    takeRefresh(h);
}

static bool takeQuiet(const std::vector<short>& pcm) {
    for (short s : pcm)
        if (std::abs((int)s) >= 33) return false;
    return true;
}

static unsigned takeGen = 0;

static void takeAdopt(std::vector<short>& pcm) {
    takeStop();
    ++takeGen;
    auto sound = std::make_shared<clip::Sound>();
    sound->pcm.swap(pcm);
    take = Take{};
    dropped = Take{};
    droppedHeld = false;
    take.edit.load(sound, (size_t)(FS / TRIM_PER_SEC));
    take.w.shape = take.edit.outline(FS);
    take.w.from = 0.0;
    take.w.to = take.w.length();
    waveShowAll(take.w);
    takeHeld = true;
}

static std::wstring nothingHeard(const std::wstring& when) {
    return recBoth() ? L"Nothing came through " + when : (recHear() ? L"Nothing played on " + hearName() + L" " + when : L"No voice came through " + when + L", check the microphone");
}

static void recordStop(HWND h) {
    if (recVoice()) gRec.stop();
    if (recHear()) gRecHear.stop();
    if (recBoth()) recMixDrain(true);
    else liveRec().drain(recTake, recordMost());
    hearTapSync();
    const size_t frames = recTake.size() / 2;
    livePeaks.clear();
    if (frames < (size_t)(RECORD_LEAST_SEC * FS)) {
        takeSay(L"That recording was too short to keep");
    } else if (takeQuiet(recTake)) {
        takeSay(nothingHeard(L"while recording"));
    } else {
        try {
            recTake.shrink_to_fit();
        } catch (const std::bad_alloc&) {
        }
        takeAdopt(recTake);
        takeSay(L"Recorded " + trimClock(take.w.length(), 1) + L". Drag across the wave to pick a part to cut, or press Listen");
    }
    std::vector<short>().swap(recTake);
    takeRefresh(h);
}


static void recordFrom(HWND h, int bit) {
    if (recording()) return;
    const int next = recFrom ^ bit;
    if (next == 0) {
        takeSay(L"Keep at least one on: your voice or what you hear");
        takeRefresh(h);
        return;
    }
    recFrom = next;
    settingsSave();
    backlog.clear();
    backPendV.clear();
    backPendH.clear();
    backlogSync();
    hearTapSync();
    if (recBoth())
        takeSay(L"Records your voice and what plays on " + hearName() + L" together");
    else if (recHear())
        takeSay(L"Records what plays on " + hearName() + L", like a recording of your speakers");
    else
        takeSay(L"Records your voice as others hear it");
    takeRefresh(h);
}

static void keepStep(HWND h, int d) {
    const int i = std::max(0, std::min(NUM_KEEP_STEPS - 1, keepIndex() + d));
    if (recording() || KEEP_STEPS[i] == keepSec) return;
    keepSec = KEEP_STEPS[i];
    keepChangedAt = GetTickCount64();
    settingsSave();
    backlogSync();
    hearTapSync();
    takeSay(keepSec ? L"Keeps the last " + keepText(keepSec) + L". Press Last to put it in the editor" : L"Keeps nothing, Last is off");
    takeRefresh(h);
}

static void takeReshape(HWND h, bool whole, const std::wstring& note) {
    takeStop();
    take.w.shape = take.edit.outline(FS);
    take.w.from = 0.0;
    take.w.to = take.w.length();
    if (whole) {
        waveShowAll(take.w);
    } else {
        take.w.viewLen = std::min(take.w.viewLen, take.w.length());
        waveClampView(take.w);
    }
    takeFitFades();
    takeSay(note);
    takeRefresh(h);
}

static void takeCut(HWND h, bool keep) {
    if (!takeReady()) return;
    const size_t a = takeFrame(take.w.from), b = takeFrame(take.w.to);
    const double part = (b - a) / (double)FS;
    if (!(keep ? take.edit.keep(a, b) : take.edit.cut(a, b))) {
        takeSay(keep ? L"Drag across the wave to pick the part to keep first" : L"Drag across the wave to pick the part to cut out first");
        takeRefresh(h);
        return;
    }
    wchar_t t[24];
    swprintf_s(t, 24, L"%.2f s", part);
    takeReshape(h, keep, keep ? std::wstring(L"Kept ") + t + L", Undo brings the rest back" : std::wstring(L"Cut out ") + t + L", Undo brings it back");
}

static std::vector<clip::SoundEdit::Span> quietSpans(const clip::SoundEdit& e) {
    return e.quiet((size_t)FS / 100, -60.0f, -38.0f, 30.0f, (size_t)FS / 2, (size_t)FS / 8, (size_t)FS / 20);
}

static size_t takeQuietCut() {
    const std::vector<clip::SoundEdit::Span> spans = quietSpans(take.edit);
    size_t gone = 0;
    for (const clip::SoundEdit::Span& q : spans) gone += q.to - q.from;
    return !spans.empty() && take.edit.cutMany(spans) ? gone : 0;
}

static void takeCutQuiet(HWND h) {
    if (!takeReady()) return;
    const size_t gone = takeQuietCut();
    if (!gone) {
        takeSay(L"No silence long enough to cut");
        takeRefresh(h);
        return;
    }
    takeReshape(h, true, L"Cut " + trimClock(gone / (double)FS, 1) + L" of silence out, Undo brings it back");
}

static void takeGrab(HWND h) {
    if (recording() || keepSec <= 0) return;
    std::vector<short> pcm = backlog.last(keepFrames());
    const double got = pcm.size() / 2.0 / FS;
    if (pcm.size() / 2 < (size_t)(RECORD_LEAST_SEC * FS)) {
        takeSay(recBoth() ? L"Nothing kept yet. It keeps your voice and what plays from now on" : (recHear() ? L"Nothing kept yet. It keeps what plays from now on" : L"Nothing kept yet. It keeps your voice from now on"));
        takeRefresh(h);
        return;
    }
    if (takeQuiet(pcm)) {
        takeSay(nothingHeard(L"in the last " + trimClock(got, 0)));
        takeRefresh(h);
        return;
    }
    takeAdopt(pcm);
    std::wstring note = got + 0.05 < keepSec ? L"Got the last " + trimClock(got, 1) + L", all that was kept so far" : L"Got the last " + keepText(keepSec);
    const size_t gone = lastQuiet ? takeQuietCut() : 0;
    if (gone) note += L", " + trimClock(gone / (double)FS, 1) + L" of silence cut out (Undo brings it back)";
    takeReshape(h, true, note + L". Drag across the wave to pick a part");
}

static void lastQuietFlip(HWND h) {
    lastQuiet = !lastQuiet;
    settingsSave();
    takeSay(lastQuiet ? L"Last cuts the silence out of what it grabs" : L"Last keeps the silence in what it grabs");
    takeRefresh(h);
}
static bool takeRestorable() { return droppedHeld && !take.w.ready() && !recording(); }

static void takeUndo(HWND h) {
    if (takeRestorable()) {
        take = std::move(dropped);
        dropped = Take{};
        droppedHeld = false;
        takeHeld = true;
        takeSay(L"The recording is back");
        takeRefresh(h);
        return;
    }
    if (takeReady() && take.edit.undo()) takeReshape(h, true, L"Undone. The forward arrow brings it back");
}

static void takeRedo(HWND h) {
    if (takeReady() && take.edit.redo()) takeReshape(h, true, L"Redone");
}

static void takeDrop(HWND h) {
    if (!takeReady() || takeSaving) return;
    takeStop();
    dropped = std::move(take);
    take = Take{};
    droppedHeld = true;
    takeHeld = false;
    takeSay(L"Recording deleted. Undo brings it back");
    takeRefresh(h);
}

static std::shared_ptr<const clip::Sound> takeRender(double from, double to) {
    try {
        return take.edit.render(takeFrame(from), takeFrame(to), fadeFrames(take.fadeIn), fadeFrames(take.fadeOut));
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

struct TakeFxSet {
    int preset = 0, amount = 0, pitchHz = 0, ratio = 1000, mask = -1, eqLevel = 0, speed = 100;
    bool echo = false, masks = false, keepPitch = true, measured = false, eqOn = false, cmss = false;
    double bassDb = 0.0;
    int eq[eqb::EQ_BANDS] = {};

    bool changes() const {
        const double shift = ratio / 1000.0 * (voice::SongChanger::REF_HZ + pitchHz) / voice::SongChanger::REF_HZ;
        const bool fx = mask >= 0 && mask < voice::NUM_MASKS && voice::MASKS[mask].fx != voice::FX_NONE;
        return masks && (std::fabs(shift - 1.0) >= 1e-4 || fx);
    }
    bool shaped() const {
        if (!eqOn) return false;
        if (eqLevel) return true;
        for (int v : eq)
            if (v) return true;
        return false;
    }
    bool any() const { return echo || changes() || speed != 100 || shaped() || cmss; }
};

struct TakeFxJob {
    std::atomic<bool> stop{false}, done{false};
    std::atomic<int> permille{0};
    unsigned gen = 0;
    bool again = false;
    std::shared_ptr<const clip::Sound> dry;
    std::shared_ptr<clip::Sound> out;
};

static std::shared_ptr<TakeFxJob> takeFxJob;

static TakeFxSet takeFxNow() {
    TakeFxSet s;
    s.preset = std::max(0, std::min(NUM_CARD_PRESETS - 1, g.preset.load()));
    s.amount = g.amount.load();
    s.echo = g.enabled.load() && s.amount > 0 && (g.echoOn.load() & ON_CLIP);
    s.masks = (g.changeOn.load() & ON_CLIP) != 0;
    s.pitchHz = g.pitchHz.load();
    s.ratio = g.voiceRatio.load();
    s.mask = g.voiceMask.load();
    const bool viaMask = (g.changeOn.load() & ON_CLIP) != 0;
    const int voicePart = viaMask || (g.clipFollow.load() && (g.changeOn.load() & ON_VOICE)) ? g.voiceSpeed.load() : 100;
    s.speed = std::clamp(g.clipSpeed.load() * voicePart / 100, 25, 400);
    s.keepPitch = g.clipKeepPitch.load() && (!viaMask || voicePart == 100 || g.voiceKeepPitch.load());
    s.measured = gConvOn.load();
    s.eqOn = g.eqOn.load();
    s.eqLevel = g.eqLevel.load();
    s.cmss = g.cmss.load();
    const float micGain = micGainFor(g.micLevel.load());
    s.bassDb = eqb::BassBoost::boostFor(micGain > 0.0f ? 20.0 * log10((double)micGain) : 0.0);
    for (int k = 0; k < eqb::EQ_BANDS; ++k) s.eq[k] = g.eqDb[k].load();
    return s;
}

static std::shared_ptr<clip::Sound> takeFxRender(const clip::Sound& in, const TakeFxSet& s, TakeFxJob& job) {
    flushDenormals();
    const int amt = s.echo ? s.amount : 0;
    const bool hc = s.preset == HAUNTED_CAVERN_INDEX, coreOn = hc && !s.measured;
    const float wet = amt <= 0 ? 0.0f : (hc && s.measured ? powf(amt / 100.0f, AmountLaw::WET_EXP) : AmountLaw::send(amt / 100.0f));
    const auto rv = std::make_unique<eax::EAXReverb>(FS, CARD_PRESETS[s.preset].p);
    if (hc) {
        rv->setCascade(wet * CASCADE_GAIN);
        rv->setCentre(AmountLaw::centre(amt));
    }
    const auto core = std::make_unique<eax::CardCore>(FS, HC_CORE);
    const float gCore = coreOn && wet > 0.0f ? AmountLaw::core(amt) : 0.0f, gSlap = hc && wet > 0.0f ? AmountLaw::slap(amt) : 0.0f;
    const float cutWant = hc && amt > 0 ? AmountLaw::dryCutDb(amt) : 0.0f;
    EchoTap slap;
    slap.prepare(FS);
    slap.setDelay(AmountLaw::slapDelay(FS, amt));
    Subsonic subL, subR;
    subL.prepare(FS);
    subR.prepare(FS);
    StepBack back;
    back.prepare(FS, HC_CORE.decayTime);
    float dryNow = 1.0f;
    eqb::BassBoost bass;
    bass.prepare(FS);
    bass.set(s.bassDb);
    const auto changer = std::make_unique<voice::SongChanger>();
    changer->prepare(FS);
    changer->start();
    changer->set(s.pitchHz, s.ratio, s.mask);
    changer->hold(false, 0);
    const auto pace = std::make_unique<voice::ClipSpeed>();
    pace->prepare(FS);
    OutChain chain;
    chain.prepare(FS);
    chain.eq.set(s.eq, s.eqLevel);

    const size_t n = in.frames(), body = s.speed == 100 ? n : (size_t)std::ceil(n * 100.0 / s.speed);
    const double ring = amt <= 0 ? 0.0 : std::min(8.0, coreOn ? HC_CORE.decayTime : CARD_PRESETS[s.preset].p.decayTime);
    const size_t total = body + (size_t)(ring * FS), BLOCK = 480;
    size_t readAt = 0;
    auto pull = [&](float* l, float* r, int k) {
        for (int i = 0; i < k; ++i, ++readAt) {
            l[i] = readAt < n ? in.pcm[2 * readAt] / 32768.0f : 0.0f;
            r[i] = readAt < n ? in.pcm[2 * readAt + 1] / 32768.0f : 0.0f;
        }
    };
    auto out = std::make_shared<clip::Sound>();
    out->pcm.resize(total * 2);
    std::vector<float> cl(BLOCK), cr(BLOCK), sendL(BLOCK), sendR(BLOCK), tapIn(BLOCK), wl(BLOCK), wr(BLOCK), coreIn(BLOCK), coreOut(BLOCK);
    for (size_t at = 0; at < total; at += BLOCK) {
        if (job.stop.load()) return nullptr;
        const int m = (int)std::min(BLOCK, total - at);
        pace->render(cl.data(), cr.data(), m, s.speed, s.keepPitch, pull);
        for (int i = 0; i < m; ++i) bass.process(cl[i], cr[i]);
        if (s.changes()) changer->process(cl.data(), cr.data(), m, true);
        for (int i = 0; i < m; ++i) {
            sendL[i] = subL.process(cl[i] * wet);
            sendR[i] = subR.process(cr[i] * wet);
            tapIn[i] = wet > 0.0f ? 0.5f * (cl[i] + cr[i]) : 0.0f;
        }
        if (coreOn) {
            for (int i = 0; i < m; ++i) coreIn[i] = tapIn[i] * gCore;
            core->process(coreIn.data(), coreOut.data(), m);
            for (int i = 0; i < m; ++i) wl[i] = wr[i] = coreOut[i];
        } else {
            rv->processStereo(sendL.data(), sendR.data(), wl.data(), wr.data(), m);
            if (hc) slap.process(tapIn.data(), gSlap, wl.data(), wr.data(), m);
        }
        const float dryWant = powf(10.0f, -back.cutDb(cutWant, tapIn.data(), m) / 20.0f);
        for (int i = 0; i < m; ++i) {
            dryNow += 0.002f * (dryWant - dryNow);
            float l = cl[i] * dryNow + wl[i], r = cr[i] * dryNow + wr[i];
            chain.process(l, r, s.eqOn, s.cmss);
            out->pcm[2 * (at + i)] = clip::toShort(l);
            out->pcm[2 * (at + i) + 1] = clip::toShort(r);
        }
        job.permille = (int)(1000.0 * at / total);
    }
    size_t keepTo = total;
    while (keepTo > body && std::abs((int)out->pcm[2 * (keepTo - 1)]) < 33 && std::abs((int)out->pcm[2 * (keepTo - 1) + 1]) < 33) --keepTo;
    out->pcm.resize(keepTo * 2);
    const size_t fadeFrom = std::max(body, keepTo > (size_t)FS ? keepTo - (size_t)FS : 0);
    for (size_t i = fadeFrom; ring > 0.0 && keepTo == total && i < keepTo; ++i) {
        const float k = clip::fadeCurve((keepTo - i) / (double)(keepTo - fadeFrom));
        out->pcm[2 * i] = clip::scaled(out->pcm[2 * i], k);
        out->pcm[2 * i + 1] = clip::scaled(out->pcm[2 * i + 1], k);
    }
    return out;
}

static void takeApplyFx(HWND h) {
    if (!takeReady()) return;
    const TakeFxSet s = takeFxNow();
    if (!s.any()) {
        takeSay(L"Nothing to add yet. Tick Sound clip under Mask on or Echo on, or change a Speed");
        takeRefresh(h);
        return;
    }
    takeStop();
    const bool again = take.fxWet && take.edit.plain(take.fxWet);
    std::shared_ptr<const clip::Sound> now = take.fxDry;
    if (!again) {
        try {
            now = take.edit.render(0, take.edit.frames(), 0, 0);
        } catch (const std::bad_alloc&) {
            now = nullptr;
        }
    }
    if (!now) {
        takeSay(L"There isn't enough free memory to add the effects");
        takeRefresh(h);
        return;
    }
    if (now->frames() * 100.0 / s.speed > FX_MOST_SEC * FS) {
        takeSay(L"With this Speed the recording would last over " + keepText((int)FX_MOST_SEC) + L", cut it shorter or pick a faster Speed");
        takeRefresh(h);
        return;
    }
    auto job = std::make_shared<TakeFxJob>();
    job->gen = takeGen;
    job->again = again;
    job->dry = now;
    takeFxJob = job;
    take.working = true;
    takeSay(L"Adding the effects...");
    takeRefresh(h);
    std::thread([job, now, s] {
        const MediaWork busy;
        try {
            job->out = takeFxRender(*now, s, *job);
        } catch (const std::bad_alloc&) {
            job->out = nullptr;
        }
        job->done = true;
    }).detach();
}

static void takeFxTick(HWND h) {
    const std::shared_ptr<TakeFxJob> job = takeFxJob;
    if (!job) return;
    if (!job->done.load()) {
        if (job->gen != takeGen) {
            job->stop = true;
            takeFxJob.reset();
            return;
        }
        wchar_t t[64];
        swprintf_s(t, 64, L"Adding the effects... %d%%", job->permille.load() / 10);
        if (take.note != t) takeSay(t);
        return;
    }
    takeFxJob.reset();
    if (job->gen != takeGen) return;
    take.working = false;
    if (!job->out || !job->out->frames()) {
        takeSay(L"The effects could not be added");
        takeRefresh(h);
        return;
    }
    const std::shared_ptr<const clip::Sound> wet = job->out;
    if (job->again && take.edit.plain(take.fxWet)) take.edit.swap(wet);
    else take.edit.replace(wet);
    take.fxDry = job->dry;
    take.fxWet = wet;
    takeReshape(h, true, job->again ? L"Effects made again from the recording without them, Undo takes them off" : L"Effects added. Undo takes them off");
}

static void takePlay(HWND h, int how) {
    if (!takeReady()) return;
    if (take.playing == how) {
        takeStop();
        take.note.clear();
        takeRefresh(h);
        return;
    }
    takeStop();
    const bool ear = how == TAKE_EAR;
    take.playFrom = ear ? take.w.from : 0.0;
    const std::shared_ptr<const clip::Sound> part = ear ? takeRender(take.w.from, take.w.to) : takeRender(0.0, take.w.length());
    if (!part) {
        takeSay(L"There isn't enough free memory to play it");
        takeRefresh(h);
        return;
    }
    if (!part->frames()) return;
    (ear ? gTakeEar : gTakeMic).play(0, part);
    take.playing = how;
    take.playAt = GetTickCount64();
    takeSay(ear ? L"Playing the part to you only" : L"Playing the recording on the mic, others hear it now");
    takeRefresh(h);
}

static void takePlayAt(HWND h, double t) {
    if (!takeReady()) return;
    const double len = take.w.length();
    t = std::max(0.0, std::min(len, t));
    if (len - t < RECORD_LEAST_SEC) t = 0.0;
    takeStop();
    const std::shared_ptr<const clip::Sound> part = takeRender(t, len);
    if (!part) {
        takeSay(L"There isn't enough free memory to play it");
        takeRefresh(h);
        return;
    }
    if (!part->frames()) return;
    gTakeEar.play(0, part);
    take.playFrom = t;
    take.playing = TAKE_EAR;
    take.playAt = GetTickCount64();
    takeSay(L"Playing from " + trimClock(t, 1) + L" to you only. Click anywhere on the wave to jump there");
    takeRefresh(h);
}

static int waveHoverX = -1;

static void drawWaveHover(HDC dc, const WaveEdit& w, int x) {
    const RECT& r = w.wave;
    if (x <= r.left + 1 || x >= r.right - 1) return;
    {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(gpColor(C_TEXT, 170), 1.0f);
        gr.DrawLine(&pen, x + 0.5f, r.top + 1.0f, x + 0.5f, r.bottom - 1.0f);
    }
    const std::wstring t = trimClock(w.timeAt(x), 1);
    HGDIOBJ of = SelectObject(dc, fntUI);
    SIZE sz{};
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    SelectObject(dc, of);
    const int pw = sz.cx + 10, ph = std::min((int)sz.cy + 2, (int)(r.bottom - r.top) - 4);
    int left = x + 5;
    if (left + pw > r.right - 2) left = x - 5 - pw;
    const RECT pill = {left, r.top + 2, left + pw, r.top + 2 + ph};
    panel(dc, pill, C_PANEL, mixc(C_EDGE, C_GOLD, 0.5));
    boxText(dc, pill, t.c_str(), C_TEXT, fntUI, DT_CENTER);
}

static void takeSave(HWND h) {
    if (!takeReady() || takeSaving) return;
    std::wstring filter;
    for (const clip::SaveFormat& f : clip::SAVE_FORMATS) {
        filter += f.filter;
        filter.push_back(L'\0');
        filter += L"*.";
        filter += f.ext;
        filter.push_back(L'\0');
    }
    filter.push_back(L'\0');
    std::vector<wchar_t> name(MAX_PATH * 2, L'\0');
    wcsncpy_s(name.data(), name.size(), timeTitle(L"Recording").c_str(), _TRUNCATE);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = h;
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = (DWORD)(takeFormat + 1);
    ofn.lpstrFile = name.data();
    ofn.nMaxFile = (DWORD)name.size();
    ofn.lpstrTitle = L"Save the recording";
    ofn.lpstrDefExt = clip::SAVE_FORMATS[takeFormat].ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;
    const int chosen = std::max(0, std::min(clip::NUM_SAVE_FORMATS - 1, (int)ofn.nFilterIndex - 1));
    std::wstring path = name.data();
    int format = clip::saveFormatOf(path);
    if (format < 0) {
        format = chosen;
        path += L".";
        path += clip::SAVE_FORMATS[format].ext;
    }
    takeFormat = chosen;
    settingsSave();
    const std::shared_ptr<const clip::Sound> all = takeRender(0.0, take.w.length());
    if (!all) {
        takeSay(L"There isn't enough free memory to save it");
        takeRefresh(h);
        return;
    }
    takeSaving = true;
    takeSay(L"Saving " + fileTitle(path) + L"...");
    takeRefresh(h);
    std::thread([h, path, all, format] {
        const MediaWork busy;
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        std::wstring why;
        const bool ok = clip::encodeSound(path, *all, FS, clip::SAVE_FORMATS[format], why);
        if (com) CoUninitialize();
        const size_t slash = path.find_last_of(L"\\/");
        auto* note = new std::wstring(ok ? L"Saved " + path.substr(slash + 1) + L", " + trimClock(all->frames() / (double)FS, 1) + L" long, in " + path.substr(0, slash) : why);
        if (!PostMessageW(h, WM_TAKE_SAVED, ok ? 1 : 0, (LPARAM)note)) delete note;
    }).detach();
}

static void takeSaved(HWND h, LPARAM l) {
    std::unique_ptr<std::wstring> note((std::wstring*)l);
    takeSaving = false;
    takeSay(*note);
    takeRefresh(h);
}

static const UINT WM_LAST_SAVED = WM_APP + 34;
static bool lastSaving = false;

static std::wstring clipsFolder() {
    if (!clipsDir.empty()) return clipsDir;
    PWSTR music = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &music))) dir = std::wstring(music) + L"\\OctopusEAX";
    CoTaskMemFree(music);
    return dir.empty() ? exeFolder() + L"\\Clips" : dir;
}

static bool clipsReady(const std::wstring& dir) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const DWORD at = GetFileAttributesW(dir.c_str());
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
}

static void clipsOpen() {
    const std::wstring dir = clipsFolder();
    if (clipsReady(dir)) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else keyNote = L"Can't make the folder " + dir;
}

static void clipsPick(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Pick the folder for saved clips");
    const std::wstring dir = clipsFolder();
    IShellItem* at = nullptr;
    if (clipsReady(dir) && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&at)))) {
        dlg->SetFolder(at);
        at->Release();
    }
    IShellItem* got = nullptr;
    if (SUCCEEDED(dlg->Show(owner)) && SUCCEEDED(dlg->GetResult(&got))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(got->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            clipsDir = path;
            settingsSave();
            keyNote = L"Clips are saved in " + clipsDir;
        }
        CoTaskMemFree(path);
        got->Release();
    }
    dlg->Release();
}

static void lastSave(HWND h) {
    if (keepSec <= 0) {
        takeSay(L"Last is off, so there is nothing to save. Pick how much to keep next to the Last button");
        return;
    }
    if (lastSaving) {
        takeSay(L"Still saving the clip before, press again in a moment");
        return;
    }
    backlogTick();
    std::vector<short> pcm = backlog.last(keepFrames());
    const double got = pcm.size() / 2.0 / FS;
    if (pcm.size() / 2 < (size_t)(RECORD_LEAST_SEC * FS)) {
        takeSay(L"Nothing kept yet, so nothing was saved");
        return;
    }
    if (takeQuiet(pcm)) {
        takeSay(nothingHeard(L"in the last " + trimClock(got, 0)) + L", so nothing was saved");
        return;
    }
    const std::wstring dir = clipsFolder();
    if (!clipsReady(dir)) {
        takeSay(L"Can't save in " + dir + L", pick another folder in Settings");
        return;
    }
    const int format = takeFormat;
    const std::wstring base = dir + L"\\" + timeTitle(L"Clip"), ext = std::wstring(L".") + clip::SAVE_FORMATS[format].ext;
    std::wstring path = base + ext;
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) path = base + L" (" + std::to_wstring(n) + L")" + ext;
    auto sound = std::make_shared<clip::Sound>();
    sound->pcm.swap(pcm);
    const bool cutQuiet = lastQuiet;
    lastSaving = true;
    takeSay(L"Saving the last " + trimClock(got, 0) + L"...");
    std::thread([h, path, sound, format, cutQuiet] {
        const MediaWork busy;
        std::shared_ptr<const clip::Sound> out = sound;
        if (cutQuiet) {
            try {
                clip::SoundEdit e;
                e.load(sound, (size_t)(FS / TRIM_PER_SEC));
                const std::vector<clip::SoundEdit::Span> spans = quietSpans(e);
                if (!spans.empty() && e.cutMany(spans)) out = e.render(0, e.frames(), 0, 0);
            } catch (const std::bad_alloc&) {
                out = sound;
            }
        }
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        std::wstring why;
        const bool ok = clip::encodeSound(path, *out, FS, clip::SAVE_FORMATS[format], why);
        if (com) CoUninitialize();
        const size_t slash = path.find_last_of(L"\\/");
        auto* note = new std::wstring(ok ? L"Saved the last " + trimClock(out->frames() / (double)FS, 1) + L" as " + path.substr(slash + 1) + L" in " + path.substr(0, slash) : why);
        if (!PostMessageW(h, WM_LAST_SAVED, ok ? 1 : 0, (LPARAM)note)) delete note;
    }).detach();
}

static void lastSaved(HWND h, LPARAM l) {
    std::unique_ptr<std::wstring> note((std::wstring*)l);
    lastSaving = false;
    takeSay(*note);
    takeRefresh(h);
}

static void takeTick(HWND h) {
    static bool noteWas = false;
    if (takeNoteShown() != noteWas) {
        noteWas = !noteWas;
        invalidateDesign(h, R_STATUS_LINE);
    }
    backlogTick();
    takeFxTick(h);
    static bool deafSaid = false;
    static unsigned waveTicks = 0;
    const bool waveDue = (++waveTicks & 1) == 0;
    if (recording()) {
        if (recBoth()) recMixDrain(false);
        else liveRec().drain(recTake, recordMost());
        livePeaksGrow();
        if (waveDue) {
            invalidateDesign(h, R_REC_WAVE);
            invalidateDesign(h, R_REC_TIME);
        }
        const bool deaf = recHear() && gHearTap.state() == clip::HearTap::FAILED;
        if (deaf && !deafSaid) takeSay(L"Can't listen to " + hearName() + L", pick other speakers");
        deafSaid = deaf;
        if (recTake.size() >= recordMost()) recordStop(h);
    } else {
        deafSaid = false;
    }
    if (take.playing != TAKE_QUIET) {
        const clip::SoundBoard& player = take.playing == TAKE_EAR ? gTakeEar : gTakeMic;
        if (!(player.playing() & 1u) && GetTickCount64() - take.playAt > 400) {
            take.playing = TAKE_QUIET;
            take.note.clear();
            takeRefresh(h);
        } else if (waveDue) {
            invalidateDesign(h, R_REC_WAVE);
        }
    }
}

static void drawLiveWave(HDC dc) {
    const RECT& r = R_REC_WAVE;
    const int left = r.left + 1, right = r.right - 1, top = r.top + 1, bottom = r.bottom - 1;
    const int mid = (top + bottom) / 2, half = (bottom - top) / 2 - 3;
    const double seconds = livePeaks.size() / (double)TRIM_PER_SEC, span = std::max(LIVE_VIEW_SEC, seconds);
    HPEN axis = linePen(C_EDGE), bar = linePen(C_GOLD);
    HGDIOBJ old = SelectObject(dc, axis);
    MoveToEx(dc, left, mid, nullptr);
    LineTo(dc, right, mid);
    SelectObject(dc, bar);
    const double perPx = span * TRIM_PER_SEC / (right - left);
    int x = left;
    for (; x < right; ++x) {
        const size_t s0 = (size_t)((x - left) * perPx), s1 = std::max(s0 + 1, (size_t)((x + 1 - left) * perPx));
        if (s0 >= livePeaks.size()) break;
        float peak = 0.0f;
        for (size_t s = s0; s < std::min(s1, livePeaks.size()); ++s) peak = std::max(peak, livePeaks[s]);
        const int hgt = (int)(std::sqrt(std::min(1.0f, peak)) * half + 0.5f);
        MoveToEx(dc, x, mid - hgt, nullptr);
        LineTo(dc, x, mid + hgt + 1);
    }
    SelectObject(dc, old);
    DeleteObject(axis);
    DeleteObject(bar);
    blendRect(dc, RECT{x, top, x + 2, bottom}, RECORD_RED, 255);
}

static void drawRecorder(HDC dc) {
    HPEN sep = linePen(C_EDGE);
    HGDIOBJ oldSep = SelectObject(dc, sep);
    MoveToEx(dc, CX + 24, REC_Y, nullptr);
    LineTo(dc, CX + COL_W - 24, REC_Y);
    SelectObject(dc, oldSep);
    DeleteObject(sep);
    const bool rec = recording(), ready = takeReady(), hear = recHear();
    const bool ear = take.playing == TAKE_EAR, mic = take.playing == TAKE_MIC;
    iconButton(dc, R_REC_FROM_VOICE, ICO_MIC, nullptr, isHot(R_REC_FROM_VOICE), !rec, recVoice());
    iconButton(dc, R_REC_FROM_HEAR, ICO_SPEAKER, nullptr, isHot(R_REC_FROM_HEAR), !rec, hear);
    iconButton(dc, R_REC_GO, rec ? ICO_STOP : ICO_RECORD, rec ? L"Stop" : L"Record", isHot(R_REC_GO), true, rec);
    {
        const double secs = rec ? recTake.size() / 2.0 / FS : take.w.length();
        RECT tr = R_REC_TIME;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, rec ? RECORD_RED : (ready ? C_TEXT : C_DIM));
        HGDIOBJ of = SelectObject(dc, rec || ready ? fntBold : fntUI);
        drawText(dc, trimClock(secs, 1).c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    iconButton(dc, R_REC_LAST_PREV, ICO_PREV, nullptr, isHot(R_REC_LAST_PREV), !rec && keepIndex() > 0);
    iconButton(dc, R_REC_LAST, ICO_BACK, (keepSec ? L"Last " + keepText(keepSec) : std::wstring(L"Last: off")).c_str(), isHot(R_REC_LAST), !rec && keepSec > 0);
    iconButton(dc, R_REC_LAST_NEXT, ICO_NEXT, nullptr, isHot(R_REC_LAST_NEXT), !rec && keepIndex() < NUM_KEEP_STEPS - 1);
    iconButton(dc, R_REC_LAST_QUIET, ICO_HUSH, nullptr, isHot(R_REC_LAST_QUIET), true, lastQuiet);
    iconButton(dc, R_REC_FX, ICO_MASK, nullptr, isHot(R_REC_FX), ready, take.working);
    iconButton(dc, R_REC_LISTEN, ear ? ICO_STOP : ICO_HEADPHONES, nullptr, isHot(R_REC_LISTEN), ready, ear);
    iconButton(dc, R_REC_SEND, mic ? ICO_STOP : ICO_SEND, nullptr, isHot(R_REC_SEND), ready, mic);
    iconButton(dc, R_REC_SAVE, ICO_SAVE, nullptr, isHot(R_REC_SAVE), ready && !takeSaving);
    iconButton(dc, R_REC_DROP, ICO_TRASH, nullptr, isHot(R_REC_DROP), ready && !takeSaving);

    panel(dc, R_REC_WAVE, C_BACK, C_EDGE);
    if (rec) {
        drawLiveWave(dc);
    } else if (take.w.ready()) {
        const std::vector<size_t> joins = take.edit.joins();
        WaveMarks marks;
        marks.joins = &joins;
        marks.fadeIn = take.fadeIn;
        marks.fadeOut = take.fadeOut;
        marks.fadeTo = take.w.length();
        if (take.playing != TAKE_QUIET)
            marks.playAt = take.playFrom + (take.playing == TAKE_EAR ? gTakeEar : gTakeMic).position(0) / (double)FS;
        drawWave(dc, take.w, marks);
        if (take.w.zoomed()) drawWaveBar(dc, take.w);
        if (waveHoverX >= 0 && take.w.drag == WD_NONE && take.w.hot == WH_WAVE) drawWaveHover(dc, take.w, waveHoverX);
    } else {
        centredText(dc, R_REC_WAVE, recBoth() ? L"Press Record to record your voice and what you hear, or Last to grab what just played"
                                              : (hear ? L"Press Record to record what you hear, or Last to grab what just played"
                                                      : L"Press Record and speak, or Last to grab what you just said"), C_DIM, fntUI);
    }

    iconButton(dc, R_REC_CUT, ICO_CUT, L"Cut out", isHot(R_REC_CUT), ready);
    button(dc, R_REC_KEEP, L"Keep only", isHot(R_REC_KEEP), ready);
    iconButton(dc, R_REC_UNDO, ICO_UNDO, nullptr, isHot(R_REC_UNDO), (ready && take.edit.canUndo()) || takeRestorable());
    iconButton(dc, R_REC_REDO, ICO_REDO, nullptr, isHot(R_REC_REDO), ready && take.edit.canRedo());
    iconButton(dc, R_REC_QUIET, ICO_HUSH, nullptr, isHot(R_REC_QUIET), ready);
    {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ready ? C_DIM : C_OFF);
        HGDIOBJ of = SelectObject(dc, fntUI);
        RECT in = {R_REC_QUIET.right + 6, R_REC_FIN.top + 4, R_REC_FIN_PREV.left - 4, R_REC_FIN.bottom};
        RECT out = {R_REC_FIN_NEXT.right + 6, R_REC_FOUT.top + 4, R_REC_FOUT_PREV.left - 4, R_REC_FOUT.bottom};
        drawText(dc, L"Fade in", -1, &in, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
        drawText(dc, L"Fade out", -1, &out, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
        SelectObject(dc, of);
    }
    iconButton(dc, R_REC_FIN_PREV, ICO_PREV, nullptr, isHot(R_REC_FIN_PREV), ready);
    valueBox(dc, R_REC_FIN, fadeText(take.fadeIn), ready && take.fadeIn > 0.0, isHot(R_REC_FIN), ready);
    iconButton(dc, R_REC_FIN_NEXT, ICO_NEXT, nullptr, isHot(R_REC_FIN_NEXT), ready);
    iconButton(dc, R_REC_FOUT_PREV, ICO_PREV, nullptr, isHot(R_REC_FOUT_PREV), ready);
    valueBox(dc, R_REC_FOUT, fadeText(take.fadeOut), ready && take.fadeOut > 0.0, isHot(R_REC_FOUT), ready);
    iconButton(dc, R_REC_FOUT_NEXT, ICO_NEXT, nullptr, isHot(R_REC_FOUT_NEXT), ready);
}

static bool takeClick(HWND h, POINT p) {
    if (!PtInRect(&R_REC_PANEL, p)) return false;
    SetFocus(h);
    const double step = GetKeyState(VK_SHIFT) < 0 ? 1.0 : TRIM_NUDGE_SEC;
    if (PtInRect(&R_REC_GO, p)) {
        if (recording()) recordStop(h);
        else recordStart(h);
        return true;
    }
    if (PtInRect(&R_REC_FROM_VOICE, p)) { recordFrom(h, FROM_VOICE); return true; }
    if (PtInRect(&R_REC_FROM_HEAR, p)) { recordFrom(h, FROM_HEAR); return true; }
    if (PtInRect(&R_REC_LAST_PREV, p) || PtInRect(&R_REC_LAST_NEXT, p)) {
        keepStep(h, PtInRect(&R_REC_LAST_NEXT, p) ? 1 : -1);
        return true;
    }
    if (PtInRect(&R_REC_LAST, p)) {
        takeGrab(h);
        return true;
    }
    if (PtInRect(&R_REC_LAST_QUIET, p)) {
        lastQuietFlip(h);
        return true;
    }
    if (PtInRect(&R_REC_UNDO, p) && takeRestorable()) {
        takeUndo(h);
        return true;
    }
    if (!takeReady()) return true;
    if (waveDown(take.w, p)) {
        SetCapture(h);
        takeRefresh(h);
        return true;
    }
    if (PtInRect(&R_REC_LISTEN, p)) takePlay(h, TAKE_EAR);
    else if (PtInRect(&R_REC_SEND, p)) takePlay(h, TAKE_MIC);
    else if (PtInRect(&R_REC_UNDO, p)) takeUndo(h);
    else if (PtInRect(&R_REC_REDO, p)) takeRedo(h);
    else if (PtInRect(&R_REC_CUT, p)) takeCut(h, false);
    else if (PtInRect(&R_REC_KEEP, p)) takeCut(h, true);
    else if (PtInRect(&R_REC_SAVE, p)) takeSave(h);
    else if (PtInRect(&R_REC_DROP, p)) takeDrop(h);
    else if (PtInRect(&R_REC_QUIET, p)) takeCutQuiet(h);
    else if (PtInRect(&R_REC_FX, p)) takeApplyFx(h);
    else if (PtInRect(&R_REC_FIN_PREV, p) || PtInRect(&R_REC_FIN_NEXT, p)) take.fadeIn = fadeStep(take.fadeIn, take.fadeRoom(), PtInRect(&R_REC_FIN_NEXT, p) ? step : -step);
    else if (PtInRect(&R_REC_FOUT_PREV, p) || PtInRect(&R_REC_FOUT_NEXT, p)) take.fadeOut = fadeStep(take.fadeOut, take.fadeRoom(), PtInRect(&R_REC_FOUT_NEXT, p) ? step : -step);
    else if (PtInRect(&R_REC_FIN, p)) take.fadeIn = fadeStep(0.0, take.fadeRoom(), take.fadeIn > 0.0 ? 0.0 : 1.0);
    else if (PtInRect(&R_REC_FOUT, p)) take.fadeOut = fadeStep(0.0, take.fadeRoom(), take.fadeOut > 0.0 ? 0.0 : 1.0);
    takeRefresh(h);
    return true;
}

static bool takeRepeatable(POINT p) {
    for (const RECT* r : {&R_REC_FIN_PREV, &R_REC_FIN_NEXT, &R_REC_FOUT_PREV, &R_REC_FOUT_NEXT, &R_REC_WAVE, &R_REC_LAST_PREV, &R_REC_LAST_NEXT})
        if (PtInRect(r, p)) return true;
    return false;
}

static bool takeWheel(HWND h, POINT p, double notches) {
    const RECT last = {R_REC_LAST_PREV.left, R_REC_LAST.top, R_REC_LAST_NEXT.right, R_REC_LAST.bottom};
    if (PtInRect(&last, p)) {
        keepStep(h, notches > 0 ? 1 : -1);
        return true;
    }
    if (!takeReady()) return false;
    if (PtInRect(&R_REC_WAVE, p)) waveZoom(take.w, p.x, notches);
    else if (PtInRect(&R_REC_FIN, p)) take.fadeIn = fadeStep(take.fadeIn, take.fadeRoom(), notches * TRIM_NUDGE_SEC);
    else if (PtInRect(&R_REC_FOUT, p)) take.fadeOut = fadeStep(take.fadeOut, take.fadeRoom(), notches * TRIM_NUDGE_SEC);
    else return false;
    takeRefresh(h);
    return true;
}

static HDC mainBufDc = nullptr;
static HBITMAP mainBufBmp = nullptr;
static int mainBufW = 0, mainBufH = 0;

static void dropMainBuffer() {
    if (mainBufDc) { DeleteDC(mainBufDc); mainBufDc = nullptr; }
    if (mainBufBmp) { DeleteObject(mainBufBmp); mainBufBmp = nullptr; }
    mainBufW = mainBufH = 0;
}

static HDC mainBuffer(HDC wdc, int w, int h) {
    if (mainBufDc && (mainBufW != w || mainBufH != h)) dropMainBuffer();
    if (!mainBufDc) {
        mainBufDc = CreateCompatibleDC(wdc);
        if (!mainBufDc) return nullptr;
        mainBufBmp = backBuffer(w, h);
        if (!mainBufBmp) { dropMainBuffer(); return nullptr; }
        SelectObject(mainBufDc, mainBufBmp);
        mainBufW = w;
        mainBufH = h;
    }
    return mainBufDc;
}

static const RECT TITLE_BOX = {0, 0, WIN_W, TITLE_H};
static const RECT DEV_BOX = {12, TITLE_H + 12, COL_W - 12, TITLE_H + 136};
static const RECT ROOM_BOX = {12, TITLE_H + 146, COL_W - 12, TITLE_H + 444};

static void drawTitleBar(HDC dc, const RECT& rc, bool mainView) {
    RECT tr = {0, 0, rc.right, TITLE_H};
    blendRect(dc, tr, C_PANEL, (BYTE)alphaFor(C_PANEL));
    HPEN gp = linePen(mixc(C_EDGE, C_GOLD, 0.6));
    HGDIOBJ opn = SelectObject(dc, gp);
    MoveToEx(dc, 0, TITLE_H - 1, nullptr);
    LineTo(dc, rc.right, TITLE_H - 1);
    SelectObject(dc, opn);
    DeleteObject(gp);
    drawLogo(dc, 10, 0);
    label(dc, 10 + (logoW ? logoW + 8 : 0), 7, L"OctopusEAX", C_GOLD, fntTitle);
    label(dc, 10 + (logoW ? logoW + 8 : 0) + 118 - titleShrink, 12, L"Audio Effects Console", C_DIM, fntUI);
    if (mainView) {
        const bool just = GetTickCount64() - savedAt < 1500;
        iconButton(dc, R_BOARD, ICO_PLAY, L"Soundboard", isHot(R_BOARD), true, hBoard != nullptr);
        iconButton(dc, R_TONE, ICO_PAD, L"Tone", isHot(R_TONE), true, hTone != nullptr || toneShaping());
        iconButton(dc, R_SETUP_PREV, ICO_PREV, nullptr, isHot(R_SETUP_PREV));
        panel(dc, R_SETUP_NAME, C_BACK, C_EDGE);
        centredText(dc, R_SETUP_NAME, curSetup.empty() ? L"No setup" : curSetup.c_str(),
                    curSetup.empty() ? C_DIM : C_TEXT, fntBold);
        iconButton(dc, R_SETUP_NEXT, ICO_NEXT, nullptr, isHot(R_SETUP_NEXT));
        iconButton(dc, R_SAVE, ICO_SAVE, just ? L"Saved" : L"Save as", isHot(R_SAVE), true, just);
        iconButton(dc, R_SETUP_DEL, ICO_TRASH, nullptr, isHot(R_SETUP_DEL), !curSetup.empty());
    }
    iconButton(dc, R_GEAR, ICO_LOOK, nullptr, isHot(R_GEAR));
    iconButton(dc, R_MIN, ICO_MIN, nullptr, isHot(R_MIN));
    {
        const double k = hotLevelFor(R_CLOSE, isHot(R_CLOSE));
        const COLORREF red = RGB(200, 58, 72);
        buttonFace(dc, R_CLOSE, ICO_CLOSE, nullptr, mixc(C_BACK, red, k), mixc(C_EDGE, red, k), mixc(C_TEXT, RGB(255, 255, 255), k), fadeAlpha(k));
    }
}

static void drawDevices(HDC dc) {
    RECT dev = {12, TITLE_H + 12, COL_W - 12, TITLE_H + 136};
    panel(dc, dev, C_PANEL, C_EDGE);
    label(dc, 24, TITLE_H + 21, L"Microphone", C_DIM, fntUI);
    label(dc, 24, TITLE_H + 51, L"Speakers", C_DIM, fntUI);
    iconTurn = devSpin();
    iconButton(dc, R_DEV_REFRESH, ICO_REFRESH, nullptr, isHot(R_DEV_REFRESH));
    iconTurn = 0.0f;
    {
        const bool on = g.isolate.load();
        drawCheck(dc, R_ISOLATE.left, R_ISOLATE.top, on, ICO_NOISE_OFF, L"Noise isolation");
        iconButton(dc, R_ISO_PREV, ICO_PREV, nullptr, isHot(R_ISO_PREV));
        RECT nm = {R_ISO_PREV.right + 2, R_ISO_PREV.top, R_ISO_NEXT.left - 2, R_ISO_NEXT.bottom};
        centredText(dc, nm, isoModeName(g.isoMode.load()), on ? C_TEXT : C_DIM, fntBold);
        iconButton(dc, R_ISO_NEXT, ICO_NEXT, nullptr, isHot(R_ISO_NEXT));
        drawFader(dc, R_ISOLVL, isoFader().load(), 100);
        wchar_t lv[16];
        swprintf_s(lv, 16, L"%d", isoFader().load());
        label(dc, R_ISOLVL.right + 8, TITLE_H + 80, lv, on ? C_TEXT : C_DIM, fntBold);
    }
    drawCheck(dc, R_STARTUP.left, R_STARTUP.top, startupOn, ICO_POWER, L"Start with Windows");
}

static void drawRoom(HDC dc) {
    RECT fx = {12, TITLE_H + 146, COL_W - 12, TITLE_H + 444};
    panel(dc, fx, C_PANEL, C_EDGE);
    drawIcon(dc, 32.0f, TITLE_H + 161.5f, ICO_ROOM, C_GOLD, C_PANEL);
    label(dc, 46, TITLE_H + 154, L"Environment", C_GOLD, fntBold);
    label(dc, 332, TITLE_H + 154, L"Effects amount", C_GOLD, fntBold);

    const int amt = g.amount.load();
    wchar_t v[64];
    if (amt <= 0) wcscpy_s(v, L"0   (off)");
    else swprintf_s(v, 64, L"%d", amt);
    label(dc, 452, TITLE_H + 154, v, C_TEXT, fntBold);
    drawFader(dc, R_SLIDER, amt, 100);
    label(dc, R_SLIDER.left - 2, R_SLIDER.bottom + 10, L"0", C_DIM, fntUI);
    label(dc, R_SLIDER.right - 20, R_SLIDER.bottom + 10, L"100", C_DIM, fntUI);

    const int mic = g.micLevel.load();
    label(dc, 332, 264, L"Microphone level", C_GOLD, fntBold);
    if (mic <= 0) wcscpy_s(v, L"off");
    else swprintf_s(v, 64, L"%d   %+.0f dB", mic, 20.0 * log10((double)micGainFor(mic)));
    label(dc, 452, 264, v, C_TEXT, fntBold);
    drawFader(dc, R_MIC, mic, MIC_MAX);
    {
        const int span = R_MIC.right - R_MIC.left - 12;
        const int ux = R_MIC.left + 6 + span * 100 / MIC_MAX;
        HPEN gp2 = CreatePen(PS_SOLID, 2, C_GOLD);
        HGDIOBJ op2 = SelectObject(dc, gp2);
        MoveToEx(dc, ux, R_MIC.bottom + 2, nullptr);
        LineTo(dc, ux, R_MIC.bottom + 10);
        SelectObject(dc, op2);
        DeleteObject(gp2);
        label(dc, ux - 8, R_MIC.bottom + 10, L"100", C_GOLD, fntUI);
        label(dc, ux + 22, R_MIC.bottom + 10, L"normal", C_DIM, fntUI);
    }
    label(dc, R_MIC.right - 30, R_MIC.bottom + 10, L"1000", C_DIM, fntUI);

    drawCheck(dc, 332, 340, g.enabled.load(), ICO_WAND, L"Enable audio effects");
    drawCheck(dc, 332, 360, g.monitor.load(), ICO_HEADPHONES, L"Hear myself");
    drawCheck(dc, 332, 380, g.wide.load(), ICO_WIDE, L"Wide echo in headphones");
    drawCheck(dc, 332, 400, g.rawMic.load(), ICO_MIC, L"Raw mic (no auto gain)");
    drawCheck(dc, 332, 420, g.cmss.load(), ICO_CUBE, L"CMSS 3D");
    {
        HPEN sp2 = linePen(C_EDGE);
        HGDIOBJ osp2 = SelectObject(dc, sp2);
        MoveToEx(dc, 24, ECHO_Y - 8, nullptr);
        LineTo(dc, COL_W - 24, ECHO_Y - 8);
        SelectObject(dc, osp2);
        DeleteObject(sp2);
        const int eo = g.echoOn.load();
        label(dc, 24, ECHO_Y - 1, L"Echo on", C_GOLD, fntBold);
        drawCheck(dc, R_ECHO_VOICE.left, ECHO_Y, (eo & ON_VOICE) != 0, ICO_PERSON, L"My voice");
        drawCheck(dc, R_ECHO_CLIP.left, ECHO_Y, (eo & ON_CLIP) != 0, ICO_NOTE, L"Sound clip");
        drawCheck(dc, R_ECHO_APP.left, ECHO_Y, (eo & ON_PROGRAM) != 0, ICO_WINDOW, L"Program");
    }
}

static void drawStatusBar(HDC dc, const RECT& rc) {
    RECT sb = {0, rc.bottom - STATUS_H, rc.right, rc.bottom};
    blendRect(dc, sb, C_PANEL, (BYTE)alphaFor(C_PANEL));
    HPEN sp = linePen(C_EDGE);
    HGDIOBJ osp = SelectObject(dc, sp);
    MoveToEx(dc, 0, rc.bottom - STATUS_H, nullptr);
    LineTo(dc, rc.right, rc.bottom - STATUS_H);
    SelectObject(dc, osp);
    DeleteObject(sp);
    const std::wstring cr = std::wstring(L"\u00A9 ") + BRAND_LABEL;
    const bool warn = g.statusWarn.load();
    wchar_t cableLine[256], micLine[256], outName[256];
    {
        std::lock_guard<std::mutex> lk(g.devMx);
        wcsncpy_s(cableLine, g.cable, 255);
        wcsncpy_s(micLine, g.status, 255);
        wcsncpy_s(outName, g.cableName, 255);
    }
    const int cy = rc.bottom - 24;
    SIZE sz{};
    HGDIOBJ o2 = SelectObject(dc, fntUI);
    GetTextExtentPoint32W(dc, cr.c_str(), (int)cr.size(), &sz);
    SelectObject(dc, o2);
    const int cx = rc.right - sz.cx - 26;
    rCredit = {cx - 4, cy - 3, cx + sz.cx + 4, cy + sz.cy + 3};
    const int leftEnd = cx - 20;
    rStatus = {8, rc.bottom - 28, leftEnd, rc.bottom - 8};
    {
        RECT sr = {16, rc.bottom - 24, leftEnd, rc.bottom - 6};
        SetBkMode(dc, TRANSPARENT);
        if (takeNoteShown()) {
            SetTextColor(dc, recording() ? RECORD_RED : C_TEXT);
            HGDIOBJ of = SelectObject(dc, fntBold);
            drawText(dc, take.note.c_str(), -1, &sr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, of);
        } else if (warn || micLine[0] || !outName[0]) {
            SetTextColor(dc, warn || micLine[0] ? C_GOLD : C_DIM);
            HGDIOBJ of = SelectObject(dc, warn || micLine[0] ? fntBold : fntUI);
            drawText(dc, warn ? cableLine : (micLine[0] ? micLine : cableLine), -1, &sr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, of);
        } else {
            const wchar_t* outPrefix = L"Pick this mic in Discord and games:  ";
            SIZE pz{};
            HGDIOBJ of = SelectObject(dc, fntUI);
            GetTextExtentPoint32W(dc, outPrefix, (int)wcslen(outPrefix), &pz);
            SelectObject(dc, of);
            label(dc, sr.left, sr.top, outPrefix, C_DIM, fntUI);
            RECT nr = {sr.left + pz.cx, sr.top, sr.right, sr.bottom};
            SetTextColor(dc, C_TEXT);
            of = SelectObject(dc, fntBold);
            drawText(dc, outName, -1, &nr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, of);
        }
    }
    label(dc, cx, cy, cr.c_str(), mixc(C_DIM, C_GOLD, hotLevelFor(rCredit, isHot(rCredit))), fntUI);
    {
        HPEN grip = linePen(zoomDrag ? C_GOLD : mixc(C_DIM, C_GOLD, hotLevelFor(R_GRIP, isHot(R_GRIP))));
        HGDIOBJ og = SelectObject(dc, grip);
        for (int k = 4; k <= 12; k += 4) {
            MoveToEx(dc, R_GRIP.right - 1, R_GRIP.bottom - 1 - k, nullptr);
            LineTo(dc, R_GRIP.right - 1 - k, R_GRIP.bottom - 1);
        }
        SelectObject(dc, og);
        DeleteObject(grip);
    }
}

static void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    const bool mainView = !cablePanel;
    {
        const RECT strip = scRect(stripRect());
        if (mainView && ps.rcPaint.top >= strip.top && ps.rcPaint.bottom <= strip.bottom) {
            paintStripOnly(wdc);
            EndPaint(hwnd, &ps);
            return;
        }
    }

    HDC dc = mainBuffer(wdc, client.right, client.bottom);
    const bool buffered = dc != nullptr;
    if (!buffered) dc = wdc;
    SelectClipRgn(dc, nullptr);
    IntersectClipRect(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
    scaleDc(dc);
    RECT rc = {0, 0, WIN_W, WIN_H};

    HBRUSH bg = CreateSolidBrush(C_BACK);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    drawWallpaper(dc, rc, 0);

    if (RectVisible(dc, &TITLE_BOX)) drawTitleBar(dc, rc, mainView);

    if (cablePanel) {
        drawCablePanel(dc, rc);
    } else {
        if (RectVisible(dc, &DEV_BOX)) drawDevices(dc);
        if (RectVisible(dc, &ROOM_BOX)) drawRoom(dc);
        drawEqPanel(dc);
        if (RectVisible(dc, &R_REC_PANEL)) drawRecorder(dc);
        drawLevelStrip(dc, stripRect());
    }

    const RECT statusBox = {0, rc.bottom - STATUS_H, rc.right, rc.bottom};
    if (RectVisible(dc, &statusBox)) drawStatusBar(dc, rc);

    if (mainView) {
        {
            PixelSnap px(dc);
            RECT presetBox = px.map(RECT{PRESET_X, PRESET_Y, PRESET_X + PRESET_LW, PRESET_Y + PRESET_H});
            InflateRect(&presetBox, px.line(), px.line());
            edgeLines(dc, presetBox, px.line(), C_EDGE);
        }
        drawPresetScrollbar(dc);
    }

    flatDc(dc);
    SelectClipRgn(dc, nullptr);
    if (buffered) {
        const RECT& d = ps.rcPaint;
        fadeApply(hwnd, dc, d);
        BitBlt(wdc, d.left, d.top, d.right - d.left, d.bottom - d.top, dc, d.left, d.top, SRCCOPY);
    }
    EndPaint(hwnd, &ps);
}

static void drawItem(LPDRAWITEMSTRUCT d) {
    if (d->itemID == (UINT)-1) return;
    const bool sel = (d->itemState & ODS_SELECTED) != 0;
    const HWND list = d->hwndItem == hPreset ? hPreset : ((d->itemState & ODS_COMBOBOXEDIT) ? nullptr : dropListOf(d->hwndItem));
    const double k = shineSel(list, (int)d->itemID, sel), over = shineOver(list, (int)d->itemID);
    HBRUSH b = CreateSolidBrush(mixc(mixc(C_BACK, C_NAVY, 0.45 * over), C_NAVY, k));
    FillRect(d->hDC, &d->rcItem, b);
    DeleteObject(b);

    wchar_t txt[256] = L"";
    const UINT i = d->itemID;
    if (d->hwndItem == hPreset) {
        if ((int)i >= 0 && (int)i < NUM_CARD_PRESETS)
            MultiByteToWideChar(CP_UTF8, 0, CARD_PRESETS[i].name, -1, txt, 256);
    } else if (d->hwndItem == hMic) {
        if (i < uiMics.size()) wcsncpy_s(txt, uiMics[i].name.c_str(), 255);
    } else if (d->hwndItem == hSpk) {
        if (i < uiSpks.size()) wcsncpy_s(txt, uiSpks[i].name.c_str(), 255);
    } else if (d->hwndItem == hApp) {
        if (i == 0) wcscpy_s(txt, L"(none)");
        else if (i - 1 < uiApps.size()) wcsncpy_s(txt, uiApps[i - 1].name.c_str(), 255);
    } else if (d->hwndItem == hEqBox) {
        if (i < uiEqNames.size()) wcsncpy_s(txt, uiEqNames[i].c_str(), 255);
    } else if (d->hwndItem == hMaskBox) {
        if ((int)i < voice::NUM_XFI_MASKS) wcsncpy_s(txt, voice::MASKS[i].name, 255);
    } else if (d->hwndItem == hOwnBox) {
        if ((int)i < voice::NUM_OWN_MASKS) wcsncpy_s(txt, voice::MASKS[voice::OWN_ORDER[i]].name, 255);
    } else if (d->hwndItem == hGlitchBox) {
        if ((int)i < voice::NUM_GLITCH_STYLES) wcsncpy_s(txt, voice::GLITCH_LOOKS[i].name, 255);
    }
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, mixc(C_TEXT, C_GOLD, k));
    HGDIOBJ of = SelectObject(d->hDC, fntCtl);
    RECT t = d->rcItem;
    t.left += sc(8);
    drawText(d->hDC, txt, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(d->hDC, of);
}

static int faderValue(const RECT& r, int x, int maxValue) {
    int span = r.right - r.left - 12;
    int v = (x - r.left - 6) * maxValue / (span > 0 ? span : 1);
    return v < 0 ? 0 : (v > maxValue ? maxValue : v);
}

static void setAmountFromX(HWND hwnd, int x) {
    g.amount = faderValue(R_SLIDER, x, 100);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void setMicFromX(HWND hwnd, int x) {
    g.micLevel = faderValue(R_MIC, x, MIC_MAX);
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void setIsoFromX(HWND hwnd, int x) {
    isoFader() = faderValue(R_ISOLVL, x, 100);
    g.isoVersion.fetch_add(1);
    settingsLater();
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void setClipFromX(HWND hwnd, int x) {
    g.clipGain = faderValue(R_CLIP_VOL, x, CLIP_MAX);
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void setPitchFromX(HWND hwnd, int x) {
    int v = faderValue(R_PITCH, x, 2 * PITCH_SPAN) - PITCH_SPAN;
    if (v > -6 && v < 6) v = 0;
    g.pitchHz = v;
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void setSizeFromX(HWND hwnd, int x) {
    int v = VSIZE_MIN + faderValue(R_SIZE, x, VSIZE_MAX - VSIZE_MIN);
    if (v > 96 && v < 104) v = 100;
    g.voiceSize = v;
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void applyVoiceMacro(int v) {
    v = v < -100 ? -100 : (v > 100 ? 100 : v);
    if (v > -5 && v < 5) v = 0;
    g.voiceMacro = v;
    g.voiceRatio = 1000;
    g.pitchHz = -v / 2;
    int sz = 100 + v / 4;
    g.voiceSize = sz < VSIZE_MIN ? VSIZE_MIN : (sz > VSIZE_MAX ? VSIZE_MAX : sz);
}
static void setVoiceFromX(HWND hwnd, int x) {
    applyVoiceMacro(faderValue(R_VOICE, x, 200) - 100);
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void setVoiceSpeedFromX(HWND hwnd, int x) {
    g.voiceSpeed = faderToSpeed(faderValue(R_VSPEED, x, 200));
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void setClipSpeedFromX(HWND hwnd, int x) {
    g.clipSpeed = faderToSpeed(faderValue(R_CLIP_SPEED, x, 200));
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void setAppFromX(HWND hwnd, int x) {
    g.appGain = faderValue(R_APP_VOL, x, APP_MAX);
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void setPosFromX(HWND hwnd, int x) {
    dragPosVal = faderValue(R_CLIP_POS, x, 1000);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static int knobValue(int k) {
    if (k >= K_LOOK) return lookFaderValue(k - K_LOOK);
    if (k >= K_EQ && k < K_EQ_LEVEL) return g.eqDb[k - K_EQ].load();
    switch (k) {
    case K_AMOUNT: return g.amount.load();
    case K_MIC: return g.micLevel.load();
    case K_ISO: return isoFader().load();
    case K_CLIP_VOL: return g.clipGain.load();
    case K_CLIP_POS: {
        const double dur = gClip.durationSec.load();
        return dur > 0.0 ? (int)(gClip.positionSec.load() / dur * 1000.0 + 0.5) : 0;
    }
    case K_CLIP_SPEED: return g.clipSpeed.load();
    case K_APP: return g.appGain.load();
    case K_PITCH: return g.pitchHz.load();
    case K_SIZE: return g.voiceSize.load();
    case K_VOICE: return g.voiceMacro.load();
    case K_VSPEED: return g.voiceSpeed.load();
    case K_EQ_LEVEL: return g.eqLevel.load();
    }
    return 0;
}

static int knobDefault(int k) {
    if (k >= K_LOOK) {
        static const int LOOK_DEFAULTS[5] = {PURPLE_THEME.depth, CLEAR_DEFAULT, BLUR_DEFAULT, DARKEN_DEFAULT, 100};
        return LOOK_DEFAULTS[k - K_LOOK];
    }
    if (k >= K_EQ && k < K_EQ_LEVEL) {
        const int* b = eqPresetBands(g.eqPreset.load());
        return b ? b[k - K_EQ] : 0;
    }
    switch (k) {
    case K_AMOUNT: return AMOUNT_DEFAULT;
    case K_MIC: return MIC_DEFAULT;
    case K_ISO: return g.isoMode.load() == ISO_VOCAL ? VOCAL_DEFAULT_LEVEL : ISO_DEFAULT;
    case K_CLIP_VOL: return CLIP_GAIN_DEFAULT;
    case K_CLIP_SPEED: return 100;
    case K_APP: return APP_GAIN_DEFAULT;
    case K_SIZE: {
        const int mi = g.voiceMask.load();
        return mi >= 0 && mi < voice::NUM_MASKS ? voice::MASKS[mi].size : 100;
    }
    case K_VSPEED: return 100;
    }
    return 0;
}

static void knobSet(int k, int v) {
    if (k >= K_LOOK) {
        if (hLook) setLookValue(hLook, k - K_LOOK, v);
        return;
    }
    if (k >= K_EQ && k < K_EQ_LEVEL) {
        g.eqDb[k - K_EQ] = v;
        g.eqVersion.fetch_add(1);
        return;
    }
    switch (k) {
    case K_AMOUNT: g.amount = v; break;
    case K_MIC: g.micLevel = v; break;
    case K_ISO: isoFader() = v; g.isoVersion.fetch_add(1); break;
    case K_CLIP_VOL: g.clipGain = v; break;
    case K_CLIP_POS: dragPosVal = v; posGlide = true; break;
    case K_CLIP_SPEED: g.clipSpeed = v; break;
    case K_APP: g.appGain = v; break;
    case K_PITCH: g.pitchHz = v; break;
    case K_SIZE: g.voiceSize = v; break;
    case K_VOICE: applyVoiceMacro(v); break;
    case K_VSPEED: g.voiceSpeed = v; break;
    case K_EQ_LEVEL: g.eqLevel = v; g.eqVersion.fetch_add(1); break;
    }
}

static void knobSettle(int k) {
    if (k >= K_LOOK) {
        if (k == K_LOOK + 2 || k == K_LOOK + 3) buildBackground();
        settingsSave();
        refreshLook(hwndMain);
        return;
    }
    if (k >= K_EQ && k <= K_EQ_LEVEL) { eqBump(); return; }
    if (k == K_CLIP_POS) {
        gClip.seek(gClip.durationSec.load() * dragPosVal / 1000.0);
        posGlide = false;
        return;
    }
    settingsSave();
}

static int knobAt(POINT p) {
    struct Area { int knob; const RECT* r; int dy; };
    static const Area AREAS[] = {
        {K_AMOUNT, &R_SLIDER, 10}, {K_MIC, &R_MIC, 10}, {K_ISO, &R_ISOLVL, 6}, {K_CLIP_VOL, &R_CLIP_VOL, 10}, {K_CLIP_POS, &R_CLIP_POS, 10},
        {K_CLIP_SPEED, &R_CLIP_SPEED, 10}, {K_APP, &R_APP_VOL, 10}, {K_PITCH, &R_PITCH, 10}, {K_SIZE, &R_SIZE, 10}, {K_VOICE, &R_VOICE, 10},
        {K_VSPEED, &R_VSPEED, 10},
    };
    for (const Area& a : AREAS) {
        const RECT grab = {a.r->left - 8, a.r->top - a.dy, a.r->right + 8, a.r->bottom + a.dy};
        if (PtInRect(&grab, p)) return a.knob;
    }
    for (int k = 0; k <= eqb::EQ_BANDS; ++k) {
        const int x = eqFaderX(k);
        const RECT fr = {x - 12, EQ_FT - 8, x + 13, EQ_FB + 8};
        if (PtInRect(&fr, p)) return K_EQ + k;
    }
    return -1;
}

struct KnobGlide {
    bool on = false;
    double from = 0.0, to = 0.0, ms = GLIDE_MS;
    ULONGLONG start = 0;
    HWND owner = nullptr;
};
static KnobGlide glides[K_COUNT];
static const double WHEEL_GLIDE_MS = 130.0;

static void glideKnobTo(HWND owner, int k, int to, double ms = GLIDE_MS) {
    if (k < 0 || k >= K_COUNT) return;
    KnobGlide& gl = glides[k];
    gl.from = knobValue(k);
    gl.to = to;
    gl.ms = ms;
    gl.on = gl.from != gl.to;
    if (!gl.on) return;
    gl.start = GetTickCount64();
    gl.owner = owner;
    SetTimer(owner, GLIDE_TIMER, 15, nullptr);
}

static int knobAim(int k) {
    return k >= 0 && k < K_COUNT && glides[k].on ? (int)std::lround(glides[k].to) : knobValue(k);
}

static void wheelKnob(HWND h, int k, int to) {
    if (uiAnim) {
        glideKnobTo(h, k, to, WHEEL_GLIDE_MS);
        if (!glides[k].on) knobSettle(k);
        return;
    }
    knobSet(k, to);
    knobSettle(k);
    InvalidateRect(h, nullptr, FALSE);
}

static void glideKnob(HWND owner, int k) {
    if (k < 0 || k >= K_COUNT) return;
    if (k == K_CLIP_POS && !gClip.seekable()) return;
    glideKnobTo(owner, k, knobDefault(k));
}

static void glideStop(int k) {
    if (k >= 0 && k < K_COUNT && glides[k].on) {
        glides[k].on = false;
        knobSettle(k);
    }
}

static bool glideStep(HWND owner) {
    const ULONGLONG now = GetTickCount64();
    bool any = false;
    for (int k = 0; k < K_COUNT; ++k) {
        KnobGlide& gl = glides[k];
        if (!gl.on || gl.owner != owner) continue;
        const double t = std::min(1.0, (double)(now - gl.start) / gl.ms);
        const double ease = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
        knobSet(k, (int)std::lround(gl.from + (gl.to - gl.from) * ease));
        if (t >= 1.0) {
            gl.on = false;
            knobSettle(k);
        } else {
            any = true;
        }
    }
    if (!any) KillTimer(owner, GLIDE_TIMER);
    InvalidateRect(owner, nullptr, FALSE);
    return any;
}

static void eqGlidePreset(HWND h, int i) {
    int was[eqb::EQ_BANDS];
    for (int k = 0; k < eqb::EQ_BANDS; ++k) was[k] = g.eqDb[k].load();
    eqApplyPreset(i);
    if (!uiAnim) return;
    for (int k = 0; k < eqb::EQ_BANDS; ++k) {
        const int to = g.eqDb[k].load();
        g.eqDb[k] = was[k];
        glideKnobTo(h, K_EQ + k, to);
    }
    g.eqVersion.fetch_add(1);
}

static void applyLayout() {
    const int on = cablePanel ? SW_HIDE : SW_SHOW;
    ShowWindow(hMic, on);
    ShowWindow(hSpk, on);
    ShowWindow(hPreset, on);
    ShowWindow(hClipUrl, on);
    ShowWindow(hApp, on);
    ShowWindow(hEqBox, on);
    ShowWindow(hMaskBox, on);
    ShowWindow(hOwnBox, on);
    ShowWindow(hGlitchBox, on);
}

static const RECT R_OPT_FX   = {332, 340, 540, 355};
static const RECT R_OPT_HEAR = {332, 360, 540, 375};
static const RECT R_OPT_WIDE = {332, 380, 540, 395};
static const RECT R_OPT_RAW  = {332, 400, 540, 415};
static const RECT R_OPT_CMSS = {332, 420, 540, 435};
static const RECT R_EQ_BANDS = {EQ_FX0 - 22, EQ_FT - 28, EQ_FX0 + (eqb::EQ_BANDS - 1) * EQ_FDX + 22, EQ_FB + 12};
static const RECT R_EQ_LEVEL = {EQ_LVX - 22, EQ_FT - 28, EQ_LVX + 22, EQ_FB + 12};

enum HintWhen { HINT_ALWAYS, HINT_MAIN };
struct Hint { const RECT* r; HintWhen when; const wchar_t* text; };
static const Hint HINTS[] = {
    {&R_CLOSE, HINT_ALWAYS, L"Close OctopusEAX."},
    {&R_MIN, HINT_ALWAYS, L"Minimise to the taskbar."},
    {&R_GEAR, HINT_ALWAYS, L"Settings: colours, background picture, fonts, animations, window options and hotkeys."},
    {&R_TONE, HINT_MAIN, L"Tone of your voice on a square from deep to sharp, and the Vocal settings: strength, Lo, Mid, Hi and Out."},
    {&R_BOARD, HINT_MAIN, L"Soundboard: twelve pads with sounds, and keys that play them from any program."},
    {&R_SETUP_PREV, HINT_MAIN, L"Previous saved setup."},
    {&R_SETUP_NEXT, HINT_MAIN, L"Next saved setup."},
    {&R_SETUP_NAME, HINT_MAIN, L"The setup in use. A setup keeps every setting on this window."},
    {&R_SAVE, HINT_MAIN, L"Save all the settings as a setup with a name."},
    {&R_SETUP_DEL, HINT_MAIN, L"Delete the setup in use."},
    {&R_DEV_REFRESH, HINT_MAIN, L"Look again for microphones and speakers you just plugged in."},
    {&R_ISOLATE, HINT_MAIN, L"Cut room noise, keyboard clicks and other voices out of your mic."},
    {&R_ISO_PREV, HINT_MAIN, L"Isolation mode. Vocal is the voice gate with its Lo, Mid and Hi lift; Gate only shuts the mic between words; Clean, Studio and Broadcast also clean the sound."},
    {&R_ISO_NEXT, HINT_MAIN, L"Isolation mode. Vocal is the voice gate with its Lo, Mid and Hi lift; Gate only shuts the mic between words; Clean, Studio and Broadcast also clean the sound."},
    {&R_ISOLVL, HINT_MAIN, L"How hard the isolation works. Higher cuts more noise and may clip quiet words. With Vocal, 50 is its normal strength and 100 cuts twice as deep; its Lo, Mid, Hi and Out are in the Tone window."},
    {&R_STARTUP, HINT_MAIN, L"Open OctopusEAX when Windows starts."},
    {&R_SLIDER, HINT_MAIN, L"How much echo. 0 turns it off; above 50 the echo starts to cover the voice."},
    {&R_MIC, HINT_MAIN, L"Your mic volume. 100 is normal; above it the voice is pushed harder."},
    {&R_OPT_FX, HINT_MAIN, L"Master switch for the echo and the voice changes. Off, your mic goes through untouched."},
    {&R_OPT_HEAR, HINT_MAIN, L"Play your own processed voice in your headphones."},
    {&R_OPT_WIDE, HINT_MAIN, L"Spread the echo left and right in your headphones only. Others hear the normal echo."},
    {&R_OPT_RAW, HINT_MAIN, L"Turn off the automatic mic gain. Try it if your voice pumps up and down."},
    {&R_OPT_CMSS, HINT_MAIN, L"The sound card's 3D widening on the echo."},
    {&R_ECHO_VOICE, HINT_MAIN, L"Put the echo on your voice."},
    {&R_ECHO_CLIP, HINT_MAIN, L"Put the echo on sound clips and soundboard pads."},
    {&R_ECHO_APP, HINT_MAIN, L"Put the echo on the program's sound."},
    {&R_EQ_ON, HINT_MAIN, L"Shape the tone of everything that goes to the mic."},
    {&R_EQ_SAVE, HINT_MAIN, L"Save these bands as a preset of yours."},
    {&R_EQ_DROP, HINT_MAIN, L"Delete the chosen preset of yours."},
    {&R_EQ_DEF, HINT_MAIN, L"All bands back to 0."},
    {&R_EQ_BANDS, HINT_MAIN, L"Drag a band up or down. Double-click puts it back to 0."},
    {&R_EQ_LEVEL, HINT_MAIN, L"Overall equalizer level."},
    {&R_CLIP_FILE, HINT_MAIN, L"Open a sound file."},
    {&R_CLIP_PLAY, HINT_MAIN, L"Play, or pause."},
    {&R_CLIP_STOP, HINT_MAIN, L"Stop the sound."},
    {&R_CLIP_POS, HINT_MAIN, L"Where the sound is. Drag to jump."},
    {&R_CLIP_BACK, HINT_MAIN, L"Back 10 seconds."},
    {&R_CLIP_FWD, HINT_MAIN, L"Forward 10 seconds."},
    {&R_CLIP_VOL, HINT_MAIN, L"Sound clip volume. 100 is normal."},
    {&R_CLIP_HEAR, HINT_MAIN, L"Hear the sound in your headphones too."},
    {&R_CLIP_MIC, HINT_MAIN, L"Send the sound to the mic so others hear it."},
    {&R_CLIP_SPEED, HINT_MAIN, L"Play the sound faster or slower."},
    {&R_CLIP_KEEP, HINT_MAIN, L"Keep the pitch when the speed changes."},
    {&R_CLIP_FOLLOW, HINT_MAIN, L"Match the sound's speed to your voice speed."},
    {&R_APP_VOL, HINT_MAIN, L"Program sound level. 100 is normal."},
    {&R_APP_ONLY, HINT_MAIN, L"The program's sound stays in your headphones and never reaches the mic."},
    {&R_APP_CARD, HINT_MAIN, L"You hear the program through the virtual mic, processed like everyone else hears it."},
    {&R_APP_SELF, HINT_MAIN, L"You hear the program plainly, from the app itself."},
    {&R_MASK_OFF, HINT_MAIN, L"Turn the mask off."},
    {&R_PITCH, HINT_MAIN, L"Raise or lower your voice."},
    {&R_SIZE, HINT_MAIN, L"Make the voice sound bigger or smaller."},
    {&R_VOICE, HINT_MAIN, L"Voice changer: from smaller to bigger in one move."},
    {&R_VSPEED, HINT_MAIN, L"Speak faster or slower. At other speeds others hear you a little late."},
    {&R_VKEEP, HINT_MAIN, L"Keep the pitch when the speed changes."},
    {&R_GLITCH, HINT_MAIN, L"Hold to glitch your voice. The style is chosen in the box next to it."},
    {&R_GLITCH_KEY, HINT_MAIN, L"Pick a key that holds the glitch from any program."},
    {&R_MASK_VOICE, HINT_MAIN, L"Put the mask on your voice."},
    {&R_MASK_CLIP, HINT_MAIN, L"Put the mask on sound clips."},
    {&R_MASK_APP, HINT_MAIN, L"Put the mask on the program's sound."},
    {&R_GRIP, HINT_ALWAYS, L"Drag to make the window bigger or smaller. Double-click to bring back the normal size."},
    {&R_REC_FROM_VOICE, HINT_MAIN, L"Record your voice as others hear it, with the effects. Can be on together with what you hear."},
    {&R_REC_FROM_HEAR, HINT_MAIN, L"Record what you hear from the Speakers above. Can be on together with your voice to record both at once."},
    {&R_REC_GO, HINT_MAIN, L"Record from the source picked on the left. Press again to stop."},
    {&R_REC_LAST, HINT_MAIN, L"Put what was just said or played into the editor. The app keeps this much of it all the time."},
    {&R_REC_LAST_PREV, HINT_MAIN, L"Keep less, from 5 seconds down to off."},
    {&R_REC_LAST_NEXT, HINT_MAIN, L"Keep more, up to 10 minutes."},
    {&R_REC_LISTEN, HINT_MAIN, L"Listen: play the picked part to yourself only."},
    {&R_REC_UNDO, HINT_MAIN, L"Undo: take back the last cut or effect, or bring back a deleted recording. Ctrl+Z does the same."},
    {&R_REC_REDO, HINT_MAIN, L"Redo: bring back what Undo took back. Ctrl+Y does the same."},
    {&R_REC_QUIET, HINT_MAIN, L"Cut the silence out: quiet stretches longer than half a second become a short pause, and the quiet start and end go."},
    {&R_REC_FX, HINT_MAIN, L"Add effects: the mask, pitch and Speed of the Voice mask box when Sound clip is ticked under Mask on, the echo when it is ticked under Echo on, and the Sound clip speed. Pressing it again makes them fresh from the recording without them. Undo takes them off."},
    {&R_REC_LAST_QUIET, HINT_MAIN, L"When lit, Last cuts the silence out of what it grabs by itself."},
    {&R_REC_DROP, HINT_MAIN, L"Delete the recording. Undo brings it back."},
    {&R_REC_WAVE, HINT_MAIN, L"Click anywhere on the wave to hear it from there, click again to jump. Drag across it to pick a part, drag the edges to adjust it. The wheel zooms in."},
    {&R_REC_CUT, HINT_MAIN, L"Remove the picked part. Delete does the same."},
    {&R_REC_KEEP, HINT_MAIN, L"Keep only the picked part and drop the rest."},
    {&R_REC_SEND, HINT_MAIN, L"Send to mic: play the whole recording on the mic so others hear it."},
    {&R_REC_SAVE, HINT_MAIN, L"Save as: save the recording as MP3, WAV, M4A, WMA or FLAC."},
    {&R_REC_FIN, HINT_MAIN, L"Let the recording start softly. Click to turn it on or off, the arrows or the wheel change it."},
    {&R_REC_FOUT, HINT_MAIN, L"Let the recording end softly. Click to turn it on or off, the arrows or the wheel change it."},
};
static const int NUM_HINTS = (int)(sizeof(HINTS) / sizeof(HINTS[0]));
struct KeyedHint { const RECT* r; int act; const wchar_t* tail; };
static const wchar_t* const ANYWHERE = L" does the same from any program.";
static const KeyedHint KEYED_HINTS[] = {
    {&R_OPT_FX, ACT_EFFECTS, ANYWHERE}, {&R_ISOLATE, ACT_ISOLATE, ANYWHERE}, {&R_OPT_HEAR, ACT_HEAR, ANYWHERE},
    {&R_REC_GO, ACT_RECORD, ANYWHERE}, {&R_REC_SEND, ACT_SEND, ANYWHERE}, {&R_MASK_VOICE, ACT_MASK, ANYWHERE},
    {&R_ECHO_VOICE, ACT_ECHO, ANYWHERE}, {&R_SETUP_NEXT, ACT_SETUP_NEXT, ANYWHERE}, {&R_SETUP_PREV, ACT_SETUP_PREV, ANYWHERE},
    {&R_REC_LAST, ACT_LAST, L" saves it straight to a file from any program."},
};
static const int HINT_ID_BASE = 1000;
struct ChildHint { HWND* box; const wchar_t* text; };
static const ChildHint CHILD_HINTS[] = {
    {&hMic, L"The microphone the program listens to."},
    {&hSpk, L"Where you hear yourself and the program sound."},
    {&hPreset, L"The room your voice echoes in."},
    {&hClipUrl, L"Paste a link to a sound here, or open a file with the folder button."},
    {&hApp, L"The program whose sound goes to your mic."},
    {&hEqBox, L"Equalizer presets. Yours are listed after the built-in ones."},
    {&hMaskBox, L"Voice masks that come with the program."},
    {&hOwnBox, L"Masks you made with the controls below."},
    {&hGlitchBox, L"What the glitch button does."},
};
static std::wstring tipShown;

static const wchar_t* tipText(UINT_PTR id, UINT flags) {
    if (flags & TTF_IDISHWND) {
        for (const ChildHint& c : CHILD_HINTS)
            if (*c.box == (HWND)id) return !cablePanel ? c.text : L"";
        return L"";
    }
    const int i = (int)(id - HINT_ID_BASE);
    if (i < 0 || i >= NUM_HINTS) return L"";
    const HintWhen when = HINTS[i].when;
    if (when == HINT_MAIN && cablePanel) return L"";
    static std::wstring joined;
    joined = HINTS[i].text;
    if ((HINTS[i].r == &R_APP_CARD || HINTS[i].r == &R_APP_SELF) && gApp.chosen()) joined += std::wstring(L" ") + appRouteNote();
    for (const KeyedHint& k : KEYED_HINTS)
        if (k.r == HINTS[i].r && shortcutKey[k.act]) joined += L" " + keyText(shortcutKey[k.act]) + k.tail;
    return joined.c_str();
}

static void tipsColour() {
    if (!hTip) return;
    SendMessageW(hTip, TTM_SETTIPBKCOLOR, C_PANEL, 0);
    SendMessageW(hTip, TTM_SETTIPTEXTCOLOR, mixc(C_EDGE, C_GOLD, 0.45), 0);
}

static void tipsPlace() {
    if (!hTip) return;
    SendMessageW(hTip, WM_SETFONT, (WPARAM)fntCtl, TRUE);
    SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, sc(320));
    RECT margin = {sc(9), sc(6), sc(9), sc(6)};
    SendMessageW(hTip, TTM_SETMARGIN, 0, (LPARAM)&margin);
    for (int i = 0; i < NUM_HINTS; ++i) {
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V2_SIZE;
        ti.hwnd = hwndMain;
        ti.uId = (UINT_PTR)(HINT_ID_BASE + i);
        ti.rect = scRect(*HINTS[i].r);
        SendMessageW(hTip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

static void tipsCreate(HWND owner) {
    hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, nullptr, nullptr);
    if (!hTip) return;
    noTheme(hTip);
    SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
    SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 12000);
    SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_RESHOW, 150);
    tipsColour();
    for (int i = 0; i < NUM_HINTS; ++i) {
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V2_SIZE;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = owner;
        ti.uId = (UINT_PTR)(HINT_ID_BASE + i);
        ti.rect = scRect(*HINTS[i].r);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    tipsPlace();
    for (const ChildHint& c : CHILD_HINTS) {
        if (!*c.box) continue;
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V2_SIZE;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = owner;
        ti.uId = (UINT_PTR)*c.box;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

static LRESULT tipDraw(NMTTCUSTOMDRAW* cd) {
    if (cd->nmcd.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HDC dc = cd->nmcd.hdc;
    RECT all;
    GetClientRect(hTip, &all);
    HBRUSH bg = CreateSolidBrush(C_PANEL);
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    RECT tr = cd->nmcd.rc;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    HGDIOBJ of = SelectObject(dc, fntCtl);
    drawText(dc, tipShown.c_str(), -1, &tr, cd->uDrawFlags | DT_NOPREFIX);
    SelectObject(dc, of);
    return CDRF_SKIPDEFAULT;
}

static HFONT uiFont(int height, int weight) {
    return faceFont(height, weight, uiFace.c_str());
}

static int ctlFontPx[3] = {0, 0, 0};

static bool ctlFontsFit() {
    for (int i = 0; i < 3; ++i)
        if (ctlFontPx[i] != sc(-typeEm[i])) return false;
    return true;
}

static void makeCtlFonts(bool redraw = true) {
    HFONT oldPlain = fntCtl, oldBold = fntCtlBold, oldTitle = fntCtlTitle;
    for (int i = 0; i < 3; ++i) ctlFontPx[i] = sc(-typeEm[i]);
    fntCtl = uiFont(ctlFontPx[0], typeWeight(0));
    fntCtlBold = uiFont(ctlFontPx[1], typeWeight(1));
    fntCtlTitle = uiFont(ctlFontPx[2], typeWeight(2));
    for (HWND c : {hMic, hSpk, hPreset, hApp, hEqBox, hMaskBox, hOwnBox, hGlitchBox, hClipUrl, hTip, hPromptEdit, trim.fromBox, trim.toBox})
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)fntCtl, redraw);
    if (boardEdit) SendMessageW(boardEdit, WM_SETFONT, (WPARAM)fntCtlBold, redraw);
    if (oldPlain) DeleteObject(oldPlain);
    if (oldBold) DeleteObject(oldBold);
    if (oldTitle) DeleteObject(oldTitle);
}

static void makeFonts() {
    fitType();
    const HFONT old[3] = {fntUI, fntBold, fntTitle};
    fntUI = uiFont(-typeEm[0], typeWeight(0));
    fntBold = uiFont(-typeEm[1], typeWeight(1));
    fntTitle = uiFont(-typeEm[2], typeWeight(2));
    makeCtlFonts();
    for (HFONT f : old)
        if (f) DeleteObject(f);
}

static void placeChild(HWND c, int x, int y, int w, int h) {
    if (c) SetWindowPos(c, nullptr, sc(x), sc(y), sc(x + w) - sc(x), sc(y + h) - sc(y), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
}

static void placeBox(HWND c, const RECT& r, int drop) {
    if (!c) return;
    placeChild(c, r.left, r.top, r.right - r.left, drop);
    SendMessageW(c, CB_SETITEMHEIGHT, 0, sc(20));
    const int want = sc(r.bottom) - sc(r.top);
    SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, want);
    RECT wr{};
    GetWindowRect(c, &wr);
    const int frame = (int)(wr.bottom - wr.top) - want;
    if (frame > 0 && want > frame) SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, want - frame);
    primeDropList(c);
}

static void placeChildren() {
    placeBox(hMic, RECT{110, TITLE_H + 18, 508, TITLE_H + 42}, 260);
    placeBox(hSpk, RECT{110, TITLE_H + 48, 508, TITLE_H + 72}, 260);
    placeChild(hPreset, PRESET_X, PRESET_Y, PRESET_LW, PRESET_H);
    SendMessageW(hPreset, LB_SETITEMHEIGHT, 0, sc(22));
    placeBox(hApp, R_APP_BOX, 240);
    placeBox(hEqBox, R_EQ_BOX, 260);
    placeBox(hMaskBox, R_MASK_BOX, 320);
    placeBox(hOwnBox, R_OWN_BOX, 420);
    placeBox(hGlitchBox, R_GLITCH_BOX, 300);
    placeChild(hClipUrl, R_CLIP_URL.left + 28, R_CLIP_URL.top + 4, R_CLIP_URL.right - R_CLIP_URL.left - 33, R_CLIP_URL.bottom - R_CLIP_URL.top - 8);
}

static void applyScale(HWND h, float s, const POINT* at, bool live) {
    uiScale = s;
    if (!live || !ctlFontsFit()) makeCtlFonts(false);
    placeChildren();
    tipsPlace();
    if (!live) {
        applyLook();
        buildBackground();
    }
    dropMainBuffer();
    RECT wr;
    GetWindowRect(h, &wr);
    SetWindowPos(h, nullptr, at ? at->x : wr.left, at ? at->y : wr.top, sc(WIN_W), sc(WIN_H), SWP_NOZORDER | SWP_NOACTIVATE);
    const struct { HWND box; int w, h; } open[2] = {{hTone, TONE_W, TONE_H}, {hBoard, BOARD_W, BOARD_H}};
    for (const auto& o : open) {
        if (!o.box) continue;
        RECT br;
        GetWindowRect(o.box, &br);
        SetWindowPos(o.box, nullptr, br.left, br.top, sc(o.w), sc(o.h), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(o.box, nullptr, FALSE);
    }
    if (boardEdit) boardRenameEnd(true);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void setUiType(const std::wstring& face, bool bold, int size) {
    size = clampInt(size, TEXT_SIZE_MIN, TEXT_SIZE_MAX);
    if (_wcsicmp(face.c_str(), uiFace.c_str()) == 0 && bold == uiBold && size == uiSize) return;
    uiFace = face;
    uiBold = bold;
    uiSize = size;
    makeFonts();
    placeChildren();
    tipsPlace();
    for (HWND w : {hwndMain, hTone, hBoard, hLook, trim.wnd})
        if (w) RedrawWindow(w, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static void setUiFace(const std::wstring& face) { setUiType(face, uiBold, uiSize); }

static float zoomFit(HWND h, float pct) {
    const float p = pct < ZOOM_MIN ? (float)ZOOM_MIN : (pct > ZOOM_MAX ? (float)ZOOM_MAX : pct);
    return fitScale(uiDpi / 96.0f * p / 100.0f, workAreaOf(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)));
}

static void setZoom(HWND h, int pct, bool live) {
    const float dpiK = uiDpi / 96.0f;
    const float want = zoomFit(h, (float)pct);
    uiZoom = live ? clampInt((int)lroundf(want / dpiK * 100.0f), ZOOM_MIN, ZOOM_MAX) : clampInt(pct, ZOOM_MIN, ZOOM_MAX);
    if (want != uiScale) applyScale(h, want, nullptr, live);
}

static POINT zoomOrigin = {0, 0};

static const UINT ZOOM_TIMER = 42;
static const UINT WM_ZOOM_SETTLE = WM_APP + 47;
static const double ZOOM_SPRING = 32.0, ZOOM_REST_MS = 150.0;

struct ZoomGlide {
    bool on = false, parked = false, stuck = false;
    double at = 1.0, speed = 0.0, last = 0.0, aimed = 0.0;
    float to = 1.0f, drawn = 0.0f;
    HWND ghost = nullptr;
    DockImage shot, frame;
};
static ZoomGlide zoomGlide;

static void zoomFinish(HWND h) {
    applyLook();
    if (wallBmp && (wallW != sc(WIN_W) || wallH != sc(WIN_H))) buildBackground();
    InvalidateRect(h, nullptr, FALSE);
}

struct PictureScale {
    const uint32_t* src;
    int sw, sh;
    uint32_t* dst;
    int stride, dw, dh;
    std::vector<int> xs, xf;

    static uint32_t mix(uint32_t a, uint32_t b, uint32_t f) {
        const uint32_t rb = (((a & 0xFF00FF) * (256 - f) + (b & 0xFF00FF) * f) >> 8) & 0xFF00FF;
        const uint32_t g = (((a & 0x00FF00) * (256 - f) + (b & 0x00FF00) * f) >> 8) & 0x00FF00;
        return rb | g;
    }

    static int place(int i, int from, int to, int& frac) {
        long long f = (long long)(2 * i + 1) * from * 256 / (2 * to) - 128;
        if (f < 0) f = 0;
        int at = (int)(f >> 8);
        frac = (int)(f & 255);
        if (at >= from - 1) {
            at = from - 1;
            frac = 0;
        }
        return at;
    }

    void fillRow(uint32_t* row, int sy) const {
        const uint32_t* s = src + (size_t)sy * sw;
        for (int x = 0; x < dw; ++x) {
            const int i = xs[x];
            row[x] = mix(s[i], s[i + 1 < sw ? i + 1 : i], (uint32_t)xf[x]);
        }
    }

    void rows(int y0, int y1) const {
        std::vector<uint32_t> upper(dw), lower(dw);
        int haveUpper = -1, haveLower = -1;
        for (int y = y0; y < y1; ++y) {
            int f = 0;
            const int sy = place(y, sh, dh, f);
            const int sy1 = sy + 1 < sh ? sy + 1 : sy;
            if (haveUpper != sy) {
                if (haveLower == sy) {
                    upper.swap(lower);
                    std::swap(haveUpper, haveLower);
                } else {
                    fillRow(upper.data(), sy);
                    haveUpper = sy;
                }
            }
            if (haveLower != sy1) {
                fillRow(lower.data(), sy1);
                haveLower = sy1;
            }
            uint32_t* d = dst + (size_t)y * stride;
            for (int x = 0; x < dw; ++x) d[x] = mix(upper[x], lower[x], (uint32_t)f);
        }
    }

    void run() {
        xs.resize(dw);
        xf.resize(dw);
        for (int x = 0; x < dw; ++x) xs[x] = place(x, sw, dw, xf[x]);
        const int parts = (size_t)dw * dh > 2000000 ? (int)std::min(4u, std::max(1u, std::thread::hardware_concurrency())) : 1;
        std::vector<std::thread> helpers;
        for (int t = 1; t < parts; ++t) {
            try {
                helpers.emplace_back([this, t, parts] { rows(dh * t / parts, dh * (t + 1) / parts); });
            } catch (const std::system_error&) {
                rows(dh * t / parts, dh * (t + 1) / parts);
            }
        }
        rows(0, dh / parts);
        for (std::thread& helper : helpers) helper.join();
    }
};

static uint32_t* dibBits(HBITMAP bmp) {
    DIBSECTION ds{};
    return bmp && GetObjectW(bmp, sizeof ds, &ds) == sizeof ds ? (uint32_t*)ds.dsBm.bmBits : nullptr;
}

static bool zoomDraw(HWND h, float s) {
    const int w = std::max(1, (int)lroundf(WIN_W * s)), hh = std::max(1, (int)lroundf(WIN_H * s));
    if ((zoomGlide.frame.w < w || zoomGlide.frame.h < hh) &&
        !zoomGlide.frame.make(std::max(w, zoomGlide.frame.w), std::max(hh, zoomGlide.frame.h))) return false;
    const uint32_t* from = dibBits(zoomGlide.shot.bmp);
    uint32_t* into = dibBits(zoomGlide.frame.bmp);
    if (!from || !into) return false;
    PictureScale{from, zoomGlide.shot.w, zoomGlide.shot.h, into, zoomGlide.frame.w, w, hh, {}, {}}.run();
    RECT wr;
    GetWindowRect(h, &wr);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, windowAlphaByte(), 0};
    POINT pos = {wr.left, wr.top}, src = {0, 0};
    SIZE size = {w, hh};
    if (!UpdateLayeredWindow(zoomGlide.ghost, nullptr, &pos, &size, zoomGlide.frame.dc, &src, 0, &bf, ULW_ALPHA)) return false;
    zoomGlide.drawn = s;
    return true;
}

static LRESULT CALLBACK zoomGhostProc(HWND g, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (HWND owner = GetWindow(g, GW_OWNER)) PostMessageW(owner, WM_ZOOM_SETTLE, 0, 0);
        return 0;
    }
    return DefWindowProcW(g, m, w, l);
}

static void zoomUncover(HWND h) {
    const bool shown = winFx.resizing;
    winFx.resizing = false;
    if (shown) applyWindowAlpha(h);
    if (zoomGlide.ghost) {
        ShowWindow(zoomGlide.ghost, SW_HIDE);
        DestroyWindow(zoomGlide.ghost);
    }
    zoomGlide.ghost = nullptr;
    zoomGlide.shot.drop();
    zoomGlide.frame.drop();
    if (shown) composeWait();
}

static bool zoomCover(HWND h) {
    RECT wr;
    GetWindowRect(h, &wr);
    if (!zoomGlide.ghost) zoomGlide.ghost = makeGhost(h, L"OctopusZoom", zoomGhostProc, 0);
    const bool shot = zoomGlide.ghost && zoomGlide.shot.make(wr.right - wr.left, wr.bottom - wr.top) &&
                      PrintWindow(h, zoomGlide.shot.dc, PW_RENDERFULLCONTENT);
    if (shot) GdiFlush();
    if (!shot || !zoomDraw(h, uiScale)) {
        zoomUncover(h);
        return false;
    }
    HWND above = GetWindow(h, GW_HWNDPREV);
    if (above == zoomGlide.ghost) above = GetWindow(above, GW_HWNDPREV);
    SetWindowPos(zoomGlide.ghost, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    winFx.resizing = true;
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    composeWait();
    return true;
}

static void zoomClock(HWND h, bool run) {
    if (run == zoomGlide.on) return;
    zoomGlide.on = run;
    if (run) SetTimer(h, ZOOM_TIMER, 8, nullptr);
    else KillTimer(h, ZOOM_TIMER);
}

static void zoomStop(HWND h) {
    if (!zoomGlide.on) return;
    zoomClock(h, false);
    zoomUncover(h);
}

static void zoomSettle(HWND h) {
    if (!zoomGlide.on) return;
    zoomClock(h, false);
    if (zoomGlide.to != uiScale) applyScale(h, zoomGlide.to, nullptr, true);
    if (!zoomDrag) zoomFinish(h);
    RedrawWindow(h, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    zoomUncover(h);
}

static void zoomPark(HWND h) {
    if (!zoomGlide.on) return;
    zoomStop(h);
    zoomGlide.parked = true;
}

static void zoomUnpark(HWND h) {
    if (!zoomGlide.parked) return;
    zoomGlide.parked = false;
    if (zoomGlide.to != uiScale) applyScale(h, zoomGlide.to, nullptr, true);
    if (!zoomDrag) zoomFinish(h);
}

static void zoomAim(HWND h, float to) {
    zoomGlide.to = to;
    zoomGlide.aimed = fxNow();
    zoomGlide.parked = false;
    if (zoomGlide.on) return;
    if (to == uiScale) {
        if (!zoomDrag) zoomFinish(h);
        return;
    }
    bool covered = false;
    if (!zoomGlide.stuck && winFx.kind == FX_NONE && !winFx.faded && !IsIconic(h) && IsWindowVisible(h)) {
        covered = zoomCover(h);
        zoomGlide.stuck = !covered;
    }
    if (!covered) {
        applyScale(h, to, nullptr, true);
        if (!zoomDrag) zoomFinish(h);
        return;
    }
    zoomGlide.at = uiScale;
    zoomGlide.speed = 0.0;
    zoomGlide.last = fxNow();
    zoomClock(h, true);
}

static void zoomStep(HWND h) {
    if (!zoomGlide.on) return;
    const double now = fxNow();
    const double dt = std::min((now - zoomGlide.last) / 1000.0, 0.05);
    zoomGlide.last = now;
    if (IsIconic(h)) return;
    for (double left = dt; left > 1e-9; left -= 0.004) {
        const double d = std::min(left, 0.004);
        zoomGlide.speed += (ZOOM_SPRING * ZOOM_SPRING * (zoomGlide.to - zoomGlide.at) - 2.0 * ZOOM_SPRING * zoomGlide.speed) * d;
        zoomGlide.at += zoomGlide.speed * d;
    }
    const bool settled = std::fabs(zoomGlide.to - zoomGlide.at) * WIN_W < 0.3 && std::fabs(zoomGlide.speed) * WIN_W < 15.0;
    const float s = settled ? zoomGlide.to : (float)zoomGlide.at;
    if (s != zoomGlide.drawn) {
        if (!zoomDraw(h, s)) {
            zoomGlide.stuck = true;
            zoomSettle(h);
            return;
        }
        composeWait();
    }
    if (settled && (!zoomDrag || now - zoomGlide.aimed >= ZOOM_REST_MS)) zoomSettle(h);
}

static void zoomDone(HWND h) {
    zoomDrag = false;
    settingsSave();
    if (uiAnim) zoomAim(h, zoomFit(h, (float)uiZoom));
    else zoomFinish(h);
    zoomGlide.stuck = false;
}
static void dropDrags(HWND h) {
    const bool save = dragPitch || dragSize || dragVoice || dragVoiceSpeed || dragClipSpeed || dragApp;
    if (zoomDrag) zoomDone(h);
    if (dragPos) gClip.seek(gClip.durationSec.load() * dragPosVal / 1000.0);
    if (dragEq >= 0) eqSave();
    dragSlider = dragMic = dragClip = dragPitch = dragSize = dragVoice = false;
    dragVoiceSpeed = dragClipSpeed = dragApp = dragPos = dragIso = false;
    dragEq = -1;
    sbDragOff = -1;
    if (save) settingsSave();
    InvalidateRect(h, nullptr, FALSE);
}
static void zoomDragTo(HWND h) {
    POINT c;
    GetCursorPos(&c);
    const double dx = c.x - zoomOrigin.x + 8, dy = c.y - zoomOrigin.y + 8;
    const double s = (dx * WIN_W + dy * WIN_H) / ((double)WIN_W * WIN_W + (double)WIN_H * WIN_H);
    const double pct = s / (uiDpi / 96.0) * 100.0;
    if (!uiAnim) {
        setZoom(h, (int)lround(pct), true);
        return;
    }
    const float want = zoomFit(h, (float)pct);
    uiZoom = clampInt((int)lroundf(want / (uiDpi / 96.0f) * 100.0f), ZOOM_MIN, ZOOM_MAX);
    zoomAim(h, want);
}

static std::vector<const RECT*> hoverables() {
    std::vector<const RECT*> v = {&R_CLOSE, &R_MIN, &R_GEAR, &rCredit, &R_GRIP};
    if (cablePanel) {
        const int view = cableView();
        v.push_back(&R_CB_RUN);
        if (view != VIEW_READY || g.cableStage.load() == STAGE_PROBLEM) v.push_back(&R_CB_BACK);
        if (view == VIEW_INSTALL) v.insert(v.end(), {&R_CB_FOLDER, &R_CB_SITE});
    } else {
        v.insert(v.end(), {&R_BOARD, &R_TONE, &R_SETUP_PREV, &R_SETUP_NEXT, &R_SAVE, &R_SETUP_DEL, &R_DEV_REFRESH, &R_ISO_PREV, &R_ISO_NEXT,
                           &R_EQ_SAVE, &R_EQ_DROP, &R_EQ_DEF, &R_CLIP_FILE, &R_CLIP_PLAY, &R_CLIP_STOP,
                           &R_CLIP_BACK, &R_CLIP_FWD, &R_MASK_OFF, &R_GLITCH, &R_GLITCH_KEY, &R_SB_UP, &R_SB_DOWN, &R_SB_TRACK});
        v.insert(v.end(), {&R_REC_FROM_VOICE, &R_REC_FROM_HEAR, &R_REC_LAST_PREV, &R_REC_LAST, &R_REC_LAST_NEXT, &R_REC_LAST_QUIET, &R_REC_FX, &R_REC_QUIET, &R_REC_REDO, &R_REC_GO, &R_REC_LISTEN, &R_REC_UNDO, &R_REC_DROP, &R_REC_FIN_PREV, &R_REC_FIN, &R_REC_FIN_NEXT, &R_REC_CUT, &R_REC_KEEP, &R_REC_SEND,
                           &R_REC_SAVE, &R_REC_FOUT_PREV, &R_REC_FOUT, &R_REC_FOUT_NEXT});
    }
    return v;
}

static bool mouseTracked = false;

static void saveSetupAs(HWND h) {
    hotLeave(h);
    std::wstring name = askSetupName(h, curSetup);
    if (!name.empty()) {
        CreateDirectoryW(setupsDir().c_str(), nullptr);
        if (settingsFileSave(setupPath(name))) {
            for (const std::wstring& d : otherSetupDirs) CopyFileW(setupPath(name).c_str(), (d + L"\\" + name + L".ini").c_str(), FALSE);
            curSetup = name;
            rememberSetup(name);
            savedAt = GetTickCount64();
        }
    }
    InvalidateRect(h, nullptr, FALSE);
}

static void devicesFound(HWND h, LPARAM l) {
    std::unique_ptr<DeviceScan> found((DeviceScan*)l);
    devScanning = false;
    const std::wstring addedMics = devicesAdded(uiMics, found->mics), addedSpks = devicesAdded(uiSpks, found->spks);
    const bool changed = !sameDevices(uiMics, found->mics) || !sameDevices(uiSpks, found->spks);
    deviceBoxSet(true, found->mics);
    deviceBoxSet(false, found->spks);
    if (devScanAsked) {
        devScanAsked = false;
        const std::wstring added = addedMics + (addedMics.empty() || addedSpks.empty() ? L"" : L", ") + addedSpks;
        takeSay(!added.empty() ? L"Found " + added : (changed ? L"The microphone and speaker lists are up to date" : L"No new microphones or speakers"));
    }
    invalidateDesign(h, R_DEV_REFRESH);
}

static bool setupStep(int d) {
    syncSetups();
    const std::vector<std::wstring> names = setupList();
    if (names.empty()) return false;
    int i = -1;
    for (size_t k = 0; k < names.size(); ++k)
        if (names[k] == curSetup) i = (int)k;
    const int n = (int)names.size();
    i = d > 0 ? (i + 1) % n : (i <= 0 ? n - 1 : i - 1);
    settingsFileLoad(setupPath(names[i]));
    curSetup = names[i];
    rememberSetup(curSetup);
    g.restart = true;
    appRepark();
    return true;
}

static void hotkeyAction(HWND h, int act) {
    switch (act) {
    case ACT_LAST:
        lastSave(h);
        break;
    case ACT_RECORD:
        if (recording()) recordStop(h);
        else recordStart(h);
        break;
    case ACT_SEND:
        if (takeReady()) takePlay(h, TAKE_MIC);
        else takeSay(L"There is no recording to play on the mic yet");
        break;
    case ACT_EFFECTS:
        g.enabled = !g.enabled.load();
        takeSay(g.enabled.load() ? L"Audio effects on" : L"Audio effects off");
        break;
    case ACT_MASK:
        g.changeOn = g.changeOn.load() ^ ON_VOICE;
        settingsSave();
        takeSay((g.changeOn.load() & ON_VOICE) ? L"Voice mask on your voice" : L"Voice mask off your voice");
        break;
    case ACT_ECHO:
        g.echoOn = g.echoOn.load() ^ ON_VOICE;
        settingsSave();
        takeSay((g.echoOn.load() & ON_VOICE) ? L"Echo on your voice" : L"Echo off your voice");
        break;
    case ACT_ISOLATE:
        g.isolate = !g.isolate.load();
        settingsSave();
        g.reopenMic = true;
        takeSay(g.isolate.load() ? L"Noise isolation on" : L"Noise isolation off");
        break;
    case ACT_HEAR:
        g.monitor = !g.monitor.load();
        takeSay(g.monitor.load() ? L"Hear myself on" : L"Hear myself off");
        break;
    case ACT_SETUP_NEXT:
    case ACT_SETUP_PREV:
        if (setupStep(act == ACT_SETUP_NEXT ? 1 : -1)) takeSay(L"Setup " + curSetup);
        else takeSay(L"There are no saved setups yet");
        break;
    default:
        return;
    }
    InvalidateRect(h, nullptr, FALSE);
}

static const UINT FADE_TIMER = 43;
static const double FADE_MS = 170.0, FADE_GRACE_MS = 100.0;

struct CrossFade {
    HWND wnd = nullptr;
    DockImage before;
    RECT area{};
    double start = 0.0, until = 0.0;
    bool collecting = false;
};
static std::vector<CrossFade> crossFades;
static int fadeDepth = 0;
static double fadeScopeUntil = 0.0;

struct FrameSurface {
    HDC dc = nullptr;
    const uint32_t* bits = nullptr;
    int w = 0, h = 0;
};

static FrameSurface frameOf(HWND h) {
    if (h == hwndMain) return {mainBufDc, dibBits(mainBufBmp), mainBufW, mainBufH};
    for (BoxFrame& b : boxFrames)
        if (b.wnd == h) return {b.img.dc, dibBits(b.img.bmp), b.img.w, b.img.h};
    return {};
}

static CrossFade* fadeOf(HWND h) {
    for (CrossFade& f : crossFades)
        if (f.wnd == h) return &f;
    return nullptr;
}

static void fadeDrop(HWND h) {
    for (size_t i = 0; i < crossFades.size(); ++i) {
        if (crossFades[i].wnd != h) continue;
        const RECT area = crossFades[i].area;
        crossFades[i].before.drop();
        crossFades.erase(crossFades.begin() + (std::ptrdiff_t)i);
        if (IsWindow(h)) {
            KillTimer(h, FADE_TIMER);
            if (!IsRectEmpty(&area)) InvalidateRect(h, &area, FALSE);
        }
        return;
    }
}

static bool fadeTrigger(HWND h, UINT m, WPARAM w, LPARAM l) {
    const HWND held = GetCapture();
    if (held && (held == h || held == GetAncestor(h, GA_ROOT)) && m != WM_LBUTTONUP && m != WM_RBUTTONUP) return false;
    if (m == WM_KEYDOWN && (l & 0x40000000)) return false;
    if (m == WM_COMMAND && l && HIWORD(w) >= 0x100) return false;
    if (m == WM_COMMAND && l && boxOfList(dropListOf((HWND)l)) == (HWND)l &&
        (HIWORD(w) == CBN_DROPDOWN || HIWORD(w) == CBN_CLOSEUP || HIWORD(w) == CBN_SETFOCUS || HIWORD(w) == CBN_KILLFOCUS)) return false;
    switch (m) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_KEYDOWN:
    case WM_COMMAND:
    case WM_HOTKEY:
        return true;
    }
    return h == hwndMain && (m == WM_DEVICES_FOUND || m == WM_TAKE_SAVED || m == WM_LAST_SAVED || m == WM_SETUPS_FOUND ||
                             m == WM_CABLE_READY || m == WM_BOARD_READY);
}

static bool fadeAllowed(HWND h) {
    if (!uiAnim || !h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) return false;
    return h != hwndMain || (winFx.kind == FX_NONE && !winFx.faded && !winFx.resizing && !zoomGlide.on && !zoomDrag);
}

struct FadeSkip {
    RECT r[2] = {};
};

static FadeSkip fadeSkip(HWND h) {
    FadeSkip s;
    if (h == hwndMain && !cablePanel) {
        s.r[0] = scRect(stripRect());
        s.r[1] = scRect(R_SB_ALL);
    } else if (h == hBoard && boardScrolls()) {
        s.r[0] = scRect(boardTrack());
        if (boardGliding) s.r[1] = scRect(RB_LIST);
    }
    return s;
}

static RECT pixelChanges(const uint32_t* was, const uint32_t* now, int stride, const RECT& r, const FadeSkip& skip) {
    LONG left = LONG_MAX, top = LONG_MAX, right = LONG_MIN, bottom = LONG_MIN;
    for (LONG y = r.top; y < r.bottom; ++y) {
        const uint32_t* a = was + (size_t)y * stride;
        const uint32_t* b = now + (size_t)y * stride;
        const RECT* cut[2];
        int cuts = 0;
        for (const RECT& k : skip.r)
            if (y >= k.top && y < k.bottom && k.left < k.right) cut[cuts++] = &k;
        for (LONG x = r.left; x < r.right; ++x) {
            if (((a[x] ^ b[x]) & 0x00FFFFFFu) == 0) continue;
            bool inside = false;
            for (int i = 0; i < cuts && !inside; ++i) inside = x >= cut[i]->left && x < cut[i]->right;
            if (inside) continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    if (left == LONG_MAX) return RECT{0, 0, 0, 0};
    return RECT{left, top, right + 1, bottom + 1};
}

static void blendOver(HDC dc, HDC old, const RECT& r, BYTE alpha) {
    if (IsRectEmpty(&r)) return;
    const BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, 0};
    const int w = (int)(r.right - r.left), hh = (int)(r.bottom - r.top);
    AlphaBlend(dc, r.left, r.top, w, hh, old, r.left, r.top, w, hh, bf);
}

static void blendAround(HDC dc, HDC old, const RECT& r, const FadeSkip& skip, BYTE alpha) {
    const int saved = SaveDC(dc);
    for (const RECT& k : skip.r)
        if (!IsRectEmpty(&k)) ExcludeClipRect(dc, k.left, k.top, k.right, k.bottom);
    blendOver(dc, old, r, alpha);
    RestoreDC(dc, saved);
}

static void fadeApply(HWND h, HDC dc, const RECT& paint) {
    CrossFade* f = fadeOf(h);
    if (!f || !f->before.dc) return;
    const FrameSurface s = frameOf(h);
    if (s.dc != dc || !s.bits || s.w != f->before.w || s.h != f->before.h) {
        fadeDrop(h);
        return;
    }
    const FadeSkip skip = fadeSkip(h);
    if (f->collecting) {
        if (fxNow() > f->until) {
            fadeDrop(h);
            return;
        }
        const uint32_t* was = dibBits(f->before.bmp);
        RECT r;
        const RECT whole = {0, 0, s.w, s.h};
        if (was && IntersectRect(&r, &paint, &whole)) {
            GdiFlush();
            const RECT changed = pixelChanges(was, s.bits, s.w, r, skip);
            if (!IsRectEmpty(&changed)) UnionRect(&f->area, &f->area, &changed);
        }
    }
    if (IsRectEmpty(&f->area)) return;
    const double k = f->start > 0.0 ? (fxNow() - f->start) / FADE_MS : 0.0;
    if (k >= 1.0) return;
    const BYTE alpha = (BYTE)std::lround(255.0 * (1.0 - easeInOut(k)));
    RECT r;
    if (alpha && IntersectRect(&r, &f->area, &paint)) blendAround(dc, f->before.dc, r, skip, alpha);
}

static void CALLBACK fadeTick(HWND h, UINT, UINT_PTR id, DWORD) {
    CrossFade* f = fadeOf(h);
    if (!f) {
        KillTimer(h, id);
        return;
    }
    if (f->collecting) {
        if (fxNow() > f->until) fadeDrop(h);
        return;
    }
    if (fxNow() - f->start >= FADE_MS) {
        fadeDrop(h);
        return;
    }
    InvalidateRect(h, &f->area, FALSE);
}

static bool fadeArm(HWND h, double until) {
    if (!fadeAllowed(h)) return false;
    const CrossFade* armed = fadeOf(h);
    if (armed && armed->collecting && armed->until == until) return true;
    UpdateWindow(h);
    FrameSurface s = frameOf(h);
    if (!s.dc && h != hwndMain) {
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        s = frameOf(h);
    }
    if (!s.dc || !s.bits || s.w <= 0 || s.h <= 0) return false;
    CrossFade* f = fadeOf(h);
    if (!f) {
        crossFades.push_back(CrossFade{});
        f = &crossFades.back();
        f->wnd = h;
    }
    if (!f->before.make(s.w, s.h)) {
        fadeDrop(h);
        return false;
    }
    BitBlt(f->before.dc, 0, 0, s.w, s.h, s.dc, 0, 0, SRCCOPY);
    GdiFlush();
    f->collecting = true;
    f->until = until;
    f->start = 0.0;
    return true;
}

static bool fadeArmCached(HWND h, double until) {
    const FrameSurface s = frameOf(h);
    if (!s.dc || !s.bits || s.w <= 0 || s.h <= 0) return false;
    CrossFade* f = fadeOf(h);
    if (!f) {
        crossFades.push_back(CrossFade{});
        f = &crossFades.back();
        f->wnd = h;
    }
    if (!f->before.make(s.w, s.h)) {
        fadeDrop(h);
        return false;
    }
    BitBlt(f->before.dc, 0, 0, s.w, s.h, s.dc, 0, 0, SRCCOPY);
    GdiFlush();
    f->collecting = true;
    f->until = until;
    f->start = 0.0;
    return true;
}

static void fadeStart(HWND h) {
    CrossFade* f = fadeOf(h);
    if (!f) return;
    if (fxNow() > f->until || !fadeAllowed(h) || GetCapture() == h) {
        fadeDrop(h);
        return;
    }
    UpdateWindow(h);
    f = fadeOf(h);
    if (!f) return;
    f->collecting = false;
    if (IsRectEmpty(&f->area)) {
        fadeDrop(h);
        return;
    }
    f->start = fxNow();
    SetTimer(h, FADE_TIMER, 15, fadeTick);
}

static bool hoverWatch(HWND h, UINT m, WPARAM w) {
    if (m != WM_MOUSEMOVE && m != WM_MOUSELEAVE) return false;
    if (h == hwndMain || !uiAnim || (fadeDepth > 0 && fxNow() <= fadeScopeUntil)) return false;
    if (m == WM_MOUSEMOVE && (w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) return false;
    const HWND held = GetCapture();
    if (held && (held == h || held == GetAncestor(h, GA_ROOT))) return false;
    const CrossFade* f = fadeOf(h);
    if (f && f->collecting) return false;
    if (!fadeAllowed(h) || GetUpdateRect(h, nullptr, FALSE)) return false;
    if (!frameOf(h).dc) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return frameOf(h).dc != nullptr;
}

static void hoverFade(HWND h) {
    if (!GetUpdateRect(h, nullptr, FALSE) || !fadeAllowed(h)) return;
    const HWND held = GetCapture();
    if (held && (held == h || held == GetAncestor(h, GA_ROOT))) return;
    if (fadeArmCached(h, fxNow() + FADE_GRACE_MS)) fadeStart(h);
}

static void fadeForget(HWND h) {
    fadeDrop(h);
    for (size_t i = 0; i < boxFrames.size(); ++i)
        if (boxFrames[i].wnd == h) {
            boxFrames[i].img.drop();
            boxFrames.erase(boxFrames.begin() + (std::ptrdiff_t)i);
            return;
        }
}

static double fadeBegin(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!uiAnim || !fadeTrigger(h, m, w, l)) return -1.0;
    for (size_t i = 0; i < crossFades.size();) {
        if (IsWindow(crossFades[i].wnd)) {
            ++i;
            continue;
        }
        crossFades[i].before.drop();
        crossFades.erase(crossFades.begin() + (std::ptrdiff_t)i);
    }
    const double now = fxNow();
    if (fadeDepth > 0 && now <= fadeScopeUntil) return -1.0;
    const double outer = fadeScopeUntil;
    ++fadeDepth;
    fadeScopeUntil = now + FADE_GRACE_MS;
    fadeArm(hwndMain, fadeScopeUntil);
    fadeArm(h, fadeScopeUntil);
    for (size_t i = 0; i < boxFrames.size(); ++i) fadeArm(boxFrames[i].wnd, fadeScopeUntil);
    return outer;
}

static void fadeEnd(double outer) {
    if (outer < 0.0) return;
    std::vector<HWND> due;
    for (const CrossFade& f : crossFades)
        if (f.collecting) due.push_back(f.wnd);
    for (HWND h : due) fadeStart(h);
    fadeScopeUntil = outer;
    --fadeDepth;
}

struct GhostFade {
    HWND ghost = nullptr;
    DockImage img;
    RECT at{};
    double start = 0.0, ms = 0.0;
    BYTE peak = 255;
};
static std::vector<GhostFade> ghostFades;
static UINT_PTR ghostTimer = 0;

static void CALLBACK ghostTick(HWND, UINT, UINT_PTR, DWORD) {
    const double now = fxNow();
    for (size_t i = 0; i < ghostFades.size();) {
        const GhostFade g = ghostFades[i];
        const double k = (now - g.start) / g.ms;
        if (k >= 1.0 || !IsWindow(g.ghost)) {
            ghostFades.erase(ghostFades.begin() + (std::ptrdiff_t)i);
            if (IsWindow(g.ghost)) DestroyWindow(g.ghost);
            DockImage img = g.img;
            img.drop();
            continue;
        }
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)std::lround(g.peak * (1.0 - easeInOut(k))), 0};
        UpdateLayeredWindow(g.ghost, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
        ++i;
    }
    if (ghostFades.empty() && ghostTimer) {
        KillTimer(nullptr, ghostTimer);
        ghostTimer = 0;
    }
}

static void ghostRun(HWND owner, const RECT& at, DockImage& img, double ms, int peak) {
    if (!uiAnim || !img.dc) {
        img.drop();
        return;
    }
    const HWND ghost = makeGhost(owner, L"OctopusDock", DefWindowProcW, WS_EX_TRANSPARENT | (owner ? 0 : WS_EX_TOPMOST));
    if (!ghost) {
        img.drop();
        return;
    }
    if (peak < 0) peak = owner == hwndMain ? windowAlphaByte() : 255;
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)peak, 0};
    POINT pos = {at.left, at.top}, src = {0, 0};
    SIZE size = {img.w, img.h};
    UpdateLayeredWindow(ghost, nullptr, &pos, &size, img.dc, &src, 0, &bf, ULW_ALPHA);
    HWND above = owner ? GetWindow(owner, GW_HWNDPREV) : HWND_TOPMOST;
    if (above == ghost) above = GetWindow(ghost, GW_HWNDPREV);
    SetWindowPos(ghost, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    GhostFade g;
    g.ghost = ghost;
    g.img = img;
    img = DockImage{};
    g.at = at;
    g.start = fxNow();
    g.ms = ms;
    g.peak = (BYTE)peak;
    ghostFades.push_back(g);
    if (!ghostTimer) ghostTimer = SetTimer(nullptr, 0, 15, ghostTick);
}

static bool snapPrint(HWND w, DockImage& img) {
    RECT r;
    if (!w || !IsWindowVisible(w) || !GetWindowRect(w, &r)) return false;
    if (!img.make((int)(r.right - r.left), (int)(r.bottom - r.top))) return false;
    const bool ok = PrintWindow(w, img.dc, PW_RENDERFULLCONTENT) != FALSE;
    GdiFlush();
    return ok;
}

static bool snapFaceAs(HWND box, DockImage& img, int open) {
    RECT r;
    if (!box || !IsWindowVisible(box) || !GetClientRect(box, &r) || !img.make((int)r.right, (int)r.bottom)) return false;
    paintComboFace(box, img.dc, open);
    GdiFlush();
    return true;
}

static HWND boxOfList(HWND list) {
    for (HWND b : {hMic, hSpk, hApp, hEqBox, hMaskBox, hOwnBox, hGlitchBox})
        if (b && dropListOf(b) == list) return b;
    return nullptr;
}

static bool snapList(HWND list, HWND item, DockImage& img) {
    RECT wr, cr;
    POINT o = {0, 0};
    if (!list || !item || !IsWindowVisible(list) || !GetWindowRect(list, &wr) || !GetClientRect(list, &cr) || !ClientToScreen(list, &o)) return false;
    const int w = (int)(wr.right - wr.left), h = (int)(wr.bottom - wr.top), ox = (int)(o.x - wr.left), oy = (int)(o.y - wr.top);
    if ((int)cr.right < w - 2 * ox) return snapPrint(list, img);
    const int count = (int)SendMessageW(list, LB_GETCOUNT, 0, 0), top = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
    const int itemH = (int)SendMessageW(list, LB_GETITEMHEIGHT, 0, 0), sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (count == LB_ERR || top == LB_ERR || itemH <= 0 || !img.make(w, h)) return false;
    HBRUSH bg = CreateSolidBrush(C_BACK);
    const RECT whole = {0, 0, w, h};
    FillRect(img.dc, &whole, bg);
    DeleteObject(bg);
    const int saved = SaveDC(img.dc);
    IntersectClipRect(img.dc, ox, oy, ox + (int)cr.right, oy + (int)cr.bottom);
    for (int i = top; i < count && (i - top) * itemH < (int)cr.bottom; ++i) {
        DRAWITEMSTRUCT d{};
        d.CtlType = item == list ? ODT_LISTBOX : ODT_COMBOBOX;
        d.CtlID = (UINT)GetDlgCtrlID(item);
        d.itemID = (UINT)i;
        d.itemAction = ODA_DRAWENTIRE;
        d.itemState = i == sel ? ODS_SELECTED : 0;
        d.hwndItem = item;
        d.hDC = img.dc;
        d.rcItem = {ox, oy + (i - top) * itemH, ox + (int)cr.right, oy + (i - top + 1) * itemH};
        drawItem(&d);
    }
    RestoreDC(img.dc, saved);
    if (ox > 0 || oy > 0) {
        HPEN pe = linePen(C_EDGE);
        HGDIOBJ op = SelectObject(img.dc, pe), ob = SelectObject(img.dc, GetStockObject(NULL_BRUSH));
        Rectangle(img.dc, 0, 0, w, h);
        SelectObject(img.dc, op);
        SelectObject(img.dc, ob);
        DeleteObject(pe);
    }
    GdiFlush();
    return true;
}

struct SlideGhost {
    HWND ghost = nullptr, list = nullptr;
    DockImage img;
    POINT at{};
    SIZE view{};
    int from = 0, to = 0, top = 0;
    double start = 0.0;
    BYTE peak = 255;
};
static std::vector<SlideGhost> slideGhosts;
static UINT_PTR slideTimer = 0;
static const double SLIDE_MS = 120.0;

static void slideEnd(size_t i) {
    const SlideGhost done = slideGhosts[i];
    slideGhosts.erase(slideGhosts.begin() + (std::ptrdiff_t)i);
    if (IsWindow(done.ghost)) DestroyWindow(done.ghost);
    DockImage img = done.img;
    img.drop();
}

static void CALLBACK slideTick(HWND, UINT, UINT_PTR, DWORD) {
    const double now = fxNow();
    for (size_t i = 0; i < slideGhosts.size();) {
        SlideGhost& s = slideGhosts[i];
        const double k = (now - s.start) / SLIDE_MS;
        if (k >= 1.0 || !IsWindow(s.ghost)) {
            slideEnd(i);
            continue;
        }
        const double e = 1.0 - (1.0 - k) * (1.0 - k) * (1.0 - k);
        POINT src = {0, (LONG)std::lround(s.from + (s.to - s.from) * e)};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, s.peak, 0};
        UpdateLayeredWindow(s.ghost, nullptr, &s.at, &s.view, s.img.dc, &src, 0, &bf, ULW_ALPHA);
        ++i;
    }
    if (slideGhosts.empty() && slideTimer) {
        KillTimer(nullptr, slideTimer);
        slideTimer = 0;
    }
}

static void ghostDropAt(const RECT& r) {
    for (size_t i = 0; i < ghostFades.size();) {
        if (!EqualRect(&ghostFades[i].at, &r)) {
            ++i;
            continue;
        }
        const GhostFade g = ghostFades[i];
        ghostFades.erase(ghostFades.begin() + (std::ptrdiff_t)i);
        if (IsWindow(g.ghost)) DestroyWindow(g.ghost);
        DockImage img = g.img;
        img.drop();
    }
}

static int slideAt(const SlideGhost& s) {
    const double k = std::min(1.0, (fxNow() - s.start) / SLIDE_MS), e = 1.0 - (1.0 - k) * (1.0 - k) * (1.0 - k);
    return (int)std::lround(s.from + (s.to - s.from) * e);
}

static bool slideOn(SlideGhost& s, DockImage& after, int ox, int oy, int shift, bool down, int toTop) {
    DockImage grown;
    if (!grown.make(s.view.cx, s.img.h + shift)) return false;
    const int cur = slideAt(s), cw = (int)s.view.cx, ch = (int)s.view.cy;
    if (down) {
        BitBlt(grown.dc, 0, 0, cw, s.img.h, s.img.dc, 0, 0, SRCCOPY);
        BitBlt(grown.dc, 0, s.img.h, cw, shift, after.dc, ox, oy + ch - shift, SRCCOPY);
        s.from = cur;
        s.to += shift;
    } else {
        BitBlt(grown.dc, 0, 0, cw, shift, after.dc, ox, oy, SRCCOPY);
        BitBlt(grown.dc, 0, shift, cw, s.img.h, s.img.dc, 0, 0, SRCCOPY);
        s.from = cur + shift;
        s.to = 0;
    }
    GdiFlush();
    s.img.drop();
    s.img = grown;
    s.top = toTop;
    s.start = fxNow();
    return true;
}

static void listScrolled(HWND owner, HWND list, DockImage& before, int fromTop, int toTop) {
    DockImage after;
    RECT wr, cr;
    POINT o = {0, 0};
    const int itemH = (int)SendMessageW(list, LB_GETITEMHEIGHT, 0, 0);
    if (!uiAnim || !before.dc || itemH <= 0 || !GetWindowRect(list, &wr) || !GetClientRect(list, &cr) || !ClientToScreen(list, &o) ||
        !snapList(list, list, after) || after.w != before.w || after.h != before.h) {
        after.drop();
        before.drop();
        return;
    }
    const int ox = (int)(o.x - wr.left), oy = (int)(o.y - wr.top), cw = (int)cr.right, ch = (int)cr.bottom;
    const int shift = std::abs(toTop - fromTop) * itemH;
    ghostDropAt(wr);
    for (size_t i = 0; i < slideGhosts.size(); ++i) {
        SlideGhost& run = slideGhosts[i];
        if (run.list != list) continue;
        const bool same = run.top == fromTop && run.view.cx == cw && run.view.cy == ch && shift > 0 && shift < ch &&
                          (toTop > fromTop) == (run.to > run.from) && run.img.h + shift <= 4 * ch;
        if (same && slideOn(run, after, ox, oy, shift, toTop > fromTop, toTop)) {
            after.drop();
            before.drop();
            return;
        }
        slideEnd(i);
        break;
    }
    if (shift <= 0 || shift >= ch || cw <= 0) {
        after.drop();
        ghostRun(owner, wr, before, 150.0);
        return;
    }
    SlideGhost s;
    if (!s.img.make(cw, ch + shift)) {
        after.drop();
        before.drop();
        return;
    }
    if (toTop > fromTop) {
        BitBlt(s.img.dc, 0, 0, cw, ch, before.dc, ox, oy, SRCCOPY);
        BitBlt(s.img.dc, 0, ch, cw, shift, after.dc, ox, oy + ch - shift, SRCCOPY);
        s.from = 0;
        s.to = shift;
    } else {
        BitBlt(s.img.dc, 0, 0, cw, shift, after.dc, ox, oy, SRCCOPY);
        BitBlt(s.img.dc, 0, shift, cw, ch, before.dc, ox, oy, SRCCOPY);
        s.from = shift;
        s.to = 0;
    }
    GdiFlush();
    after.drop();
    before.drop();
    s.ghost = makeGhost(owner, L"OctopusDock", DefWindowProcW, WS_EX_TRANSPARENT);
    if (!s.ghost) {
        s.img.drop();
        return;
    }
    s.list = list;
    s.top = toTop;
    s.at = o;
    s.view = {cw, ch};
    s.peak = owner == hwndMain ? windowAlphaByte() : 255;
    POINT src = {0, s.from};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, s.peak, 0};
    UpdateLayeredWindow(s.ghost, nullptr, &s.at, &s.view, s.img.dc, &src, 0, &bf, ULW_ALPHA);
    HWND above = GetWindow(owner, GW_HWNDPREV);
    if (above == s.ghost) above = GetWindow(s.ghost, GW_HWNDPREV);
    SetWindowPos(s.ghost, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    s.start = fxNow();
    slideGhosts.push_back(s);
    if (!slideTimer) slideTimer = SetTimer(nullptr, 0, 10, slideTick);
}

static void ghostClear() {
    for (GhostFade& g : ghostFades) {
        if (IsWindow(g.ghost)) DestroyWindow(g.ghost);
        g.img.drop();
    }
    ghostFades.clear();
    if (ghostTimer) KillTimer(nullptr, ghostTimer);
    ghostTimer = 0;
    while (!slideGhosts.empty()) slideEnd(slideGhosts.size() - 1);
    if (slideTimer) KillTimer(nullptr, slideTimer);
    slideTimer = 0;
    while (!rollGhosts.empty()) rollEnd(rollGhosts.size() - 1);
    if (rollTimer) KillTimer(nullptr, rollTimer);
    rollTimer = 0;
    dropStop();
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT:
        ticksLive = true;
        paint(h);
        ticksLive = false;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        drawItem((LPDRAWITEMSTRUCT)l);
        return TRUE;
    case WM_CTLCOLORSCROLLBAR:
        if (!brLookPanel) lookBrushes();
        return (LRESULT)brLookPanel;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        SetBkColor((HDC)w, C_BACK);
        SetTextColor((HDC)w, C_TEXT);
        if (!brLookBack) lookBrushes();
        return (LRESULT)brLookBack;
    }
    case WM_CABLE_READY:
        runCableSetup(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_STARTUP_OLD:
        startupOn = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETUPS_FOUND: {
        const std::unique_ptr<std::vector<std::wstring>> found((std::vector<std::wstring>*)l);
        addSetupDirs(*found);
        syncSetups();
        const std::wstring last = lastSetup();
        if (curSetup.empty() && !last.empty() && GetFileAttributesW(setupPath(last).c_str()) != INVALID_FILE_ATTRIBUTES) {
            settingsFileLoad(setupPath(last));
            curSetup = last;
            g.restart = true;
            appRepark();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_BOARD_READY:
        boardLoaded((BoardLoaded*)l);
        return 0;
    case WM_DEVICES_FOUND:
        devicesFound(h, l);
        return 0;
    case WM_FIT_DROP:
        dropShown((HWND)l);
        fitDropList((HWND)l);
        return 0;
    case WM_FACE_FADE:
        faceFade(h, (HWND)l);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && glitchListening) {
            glitchListening = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_DPICHANGED: {
        zoomStop(h);
        zoomGlide.parked = false;
        uiDpi = HIWORD(w);
        const RECT* at = (const RECT*)l;
        const float s = fitScale(zoomScale(), workAreaOf(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)));
        const POINT corner = {(at->left + at->right - (int)lroundf(WIN_W * s)) / 2, (at->top + at->bottom - (int)lroundf(WIN_H * s)) / 2};
        applyScale(h, s, &corner, false);
        if (winFx.kind == FX_UNDOCK) GetWindowRect(h, &dock.to);
        return 0;
    }
    case WM_TAKE_SAVED:
        takeSaved(h, l);
        return 0;
    case WM_ZOOM_SETTLE:
        zoomSettle(h);
        return 0;
    case WM_LAST_SAVED:
        lastSaved(h, l);
        return 0;
    case WM_HOTKEY: {
        const int pad = (int)w - HOTKEY_BASE;
        if (pad == BOARD_PADS) gBoard.stopAll();
        else if (pad >= 0 && pad < BOARD_PADS) boardPlay(pad);
        else if ((int)w >= HOTKEY_APP && (int)w < HOTKEY_APP + NUM_SHORTCUTS) hotkeyAction(h, (int)w - HOTKEY_APP);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)l;
        if (hTip && hdr->hwndFrom == hTip) {
            if (hdr->code == TTN_GETDISPINFOW) {
                NMTTDISPINFOW* di = (NMTTDISPINFOW*)l;
                tipShown = tipText(di->hdr.idFrom, di->uFlags);
                di->lpszText = tipShown.empty() ? nullptr : (LPWSTR)tipShown.c_str();
                di->hinst = nullptr;
                return 0;
            }
            if (hdr->code == NM_CUSTOMDRAW) return tipDraw((NMTTCUSTOMDRAW*)l);
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (PtInRect(&hotRect, p)) hotPressAt(hotRect);
        if (PtInRect(&R_GRIP, p)) {
            RECT wr;
            GetWindowRect(h, &wr);
            zoomOrigin = {wr.left, wr.top};
            zoomDrag = true;
            SetCapture(h);
            invalidateDesign(h, R_GRIP);
            return 0;
        }
        if (PtInRect(&R_CLOSE, p)) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (PtInRect(&R_MIN, p)) { SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0); return 0; }
        if (PtInRect(&R_GEAR, p)) { hotLeave(h); pickLook(h); return 0; }
        if (PtInRect(&rCredit, p)) { hotLeave(h); showAbout(h); return 0; }
        if (cablePanel) {
            const int view = cableView();
            if (PtInRect(&R_CB_RUN, p)) {
                if (view == VIEW_READY) {
                    cablePanel = false;
                    applyLayout();
                } else if (view == VIEW_RESTART) {
                    hotLeave(h);
                    restartPc(h);
                } else {
                    startCableDownload(h);
                }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (view == VIEW_INSTALL && PtInRect(&R_CB_FOLDER, p) && !cableSetup().empty()) { openCableFolder(); return 0; }
            if (view == VIEW_INSTALL && PtInRect(&R_CB_SITE, p)) {
                ShellExecuteW(nullptr, L"open", CABLE_SITE, nullptr, nullptr,
                              SW_SHOWNORMAL);
                return 0;
            }
            if (PtInRect(&R_CB_BACK, p)) {
                if (view == VIEW_READY) {
                    if (g.cableStage.load() == STAGE_PROBLEM)
                        ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl", nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                cablePanel = false;
                applyLayout();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (p.y < TITLE_H) {
                ReleaseCapture();
                SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }
        if (g.needCable.load() && PtInRect(&rStatus, p)) {
            cablePanel = true;
            applyLayout();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (takeClick(h, p)) return 0;
        if (PtInRect(&R_DEV_REFRESH, p)) {
            devicesScan(h, true);
            invalidateDesign(h, R_DEV_REFRESH);
            return 0;
        }
        glideStop(knobAt(p));
        RECT grab = {R_SLIDER.left - 8, R_SLIDER.top - 10,
                     R_SLIDER.right + 8, R_SLIDER.bottom + 10};
        if (PtInRect(&grab, p)) {
            dragSlider = true;
            SetCapture(h);
            setAmountFromX(h, p.x);
            return 0;
        }
        if (PtInRect(&R_CLIP_FILE, p)) {
            wchar_t path[MAX_PATH] = L"";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof ofn;
            ofn.hwndOwner   = h;
            ofn.lpstrFilter = L"Sound files\0"
                              L"*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac;*.mp4;*.webm;*.opus;*.ogg\0"
                              L"All files\0*.*\0";
            ofn.lpstrFile   = path;
            ofn.nMaxFile    = MAX_PATH;
            ofn.lpstrTitle  = L"Choose a sound file";
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                              OFN_NOCHANGEDIR | OFN_EXPLORER;
            if (GetOpenFileNameW(&ofn) && path[0]) {
                SetWindowTextW(hClipUrl, path);
                gClip.start(path);
                settingsSave();
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_CLIP_PLAY, p)) {
            wchar_t u[2048] = L"";
            GetWindowTextW(hClipUrl, u, 2048);
            std::wstring url = u;
            while (!url.empty() && iswspace(url.back())) url.pop_back();
            size_t s0 = 0; while (s0 < url.size() && iswspace(url[s0])) ++s0;
            url = url.substr(s0);
            if (gClip.isPaused()) gClip.resume();
            else if (!url.empty()) { gClip.start(url); settingsSave(); }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_CLIP_STOP, p)) {
            if (gClip.isPaused()) gClip.stop(); else gClip.pause();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        RECT grabClip = {R_CLIP_VOL.left - 8, R_CLIP_VOL.top - 10, R_CLIP_VOL.right + 8, R_CLIP_VOL.bottom + 10};
        if (PtInRect(&grabClip, p)) { dragClip = true; SetCapture(h); setClipFromX(h, p.x); return 0; }
        if (PtInRect(&R_CLIP_BACK, p)) { gClip.skip(-10.0); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (PtInRect(&R_CLIP_FWD, p))  { gClip.skip(10.0);  InvalidateRect(h, nullptr, FALSE); return 0; }
        RECT grabPos = {R_CLIP_POS.left - 8, R_CLIP_POS.top - 10, R_CLIP_POS.right + 8, R_CLIP_POS.bottom + 10};
        if (PtInRect(&grabPos, p)) {
            if (gClip.seekable()) { dragPos = true; SetCapture(h); setPosFromX(h, p.x); }
            return 0;
        }
        if (PtInRect(&R_MASK_OFF, p)) {
            g.voiceMask = voice::MASK_NEUTRAL;
            g.voiceRatio = 1000;
            g.voiceSize = 100;
            g.pitchHz = 0;
            g.voiceMacro = 0;
            g.voiceSpeed = 100;
            showMask();
            settingsSave();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        RECT grabPitch = {R_PITCH.left - 8, R_PITCH.top - 10, R_PITCH.right + 8, R_PITCH.bottom + 10};
        if (PtInRect(&grabPitch, p)) { dragPitch = true; SetCapture(h); setPitchFromX(h, p.x); return 0; }
        RECT grabSize = {R_SIZE.left - 8, R_SIZE.top - 10, R_SIZE.right + 8, R_SIZE.bottom + 10};
        if (PtInRect(&grabSize, p)) { dragSize = true; SetCapture(h); setSizeFromX(h, p.x); return 0; }
        RECT grabVoice = {R_VOICE.left - 8, R_VOICE.top - 10, R_VOICE.right + 8, R_VOICE.bottom + 10};
        if (PtInRect(&grabVoice, p)) { dragVoice = true; SetCapture(h); setVoiceFromX(h, p.x); return 0; }
        RECT grabVoiceSpeed = {R_VSPEED.left - 8, R_VSPEED.top - 10, R_VSPEED.right + 8, R_VSPEED.bottom + 10};
        if (PtInRect(&grabVoiceSpeed, p)) { dragVoiceSpeed = true; SetCapture(h); setVoiceSpeedFromX(h, p.x); return 0; }
        if (PtInRect(&R_VKEEP, p)) { g.voiceKeepPitch = !g.voiceKeepPitch.load(); settingsSave(); InvalidateRect(h, nullptr, FALSE); return 0; }
        RECT grabClipSpeed = {R_CLIP_SPEED.left - 8, R_CLIP_SPEED.top - 10, R_CLIP_SPEED.right + 8, R_CLIP_SPEED.bottom + 10};
        if (PtInRect(&grabClipSpeed, p)) { dragClipSpeed = true; SetCapture(h); setClipSpeedFromX(h, p.x); return 0; }
        if (PtInRect(&R_CLIP_KEEP, p)) { g.clipKeepPitch = !g.clipKeepPitch.load(); settingsSave(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (PtInRect(&R_CLIP_FOLLOW, p)) { g.clipFollow = !g.clipFollow.load(); settingsSave(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (PtInRect(&R_GLITCH_KEY, p)) {
            glitchListening = !glitchListening;
            glitchNote.clear();
            if (glitchListening) SetFocus(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (glitchListening) {
            glitchListening = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (PtInRect(&R_GLITCH, p)) {
            holdGlitch = true;
            g.glitchHold = true;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        RECT grabApp = {R_APP_VOL.left - 8, R_APP_VOL.top - 10, R_APP_VOL.right + 8, R_APP_VOL.bottom + 10};
        if (PtInRect(&grabApp, p)) { dragApp = true; SetCapture(h); setAppFromX(h, p.x); return 0; }
        if (PtInRect(&R_CLIP_HEAR, p)) {
            g.clipHear = !g.clipHear.load();
            g.restart = true;
            settingsSave(); InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (PtInRect(&R_CLIP_MIC, p)) { g.clipToMic = !g.clipToMic.load(); settingsSave(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (PtInRect(&R_APP_ONLY, p)) { g.appOnly = !g.appOnly.load(); appRepark(); settingsSave(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (PtInRect(&R_APP_CARD, p) && !g.appHear.load()) {
            g.appHear = true;
            g.restart = true;
            appRepark();
            settingsSave(); InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (PtInRect(&R_APP_SELF, p) && g.appHear.load()) {
            g.appHear = false;
            g.restart = true;
            appRepark();
            settingsSave(); InvalidateRect(h, nullptr, FALSE); return 0;
        }
        RECT grabMic = {R_MIC.left - 8, R_MIC.top - 10,
                        R_MIC.right + 8, R_MIC.bottom + 10};
        if (PtInRect(&grabMic, p)) {
            dragMic = true;
            SetCapture(h);
            setMicFromX(h, p.x);
            return 0;
        }
        RECT en = {332, 340, 540, 355}, mo = {332, 360, 540, 375}, wd = {332, 380, 540, 395},
             rw = {332, 400, 540, 415}, cm = {332, 420, 540, 435};
        if (PtInRect(&en, p)) {
            g.enabled = !g.enabled.load();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        {
            const int bit = PtInRect(&R_ECHO_VOICE, p) ? ON_VOICE : PtInRect(&R_ECHO_CLIP, p) ? ON_CLIP
                          : PtInRect(&R_ECHO_APP, p) ? ON_PROGRAM : 0;
            if (bit) {
                g.echoOn = g.echoOn.load() ^ bit;
                settingsSave();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            const int maskBit = PtInRect(&R_MASK_VOICE, p) ? ON_VOICE : PtInRect(&R_MASK_CLIP, p) ? ON_CLIP
                              : PtInRect(&R_MASK_APP, p) ? ON_PROGRAM : 0;
            if (maskBit) {
                g.changeOn = g.changeOn.load() ^ maskBit;
                settingsSave();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        if (PtInRect(&mo, p)) {
            g.monitor = !g.monitor.load();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&wd, p)) {
            g.wide = !g.wide.load();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&rw, p)) {
            g.rawMic = !g.rawMic.load();
            g.reopenMic = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&cm, p)) {
            g.cmss = !g.cmss.load();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_ISOLATE, p)) {
            g.isolate = !g.isolate.load();
            settingsSave();
            g.reopenMic = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_STARTUP, p)) {
            if (startupSet(!startupOn)) startupOn = !startupOn;
            oldStartupTasks(h, true);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_ISO_PREV, p) || PtInRect(&R_ISO_NEXT, p)) {
            int at = 0;
            for (int k = 0; k < ISO_MODES; ++k)
                if (ISO_ORDER[k] == g.isoMode.load()) at = k;
            at = PtInRect(&R_ISO_NEXT, p) ? (at + 1) % ISO_MODES : (at + ISO_MODES - 1) % ISO_MODES;
            g.isoMode = ISO_ORDER[at];
            g.isoVersion.fetch_add(1);
            settingsSave();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }

        if (PtInRect(&R_TONE, p)) {
            hotLeave(h);
            openToneBox(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        {
            RECT grabIso = {R_ISOLVL.left - 8, R_ISOLVL.top - 6, R_ISOLVL.right + 8, R_ISOLVL.bottom + 6};
            if (PtInRect(&grabIso, p)) {
                dragIso = true;
                SetCapture(h);
                setIsoFromX(h, p.x);
                return 0;
            }
        }
        if (PtInRect(&R_BOARD, p)) {
            hotLeave(h);
            openBoard(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_SAVE, p)) {
            saveSetupAs(h);
            return 0;
        }
        if (PtInRect(&R_SETUP_DEL, p) && !curSetup.empty()) {
            DeleteFileW(setupPath(curSetup).c_str());
            for (const std::wstring& d : otherSetupDirs) DeleteFileW((d + L"\\" + curSetup + L".ini").c_str());
            curSetup.clear();
            rememberSetup(L"");
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_SETUP_PREV, p) || PtInRect(&R_SETUP_NEXT, p)) {
            setupStep(PtInRect(&R_SETUP_NEXT, p) ? 1 : -1);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_EQ_ON, p)) {
            g.eqOn = !g.eqOn.load();
            eqSave();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_EQ_SAVE, p)) {
            const int cur = g.eqPreset.load();
            hotLeave(h);
            std::wstring name = askSetupName(h, cur >= eqb::NUM_EQ_PRESETS ? eqPresetName(cur) : L"");
            if (!name.empty()) {
                EqUserPreset np; np.name = name;
                for (int k = 0; k < eqb::EQ_BANDS; ++k) np.band[k] = g.eqDb[k].load();
                eqUserStore(np);
                eqUserLoad();
                int i = eqb::NUM_EQ_PRESETS;
                for (size_t u = 0; u < eqUser.size(); ++u) if (eqUser[u].name == name) i = eqb::NUM_EQ_PRESETS + (int)u;
                g.eqPreset = i;
                fillEqPresets();
                eqSave();
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_EQ_DROP, p)) {
            const int cur = g.eqPreset.load();
            if (cur >= eqb::NUM_EQ_PRESETS) {
                eqUserDrop(eqPresetName(cur));
                eqUserLoad();
                g.eqPreset = 0;
                fillEqPresets();
                eqSave();
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (PtInRect(&R_EQ_DEF, p)) {
            if (uiAnim) glideKnobTo(h, K_EQ_LEVEL, 0);
            else g.eqLevel = 0;
            eqGlidePreset(h, 0);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        for (int k = 0; k <= eqb::EQ_BANDS; ++k) {
            const int x = eqFaderX(k);
            RECT fr = {x - 12, EQ_FT - 8, x + 13, EQ_FB + 8};
            if (PtInRect(&fr, p)) {
                dragEq = k;
                SetCapture(h);
                setEqFromY(h, k, p.y);
                return 0;
            }
        }

        if (!cablePanel) {
            RECT sbr = {PRESET_SBX, PRESET_Y, PRESET_SBX + PRESET_SBW, PRESET_Y + PRESET_H};
            if (PtInRect(&sbr, p)) {
                const int AH    = PRESET_SBW;
                const int ITEM_H = 22;
                int total = (int)SendMessageW(hPreset, LB_GETCOUNT, 0, 0);
                int top   = (int)SendMessageW(hPreset, LB_GETTOPINDEX, 0, 0);
                int vis   = PRESET_H / ITEM_H;
                int ry    = p.y - PRESET_Y;
                if (ry < AH) {
                    if (top > 0) presetTopTo(h, top - 1);
                } else if (ry >= PRESET_H - AH) {
                    if (top + vis < total) presetTopTo(h, top + 1);
                } else {
                    int trackH = PRESET_H - 2*AH;
                    int thumbH = (total > vis) ? max(AH, trackH * vis / total) : trackH;
                    int maxTop = max(1, total - vis);
                    int thumbY = (int)((long long)top * (trackH - thumbH) / maxTop);
                    int relY   = ry - AH;
                    if (relY >= thumbY && relY < thumbY + thumbH) {
                        sbDragOff = relY - thumbY;
                        SetCapture(h);
                    } else if (relY < thumbY) {
                        presetTopTo(h, max(0, top - vis));
                    } else {
                        presetTopTo(h, min(total - vis, top + vis));
                    }
                }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        if (p.y < TITLE_H) {
            ReleaseCapture();
            SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (PtInRect(&R_GRIP, p)) {
            if (uiAnim) {
                uiZoom = 100;
                zoomAim(h, zoomFit(h, 100.0f));
            } else {
                setZoom(h, 100, false);
            }
            settingsSave();
            return 0;
        }
        if (!cablePanel && PtInRect(&R_REC_PANEL, p) && !takeRepeatable(p)) return 0;
        if (!cablePanel) {
            if (const int k = knobAt(p); k >= 0) {
                glideKnob(h, k);
                return 0;
            }
        }
        return wndProc(h, WM_LBUTTONDOWN, w, l);
    }
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (!mouseTracked) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            mouseTracked = TrackMouseEvent(&tme) != FALSE;
        }
        if (zoomDrag) { zoomDragTo(h); return 0; }
        if (take.w.drag != WD_NONE) {
            if (waveMove(take.w, p, nullptr)) take.note.clear();
            takeRefresh(h);
            return 0;
        }
        if (dragSlider) { setAmountFromX(h, p.x); return 0; }
        if (dragMic) { setMicFromX(h, p.x); return 0; }
        if (dragClip) { setClipFromX(h, p.x); return 0; }
        if (dragPitch) { setPitchFromX(h, p.x); return 0; }
        if (dragSize) { setSizeFromX(h, p.x); return 0; }
        if (dragVoice) { setVoiceFromX(h, p.x); return 0; }
        if (dragVoiceSpeed) { setVoiceSpeedFromX(h, p.x); return 0; }
        if (dragClipSpeed) { setClipSpeedFromX(h, p.x); return 0; }
        if (dragApp) { setAppFromX(h, p.x); return 0; }
        if (dragPos) { setPosFromX(h, p.x); return 0; }
        if (dragIso) { setIsoFromX(h, p.x); return 0; }
        if (dragEq >= 0) { setEqFromY(h, dragEq, p.y); return 0; }
        if (sbDragOff >= 0) {
            const int AH    = PRESET_SBW;
            const int ITEM_H = 22;
            int total   = (int)SendMessageW(hPreset, LB_GETCOUNT, 0, 0);
            int vis     = PRESET_H / ITEM_H;
            int trackH  = PRESET_H - 2*AH;
            int thumbH  = (total > vis) ? max(AH, trackH * vis / total) : trackH;
            int maxTop  = max(1, total - vis);
            int relY    = (p.y - PRESET_Y) - AH - sbDragOff;
            int newTop  = (int)((long long)relY * maxTop / max(1, trackH - thumbH));
            if (newTop < 0) newTop = 0;
            if (newTop > maxTop) newTop = maxTop;
            if (newTop != (int)SendMessageW(hPreset, LB_GETTOPINDEX, 0, 0)) {
                SendMessageW(hPreset, WM_SETREDRAW, FALSE, 0);
                SendMessageW(hPreset, LB_SETTOPINDEX, newTop, 0);
                SendMessageW(hPreset, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(hPreset, nullptr, FALSE);
                roomHoverRefresh();
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        hotStatus = PtInRect(&rStatus, p) != 0;
        {
            const int wh = !cablePanel && takeReady() ? waveHit(take.w, p) : WH_NONE;
            const int hx = wh == WH_WAVE ? p.x : -1;
            if (wh != take.w.hot || hx != waveHoverX) {
                take.w.hot = wh;
                waveHoverX = hx;
                invalidateDesign(h, R_REC_WAVE);
            }
        }
        RECT now = {0, 0, 0, 0};
        for (const RECT* r : hoverables())
            if (PtInRect(r, p)) { now = *r; break; }
        if (!EqualRect(&now, &hotRect)) {
            invalidateDesign(h, hotRect);
            invalidateDesign(h, now);
            hotRect = now;
            hotMoveTo(now);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        mouseTracked = false;
        hotLeave(h);
        if (waveHoverX >= 0 || take.w.hot != WH_NONE) {
            waveHoverX = -1;
            take.w.hot = WH_NONE;
            invalidateDesign(h, R_REC_WAVE);
        }
        return 0;
    case WM_SETCURSOR: {
        RECT probe;
        if (LOWORD(l) == HTCLIENT && waveEdgeCursor(take.w)) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32644)));
            return TRUE;
        }
        if (LOWORD(l) == HTCLIENT && (zoomDrag || isHot(R_GRIP))) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32642)));
            return TRUE;
        }
        if (LOWORD(l) == HTCLIENT &&
            ((!IsRectEmpty(&hotRect) && !IntersectRect(&probe, &hotRect, &R_SB_ALL)) ||
             (!cablePanel && g.needCable.load() && hotStatus))) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;
        }
        break;
    }
    case WM_RBUTTONDOWN:
        break;
    case WM_LBUTTONUP:
        if (zoomDrag) {
            ReleaseCapture();
            zoomDone(h);
        }
        if (dragSlider) { dragSlider = false; ReleaseCapture(); }
        if (dragMic) { dragMic = false; ReleaseCapture(); }
        if (dragClip) { dragClip = false; ReleaseCapture(); }
        if (dragPitch) { dragPitch = false; ReleaseCapture(); settingsSave(); }
        if (dragSize) { dragSize = false; ReleaseCapture(); settingsSave(); }
        if (dragVoice) { dragVoice = false; ReleaseCapture(); settingsSave(); }
        if (dragVoiceSpeed) { dragVoiceSpeed = false; ReleaseCapture(); settingsSave(); }
        if (dragClipSpeed) { dragClipSpeed = false; ReleaseCapture(); settingsSave(); }
        if (dragApp) { dragApp = false; ReleaseCapture(); settingsSave(); }
        if (dragPos) {
            dragPos = false; ReleaseCapture();
            gClip.seek(gClip.durationSec.load() * dragPosVal / 1000.0);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (holdGlitch) { holdGlitch = false; g.glitchHold = false; ReleaseCapture(); InvalidateRect(h, nullptr, FALSE); }
        if (dragIso) { dragIso = false; ReleaseCapture(); }
        if (dragEq >= 0) { dragEq = -1; ReleaseCapture(); eqSave(); }
        if (sbDragOff >= 0) { sbDragOff = -1; ReleaseCapture(); }
        if (take.w.drag != WD_NONE) {
            const int x = (short)LOWORD(l);
            const bool click = waveUp(take.w, x);
            ReleaseCapture();
            if (click) {
                takePlayAt(h, take.w.timeAt(x));
            } else {
                takeSay(take.w.whole() ? L"" : L"Picked " + trimClock(take.w.to - take.w.from) + L". Cut out removes it, Keep only keeps just that part");
                takeRefresh(h);
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        take.w.drag = WD_NONE;
        if (holdGlitch) { holdGlitch = false; g.glitchHold = false; InvalidateRect(h, nullptr, FALSE); }
        if ((HWND)l != h) dropDrags(h);
        return 0;
    case WM_MOUSEWHEEL: {
        if (cablePanel) return 0;
        const POINT pt = wheelPoint(h, l);
        if (takeWheel(h, pt, GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA)) return 0;
        int d = GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1;
        RECT micArea = {R_MIC.left - 8, R_MIC.top - 14,
                        R_MIC.right + 8, R_MIC.bottom + 14};
        if (PtInRect(&micArea, pt)) {
            wheelKnob(h, K_MIC, std::max(0, std::min(MIC_MAX, knobAim(K_MIC) + d * 10)));
            return 0;
        }
        {
            RECT pitchArea = {R_PITCH.left - 8, R_PITCH.top - 10, R_PITCH.right + 8, R_PITCH.bottom + 10};
            RECT sizeArea  = {R_SIZE.left - 8, R_SIZE.top - 10, R_SIZE.right + 8, R_SIZE.bottom + 10};
            if (PtInRect(&pitchArea, pt)) {
                wheelKnob(h, K_PITCH, std::max(-PITCH_SPAN, std::min(PITCH_SPAN, knobAim(K_PITCH) + d * 5)));
                return 0;
            }
            if (PtInRect(&sizeArea, pt)) {
                wheelKnob(h, K_SIZE, std::max(VSIZE_MIN, std::min(VSIZE_MAX, knobAim(K_SIZE) + d * 2)));
                return 0;
            }
            RECT voiceArea = {R_VOICE.left - 8, R_VOICE.top - 10, R_VOICE.right + 8, R_VOICE.bottom + 10};
            if (PtInRect(&voiceArea, pt)) {
                wheelKnob(h, K_VOICE, std::max(-100, std::min(100, knobAim(K_VOICE) + d * 5)));
                return 0;
            }
            RECT voiceSpeedArea = {R_VSPEED.left - 8, R_VSPEED.top - 10, R_VSPEED.right + 8, R_VSPEED.bottom + 10};
            if (PtInRect(&voiceSpeedArea, pt)) {
                wheelKnob(h, K_VSPEED, stepSpeed(knobAim(K_VSPEED), d));
                return 0;
            }
            RECT clipSpeedArea = {R_CLIP_SPEED.left - 8, R_CLIP_SPEED.top - 10, R_CLIP_SPEED.right + 8, R_CLIP_SPEED.bottom + 10};
            if (PtInRect(&clipSpeedArea, pt)) {
                wheelKnob(h, K_CLIP_SPEED, stepSpeed(knobAim(K_CLIP_SPEED), d));
                return 0;
            }
        }
        RECT presetArea = {PRESET_X, PRESET_Y, PRESET_SBX + PRESET_SBW, PRESET_Y + PRESET_H};
        if (PtInRect(&presetArea, pt)) {
            int top   = (int)SendMessageW(hPreset, LB_GETTOPINDEX, 0, 0);
            int total = (int)SendMessageW(hPreset, LB_GETCOUNT, 0, 0);
            int vis   = PRESET_H / 22;
            int nt    = top - d;
            if (nt < 0) nt = 0;
            if (nt > max(0, total - vis)) nt = max(0, total - vis);
            presetTopTo(h, nt);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        {
            RECT isoArea = {R_ISOLVL.left - 8, R_ISOLVL.top - 10, R_ISOLVL.right + 8, R_ISOLVL.bottom + 10};
            if (PtInRect(&isoArea, pt)) {
                wheelKnob(h, K_ISO, std::max(0, std::min(100, knobAim(K_ISO) + d * 5)));
                return 0;
            }
        }
        for (int k = 0; k <= eqb::EQ_BANDS; ++k) {
            const int x = eqFaderX(k);
            RECT fr = {x - 14, EQ_FT - 10, x + 15, EQ_FB + 26};
            if (!PtInRect(&fr, pt)) continue;
            if (k < eqb::EQ_BANDS) wheelKnob(h, K_EQ + k, std::max(-eqb::EQ_RANGE, std::min(eqb::EQ_RANGE, knobAim(K_EQ + k) + d)));
            else wheelKnob(h, K_EQ_LEVEL, std::max(0, std::min(eqb::EQ_LEVEL_RANGE, knobAim(K_EQ_LEVEL) - d)));
            return 0;
        }
        RECT roomArea = {12, TITLE_H + 146, COL_W - 12, TITLE_H + 444};
        if (PtInRect(&roomArea, pt)) wheelKnob(h, K_AMOUNT, std::max(0, std::min(100, knobAim(K_AMOUNT) + d)));
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if ((code == CBN_DROPDOWN || code == CBN_CLOSEUP) && l) faceChange((HWND)l, h, code == CBN_DROPDOWN);
        if (code == CBN_CLOSEUP && l) PostMessageW(h, WM_FACE_FADE, 0, l);
        if (code == CBN_DROPDOWN && l) {
            if (const HWND list = dropListOf((HWND)l)) shineReset(list, (int)SendMessageW((HWND)l, CB_GETCURSEL, 0, 0));
            dropArm((HWND)l);
            PostMessageW(h, WM_FIT_DROP, 0, l);
        }
        if (id == ID_MIC && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(hMic, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)uiMics.size()) {
                std::lock_guard<std::mutex> lk(g.devMx);
                g.micId = uiMics[i].id;
            }
            g.restart = true;
        } else if (id == ID_SPK && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(hSpk, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)uiSpks.size()) {
                std::lock_guard<std::mutex> lk(g.devMx);
                g.spkId = uiSpks[i].id;
            }
            g.restart = true;
        } else if ((id == ID_MIC || id == ID_SPK) && code == CBN_DROPDOWN) {
            devicesScan(h, false);
        } else if (id == ID_APP && code == CBN_DROPDOWN) {
            fillPrograms();
        } else if (id == ID_APP && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(hApp, CB_GETCURSEL, 0, 0);
            appNote.clear();
            if (i <= 0) gApp.stop();
            else if (i - 1 < (int)uiApps.size()) appPick(uiApps[i - 1]);
            InvalidateRect(h, nullptr, FALSE);
        } else if (id == ID_EQBOX && code == CBN_SELCHANGE) {
            eqGlidePreset(h, (int)SendMessageW(hEqBox, CB_GETCURSEL, 0, 0));
            InvalidateRect(h, nullptr, FALSE);
        } else if (id == ID_GLITCHBOX && code == CBN_SELCHANGE) {
            const int i = (int)SendMessageW(hGlitchBox, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < voice::NUM_GLITCH_STYLES) {
                g.glitchStyle = i;
                settingsSave();
            }
            InvalidateRect(h, nullptr, FALSE);
        } else if (id == ID_MASKBOX && code == CBN_SELCHANGE) {
            const int i = (int)SendMessageW(hMaskBox, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < voice::NUM_XFI_MASKS) {
                pickMask(i);
                settingsSave();
                InvalidateRect(h, nullptr, FALSE);
            }
        } else if (id == ID_OWNBOX && code == CBN_SELCHANGE) {
            const int i = (int)SendMessageW(hOwnBox, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < voice::NUM_OWN_MASKS) {
                pickMask(voice::OWN_ORDER[i]);
                settingsSave();
                InvalidateRect(h, nullptr, FALSE);
            }
        } else if (id == ID_PRESET && code == LBN_SELCHANGE) {
            int i = (int)SendMessageW(hPreset, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < NUM_CARD_PRESETS) g.preset = i;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_TIMER: {
        if (w == FX_TIMER) {
            fxFrame(h);
            return 0;
        }
        if (w == ZOOM_TIMER) {
            zoomStep(h);
            return 0;
        }
        if (w == GLIDE_TIMER) {
            glideStep(h);
            return 0;
        }
        if (w == 2) {
            if (devSpinAt) invalidateDesign(h, R_DEV_REFRESH);
            meterTick();
            takeTick(h);
            const RECT fadeIn = hotIn.r, fadeOut = hotOut.r, pressed = hotPress.r;
            if (hotFadeStep(16.0)) { invalidateDesign(h, fadeIn); invalidateDesign(h, fadeOut); invalidateDesign(h, pressed); }
            if (!cablePanel && presetBarStep(16.0)) invalidateDesign(h, R_SB_ALL);
            ticksRepaint(h);
            if (!cablePanel) {
                UpdateWindow(h);
                const RECT strip = scRect(stripRect());
                InvalidateRect(h, &strip, FALSE);
                static int lagShown = -1, lagTicks = 0;
                const int lagNow = g.voiceSpeed.load() != 100 ? g.voiceLagMs.load() / 100 : -1;
                if (++lagTicks >= 15 && lagNow != lagShown) { lagTicks = 0; lagShown = lagNow; invalidateDesign(h, RECT{R_VKEEP.left, R_VLAG.top, R_VLAG.right, R_VLAG.bottom}); }
                static bool keyShown = false;
                const bool keyNow = g.glitchKeyHeld.load();
                if (keyNow != keyShown) { keyShown = keyNow; invalidateDesign(h, R_GLITCH); }
                if (!glitchNote.empty() && GetTickCount64() - glitchNoteAt >= 3000) { glitchNote.clear(); invalidateDesign(h, R_GLITCH); }
            }
            return 0;
        }
        const ULONGLONG now = GetTickCount64();
        if (g.askCable.load() && !cablePanel && !cableAsked) {
            cableAsked = true;
            cablePanel = true;
            applyLayout();
        }
        if (gApp.finished()) {
            appNote = appEndNote();
            gApp.stop();
            if (hApp) SendMessageW(hApp, CB_SETCURSEL, 0, 0);
        }
        static ULONGLONG healAt = 0;
        if (now - healAt >= 10000) {
            healAt = now;
            healPrograms();
            if (hotkeysBusy && !boardListening) hotkeysRegister(-1, true);
        }
        gBoard.sweep();
        gTakeEar.sweep();
        gTakeMic.sweep();
        presetTopKeep();
        settingsFlush();
        backlogSync();
        hearTapSync();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        if (zoomDrag) {
            zoomDone(h);
            ReleaseCapture();
        }
        zoomSettle(h);
        if (winFx.kind == FX_DOCK || winFx.kind == FX_UNDOCK) {
            KillTimer(h, FX_TIMER);
            winFx.kind = FX_NONE;
            dockDrop();
            break;
        }
        if (uiAnim && IsWindowVisible(h) && !IsIconic(h)) {
            if (winFx.kind != FX_CLOSE) fxStart(h, FX_CLOSE);
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_MINIMIZE) zoomSettle(h);
        if ((w & 0xFFF0) == SC_MINIMIZE && uiAnim && IsWindowVisible(h) && !IsIconic(h) &&
            (winFx.kind == FX_NONE || winFx.kind == FX_OPEN || winFx.kind == FX_UNDOCK)) {
            if (!dockDown(h)) fxStart(h, FX_HIDE);
            return 0;
        }
        break;
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) zoomPark(h);
        if (w == SIZE_MINIMIZED && uiAnim) {
            if (winFx.kind == FX_OPEN || winFx.kind == FX_HIDE) {
                KillTimer(h, FX_TIMER);
                winFx.kind = FX_NONE;
            }
            if (winFx.kind == FX_UNDOCK) dockAway(h, dock.to);
            SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
            winFx.faded = true;
        } else if (w == SIZE_RESTORED && winFx.faded && winFx.kind != FX_CLOSE) {
            winFx.faded = false;
            zoomUnpark(h);
            if (!uiAnim) applyWindowAlpha(h);
            else if (!dockUp(h)) fxStart(h, FX_OPEN);
        } else if (w == SIZE_RESTORED) {
            zoomUnpark(h);
        }
        break;
    case WM_EXITSIZEMOVE:
        if (winFx.kind == FX_NONE && !IsIconic(h)) {
            RECT wr;
            GetWindowRect(h, &wr);
            winX = wr.left;
            winY = wr.top;
            settingsLater();
        }
        break;
    case WM_ENDSESSION:
        if (w) {
            settingsFlush();
            presetTopKeep();
            g.quit = true;
            gApp.stop();
            if (takeFxJob) takeFxJob->stop = true;
            mediaSettle(4000);
        }
        return 0;
    case WM_DESTROY:
        dockDrop();
        ghostClear();
        settingsFlush();
        presetTopKeep();
        g.quit = true;
        gApp.stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool glitchCapture(const MSG& m) {
    if (!glitchListening || !m.hwnd || GetAncestor(m.hwnd, GA_ROOT) != hwndMain) return false;
    UINT vk = 0;
    switch (m.message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        vk = (UINT)m.wParam;
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return true;
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN: {
        const LPARAM at = designPt(m.lParam);
        const POINT p = {(short)LOWORD(at), (short)HIWORD(at)};
        if ((m.message == WM_LBUTTONDOWN || m.message == WM_LBUTTONDBLCLK) && m.hwnd == hwndMain && PtInRect(&R_GLITCH_KEY, p)) return false;
        glitchListening = false;
        glitchNote.clear();
        InvalidateRect(hwndMain, nullptr, FALSE);
        return false;
    }
    case WM_MBUTTONDOWN:
        vk = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN:
        vk = GET_XBUTTON_WPARAM(m.wParam) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        return true;
    default:
        return false;
    }
    glitchListening = false;
    glitchNote.clear();
    if (vk == VK_BACK || vk == VK_DELETE) {
        g.glitchKey = 0;
        glitchNote = L"Glitch key removed";
    } else if (vk != VK_ESCAPE) {
        const UINT mods = heldMods();
        if (!mods && plainKey(vk)) {
            glitchNote = L"Hold Ctrl, Alt or Shift with that key";
        } else {
            boardStopListening();
            const std::wstring was = freeChord(mods, vk, BOARD_PADS + 1);
            hotkeysRegister();
            boardSave();
            g.glitchArmed = false;
            g.glitchKey = (int)((mods << 16) | vk);
            glitchNote = L"Hold " + keyName(mods, vk) + was;
        }
    }
    glitchNoteAt = GetTickCount64();
    settingsSave();
    if (hBoard) InvalidateRect(hBoard, nullptr, FALSE);
    if (hwndMain) InvalidateRect(hwndMain, nullptr, FALSE);
    return true;
}

static void placeKept(HWND h) {
    RECT wr;
    GetWindowRect(h, &wr);
    const int w = wr.right - wr.left, ht = wr.bottom - wr.top;
    const POINT probe = {winX + w / 2, winY + std::min(ht / 2, sc(TITLE_H))};
    HMONITOR mon = MonitorFromPoint(probe, MONITOR_DEFAULTTONULL);
    if (!mon) return;
    const RECT wa = workAreaOf(mon);
    const int x = std::max((int)wa.left, std::min(winX, (int)wa.right - w));
    const int y = std::max((int)wa.top, std::min(winY, (int)wa.bottom - ht));
    SetWindowPos(h, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static bool typingIn(HWND h) {
    wchar_t cls[16] = L"";
    return GetClassNameW(h, cls, 16) > 0 && _wcsicmp(cls, L"Edit") == 0;
}

static bool appShortcut(const MSG& m) {
    if (m.message != WM_KEYDOWN || !hwndMain || (m.lParam & 0x40000000) != 0) return false;
    if (m.hwnd != hwndMain && !IsChild(hwndMain, m.hwnd)) return false;
    if (typingIn(m.hwnd) || !IsWindowEnabled(hwndMain) || cablePanel) return false;
    const UINT mods = heldMods(), vk = (UINT)m.wParam;
    HWND h = hwndMain;
    if (vk == VK_SPACE && !mods && takeReady()) takePlay(h, TAKE_EAR);
    else if (vk == VK_DELETE && !mods && takeReady()) takeCut(h, false);
    else if (vk == 'Z' && mods == MOD_CONTROL && (takeReady() || takeRestorable())) takeUndo(h);
    else if (((vk == 'Y' && mods == MOD_CONTROL) || (vk == 'Z' && mods == (MOD_CONTROL | MOD_SHIFT))) && takeReady()) takeRedo(h);
    else return false;
    InvalidateRect(h, nullptr, FALSE);
    return true;
}

int WINAPI WinMain(_In_ HINSTANCE hi, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nShow) {
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        const bool install = argv && argc > 2 && wcscmp(argv[1], INSTALL_FLAG) == 0;
        const std::wstring setup = install ? std::wstring(argv[2]) : std::wstring();
        if (argv) LocalFree(argv);
        if (install) return elevatedInstall(setup);
    }
    HANDLE single = nullptr;
    for (int wait = 0;; ++wait) {
        single = CreateMutexW(nullptr, FALSE, L"Local\\OctopusEAX.Running");
        const DWORD err = GetLastError();
        const bool taken = single ? err == ERROR_ALREADY_EXISTS : err == ERROR_ACCESS_DENIED;
        const HWND other = FindWindowW(L"OctopusEAXWnd", nullptr);
        if (other) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            ShowWindow(other, SW_SHOW);
            SetForegroundWindow(other);
        }
        if (!taken && !other) break;
        if (single) CloseHandle(single);
        single = nullptr;
        if (other || wait >= 30) return 0;
        Sleep(100);
    }
    const bool firstRun = !settingsKept();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool media = clip::mediaReady() && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    Gdiplus::GdiplusStartupInput gdipIn;
    Gdiplus::GdiplusStartup(&gdipTok, &gdipIn, nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    HICON hAppIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON hSmallIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    eqUserLoad();
    eqLoad();
    settingsLoad();
    {
        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        uiDpi = dpiOfMonitor(primary);
        uiScale = fitScale(zoomScale(), workAreaOf(primary));
    }
    makeFonts();
    gApp.hijack.memory.key = L"Software\\OctopusEAX\\Parked";
    applyLook();
    buildBackground();
    startupOn = startupIsOn();
    rememberCopyFolder(runFolder());
    rememberCopyFolder(exeFolder());
    if (startupOn) startupFollow();

    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = scaledProc<wndProc>;
    wc.hInstance = hi;
    wc.lpszClassName = L"OctopusEAXWnd";
    {
        WNDCLASSW pc{}; pc.lpfnWndProc = scaledProc<promptProc>; pc.hInstance = hi; pc.lpszClassName = L"OctopusPrompt";
        pc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&pc);
        WNDCLASSW cc{}; cc.lpfnWndProc = scaledProc<colorProc>; cc.hInstance = hi; cc.lpszClassName = L"OctopusColor";
        cc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&cc);
        WNDCLASSW lk{}; lk.style = CS_DBLCLKS; lk.lpfnWndProc = scaledProc<lookProc>; lk.hInstance = hi; lk.lpszClassName = L"OctopusLook";
        lk.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&lk);
        WNDCLASSW ab{}; ab.lpfnWndProc = scaledProc<aboutProc>; ab.hInstance = hi; ab.lpszClassName = L"OctopusAbout";
        ab.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&ab);
        WNDCLASSW tn{}; tn.style = CS_DBLCLKS; tn.lpfnWndProc = scaledProc<toneProc>; tn.hInstance = hi; tn.lpszClassName = L"OctopusTone";
        tn.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&tn);
        WNDCLASSW bd{}; bd.style = CS_DBLCLKS; bd.lpfnWndProc = scaledProc<boardProc>; bd.hInstance = hi; bd.lpszClassName = L"OctopusBoard";
        bd.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&bd);
        WNDCLASSW tr{}; tr.lpfnWndProc = scaledProc<trimProc>; tr.hInstance = hi; tr.lpszClassName = L"OctopusTrim";
        tr.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&tr);
    }
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = hAppIcon;
    RegisterClassW(&wc);

    const RECT wa = workAreaOf(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    int sx = wa.left + (wa.right - wa.left - sc(WIN_W)) / 2;
    int sy = wa.top + (wa.bottom - wa.top - sc(WIN_H)) / 2;
    if (sx < wa.left) sx = wa.left;
    if (sy < wa.top) sy = wa.top;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, L"OctopusEAXWnd", L"OctopusEAX",
                                WS_POPUP | WS_CLIPCHILDREN, sx, sy, sc(WIN_W), sc(WIN_H),
                                nullptr, nullptr, hi, nullptr);
    hwndMain = hwnd;
    oldStartupTasks(hwnd, false);
    applyWindowAlpha(hwnd);
    SetWindowPos(hwnd, nullptr, sx, sy, sc(WIN_W), sc(WIN_H), SWP_NOZORDER);
    if (uiPlace && winX != NO_PLACE) placeKept(hwnd);
    if (uiTop) applyTopmost(hwnd);
    if (hAppIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hAppIcon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)(hSmallIcon ? hSmallIcon : hAppIcon));
    }

    auto mk = [&](const wchar_t* cls, DWORD style, int id) {
        HWND c = CreateWindowW(cls, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, hi, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)fntCtl, TRUE);
        return c;
    };
    hMic = mk(L"COMBOBOX", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | WS_VSCROLL, ID_MIC);
    hSpk = mk(L"COMBOBOX", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | WS_VSCROLL, ID_SPK);
    hPreset = mk(L"LISTBOX", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, ID_PRESET);
    hApp = mk(L"COMBOBOX", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ID_APP);
    fillPrograms();
    hEqBox = mk(L"COMBOBOX", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ID_EQBOX);
    fillEqPresets();
    hMaskBox = mk(L"COMBOBOX", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ID_MASKBOX);
    hOwnBox = mk(L"COMBOBOX", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ID_OWNBOX);
    hGlitchBox = mk(L"COMBOBOX", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ID_GLITCHBOX);
    placeChildren();
    fillMasks();
    for (int i = 0; i < voice::NUM_GLITCH_STYLES; ++i) SendMessageW(hGlitchBox, CB_ADDSTRING, 0, (LPARAM)voice::GLITCH_LOOKS[i].name);
    showGlitch();

    for (int i = 0; i < NUM_CARD_PRESETS; ++i) {
        wchar_t nm[96];
        MultiByteToWideChar(CP_UTF8, 0, CARD_PRESETS[i].name, -1, nm, 96);
        SendMessageW(hPreset, LB_ADDSTRING, 0, (LPARAM)nm);
    }
    SendMessageW(hPreset, LB_SETCURSEL, DEFAULT_PRESET, 0);

    SetWindowSubclass(hMic, comboSubProc, 0, 0);
    SetWindowSubclass(hSpk, comboSubProc, 0, 0);
    SetWindowSubclass(hApp, comboSubProc, 0, 0);
    SetWindowSubclass(hEqBox, comboSubProc, 0, 0);
    SetWindowSubclass(hMaskBox, comboSubProc, 0, 0);
    SetWindowSubclass(hOwnBox, comboSubProc, 0, 0);
    SetWindowSubclass(hGlitchBox, comboSubProc, 0, 0);
    SetWindowSubclass(hPreset, listWheelProc, 0, 0);
    noTheme(hMic); noTheme(hSpk); noTheme(hPreset); noTheme(hApp); noTheme(hEqBox); noTheme(hMaskBox); noTheme(hOwnBox); noTheme(hGlitchBox);

    auto stripBorder = [](HWND h) {
        LONG_PTR s = GetWindowLongPtr(h, GWL_STYLE);
        SetWindowLongPtr(h, GWL_STYLE, s & ~WS_BORDER);
        LONG_PTR e = GetWindowLongPtr(h, GWL_EXSTYLE);
        SetWindowLongPtr(h, GWL_EXSTYLE, e & ~WS_EX_CLIENTEDGE);
        SetWindowPos(h, nullptr, 0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    };
    stripBorder(hMic); stripBorder(hSpk); stripBorder(hApp);
    dressList(hMic); dressList(hSpk); dressList(hApp); dressList(hEqBox); dressList(hMaskBox); dressList(hOwnBox); dressList(hGlitchBox);

    hClipUrl = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_CLIPURL, hi, nullptr);
    SendMessageW(hClipUrl, WM_SETFONT, (WPARAM)fntCtl, TRUE);
    SendMessageW(hClipUrl, EM_SETLIMITTEXT, 2000, 0);
    SendMessageW(hClipUrl, EM_SETCUEBANNER, TRUE, (LPARAM)L"Paste the sound link");
    noTheme(hClipUrl);
    placeChildren();
    tipsCreate(hwnd);
    if (hTip && !uiHints) SendMessageW(hTip, TTM_ACTIVATE, FALSE, 0);
    applyLayout();
    g.restart = true;
    timeBeginPeriod(1);

    fillDevices();
    {
        addCopySetupDirs();
        syncSetups();
        std::wstring last = lastSetup();
        if (!last.empty() && GetFileAttributesW(setupPath(last).c_str()) != INVALID_FILE_ATTRIBUTES) {
            settingsFileLoad(setupPath(last));
            curSetup = last;
        } else if (firstRun && GetFileAttributesW(iniPath().c_str()) != INVALID_FILE_ATTRIBUTES) {
            settingsFileLoad(iniPath());
        }
        std::thread([owner = hwnd] {
            const MediaWork busy;
            auto* found = new std::vector<std::wstring>(scanSetupRoots());
            if (!PostMessageW(owner, WM_SETUPS_FOUND, 0, (LPARAM)found)) delete found;
        }).detach();
    }
    presetTopPlace();

    std::thread cableThread(provisionCable);

    boardLoad();
    hotkeysRegister();
    {
        std::vector<int> all;
        for (int i = 0; i < BOARD_PADS; ++i) all.push_back(i);
        boardDecode(all);
    }
    SetTimer(hwnd, 1, 400, nullptr);
    SetTimer(hwnd, 2, 16, nullptr);

    std::thread th(audioThread);
    windowOpen(hwnd, nShow);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (glitchCapture(msg)) continue;
        if (appShortcut(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g.quit = true;
    gApp.stop();
    gClip.stop();
    gHearTap.stop();
    th.join();
    cableThread.join();
    clearWallpaper();
    if (logoBmp) { DeleteObject(logoBmp); logoBmp = nullptr; }
    delete logoSrc; logoSrc = nullptr;
    if (artBmp) { DeleteObject(artBmp); artBmp = nullptr; }
    delete artSrc; artSrc = nullptr;
    if (brLookBack) { DeleteObject(brLookBack); brLookBack = nullptr; }
    if (brLookPanel) { DeleteObject(brLookPanel); brLookPanel = nullptr; }
    if (gdipTok) Gdiplus::GdiplusShutdown(gdipTok);
    if (takeFxJob) takeFxJob->stop = true;
    const bool clipDone = gClip.settle(3000);
    const bool mediaDone = mediaSettle(20000);
    if (single) CloseHandle(single);
    if (!clipDone || !mediaDone) ExitProcess(0);
    if (media) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
