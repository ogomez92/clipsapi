#include "ClipboardVoiceEngine.h"
#include <strsafe.h>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

using std::wstring;

namespace {
    std::mutex g_qMutex;
    std::condition_variable g_qCv;
    std::deque<std::wstring> g_queue;
    std::atomic<bool> g_workerStarted{ false };
    std::atomic<bool> g_stopWorker{ false };
    std::thread g_worker;
    std::atomic<bool> g_cancelWrite{ false };

    void WorkerProc()
    {
        for(;;)
        {
            std::wstring item;
            {
                std::unique_lock<std::mutex> lock(g_qMutex);
                g_qCv.wait(lock, [] { return !g_queue.empty() || g_stopWorker.load(); });
                if (g_stopWorker.load()) break;
                item = std::move(g_queue.front());
                g_queue.pop_front();
            }

            // Try to copy; retry until success, cancellation, or shutdown
            for (;;)
            {
                if (g_stopWorker.load())
                    break; // drop item and exit loop
                if (g_cancelWrite.exchange(false))
                    break; // drop current item due to abort/purge
                if (CClipboardVoiceEngine::CopyTextToClipboard(item))
                    break;
                CClipboardVoiceEngine::SleepMs(50);
            }
        }
    }

    void StartWorkerIfNeeded()
    {
        bool expected = false;
        if (g_workerStarted.compare_exchange_strong(expected, true))
        {
            g_stopWorker.store(false);
            g_worker = std::thread(WorkerProc);
        }
    }

    void StopWorker()
    {
        if (!g_workerStarted.load()) return;
        g_stopWorker.store(true);
        g_qCv.notify_all();
        if (g_worker.joinable())
        {
            // Wait briefly to avoid deadlocks during shutdown
            g_worker.join();
        }
        g_workerStarted.store(false);
    }

    void ClearQueue(bool cancelCurrent)
    {
        if (cancelCurrent) g_cancelWrite.store(true);
        std::lock_guard<std::mutex> lock(g_qMutex);
        g_queue.clear();
    }
}

// Expose start/stop for DllMain
void StartClipboardWorker() { StartWorkerIfNeeded(); }
void StopClipboardWorker() { StopWorker(); }
void ClearClipboardQueueAndCancel() { ClearQueue(true); }
void ClearClipboardQueue() { ClearQueue(false); }

CClipboardVoiceEngine::CClipboardVoiceEngine()
{
    ZeroMemory(&m_wfx, sizeof(m_wfx));
    m_wfx.wFormatTag = WAVE_FORMAT_PCM;
    m_wfx.nSamplesPerSec = 16000;
    m_wfx.wBitsPerSample = 16;
    m_wfx.nChannels = 1;
    m_wfx.nBlockAlign = (m_wfx.nChannels * m_wfx.wBitsPerSample) / 8;
    m_wfx.nAvgBytesPerSec = m_wfx.nSamplesPerSec * m_wfx.nBlockAlign;
    m_wfx.cbSize = 0;
}

// ISpObjectWithToken
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

    // If host purges queue before speak, or aborts, clear our clipboard queue
    // Note: dwFlags may carry SPF_PURGEBEFORESPEAK, but we didn't capture it above; keep conservative behavior here.
    // Also, if an abort is signaled, clear and exit immediately.
    if (pOutputSite->GetActions() & SPVES_ABORT) {
        ClearClipboardQueueAndCancel();
        EmitEndStream(pOutputSite, 0);
        return S_OK;
    }

    // Determine the format we'll pretend to output
    const WAVEFORMATEX* fmt = pWaveFormatEx ? pWaveFormatEx : &m_wfx;

    // Signal start of stream
    {
        SPEVENT startEvt = {};
        startEvt.eEventId = SPEI_START_INPUT_STREAM;
        startEvt.elParamType = SPET_LPARAM_IS_UNDEFINED;
        startEvt.ulStreamNum = 0;
        startEvt.ullAudioStreamOffset = 0;
        pOutputSite->AddEvents(&startEvt, 1);
    }

    // Build the text to place on the clipboard
    std::wstring text = BuildTextFromFrags(pTextFragList);

    // If this Speak was invoked with PurgeBeforeSpeak, clear pending clipboard updates
    if (dwFlags & SPF_PURGEBEFORESPEAK)
    {
        ClearClipboardQueueAndCancel();
    }

    // Queue clipboard write asynchronously to avoid losing inputs when clipboard is busy
    EnqueueTextAsync(text);

    // Emit progress events if requested and trickle out a bit of audio to advance offsets
    size_t audioBytesWritten = 0;

    // 1000 ms total output target; apportion ~20 ms per word as a default pacing
    const double totalMs = 1000.0;
    const double defaultMsPerWord = 20.0;
    const ULONGLONG forcedInterest = SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY);
    EmitWordAndSentenceEvents(text, pOutputSite, fmt, audioBytesWritten, defaultMsPerWord, forcedInterest);
    if (pOutputSite->GetActions() & SPVES_ABORT) {
        ClearClipboardQueueAndCancel();
        EmitEndStream(pOutputSite, audioBytesWritten);
        return S_OK;
    }

    // Ensure at least 300ms of audio total
    double remainingMs = 0.0;
    if (fmt->nAvgBytesPerSec)
    {
        double producedMs = (double)audioBytesWritten * 1000.0 / (double)fmt->nAvgBytesPerSec;
        if (producedMs < totalMs) remainingMs = totalMs - producedMs;
    }
    if (remainingMs > 0.0)
    {
        WriteTinySilence(pOutputSite, fmt, remainingMs / 1000.0);
        audioBytesWritten += (size_t)(fmt->nAvgBytesPerSec * (remainingMs / 1000.0));
    }

    // End of stream event
    {
        SPEVENT endEvt = {};
        endEvt.eEventId = SPEI_END_INPUT_STREAM;
        endEvt.elParamType = SPET_LPARAM_IS_UNDEFINED;
        endEvt.ulStreamNum = 0;
        endEvt.ullAudioStreamOffset = audioBytesWritten;
        pOutputSite->AddEvents(&endEvt, 1);
    }

    return S_OK;
}

