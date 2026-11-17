#include "ClipboardVoiceEngine.h"
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>

// ---------- Background clipboard writer ----------
//
// Speak() queues the text and returns to its pacing loop right away; a worker
// thread puts it on the clipboard, retrying while another program holds it open.
// Cancelling (abort / purge) bumps a generation counter: anything queued before
// the bump is dropped, anything queued after it is still written.
namespace {
    struct ClipItem
    {
        std::wstring text;
        unsigned generation;
    };

    std::mutex g_qMutex;
    std::condition_variable g_qCv;
    std::deque<ClipItem> g_queue;
    std::atomic<unsigned> g_generation{ 0 };
    std::once_flag g_workerOnce;

    void WorkerProc()
    {
        for (;;)
        {
            ClipItem item;
            {
                std::unique_lock<std::mutex> lock(g_qMutex);
                g_qCv.wait(lock, [] { return !g_queue.empty(); });
                item = std::move(g_queue.front());
                g_queue.pop_front();
            }
            while (item.generation == g_generation.load())
            {
                if (CClipboardVoiceEngine::CopyTextToClipboard(item.text))
                    break;
                ::Sleep(50);
            }
        }
    }

    void StartWorkerIfNeeded()
    {
        std::call_once(g_workerOnce, [] {
            // The worker runs code from this DLL for the rest of the process, so pin
            // the DLL in memory: COM may otherwise unload it (CoFreeUnusedLibraries)
            // while the thread is still running, and joining a thread from DllMain
            // would deadlock on the loader lock. The OS ends the thread at exit.
            HMODULE self = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                 reinterpret_cast<LPCWSTR>(&WorkerProc), &self);
            std::thread(WorkerProc).detach();
        });
    }

    void EnqueueClipboardText(const std::wstring& text)
    {
        StartWorkerIfNeeded();
        {
            std::lock_guard<std::mutex> lock(g_qMutex);
            g_queue.push_back({ text, g_generation.load() });
        }
        g_qCv.notify_one();
    }

    void CancelPendingClipboardWrites()
    {
        std::lock_guard<std::mutex> lock(g_qMutex);
        g_generation.fetch_add(1);
        g_queue.clear();
    }

    bool Aborted(ISpTTSEngineSite* pSite)
    {
        return (pSite->GetActions() & SPVES_ABORT) != 0;
    }
}

// ---------- Engine ----------

// Pacing: every word and every whitespace character takes this long, and every
// utterance lasts at least kMinUtteranceMs. Callers that wait for speech to end
// (some games) need the voice to take a little time.
static const double kMsPerStep = 20.0;
static const double kMinUtteranceMs = 1000.0;

CClipboardVoiceEngine::CClipboardVoiceEngine()
{
    m_wfx.wFormatTag = WAVE_FORMAT_PCM;
    m_wfx.nSamplesPerSec = 16000;
    m_wfx.wBitsPerSample = 16;
    m_wfx.nChannels = 1;
    m_wfx.nBlockAlign = (m_wfx.nChannels * m_wfx.wBitsPerSample) / 8;
    m_wfx.nAvgBytesPerSec = m_wfx.nSamplesPerSec * m_wfx.nBlockAlign;
    m_wfx.cbSize = 0;
}

STDMETHODIMP CClipboardVoiceEngine::SetObjectToken(ISpObjectToken* pToken)
{
    m_cpToken = pToken;
    return S_OK;
}

STDMETHODIMP CClipboardVoiceEngine::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) return E_POINTER;
    *ppToken = m_cpToken;
    if (*ppToken) (*ppToken)->AddRef();
    return S_OK;
}

