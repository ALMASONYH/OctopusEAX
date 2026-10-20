#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <audioclientactivationparams.h>
#include <inspectable.h>
#include <hstring.h>
#include <avrt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace clip {

struct Program {
    DWORD pid = 0;
    std::wstring name;
};

struct ProcessTable {
    struct Row {
        DWORD pid, parent;
        std::wstring exe;
    };
    std::vector<Row> rows;

    ProcessTable() {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W e{};
        e.dwSize = sizeof e;
        if (Process32FirstW(snap, &e))
            do rows.push_back({e.th32ProcessID, e.th32ParentProcessID, e.szExeFile});
            while (Process32NextW(snap, &e));
        CloseHandle(snap);
    }

    const Row* find(DWORD pid) const {
        for (const Row& r : rows) if (r.pid == pid) return &r;
        return nullptr;
    }

    std::wstring name(DWORD pid) const {
        const Row* r = find(pid);
        if (!r) return L"";
        const size_t dot = r->exe.find_last_of(L'.');
        return dot != std::wstring::npos && dot > 0 ? r->exe.substr(0, dot) : r->exe;
    }

    DWORD root(DWORD pid) const {
        const Row* at = find(pid);
        if (!at) return pid;
        for (int hops = 0; hops < 16; ++hops) {
            const Row* up = find(at->parent);
            if (!up || up->pid == at->pid || _wcsicmp(up->exe.c_str(), at->exe.c_str()) != 0) break;
            const ULONGLONG born = started(at->pid), bornUp = started(up->pid);
            if (!born || !bornUp || bornUp > born) break;
            at = up;
        }
        return at->pid;
    }

private:
    static ULONGLONG started(DWORD pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return 0;
        FILETIME made, gone, kernel, user;
        const bool ok = GetProcessTimes(h, &made, &gone, &kernel, &user) != 0;
        CloseHandle(h);
        return ok ? ((ULONGLONG)made.dwHighDateTime << 32) | made.dwLowDateTime : 0;
    }
};

struct WindowScan {
    std::vector<DWORD> pids;
    static BOOL CALLBACK step(HWND h, LPARAM me) {
        WindowScan* w = (WindowScan*)me;
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        wchar_t title[8];
        if (GetWindowTextW(h, title, 8) == 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid) w->pids.push_back(pid);
        return TRUE;
    }
};

inline void addProgram(std::vector<Program>& out, const ProcessTable& table, DWORD pid) {
    if (!pid) return;
    pid = table.root(pid);
    if (pid == GetCurrentProcessId()) return;
    for (const Program& p : out) if (p.pid == pid) return;
    Program p;
    p.pid = pid;
    p.name = table.name(pid);
    if (p.name.empty()) return;
    static const wchar_t* shell[] = {L"TextInputHost", L"ApplicationFrameHost", L"ShellExperienceHost", L"StartMenuExperienceHost", L"SearchHost", L"SystemSettings", L"LockApp"};
    for (const wchar_t* n : shell) if (_wcsicmp(p.name.c_str(), n) == 0) return;
    out.push_back(p);
}

inline std::vector<Program> listPrograms() {
    const ProcessTable table;
    std::vector<Program> out;
    WindowScan scan;
    EnumWindows(WindowScan::step, (LPARAM)&scan);
    for (DWORD pid : scan.pids) addProgram(out, table, pid);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) && en) {
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT n = 0;
            col->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                IMMDevice* d = nullptr;
                if (FAILED(col->Item(i, &d)) || !d) continue;
                IAudioSessionManager2* mgr = nullptr;
                if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) && mgr) {
                    IAudioSessionEnumerator* se = nullptr;
                    if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
                        int cnt = 0;
                        se->GetCount(&cnt);
                        for (int k = 0; k < cnt; ++k) {
                            IAudioSessionControl* sc = nullptr;
                            if (FAILED(se->GetSession(k, &sc)) || !sc) continue;
                            IAudioSessionControl2* sc2 = nullptr;
                            AudioSessionState st = AudioSessionStateExpired;
                            sc->GetState(&st);
                            if (st != AudioSessionStateExpired && SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2)) && sc2) {
                                DWORD pid = 0;
                                sc2->GetProcessId(&pid);
                                addProgram(out, table, pid);
                                sc2->Release();
                            }
                            sc->Release();
                        }
                        se->Release();
                    }
                    mgr->Release();
                }
                d->Release();
            }
            col->Release();
        }
        en->Release();
    }
    std::sort(out.begin(), out.end(), [](const Program& a, const Program& b) {
        const int c = _wcsicmp(a.name.c_str(), b.name.c_str());
        return c ? c < 0 : a.pid < b.pid;
    });
    for (size_t i = 1, twin = 1; i < out.size(); ++i) {
        const std::wstring& first = out[i - twin].name;
        if (_wcsicmp(out[i].name.c_str(), first.c_str()) != 0) { twin = 1; continue; }
        ++twin;
        out[i].name = first + L" (" + std::to_wstring(twin) + L")";
    }
    return out;
}