// static
WAVEFORMATEX* CClipboardVoiceEngine::CreatePcmWaveFormat(WORD samplesPerSec)
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
void CClipboardVoiceEngine::SleepMs(DWORD ms)
{
    ::Sleep(ms);
}

// static
bool CClipboardVoiceEngine::CopyTextToClipboard(const std::wstring& text)
{
    // Best-effort with a couple retries in case clipboard is busy
    const int kMaxAttempts = 4;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        if (OpenClipboard(nullptr))
        {
            EmptyClipboard();
            size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL hglob = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hglob)
            {
                void* mem = GlobalLock(hglob);
                if (mem)
                {
                    memcpy(mem, text.c_str(), bytes);
                    GlobalUnlock(hglob);
                    SetClipboardData(CF_UNICODETEXT, hglob);
                }
                else
                {
                    GlobalFree(hglob);
                }
            }
            CloseClipboard();
            return true;
        }
        SleepMs(30);
    }
    return false;
}


// static
std::wstring CClipboardVoiceEngine::BuildTextFromFrags(const SPVTEXTFRAG* pList)
{
    std::wstring result;
    for (auto pFrag = pList; pFrag != nullptr; pFrag = pFrag->pNext)
    {
        if (pFrag->State.eAction == SPVA_Speak || pFrag->State.eAction == SPVA_SpellOut)
        {
            if (pFrag->pTextStart && pFrag->ulTextLen)
            {
                result.append(pFrag->pTextStart, pFrag->ulTextLen);
            }
        }
        // Respect abort during long inputs
        // Note: Cannot access pOutputSite here; only build text.
    }
    return result;
}

HRESULT CClipboardVoiceEngine::WriteTinySilence(ISpTTSEngineSite* pSite, const WAVEFORMATEX* pFmt, double seconds)
{
    if (!pSite || !pFmt || pFmt->wFormatTag != WAVE_FORMAT_PCM) return S_OK;
    size_t totalBytes = static_cast<size_t>(pFmt->nAvgBytesPerSec * seconds);
    if (totalBytes == 0) totalBytes = pFmt->nBlockAlign;
    const size_t bytesPer10ms = (size_t)((double)pFmt->nAvgBytesPerSec / 100.0);
    size_t chunk = bytesPer10ms;
    if (chunk < pFmt->nBlockAlign) chunk = pFmt->nBlockAlign;
    // round chunk to block align
    chunk = (chunk / pFmt->nBlockAlign) * pFmt->nBlockAlign;
    if (chunk == 0) chunk = pFmt->nBlockAlign;
    std::vector<BYTE> zero(chunk, 0);
    size_t sent = 0;
    while (sent < totalBytes)
    {
        if (pSite->GetActions() & SPVES_ABORT)
        {
            // Stop writing immediately on abort
            return S_OK;
        }
        size_t toWriteSz = (totalBytes - sent < chunk) ? (totalBytes - sent) : chunk;
        ULONG toWrite = (ULONG)toWriteSz;
        ULONG wrote = 0;
        HRESULT hr = pSite->Write(zero.data(), toWrite, &wrote);
        if (FAILED(hr)) return hr;
        sent += wrote;
        if (wrote == 0) break; // avoid tight loop on weird hosts
    }
    return S_OK;
}