STDMETHODIMP CClipboardVoiceEngine::GetOutputFormat(const GUID* /*pTargetFormatId*/, const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
                                                    GUID* pDesiredFormatId, WAVEFORMATEX** ppCoMemDesiredWaveFormatEx)
{
    if (!pDesiredFormatId || !ppCoMemDesiredWaveFormatEx) return E_POINTER;
    *pDesiredFormatId = SPDFID_WaveFormatEx;
    *ppCoMemDesiredWaveFormatEx = CreatePcmWaveFormat(16000);
    return *ppCoMemDesiredWaveFormatEx ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CClipboardVoiceEngine::Speak(DWORD dwFlags, REFGUID /*rguidFormatId*/, const WAVEFORMATEX* pWaveFormatEx,
                                          const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite)
{
    if (!pOutputSite || !pTextFragList) return E_INVALIDARG;

    // Note: SAPI raises START_INPUT_STREAM / END_INPUT_STREAM itself; an engine
    // that adds its own makes every utterance report them twice.

    if (Aborted(pOutputSite))
    {
        CancelPendingClipboardWrites();
        return S_OK;
    }

    if (dwFlags & SPF_PURGEBEFORESPEAK)
        CancelPendingClipboardWrites();

    const WAVEFORMATEX* fmt = (pWaveFormatEx && pWaveFormatEx->wFormatTag == WAVE_FORMAT_PCM &&
                               pWaveFormatEx->nAvgBytesPerSec && pWaveFormatEx->nBlockAlign)
                                  ? pWaveFormatEx : &m_wfx;

    const SpeechInput input = BuildInput(pTextFragList);

    // Empty or whitespace-only requests (e.g. a "stop talking" call) leave the
    // clipboard alone instead of wiping it.
    if (!IsBlank(input.text))
        EnqueueClipboardText(input.text);

    ULONGLONG audioBytes = 0;
    if (!EmitProgress(input, pOutputSite, fmt, audioBytes))
    {
        CancelPendingClipboardWrites();
        return S_OK;
    }

    const ULONGLONG minBytes = (ULONGLONG)(fmt->nAvgBytesPerSec * (kMinUtteranceMs / 1000.0));
    if (audioBytes < minBytes)
    {
        if (!WriteSilence(pOutputSite, fmt, (size_t)(minBytes - audioBytes), audioBytes))
            CancelPendingClipboardWrites();
    }
    return S_OK;
}

// static
WAVEFORMATEX* CClipboardVoiceEngine::CreatePcmWaveFormat(DWORD samplesPerSec)
{
    WAVEFORMATEX* pwfx = (WAVEFORMATEX*)::CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!pwfx) return nullptr;
    ZeroMemory(pwfx, sizeof(WAVEFORMATEX));
    pwfx->wFormatTag = WAVE_FORMAT_PCM;
    pwfx->nChannels = 1;
    pwfx->nSamplesPerSec = samplesPerSec;
    pwfx->wBitsPerSample = 16;
    pwfx->nBlockAlign = (pwfx->nChannels * pwfx->wBitsPerSample) / 8;
    pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
    pwfx->cbSize = 0;
    return pwfx;
}

// static
bool CClipboardVoiceEngine::CopyTextToClipboard(const std::wstring& text)
{
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    const int kMaxAttempts = 4;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        if (::OpenClipboard(nullptr))
        {
            bool ok = false;
            if (::EmptyClipboard())
            {
                HGLOBAL hglob = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (hglob)
                {
                    if (void* mem = ::GlobalLock(hglob))
                    {
                        memcpy(mem, text.c_str(), bytes);
                        ::GlobalUnlock(hglob);
                        // On success the clipboard owns the memory.
                        ok = ::SetClipboardData(CF_UNICODETEXT, hglob) != nullptr;
                    }
                    if (!ok) ::GlobalFree(hglob);
                }
            }
            ::CloseClipboard();
            if (ok) return true;
        }
        ::Sleep(30);
    }
    return false;
}

// static
CClipboardVoiceEngine::SpeechInput CClipboardVoiceEngine::BuildInput(const SPVTEXTFRAG* pList)
{
    SpeechInput in;
    for (auto pFrag = pList; pFrag != nullptr; pFrag = pFrag->pNext)
    {
        switch (pFrag->State.eAction)
        {
        case SPVA_Speak:
        case SPVA_SpellOut:
        case SPVA_Pronounce:
            if (pFrag->pTextStart && pFrag->ulTextLen)
            {
                in.text.append(pFrag->pTextStart, pFrag->ulTextLen);
                for (ULONG i = 0; i < pFrag->ulTextLen; ++i)
                    in.srcOffset.push_back(pFrag->ulTextSrcOffset + i);
            }
            break;
        case SPVA_Bookmark:
            in.bookmarks.push_back({ in.text.size(),
                                     pFrag->pTextStart ? std::wstring(pFrag->pTextStart, pFrag->ulTextLen)
                                                       : std::wstring() });
            break;
        default:
            break;
        }
    }
    return in;
}

// static
bool CClipboardVoiceEngine::IsWhitespace(wchar_t ch)
{
    return ch == 0x00A0 || std::iswspace(ch);
}

// static
bool CClipboardVoiceEngine::IsBlank(const std::wstring& text)
{
    for (wchar_t ch : text)
        if (!IsWhitespace(ch)) return false;
    return true;
}