inline bool pidInList(const std::vector<DWORD>& ids, DWORD pid) {
    for (DWORD p : ids) if (p == pid) return true;
    return false;
}

inline void collectTree(DWORD root, std::vector<DWORD>& out) {
    out.clear();
    if (!root) return;
    const ProcessTable table;
    out.push_back(root);
    bool grew = true;
    while (grew) {
        grew = false;
        for (const ProcessTable::Row& r : table.rows) {
            if (!r.pid) continue;
            bool have = false, par = false;
            for (DWORD p : out) {
                if (p == r.pid) have = true;
                if (p == r.parent) par = true;
            }
            if (!have && par) { out.push_back(r.pid); grew = true; }
        }
    }
    const ProcessTable::Row* self = table.find(root);
    if (self && !self->exe.empty()) {
        for (const ProcessTable::Row& r : table.rows) {
            if (!r.pid || pidInList(out, r.pid)) continue;
            if (_wcsicmp(r.exe.c_str(), self->exe.c_str()) == 0) out.push_back(r.pid);
        }
    }
}

struct IAudioPolicyConfig : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE stub00() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub01() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub02() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub03() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub04() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub05() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub06() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub07() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub08() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub09() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub10() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub11() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub12() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub13() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub14() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub15() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub16() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub17() = 0;
    virtual HRESULT STDMETHODCALLTYPE stub18() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(UINT32 processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(UINT32 processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

inline std::wstring packRenderId(const std::wstring& mmid) {
    if (mmid.empty()) return L"";
    if (mmid.find(L"\\\\?\\SWD#MMDEVAPI#") == 0) return mmid;
    return L"\\\\?\\SWD#MMDEVAPI#" + mmid + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}

inline IAudioPolicyConfig* openPolicy() {
    static const IID kIids[] = {
        {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}},
        {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}},
        {0x32aa8e18, 0x6496, 0x4e24, {0x9f, 0x94, 0xb8, 0x00, 0xe7, 0xec, 0xcc, 0x45}},
    };
    typedef HRESULT (WINAPI * RoGetFn)(HSTRING, REFIID, void**);
    typedef HRESULT (WINAPI * MkStrFn)(const wchar_t*, UINT32, HSTRING*);
    typedef HRESULT (WINAPI * DelStrFn)(HSTRING);
    typedef HRESULT (WINAPI * ActFn)(HSTRING, IInspectable**);
    const HMODULE combase = GetModuleHandleW(L"combase.dll");
    const RoGetFn roGet = combase ? (RoGetFn)(void*)GetProcAddress(combase, "RoGetActivationFactory") : nullptr;
    const MkStrFn mkStr = combase ? (MkStrFn)(void*)GetProcAddress(combase, "WindowsCreateString") : nullptr;
    const DelStrFn delStr = combase ? (DelStrFn)(void*)GetProcAddress(combase, "WindowsDeleteString") : nullptr;
    if (!mkStr || !delStr) return nullptr;
    HSTRING cls = nullptr;
    const wchar_t* nm = L"Windows.Media.Internal.AudioPolicyConfig";
    if (FAILED(mkStr(nm, (UINT32)wcslen(nm), &cls)) || !cls) return nullptr;
    IAudioPolicyConfig* fac = nullptr;
    if (roGet) {
        for (const IID& id : kIids) {
            if (SUCCEEDED(roGet(cls, id, (void**)&fac)) && fac) break;
            fac = nullptr;
        }
    }
    if (!fac) {
        const HMODULE ses = LoadLibraryW(L"AudioSes.dll");
        const ActFn act = ses ? (ActFn)(void*)GetProcAddress(ses, "DllGetActivationFactory") : nullptr;
        IInspectable* af = nullptr;
        if (act && SUCCEEDED(act(cls, &af)) && af) {
            for (const IID& id : kIids) {
                if (SUCCEEDED(af->QueryInterface(id, (void**)&fac)) && fac) break;
                fac = nullptr;
            }
            af->Release();
        }
    }
    delStr(cls);
    return fac;
}

