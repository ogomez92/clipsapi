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

}

// static
bool CVoiceEngineBase::Aborted(ISpTTSEngineSite* pSite)
{
    return (pSite->GetActions() & SPVES_ABORT) != 0;
}

// ---------- Engine ----------

// Pacing: every word and every whitespace character takes this long, and every
// utterance lasts at least kMinUtteranceMs. Callers that wait for speech to end
// (some games) need the voice to take a little time.
static const double kMsPerStep = 20.0;
static const double kMinUtteranceMs = 1000.0;

// The time passes inside Speak (sleeping); no audio is written at all, and
// every event is raised at audio offset 0. Programs that render speech to a file
// and replay that file for the same text otherwise never call the voice again,
// and the text is lost: RT-Voice (Unity games) does that for files over 1 KB.
// With no audio, SAPI delivers each offset-0 event as soon as it is raised, so
// events still arrive in real time; with a few samples of audio SAPI held them
// back until the end of the utterance.
static const ULONGLONG kEventOffset = 0;

STDMETHODIMP CVoiceEngineBase::SetObjectToken(ISpObjectToken* pToken)
{
    m_cpToken = pToken;
    return S_OK;
}

STDMETHODIMP CVoiceEngineBase::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) return E_POINTER;
    *ppToken = m_cpToken;
    if (*ppToken) (*ppToken)->AddRef();
    return S_OK;
}

STDMETHODIMP CVoiceEngineBase::GetOutputFormat(const GUID* /*pTargetFormatId*/, const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
                                                    GUID* pDesiredFormatId, WAVEFORMATEX** ppCoMemDesiredWaveFormatEx)
{
    if (!pDesiredFormatId || !ppCoMemDesiredWaveFormatEx) return E_POINTER;
    *pDesiredFormatId = SPDFID_WaveFormatEx;
    *ppCoMemDesiredWaveFormatEx = CreatePcmWaveFormat(16000);
    return *ppCoMemDesiredWaveFormatEx ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CVoiceEngineBase::Speak(DWORD dwFlags, REFGUID /*rguidFormatId*/, const WAVEFORMATEX* /*pWaveFormatEx*/,
                                          const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite)
{
    if (!pOutputSite || !pTextFragList) return E_INVALIDARG;

    // Note: SAPI raises START_INPUT_STREAM / END_INPUT_STREAM itself; an engine
    // that adds its own makes every utterance report them twice.

    if (Aborted(pOutputSite))
    {
        OnAbort();
        OnDone(false);
        return S_OK;
    }

    const SpeechInput input = BuildInput(pTextFragList);
    OnText(input.text, (dwFlags & SPF_PURGEBEFORESPEAK) != 0);

    auto aborted = [&] {
        OnAbort();
        OnDone(false);
        return S_OK;
    };

    Pacer pacer(pOutputSite);
    if (!EmitProgress(input, pOutputSite, pacer)) return aborted();
    if (!WaitForOutput(input.text, pOutputSite)) return aborted();
    if (!pacer.WaitUntil(kMinUtteranceMs)) return aborted();
    OnDone(true);
    return S_OK;
}

// ---------- Pacer ----------

CVoiceEngineBase::Pacer::Pacer(ISpTTSEngineSite* pSite)
    : m_site(pSite)
    , m_start(::GetTickCount64())
{
}

bool CVoiceEngineBase::Pacer::WaitUntil(double ms)
{
    // Against the start time, so oversleeping (timer granularity) doesn't add up.
    for (;;)
    {
        if (Aborted(m_site)) return false;
        const double now = (double)(::GetTickCount64() - m_start);
        if (now >= ms) return true;
        const double left = ms - now;
        ::Sleep((DWORD)(left < 1.0 ? 1.0 : (left < 10.0 ? left : 10.0))); // notice an abort quickly
    }
}

bool CVoiceEngineBase::Pacer::Step(double ms)
{
    m_target += ms;
    return WaitUntil(m_target);
}

// ---------- Clipboard Voice ----------

void CClipboardVoiceEngine::OnText(const std::wstring& text, bool purge)
{
    if (purge)
        CancelPendingClipboardWrites();
    // Empty or whitespace-only requests (e.g. a "stop talking" call) leave the
    // clipboard alone instead of wiping it.
    if (!IsBlank(text))
        EnqueueClipboardText(text);
}

void CClipboardVoiceEngine::OnAbort()
{
    // Text still waiting for the clipboard is dropped.
    CancelPendingClipboardWrites();
}

// static
WAVEFORMATEX* CVoiceEngineBase::CreatePcmWaveFormat(DWORD samplesPerSec)
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
CVoiceEngineBase::SpeechInput CVoiceEngineBase::BuildInput(const SPVTEXTFRAG* pList)
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
bool CVoiceEngineBase::IsWhitespace(wchar_t ch)
{
    return ch == 0x00A0 || std::iswspace(ch);
}

// static
bool CVoiceEngineBase::IsBlank(const std::wstring& text)
{
    for (wchar_t ch : text)
        if (!IsWhitespace(ch)) return false;
    return true;
}

// static
void CVoiceEngineBase::AddEvent(ISpTTSEngineSite* pSite, SPEVENTENUM evtId, ULONGLONG audioOffset,
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
bool CVoiceEngineBase::EmitProgress(const SpeechInput& in, ISpTTSEngineSite* pSite, Pacer& pacer)
{
    const std::wstring& text = in.text;
    size_t nextBookmark = 0;

    auto emitBookmarksUpTo = [&](size_t pos) {
        for (; nextBookmark < in.bookmarks.size() && in.bookmarks[nextBookmark].at <= pos; ++nextBookmark)
        {
            const std::wstring& name = in.bookmarks[nextBookmark].name;
            // SAPI copies the string; wParam carries the numeric value as SAPI's own engines do.
            AddEvent(pSite, SPEI_TTS_BOOKMARK, kEventOffset, (WPARAM)_wtol(name.c_str()),
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
            if (!pacer.Step(kMsPerStep)) return false;
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
            AddEvent(pSite, SPEI_SENTENCE_BOUNDARY, kEventOffset, srcLen(start, end), (LPARAM)in.srcOffset[start]);
            atSentenceStart = false;
        }
        AddEvent(pSite, SPEI_WORD_BOUNDARY, kEventOffset, srcLen(start, i), (LPARAM)in.srcOffset[start]);
        if (endsSentence(i, start)) atSentenceStart = true;

        if (!pacer.Step(kMsPerStep)) return false;
    }
    emitBookmarksUpTo(SIZE_MAX);
    return true;
}