// static
void CClipboardVoiceEngine::AddEvent(ISpTTSEngineSite* pSite, SPEVENTENUM evtId, ULONGLONG audioOffset,
                                     WPARAM wParam, LPARAM lParam, SPEVENTLPARAMTYPE lParamType)
{
    SPEVENT evt = {};
    evt.eEventId = evtId;
    evt.elParamType = lParamType;
    evt.ullAudioStreamOffset = audioOffset;
    evt.wParam = wParam;
    evt.lParam = lParam;
    pSite->AddEvents(&evt, 1);
}

// static
bool CClipboardVoiceEngine::WriteSilence(ISpTTSEngineSite* pSite, const WAVEFORMATEX* pFmt,
                                         size_t bytes, ULONGLONG& audioBytes)
{
    const size_t align = pFmt->nBlockAlign ? pFmt->nBlockAlign : 1;
    bytes = (bytes / align) * align;
    // Write in ~10 ms chunks so an abort is noticed quickly.
    size_t chunk = ((size_t)(pFmt->nAvgBytesPerSec / 100) / align) * align;
    if (chunk == 0) chunk = align;
    static const BYTE kZeros[4096] = {};
    if (chunk > sizeof(kZeros)) chunk = (sizeof(kZeros) / align) * align;

    size_t sent = 0;
    while (sent < bytes)
    {
        if (Aborted(pSite)) return false;
        const ULONG toWrite = (ULONG)((bytes - sent < chunk) ? (bytes - sent) : chunk);
        ULONG wrote = 0;
        if (FAILED(pSite->Write(kZeros, toWrite, &wrote)) || wrote == 0)
            break; // output gone; nothing more to do
        sent += wrote;
        audioBytes += wrote;
    }
    return !Aborted(pSite);
}

// static
bool CClipboardVoiceEngine::EmitProgress(const SpeechInput& in, ISpTTSEngineSite* pSite,
                                         const WAVEFORMATEX* pFmt, ULONGLONG& audioBytes)
{
    const std::wstring& text = in.text;
    const size_t stepBytes = (size_t)(pFmt->nAvgBytesPerSec * (kMsPerStep / 1000.0));
    size_t nextBookmark = 0;

    auto emitBookmarksUpTo = [&](size_t pos) {
        for (; nextBookmark < in.bookmarks.size() && in.bookmarks[nextBookmark].at <= pos; ++nextBookmark)
        {
            const std::wstring& name = in.bookmarks[nextBookmark].name;
            // SAPI copies the string; wParam carries the numeric value as SAPI's own engines do.
            AddEvent(pSite, SPEI_TTS_BOOKMARK, audioBytes, (WPARAM)_wtol(name.c_str()),
                     (LPARAM)name.c_str(), SPET_LPARAM_IS_STRING);
        }
    };
    // Length in the caller's string of text[from, to): ends of a range can sit in
    // different fragments, so measure with the source offsets.
    auto srcLen = [&](size_t from, size_t to) -> WPARAM {
        return (WPARAM)(in.srcOffset[to - 1] + 1 - in.srcOffset[from]);
    };
    auto endsSentence = [&](size_t wordEnd, size_t wordStart) {
        size_t k = wordEnd;
        while (k > wordStart && wcschr(L"\"')]}\x201D\x2019\x00BB", text[k - 1])) --k;
        return k > wordStart && wcschr(L".!?\x2026", text[k - 1]) != nullptr;
    };

    bool atSentenceStart = true;
    size_t i = 0;
    while (i < text.size())
    {
        if (Aborted(pSite)) return false;
        emitBookmarksUpTo(i);

        if (IsWhitespace(text[i]))
        {
            if (!WriteSilence(pSite, pFmt, stepBytes, audioBytes)) return false;
            ++i;
            continue;
        }

        const size_t start = i;
        while (i < text.size() && !IsWhitespace(text[i])) ++i;

        if (atSentenceStart)
        {
            size_t end = i, j = i;
            while (!endsSentence(end, start) && j < text.size())
            {
                while (j < text.size() && IsWhitespace(text[j])) ++j;
                const size_t ws = j;
                while (j < text.size() && !IsWhitespace(text[j])) ++j;
                if (j > ws) end = j;
                if (j > ws && endsSentence(j, ws)) break;
            }
            // SPEI_SENTENCE_BOUNDARY / SPEI_WORD_BOUNDARY: lParam = position, wParam = length.
            AddEvent(pSite, SPEI_SENTENCE_BOUNDARY, audioBytes, srcLen(start, end), (LPARAM)in.srcOffset[start]);
            atSentenceStart = false;
        }
        AddEvent(pSite, SPEI_WORD_BOUNDARY, audioBytes, srcLen(start, i), (LPARAM)in.srcOffset[start]);
        if (endsSentence(i, start)) atSentenceStart = true;

        if (!WriteSilence(pSite, pFmt, stepBytes, audioBytes)) return false;
    }
    emitBookmarksUpTo(SIZE_MAX);
    return true;
}