inline HSTRING makeHs(const std::wstring& s, HRESULT (WINAPI * mkStr)(const wchar_t*, UINT32, HSTRING*)) {
    if (!mkStr || s.empty()) return nullptr;
    HSTRING hs = nullptr;
    mkStr(s.c_str(), (UINT32)s.size(), &hs);
    return hs;
}

inline std::wstring imagePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t path[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
    CloseHandle(h);
    return ok ? std::wstring(path, n) : L"";
}

inline bool sameProgram(DWORD pid, const std::wstring& exe) {
    if (!pid) return false;
    if (exe.empty()) return true;
    return _wcsicmp(imagePath(pid).c_str(), exe.c_str()) == 0;
}

inline bool sameDevice(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(packRenderId(a).c_str(), packRenderId(b).c_str()) == 0;
}

inline bool getPersisted(IAudioPolicyConfig* fac, DWORD pid, std::wstring& out) {
    const HMODULE combase = GetModuleHandleW(L"combase.dll");
    auto delStr = combase ? (HRESULT (WINAPI *)(HSTRING))GetProcAddress(combase, "WindowsDeleteString") : nullptr;
    auto raw = combase ? (PCWSTR (WINAPI *)(HSTRING, UINT32*))GetProcAddress(combase, "WindowsGetStringRawBuffer") : nullptr;
    out.clear();
    HSTRING cur = nullptr;
    if (FAILED(fac->GetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, &cur))) return false;
    if (cur) {
        UINT32 n = 0;
        const PCWSTR txt = raw ? raw(cur, &n) : nullptr;
        if (txt && n) out.assign(txt, n);
        if (delStr) delStr(cur);
    }
    return true;
}

inline void setPersisted(IAudioPolicyConfig* fac, DWORD pid, const std::wstring prev[3]) {
    const HMODULE combase = GetModuleHandleW(L"combase.dll");
    auto mkStr = combase ? (HRESULT (WINAPI *)(const wchar_t*, UINT32, HSTRING*))GetProcAddress(combase, "WindowsCreateString") : nullptr;
    auto delStr = combase ? (HRESULT (WINAPI *)(HSTRING))GetProcAddress(combase, "WindowsDeleteString") : nullptr;
    static const ERole roles[3] = {eConsole, eMultimedia, eCommunications};
    for (int r = 0; r < 3; ++r) {
        HSTRING hs = makeHs(prev[r], mkStr);
        fac->SetPersistedDefaultAudioEndpoint(pid, eRender, roles[r], hs);
        if (hs && delStr) delStr(hs);
    }
}

inline bool putBack(IAudioPolicyConfig* fac, DWORD pid, const std::wstring prev[3]) {
    setPersisted(fac, pid, prev);
    std::wstring now;
    return getPersisted(fac, pid, now) && sameDevice(now, prev[0]);
}

struct ParkMemory {
    struct Entry {
        std::wstring prev[3], park;
    };
    std::wstring key;