bool CClipboardVoiceEngine::IsWhitespace(wchar_t ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x00A0;
}

void CClipboardVoiceEngine::AddEvent(ISpTTSEngineSite* pSite, SPEVENTENUM evtId,
                         ULONGLONG audioOffset, ULONG wParam, LONG lParam)
{
    SPEVENT evt = {};
    evt.eEventId = evtId;
    evt.elParamType = SPET_LPARAM_IS_UNDEFINED;
    evt.ulStreamNum = 0;
    evt.ullAudioStreamOffset = audioOffset;
    evt.wParam = wParam;
    evt.lParam = lParam;
    pSite->AddEvents(&evt, 1);
}

void CClipboardVoiceEngine::EnqueueTextAsync(const std::wstring& text)
{
    StartWorkerIfNeeded();
    {
        std::lock_guard<std::mutex> lock(g_qMutex);
        g_queue.emplace_back(text);
    }
    g_qCv.notify_one();
}

void CClipboardVoiceEngine::EmitWordAndSentenceEvents(const std::wstring& text,
                                          ISpTTSEngineSite* pSite,
                                          const WAVEFORMATEX* pFmt,
                                          size_t& audioBytesWritten,
                                          double msPerWord,
                                          ULONGLONG interestMask)
{
    if (!pSite || !pFmt || !pFmt->nAvgBytesPerSec) return;

    const bool wantWord = (interestMask & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
    const bool wantSent = (interestMask & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;

    const size_t bytesPerMs = (size_t)((double)pFmt->nAvgBytesPerSec / 1000.0);
    const size_t chunkBytes = (size_t)(bytesPerMs * (msPerWord > 0.0 ? msPerWord : 10.0));

    size_t i = 0;
    while (i < text.size())
    {
        if (pSite->GetActions() & SPVES_ABORT) {
            ClearClipboardQueueAndCancel();
            EmitEndStream(pSite, audioBytesWritten);
            return; // stop emitting
        }

        // Skip leading whitespace
        while (i < text.size() && IsWhitespace(text[i]))
        {
            if (wantWord)
            {
                // Treat whitespace as advancing time slightly
                ULONG dummyWritten = 0;
                if (chunkBytes)
                {
                    std::vector<BYTE> z(chunkBytes, 0);
                    pSite->Write(z.data(), (ULONG)z.size(), &dummyWritten);
                    audioBytesWritten += dummyWritten;
                }
            }
            ++i;
        }
        if (i >= text.size()) break;

        // Start of a word
        size_t start = i;
        while (i < text.size() && !IsWhitespace(text[i])) ++i;
        size_t len = i - start;

        // Emit word boundary
        if (wantWord)
        {
            AddEvent(pSite, SPEI_WORD_BOUNDARY, audioBytesWritten, (ULONG)start, (LONG)len);
        }

        // Advance audio a bit to simulate progress
        if (chunkBytes)
        {
            ULONG wrote = 0;
            std::vector<BYTE> z(chunkBytes, 0);
            pSite->Write(z.data(), (ULONG)z.size(), &wrote);
            audioBytesWritten += wrote;
        }

        // Sentence boundary detection: on punctuation . ! ? followed by space or end
        if (wantSent && len > 0)
        {
            wchar_t last = text[start + len - 1];
            if (last == L'.' || last == L'!' || last == L'?')
            {
                // Find sentence start (very rough): previous whitespace
                size_t sBeg = start;
                while (sBeg > 0 && !IsWhitespace(text[sBeg - 1])) --sBeg;
                AddEvent(pSite, SPEI_SENTENCE_BOUNDARY, audioBytesWritten, (ULONG)sBeg, (LONG)(i - sBeg));
            }
        }
    }
}

void CClipboardVoiceEngine::EmitEndStream(ISpTTSEngineSite* pSite, ULONGLONG audioOffset)
{
    if (!pSite) return;
    SPEVENT endEvt = {};
    endEvt.eEventId = SPEI_END_INPUT_STREAM;
    endEvt.elParamType = SPET_LPARAM_IS_UNDEFINED;
    endEvt.ulStreamNum = 0;
    endEvt.ullAudioStreamOffset = audioOffset;
    pSite->AddEvents(&endEvt, 1);
}

// ---------- Registration of SAPI token ----------
#include <shlwapi.h>

static const wchar_t* kTokenKey64 = L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\ClipboardVoice";
static const wchar_t* kTokenKey32 = L"SOFTWARE\\WOW6432Node\\Microsoft\\Speech\\Voices\\Tokens\\ClipboardVoice";
static const wchar_t* kVendor = L"Clipboard Voice";
static const wchar_t* kName = L"Clipboard Voice";
static const wchar_t* kLangHex = L"409"; // en-US
static const wchar_t* kClsidStr = L"{F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}";

static HRESULT WriteToken(HKEY root, REGSAM view)
{
    HKEY hKey = nullptr;
    LONG r = RegCreateKeyExW(root, kTokenKey64, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | view, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);

    // Default = readable name
    RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)kName, (DWORD)((wcslen(kName)+1)*sizeof(wchar_t)));
    RegSetValueExW(hKey, L"CLSID", 0, REG_SZ, (const BYTE*)kClsidStr, (DWORD)((wcslen(kClsidStr)+1)*sizeof(wchar_t)));
    RegSetValueExW(hKey, L"Version", 0, REG_SZ, (const BYTE*)L"1.0", (DWORD)(sizeof(L"1.0")));

    // Attributes
    HKEY hAttr = nullptr;
    r = RegCreateKeyExW(hKey, L"Attributes", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | view, nullptr, &hAttr, nullptr);
    if (r == ERROR_SUCCESS)
    {
        RegSetValueExW(hAttr, L"Language", 0, REG_SZ, (const BYTE*)kLangHex, (DWORD)((wcslen(kLangHex)+1)*sizeof(wchar_t)));
        RegSetValueExW(hAttr, L"Name", 0, REG_SZ, (const BYTE*)kName, (DWORD)((wcslen(kName)+1)*sizeof(wchar_t)));
        RegSetValueExW(hAttr, L"Vendor", 0, REG_SZ, (const BYTE*)kVendor, (DWORD)((wcslen(kVendor)+1)*sizeof(wchar_t)));
        RegSetValueExW(hAttr, L"Gender", 0, REG_SZ, (const BYTE*)L"Neutral", (DWORD)(sizeof(L"Neutral")));
        RegSetValueExW(hAttr, L"Age", 0, REG_SZ, (const BYTE*)L"Adult", (DWORD)(sizeof(L"Adult")));
        RegCloseKey(hAttr);
    }

    // Optionally declare audio format
    HKEY hAF = nullptr;
    r = RegCreateKeyExW(hKey, L"AudioFormats", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | view, nullptr, &hAF, nullptr);
    if (r == ERROR_SUCCESS)
    {
        // Declaring SAFT16kHz16BitMono as supported
        RegSetValueExW(hAF, L"1", 0, REG_SZ, (const BYTE*)L"SAFT16kHz16BitMono", (DWORD)(sizeof(L"SAFT16kHz16BitMono")));
        RegCloseKey(hAF);
    }

    RegCloseKey(hKey);
    return S_OK;
}