// ---------- SAPI voice token registration ----------
//
// One token per language. English (en-US) keeps the original key and name
// ("ClipboardVoice" / "Clipboard Voice") so existing setups keep working; other
// languages get "ClipboardVoice_<locale>" / "Clipboard Voice (<Language>)".

static const wchar_t* kTokensKey = L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens";
static const wchar_t* kTokenPrefix = L"ClipboardVoice";
static const wchar_t* kVendor = L"Clipboard Voice";
static const wchar_t* kBaseName = L"Clipboard Voice";
static const wchar_t* kClsidStr = L"{F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}";
static const REGSAM kViews[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

struct VoiceLanguage
{
    std::wstring keyName;   // token key under ...\Voices\Tokens
    std::wstring name;      // voice name shown to users
    std::wstring langAttr;  // SAPI "Language" attribute, e.g. "C0A;A"
};

static HRESULT ResolveVoiceLanguage(const std::wstring& requested, VoiceLanguage& out)
{
    // "es" -> "es-ES", "en" -> "en-US"; specific names pass through.
    wchar_t specific[LOCALE_NAME_MAX_LENGTH] = {};
    if (!::ResolveLocaleName(requested.c_str(), specific, LOCALE_NAME_MAX_LENGTH) || !specific[0])
        return E_INVALIDARG;
    const LCID lcid = ::LocaleNameToLCID(specific, 0);
    if (lcid == 0 || lcid == LOCALE_CUSTOM_UNSPECIFIED || lcid == LOCALE_CUSTOM_DEFAULT)
        return E_INVALIDARG;
    const LANGID langId = LANGIDFROMLCID(lcid);
    const LANGID neutral = MAKELANGID(PRIMARYLANGID(langId), SUBLANG_NEUTRAL);

    wchar_t buf[128];
    swprintf_s(buf, L"%X;%X", langId, neutral);
    out.langAttr = buf;

    if (_wcsicmp(specific, L"en-US") == 0)
    {
        out.keyName = kTokenPrefix;
        out.name = kBaseName;
        return S_OK;
    }

    // A neutral request ("es") is shown as the language ("Spanish"); a specific
    // one ("es-MX") as language and region ("Spanish (Mexico)").
    const bool isNeutral = requested.find(L'-') == std::wstring::npos && requested.find(L'_') == std::wstring::npos;
    wchar_t display[128] = {};
    if (!::GetLocaleInfoEx(specific, isNeutral ? LOCALE_SENGLISHLANGUAGENAME : LOCALE_SENGLISHDISPLAYNAME,
                           display, ARRAYSIZE(display)))
        wcscpy_s(display, specific);

    std::wstring id = isNeutral ? requested : std::wstring(specific);
    for (auto& ch : id) ch = (wchar_t)towlower(ch);
    out.keyName = std::wstring(kTokenPrefix) + L"_" + id;
    out.name = std::wstring(kBaseName) + L" (" + display + L")";
    return S_OK;
}

static LONG SetString(HKEY hKey, const wchar_t* name, const std::wstring& value)
{
    return ::RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

static HRESULT WriteToken(REGSAM view, const VoiceLanguage& lang)
{
    const std::wstring path = std::wstring(kTokensKey) + L"\\" + lang.keyName;
    HKEY hKey = nullptr;
    LONG r = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                               KEY_WRITE | view, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);

    if (r == ERROR_SUCCESS) r = SetString(hKey, nullptr, lang.name);
    if (r == ERROR_SUCCESS) r = SetString(hKey, L"CLSID", kClsidStr);
    if (r == ERROR_SUCCESS) r = SetString(hKey, L"Version", L"1.0");

    HKEY hAttr = nullptr;
    if (r == ERROR_SUCCESS)
        r = ::RegCreateKeyExW(hKey, L"Attributes", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | view,
                              nullptr, &hAttr, nullptr);
    if (r == ERROR_SUCCESS)
    {
        r = SetString(hAttr, L"Language", lang.langAttr);
        if (r == ERROR_SUCCESS) r = SetString(hAttr, L"Name", lang.name);
        if (r == ERROR_SUCCESS) r = SetString(hAttr, L"Vendor", kVendor);
        if (r == ERROR_SUCCESS) r = SetString(hAttr, L"Gender", L"Neutral");
        if (r == ERROR_SUCCESS) r = SetString(hAttr, L"Age", L"Adult");
        ::RegCloseKey(hAttr);
    }

    HKEY hAF = nullptr;
    if (r == ERROR_SUCCESS &&
        ::RegCreateKeyExW(hKey, L"AudioFormats", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | view,
                          nullptr, &hAF, nullptr) == ERROR_SUCCESS)
    {
        SetString(hAF, L"1", L"SAFT16kHz16BitMono");
        ::RegCloseKey(hAF);
    }

    ::RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(r);
}

static bool IsOurTokenKey(const wchar_t* name)
{
    const size_t n = wcslen(kTokenPrefix);
    return _wcsnicmp(name, kTokenPrefix, n) == 0 && (name[n] == L'\0' || name[n] == L'_');
}

// Names of our tokens present in one registry view.
static std::vector<std::wstring> ListOurTokens(REGSAM view)
{
    std::vector<std::wstring> found;
    HKEY hTokens = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kTokensKey, 0, KEY_ENUMERATE_SUB_KEYS | view, &hTokens) != ERROR_SUCCESS)
        return found;
    wchar_t name[256];
    for (DWORD i = 0;; ++i)
    {
        DWORD len = ARRAYSIZE(name);
        const LONG r = ::RegEnumKeyExW(hTokens, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_SUCCESS && IsOurTokenKey(name)) found.emplace_back(name);
    }
    ::RegCloseKey(hTokens);
    return found;
}