    bool get(const std::wstring& exe, Entry& e) const {
        if (key.empty() || exe.empty()) return false;
        wchar_t buf[4096];
        DWORD size = sizeof buf - sizeof(wchar_t);
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), exe.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return false;
        const std::wstring all = buf;
        std::wstring* field[4] = {&e.prev[0], &e.prev[1], &e.prev[2], &e.park};
        size_t from = 0;
        for (std::wstring* f : field) {
            const size_t bar = all.find(L'|', from);
            *f = all.substr(from, bar == std::wstring::npos ? std::wstring::npos : bar - from);
            from = bar == std::wstring::npos ? all.size() : bar + 1;
        }
        return true;
    }

    void put(const std::wstring& exe, const Entry& e) const {
        if (key.empty() || exe.empty()) return;
        HKEY hk;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
        const std::wstring all = e.prev[0] + L"|" + e.prev[1] + L"|" + e.prev[2] + L"|" + e.park;
        RegSetValueExW(hk, exe.c_str(), 0, REG_SZ, (const BYTE*)all.c_str(), (DWORD)((all.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }

    void drop(const std::wstring& exe) const {
        if (!key.empty() && !exe.empty()) RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), exe.c_str());
    }

    std::vector<std::wstring> programs() const {
        std::vector<std::wstring> out;
        HKEY hk;
        if (key.empty() || RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return out;
        wchar_t name[MAX_PATH * 2];
        for (DWORD i = 0;; ++i) {
            DWORD len = MAX_PATH * 2;
            if (RegEnumValueW(hk, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            out.emplace_back(name, len);
        }
        RegCloseKey(hk);
        return out;
    }
};

inline void healParked(const ParkMemory& memory, const std::vector<std::wstring>& skip) {
    const std::vector<std::wstring> programs = memory.programs();
    if (programs.empty()) return;
    const ProcessTable table;
    IAudioPolicyConfig* fac = nullptr;
    for (const std::wstring& exe : programs) {
        if (std::any_of(skip.begin(), skip.end(), [&](const std::wstring& s) { return _wcsicmp(s.c_str(), exe.c_str()) == 0; })) continue;
        ParkMemory::Entry e;
        if (!memory.get(exe, e)) continue;
        const size_t slash = exe.find_last_of(L"\\/");
        const std::wstring file = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
        for (const ProcessTable::Row& r : table.rows) {
            if (!r.pid || _wcsicmp(r.exe.c_str(), file.c_str()) != 0 || !sameProgram(r.pid, exe)) continue;
            if (!fac && !(fac = openPolicy())) return;
            std::wstring now;
            if (!getPersisted(fac, r.pid, now)) continue;
            if (!sameDevice(now, e.park) || putBack(fac, r.pid, e.prev)) memory.drop(exe);
            break;
        }
    }
    if (fac) fac->Release();
}

struct AppTakeover {
    struct Persist {
        DWORD pid = 0;
        std::wstring exe;
        std::wstring prev[3];
    };
    std::vector<Persist> persist;
    ParkMemory memory;
    std::wstring parkedOn;
    std::atomic<bool> taken{false};
    DWORD targetPid = 0;

    ~AppTakeover() { restore(); }

    void unmuteSessions(DWORD root) {
        std::vector<DWORD> tree;
        collectTree(root, tree);
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en)
            return;
        IMMDeviceCollection* col = nullptr;
        if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) || !col) {
            en->Release();
            return;
        }
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            IAudioSessionManager2* mgr = nullptr;
            if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) && mgr) {
                IAudioSessionEnumerator* se = nullptr;
                if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
                    int cnt = 0;
                    se->GetCount(&cnt);
                    for (int k = 0; k < cnt; ++k) {
                        IAudioSessionControl* sc = nullptr;
                        if (FAILED(se->GetSession(k, &sc)) || !sc) continue;
                        IAudioSessionControl2* sc2 = nullptr;
                        DWORD spid = 0;
                        if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2)) && sc2) {
                            sc2->GetProcessId(&spid);
                            if (spid && pidInList(tree, spid)) {
                                ISimpleAudioVolume* av = nullptr;
                                if (SUCCEEDED(sc->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&av)) && av) {
                                    float vol = 1.f;
                                    BOOL muted = FALSE;
                                    av->GetMasterVolume(&vol);
                                    if (vol < 0.01f) av->SetMasterVolume(1.f, nullptr);
                                    if (FAILED(av->GetMute(&muted)) || muted) av->SetMute(FALSE, nullptr);
                                    av->Release();
                                }
                            }
                            sc2->Release();
                        }
                        sc->Release();
                    }
                    se->Release();
                }
                mgr->Release();
            }
            d->Release();
        }
        col->Release();
        en->Release();
    }

    void routeToPark(DWORD root, const std::wstring& parkId) {
        if (parkId.empty()) return;
        IAudioPolicyConfig* fac = openPolicy();
        if (!fac) return;
        const HMODULE combase = GetModuleHandleW(L"combase.dll");
        auto mkStr = combase ? (HRESULT (WINAPI *)(const wchar_t*, UINT32, HSTRING*))GetProcAddress(combase, "WindowsCreateString") : nullptr;
        auto delStr = combase ? (HRESULT (WINAPI *)(HSTRING))GetProcAddress(combase, "WindowsDeleteString") : nullptr;
        auto raw = combase ? (PCWSTR (WINAPI *)(HSTRING, UINT32*))GetProcAddress(combase, "WindowsGetStringRawBuffer") : nullptr;
        const std::wstring packed = packRenderId(parkId);
        auto parked = [&](const std::wstring& id) { return !id.empty() && _wcsicmp(packRenderId(id).c_str(), packed.c_str()) == 0; };
        HSTRING want = makeHs(packed, mkStr);
        HSTRING rawId = makeHs(parkId, mkStr);
        std::vector<DWORD> tree;
        collectTree(root, tree);
        static const ERole roles[3] = {eConsole, eMultimedia, eCommunications};
        struct Seen {
            DWORD pid = 0;
            std::wstring exe, have[3];
            bool known[3] = {false, false, false};
            bool ours = false;
        };
        std::vector<Seen> seen;
        for (DWORD p : tree) {
            if (!p) continue;
            Seen s;
            s.pid = p;
            for (int r = 0; r < 3; ++r) {
                HSTRING cur = nullptr;
                if (FAILED(fac->GetPersistedDefaultAudioEndpoint(p, eRender, roles[r], &cur))) continue;
                s.known[r] = true;
                if (!cur) continue;
                UINT32 n = 0;
                const PCWSTR txt = raw ? raw(cur, &n) : nullptr;
                if (txt && n) s.have[r].assign(txt, n);
                if (delStr) delStr(cur);
            }
            for (const Persist& q : persist) if (q.pid == p) { s.ours = true; break; }
            s.exe = imagePath(p);
            seen.push_back(s);
        }
        parkedOn = packed;
        for (Seen& s : seen) {
            if (s.ours || s.exe.empty() || !s.known[0]) continue;
            s.ours = true;
            Persist rec;
            rec.pid = s.pid;
            rec.exe = s.exe;
            const Persist* twin = nullptr;
            for (const Persist& q : persist)
                if (_wcsicmp(q.exe.c_str(), rec.exe.c_str()) == 0) { twin = &q; break; }
            ParkMemory::Entry kept;
            const bool remembered = !twin && memory.get(rec.exe, kept);
            for (int r = 0; r < 3; ++r) {
                if (twin) rec.prev[r] = twin->prev[r];
                else if (remembered) rec.prev[r] = kept.prev[r];
                else
                    for (const Seen& o : seen)
                        if (o.known[r] && _wcsicmp(o.exe.c_str(), rec.exe.c_str()) == 0) {
                            rec.prev[r] = parked(o.have[r]) ? std::wstring() : o.have[r];
                            break;
                        }
            }
            persist.push_back(rec);
        }
        for (size_t i = 0; i < persist.size(); ++i) {
            bool first = true;
            for (size_t j = 0; j < i && first; ++j) if (_wcsicmp(persist[j].exe.c_str(), persist[i].exe.c_str()) == 0) first = false;
            ParkMemory::Entry have;
            if (!first || (memory.get(persist[i].exe, have) && sameDevice(have.park, packed))) continue;
            ParkMemory::Entry e;
            for (int r = 0; r < 3; ++r) e.prev[r] = persist[i].prev[r];
            e.park = packed;
            memory.put(persist[i].exe, e);
        }
        for (const Seen& s : seen) {
            if (!s.ours) continue;
            for (int r = 0; r < 3; ++r) {
                if (s.known[r] && parked(s.have[r])) continue;
                fac->SetPersistedDefaultAudioEndpoint(s.pid, eRender, roles[r], want);
                if (rawId) fac->SetPersistedDefaultAudioEndpoint(s.pid, eRender, roles[r], rawId);
            }
        }
        if (want && delStr) delStr(want);
        if (rawId && delStr) delStr(rawId);
        fac->Release();
    }

    bool apply(DWORD root, const std::wstring& parkId) {
        if (!root) return taken.load();
        targetPid = root;
        unmuteSessions(root);
        routeToPark(root, parkId);
        taken = !parkId.empty();
        return taken.load();
    }

    void restore() {
        if (targetPid) unmuteSessions(targetPid);
        if (!persist.empty()) {
            if (IAudioPolicyConfig* fac = openPolicy()) {
                std::vector<std::wstring> back;
                for (const Persist& s : persist)
                    if (sameProgram(s.pid, s.exe) && putBack(fac, s.pid, s.prev)) back.push_back(s.exe);
                fac->Release();
                for (const Persist& s : persist) {
                    bool done = false;
                    for (const std::wstring& b : back) if (_wcsicmp(b.c_str(), s.exe.c_str()) == 0) { done = true; break; }
                    if (done) {
                        memory.drop(s.exe);
                        continue;
                    }
                    ParkMemory::Entry e;
                    for (int r = 0; r < 3; ++r) e.prev[r] = s.prev[r];
                    e.park = parkedOn;
                    memory.put(s.exe, e);
                }
            }
        }
        persist.clear();
        taken = false;
        targetPid = 0;
    }
};