HRESULT RegisterSapiToken()
{
    // Write both 64-bit and 32-bit token views from a 64-bit process; if registering from 32-bit, the 64-key write will still work.
    HRESULT hr1 = WriteToken(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
    HRESULT hr2 = WriteToken(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY);
    // Prefer returning success if at least one succeeded
    if (SUCCEEDED(hr1) || SUCCEEDED(hr2)) return S_OK;
    return FAILED(hr1) ? hr1 : hr2;
}

static void DeleteKeyRecursive(HKEY root, const wchar_t* subkey, REGSAM view)
{
    // Simple recursive delete using SHDeleteKey if available
    HMODULE hShlwapi = LoadLibraryW(L"Shlwapi.dll");
    if (hShlwapi)
    {
        typedef LSTATUS (WINAPI *PFNSHDELETEKEYW)(HKEY, LPCWSTR);
        auto pfn = (PFNSHDELETEKEYW)GetProcAddress(hShlwapi, "SHDeleteKeyW");
        if (pfn)
        {
            HKEY hView = nullptr;
            if (RegOpenKeyExW(root, L"", 0, KEY_WRITE | view, &hView) == ERROR_SUCCESS)
            {
                pfn(hView, subkey);
                RegCloseKey(hView);
            }
        }
        FreeLibrary(hShlwapi);
    }
}

HRESULT UnregisterSapiToken()
{
    DeleteKeyRecursive(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\ClipboardVoice", KEY_WOW64_64KEY);
    DeleteKeyRecursive(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Speech\\Voices\\Tokens\\ClipboardVoice", KEY_WOW64_32KEY);
    return S_OK;
}