HRESULT UnregisterSapiTokens()
{
    HRESULT result = S_OK;
    for (REGSAM view : kViews)
    {
        const auto names = ListOurTokens(view);
        if (names.empty()) continue;
        HKEY hTokens = nullptr;
        const LONG r = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kTokensKey, 0, KEY_ALL_ACCESS | view, &hTokens);
        if (r != ERROR_SUCCESS) { result = HRESULT_FROM_WIN32(r); continue; }
        for (const auto& name : names)
        {
            const LONG d = ::RegDeleteTreeW(hTokens, name.c_str());
            if (d != ERROR_SUCCESS && d != ERROR_FILE_NOT_FOUND) result = HRESULT_FROM_WIN32(d);
        }
        ::RegCloseKey(hTokens);
    }
    return result;
}

static std::vector<std::wstring> SplitLanguages(const wchar_t* list)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&] {
        if (!cur.empty())
        {
            for (auto& ch : cur) if (ch == L'_') ch = L'-';
            bool dup = false;
            for (const auto& s : out) dup = dup || _wcsicmp(s.c_str(), cur.c_str()) == 0;
            if (!dup) out.push_back(cur);
            cur.clear();
        }
    };
    for (const wchar_t* p = list ? list : L""; *p; ++p)
    {
        if (*p == L',' || *p == L';' || *p == L'"' || std::iswspace(*p)) flush();
        else cur.push_back(*p);
    }
    flush();
    return out;
}

HRESULT RegisterSapiTokens(const wchar_t* languages)
{
    auto requested = SplitLanguages(languages);
    if (requested.empty()) requested.push_back(L"en");

    // Validate everything before touching the registry.
    std::vector<VoiceLanguage> voices;
    for (const auto& code : requested)
    {
        VoiceLanguage v;
        const HRESULT hr = ResolveVoiceLanguage(code, v);
        if (FAILED(hr)) return hr;
        bool dup = false;
        for (const auto& existing : voices) dup = dup || _wcsicmp(existing.keyName.c_str(), v.keyName.c_str()) == 0;
        if (!dup) voices.push_back(v);
    }

    // Replace the previous set so removed languages disappear.
    UnregisterSapiTokens();

    HRESULT result = E_FAIL;
    for (REGSAM view : kViews)
    {
        HRESULT viewHr = S_OK;
        for (const auto& v : voices)
        {
            const HRESULT hr = WriteToken(view, v);
            if (FAILED(hr)) viewHr = hr;
        }
        // Succeed if at least one registry view got every voice.
        if (SUCCEEDED(viewHr)) result = S_OK;
        else if (FAILED(result)) result = viewHr;
    }
    return result;
}

bool AnySapiTokenRegistered()
{
    for (REGSAM view : kViews)
        if (!ListOurTokens(view).empty()) return true;
    return false;
}