typedef HRESULT (WINAPI * ActivateAudioFn)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                           IActivateAudioInterfaceAsyncOperation**);

inline ActivateAudioFn activateAudio() {
    static const ActivateAudioFn fn = [] {
        wchar_t dir[MAX_PATH] = {0};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        const HMODULE lib = (n && n < MAX_PATH) ? LoadLibraryW((std::wstring(dir) + L"\\mmdevapi.dll").c_str()) : nullptr;
        return lib ? (ActivateAudioFn)(void*)GetProcAddress(lib, "ActivateAudioInterfaceAsync") : nullptr;
    }();
    return fn;
}

inline DWORD windowsBuild() {
    typedef LONG (WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    const RtlGetVersionFn ask = nt ? (RtlGetVersionFn)(void*)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    return ask && ask(&v) == 0 ? v.dwBuildNumber : 0;
}

struct StereoRing {
    std::vector<float> buf;
    size_t cap = 0;
    std::atomic<size_t> wr{0}, rd{0};
    std::atomic<bool> stale{false};
    UINT32 target = 0, slack = 0, limit = 0;
    double level = 0.0;
    int sinceTrim = 0;
    bool primed = false;
    std::vector<float> scratch;

    void init(UINT32 capacityFrames, UINT32 targetFrames, UINT32 slackFrames) {
        cap = capacityFrames;
        if (buf.size() != cap * 2) buf.assign(cap * 2, 0.f);
        target = targetFrames; slack = slackFrames; limit = targetFrames * 6;
    }
    void restart() {
        level = (double)target; sinceTrim = 0;
        stale = true;
    }
    UINT32 filled() const {
        const size_t w = wr.load(std::memory_order_acquire), r = rd.load(std::memory_order_acquire);
        return (UINT32)((w + cap - r) % cap);
    }

    void push(const float* st, UINT32 n) {
        if (n == 0) return;
        const UINT32 have = filled();
        level += 0.05 * ((double)have - level);
        ++sinceTrim;
        const double off = level - (double)target;
        int delta = 0;
        if (n >= 16 && sinceTrim >= 2 && fabs(off) > (double)slack) delta = (int)std::clamp(-off / (double)slack, -4.0, 4.0);
        if (delta == 0) { put(st, n); return; }
        sinceTrim = 0; level += delta;
        const UINT32 m = n + delta;
        scratch.resize((size_t)m * 2);
        const double step = (double)(n - 1) / (double)(m - 1);
        for (UINT32 i = 0; i < m; ++i) {
            const double p = i * step;
            UINT32 k = (UINT32)p;
            if (k >= n - 1) k = n - 2;
            const float t = (float)(p - k);
            scratch[2 * i]     = st[2 * k]     + (st[2 * k + 2] - st[2 * k])     * t;
            scratch[2 * i + 1] = st[2 * k + 1] + (st[2 * k + 3] - st[2 * k + 1]) * t;
        }
        put(scratch.data(), m);
    }

    void pull(float* st, UINT32 n) {
        size_t r = rd.load(std::memory_order_relaxed);
        UINT32 have = filled();
        if (stale.exchange(false)) { r = wr.load(std::memory_order_acquire); have = 0; primed = false; }
        else if (have > limit) { r = (r + (have - target)) % cap; have = target; }
        if (!primed && have >= target) primed = true;
        UINT32 take = !primed ? 0 : (have < n ? have : n);
        if (primed && take < n) primed = false;
        for (UINT32 i = 0; i < take; ++i) {
            st[2 * i] = buf[r * 2]; st[2 * i + 1] = buf[r * 2 + 1];
            if (++r >= cap) r = 0;
        }
        memset(st + (size_t)take * 2, 0, (size_t)(n - take) * 8);
        rd.store(r, std::memory_order_release);
    }

private:
    void put(const float* st, UINT32 n) {
        const UINT32 room = (UINT32)(cap - 1) - filled();
        if (n > room) { n = room; stale = true; }
        size_t w = wr.load(std::memory_order_relaxed);
        for (UINT32 i = 0; i < n; ++i) {
            buf[w * 2] = st[2 * i]; buf[w * 2 + 1] = st[2 * i + 1];
            if (++w >= cap) w = 0;
        }
        wr.store(w, std::memory_order_release);
    }
};

struct Activation : IActivateAudioInterfaceCompletionHandler {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IAudioClient* client = nullptr;
    HRESULT hr = E_FAIL;
    LONG refs = 1;
    ~Activation() { if (client) client->Release(); if (done) CloseHandle(done); }
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler) || iid == __uuidof(IAgileObject)) {
            *out = this; AddRef(); return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT res = E_FAIL;
        IUnknown* u = nullptr;
        hr = op->GetActivateResult(&res, &u);
        if (SUCCEEDED(hr)) hr = res;
        if (SUCCEEDED(hr) && u) u->QueryInterface(__uuidof(IAudioClient), (void**)&client);
        if (u) u->Release();
        SetEvent(done);
        return S_OK;
    }
};

