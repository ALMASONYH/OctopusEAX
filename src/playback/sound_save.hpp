#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include "sound_board.hpp"
#include "sound_edit.hpp"

namespace clip {

struct SaveFormat {
    const wchar_t* filter;
    const wchar_t* ext;
    GUID subtype;
    UINT32 bytesPerSec;
};

static const SaveFormat SAVE_FORMATS[] = {
    {L"MP3 sound (*.mp3)", L"mp3", MFAudioFormat_MP3, 24000},
    {L"WAV sound (*.wav)", L"wav", GUID_NULL, 0},
    {L"M4A sound, AAC (*.m4a)", L"m4a", MFAudioFormat_AAC, 24000},
    {L"WMA sound (*.wma)", L"wma", MFAudioFormat_WMAudioV8, 24000},
    {L"FLAC sound (*.flac)", L"flac", MFAudioFormat_FLAC, 0},
};
static const int NUM_SAVE_FORMATS = (int)(sizeof SAVE_FORMATS / sizeof SAVE_FORMATS[0]);

inline int saveFormatOf(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return -1;
    const std::wstring ext = path.substr(dot + 1);
    for (int i = 0; i < NUM_SAVE_FORMATS; ++i)
        if (_wcsicmp(ext.c_str(), SAVE_FORMATS[i].ext) == 0) return i;
    return -1;
}

inline IMFMediaType* encodedType(const GUID& subtype, UINT32 rate, UINT32 bytesPerSec) {
    IMFCollection* all = nullptr;
    if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(subtype, MFT_ENUM_FLAG_ALL, nullptr, &all)) || !all) return nullptr;
    DWORD n = 0;
    all->GetElementCount(&n);
    IMFMediaType* best = nullptr;
    long long bestScore = LLONG_MAX;
    for (DWORD i = 0; i < n; ++i) {
        IUnknown* u = nullptr;
        IMFMediaType* t = nullptr;
        if (FAILED(all->GetElement(i, &u)) || !u) continue;
        u->QueryInterface(IID_PPV_ARGS(&t));
        u->Release();
        if (!t) continue;
        UINT32 sr = 0, ch = 0, bps = 0, bits = 16;
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
        t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        const long long score = (sr == rate && ch == 2)
                                    ? (bits == 16 ? 0 : 1000000000LL) + (bytesPerSec ? std::llabs((long long)bps - (long long)bytesPerSec) : 0)
                                    : LLONG_MAX - 1;
        if (score < bestScore) {
            if (best) best->Release();
            best = t;
            bestScore = score;
        } else {
            t->Release();
        }
    }
    all->Release();
    if (best && bestScore == LLONG_MAX - 1) {
        best->Release();
        best = nullptr;
    }
    return best;
}

inline bool encodeSound(const std::wstring& path, const Sound& s, UINT32 fs, const SaveFormat& f, std::wstring& why) {
    if (f.subtype == GUID_NULL) {
        if (writeWav(path, s, (int)fs)) return true;
        why = L"The file could not be written there";
        return false;
    }
    if (!mediaReady()) {
        why = NO_MEDIA;
        return false;
    }
    IMFMediaType* out = encodedType(f.subtype, fs, f.bytesPerSec);
    if (!out) {
        why = L"Windows has no encoder for that format, pick WAV";
        return false;
    }
    const size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L"\\/");
    const bool named = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
    const std::wstring temp = named ? path.substr(0, dot) + L".saving" + path.substr(dot) : path + L".saving";
    IMFMediaType* in = nullptr;
    IMFSinkWriter* w = nullptr;
    DWORD stream = 0;
    HRESULT hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) {
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, fs);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, fs * 4);
        in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        hr = MFCreateSinkWriterFromURL(temp.c_str(), nullptr, nullptr, &w);
    }
    if (SUCCEEDED(hr)) hr = w->AddStream(out, &stream);
    if (SUCCEEDED(hr)) hr = w->SetInputMediaType(stream, in, nullptr);
    if (SUCCEEDED(hr)) hr = w->BeginWriting();
    const size_t frames = s.frames(), chunk = fs / 10;
    for (size_t at = 0; SUCCEEDED(hr) && at < frames; at += chunk) {
        const size_t n = std::min(chunk, frames - at);
        const DWORD bytes = (DWORD)(n * 4);
        IMFMediaBuffer* b = nullptr;
        IMFSample* smp = nullptr;
        hr = MFCreateMemoryBuffer(bytes, &b);
        BYTE* d = nullptr;
        if (SUCCEEDED(hr)) hr = b->Lock(&d, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(d, s.pcm.data() + at * 2, bytes);
            b->Unlock();
            hr = b->SetCurrentLength(bytes);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&smp);
        if (SUCCEEDED(hr)) hr = smp->AddBuffer(b);
        if (SUCCEEDED(hr)) hr = smp->SetSampleTime((LONGLONG)at * 10000000 / fs);
        if (SUCCEEDED(hr)) hr = smp->SetSampleDuration((LONGLONG)(at + n) * 10000000 / fs - (LONGLONG)at * 10000000 / fs);
        if (SUCCEEDED(hr)) hr = w->WriteSample(stream, smp);
        if (smp) smp->Release();
        if (b) b->Release();
    }
    if (SUCCEEDED(hr)) hr = w->Finalize();
    if (w) w->Release();
    if (in) in->Release();
    out->Release();
    if (SUCCEEDED(hr) && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) {
        DeleteFileW(temp.c_str());
        wchar_t t[96];
        swprintf_s(t, 96, L"Windows could not write that format (error %08lX)", (unsigned long)hr);
        why = t;
        return false;
    }
    return true;
}

}
