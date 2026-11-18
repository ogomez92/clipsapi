// eventdump: speaks text with a SAPI voice and prints every event SAPI delivers.
// Used to check the events Clipboard Voice raises (word/sentence boundaries,
// bookmarks, end of stream) and that the text lands on the clipboard.
//
// Build (from a VS Developer Command Prompt, x64 or x86):
//   cl /nologo /EHsc /W4 eventdump.cpp ole32.lib user32.lib
// Usage:
//   eventdump "Clipboard Voice (English)" "Hello world. Second sentence."
//   eventdump "Clipboard Voice (Spanish)" "Hola <bookmark mark=\"7\"/>mundo"
// Text starting with '<' or containing "<bookmark" is spoken as SAPI XML.

#include <windows.h>
#include <sapi.h>
#include <sperror.h>
#include <cstdio>
#include <cwchar>
#include <string>

static const wchar_t* EventName(SPEVENTENUM id)
{
    switch (id)
    {
    case SPEI_START_INPUT_STREAM: return L"START_INPUT_STREAM";
    case SPEI_END_INPUT_STREAM: return L"END_INPUT_STREAM";
    case SPEI_VOICE_CHANGE: return L"VOICE_CHANGE";
    case SPEI_TTS_BOOKMARK: return L"TTS_BOOKMARK";
    case SPEI_WORD_BOUNDARY: return L"WORD_BOUNDARY";
    case SPEI_PHONEME: return L"PHONEME";
    case SPEI_SENTENCE_BOUNDARY: return L"SENTENCE_BOUNDARY";
    case SPEI_VISEME: return L"VISEME";
    case SPEI_TTS_AUDIO_LEVEL: return L"TTS_AUDIO_LEVEL";
    default: return L"OTHER";
    }
}

static std::wstring ReadClipboard()
{
    std::wstring out;
    for (int i = 0; i < 20 && !OpenClipboard(nullptr); ++i) Sleep(25);
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h)
    {
        if (auto p = static_cast<const wchar_t*>(GlobalLock(h)))
        {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        fwprintf(stderr, L"usage: eventdump <voice name> <text>\n");
        return 2;
    }
    const std::wstring name = argv[1];
    const std::wstring text = argv[2];

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    int rc = 1;
    {
        ISpObjectTokenCategory* cat = nullptr;
        IEnumSpObjectTokens* tokens = nullptr;
        ISpObjectToken* token = nullptr;
        ISpVoice* voice = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&cat));
        if (SUCCEEDED(hr)) hr = cat->SetId(SPCAT_VOICES, FALSE);
        const std::wstring req = L"Name=" + name;
        if (SUCCEEDED(hr)) hr = cat->EnumTokens(req.c_str(), nullptr, &tokens);
        ULONG fetched = 0;
        if (SUCCEEDED(hr)) hr = tokens->Next(1, &token, &fetched);
        if (SUCCEEDED(hr) && fetched == 0) hr = SPERR_NOT_FOUND;
        if (FAILED(hr)) fwprintf(stderr, L"voice '%ls' not found (0x%08lX)\n", name.c_str(), hr);

        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice));
        if (SUCCEEDED(hr)) hr = voice->SetVoice(token);
        const ULONGLONG all = SPFEI_ALL_TTS_EVENTS;
        if (SUCCEEDED(hr)) hr = voice->SetInterest(all, all);
        if (SUCCEEDED(hr))
        {
            OpenClipboard(nullptr);
            EmptyClipboard();
            CloseClipboard();

            DWORD flags = SPF_DEFAULT;
            if (text.find(L'<') != std::wstring::npos) flags |= SPF_IS_XML;
            const DWORD t0 = GetTickCount();
            hr = voice->Speak(text.c_str(), flags, nullptr);
            const DWORD elapsed = GetTickCount() - t0;
            if (FAILED(hr)) fwprintf(stderr, L"Speak failed 0x%08lX\n", hr);

            SPEVENT ev;
            ULONG got = 0;
            while (voice->GetEvents(1, &ev, &got) == S_OK && got == 1)
            {
                wprintf(L"%-18ls offset=%-7llu wParam=%-5llu lParam=",
                        EventName(ev.eEventId), ev.ullAudioStreamOffset, (unsigned long long)ev.wParam);
                if (ev.elParamType == SPET_LPARAM_IS_STRING && ev.lParam)
                    wprintf(L"\"%ls\"", reinterpret_cast<const wchar_t*>(ev.lParam));
                else
                    wprintf(L"%lld", (long long)ev.lParam);
                if (ev.eEventId == SPEI_WORD_BOUNDARY || ev.eEventId == SPEI_SENTENCE_BOUNDARY)
                {
                    const size_t pos = (size_t)ev.lParam, len = (size_t)ev.wParam;
                    if (pos <= text.size())
                        wprintf(L"  -> \"%ls\"", text.substr(pos, len).c_str());
                }
                wprintf(L"\n");
                // Free string/token payloads the way SpClearEvent does.
                if (ev.elParamType == SPET_LPARAM_IS_STRING || ev.elParamType == SPET_LPARAM_IS_POINTER)
                    CoTaskMemFree(reinterpret_cast<void*>(ev.lParam));
                else if (ev.elParamType == SPET_LPARAM_IS_TOKEN || ev.elParamType == SPET_LPARAM_IS_OBJECT)
                    reinterpret_cast<IUnknown*>(ev.lParam)->Release();
            }
            Sleep(300); // let the engine's clipboard thread finish
            wprintf(L"speak took %lu ms\nclipboard: \"%ls\"\n", elapsed, ReadClipboard().c_str());
            rc = SUCCEEDED(hr) ? 0 : 1;
        }
        if (voice) voice->Release();
        if (token) token->Release();
        if (tokens) tokens->Release();
        if (cat) cat->Release();
    }
    CoUninitialize();
    return rc;
}