struct ProgramTap {
    enum State { OFF, OPENING, CAPTURING, TOO_OLD, REFUSED, GONE };
    static constexpr DWORD FIRST_BUILD = 19041;

    std::thread th, keeper;
    std::atomic<bool> quit{false};
    std::atomic<int> state{OFF};
    std::atomic<long> refusal{0};
    std::atomic<ULONGLONG> lastSound{0};
    std::atomic<bool> parking{false};
    DWORD pid = 0;
    std::wstring name;
    StereoRing ring;
    std::vector<float> tmp;
    std::vector<float> silence;
    AppTakeover hijack;
    std::wstring stealTo;

    ~ProgramTap() { stop(); }

    void start(const Program& p, int fs, const std::wstring& stealId = {}) {
        stop();
        pid = p.pid; name = p.name; stealTo = stealId;
        parking = !stealId.empty();
        quit = false; refusal = 0; lastSound = 0;
        ring.init((UINT32)fs, (UINT32)(fs * 40 / 1000), (UINT32)(fs * 10 / 1000));
        state = OPENING;
        th = std::thread([this, fs] { run(fs); });
        keeper = std::thread([this] { keep(); });
    }
    void stop() {
        quit = true;
        if (th.joinable()) th.join();
        if (keeper.joinable()) keeper.join();
        hijack.restore();
        parking = false;
        state = OFF;
    }
    bool active() const { return state.load() == CAPTURING; }
    bool chosen() const { const int s = state.load(); return s == OPENING || s == CAPTURING; }
    bool finished() const { const int s = state.load(); return s == TOO_OLD || s == REFUSED || s == GONE; }
    bool heard() const { const ULONGLONG at = lastSound.load(); return at != 0 && GetTickCount64() - at < 1500; }
    void resync() { ring.stale = true; }

    void pull(float* L, float* R, int n) {
        tmp.resize((size_t)n * 2);
        ring.pull(tmp.data(), (UINT32)n);
        for (int i = 0; i < n; ++i) {
            const float l = tmp[2 * i], r = tmp[2 * i + 1];
            L[i] = (l > -4.0f && l < 4.0f) ? l : 0.0f;
            R[i] = (r > -4.0f && r < 4.0f) ? r : 0.0f;
        }
    }

private:
    struct Stream {
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        ~Stream() {
            if (capture) capture->Release();
            if (client) { client->Stop(); client->Release(); }
        }
    };

    HRESULT open(Stream& s, int fs, HANDLE wake) const {
        AUDIOCLIENT_ACTIVATION_PARAMS ap{};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = pid;
        ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(ap);
        pv.blob.pBlobData = (BYTE*)&ap;
        const ActivateAudioFn activate = activateAudio();
        if (!activate) return E_NOTIMPL;
        Activation* act = new Activation();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        HRESULT hr = activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, act, &op);
        if (FAILED(hr)) { act->Release(); return hr; }
        const bool answered = WaitForSingleObject(act->done, 3000) == WAIT_OBJECT_0;
        if (op) op->Release();
        if (!answered) {
            act->Release();
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        hr = act->hr;
        s.client = act->client;
        act->client = nullptr;
        act->Release();
        if (FAILED(hr)) return hr;
        if (!s.client) return E_NOINTERFACE;
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        f.nChannels = 2;
        f.nSamplesPerSec = (DWORD)fs;
        f.wBitsPerSample = 32;
        f.nBlockAlign = 8;
        f.nAvgBytesPerSec = (DWORD)fs * 8;
        hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 400000, 0, &f, nullptr);
        if (SUCCEEDED(hr)) hr = s.client->SetEventHandle(wake);
        if (SUCCEEDED(hr)) hr = s.client->GetService(__uuidof(IAudioCaptureClient), (void**)&s.capture);
        if (SUCCEEDED(hr)) hr = s.client->Start();
        return hr;
    }

    bool drain(IAudioCaptureClient* capture) {
        UINT32 pk = 0;
        for (;;) {
            if (FAILED(capture->GetNextPacketSize(&pk))) return false;
            if (pk == 0) return true;
            BYTE* data = nullptr;
            UINT32 n = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &n, &flags, nullptr, nullptr))) return false;
            const float* s = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : (const float*)data;
            float peak = 0.0f;
            if (s) {
                for (UINT32 i = 0; i < n * 2; ++i) peak = std::max(peak, fabsf(s[i]));
            }
            if (!s || n == 0 || peak < 1e-7f) {
                if (silence.size() < (size_t)n * 2) silence.assign((size_t)n * 2, 0.f);
                ring.push(silence.data(), n);
            } else {
                if (peak > 1e-4f) lastSound = GetTickCount64();
                ring.push(s, n);
            }
            capture->ReleaseBuffer(n);
        }
    }

    void run(int fs) {
        if (const DWORD build = windowsBuild(); build != 0 && build < FIRST_BUILD) { state = TOO_OLD; return; }
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task = 0;
        const HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        const HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        const HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ULONGLONG lookedAt = GetTickCount64();
        auto alive = [&] {
            if (proc) return WaitForSingleObject(proc, 0) != WAIT_OBJECT_0;
            if (GetTickCount64() - lookedAt < 2000) return true;
            lookedAt = GetTickCount64();
            return ProcessTable().find(pid) != nullptr;
        };
        int end = GONE;
        if (!wake) { refusal = (long)HRESULT_FROM_WIN32(GetLastError()); end = REFUSED; }
        bool everOpened = false;
        while (wake && !quit.load() && alive()) {
            Stream s;
            const HRESULT hr = open(s, fs, wake);
            if (FAILED(hr)) {
                if (!everOpened) { refusal = (long)hr; end = REFUSED; break; }
                for (int i = 0; i < 10 && !quit.load(); ++i) Sleep(50);
                continue;
            }
            everOpened = true;
            ring.restart();
            state = CAPTURING;
            while (!quit.load() && alive()) {
                WaitForSingleObject(wake, 100);
                if (!drain(s.capture)) break;
            }
            state = OPENING;
        }
        if (!quit.load()) state = end;
        if (wake) CloseHandle(wake);
        if (proc) CloseHandle(proc);
        if (mm) AvRevertMmThreadCharacteristics(mm);
        if (SUCCEEDED(com)) CoUninitialize();
    }

    void keep() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ULONGLONG due = 0;
        while (!quit.load() && !finished()) {
            if (state.load() != CAPTURING) due = 0;
            else if (GetTickCount64() >= due) {
                hijack.apply(pid, stealTo);
                due = GetTickCount64() + 2000;
            }
            Sleep(50);
        }
        hijack.restore();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

}
