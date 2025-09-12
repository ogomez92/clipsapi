#include "ClipboardVoiceEngine.h"
#include <strsafe.h>

using std::wstring;

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

STDMETHODIMP CClipboardVoiceEngine::Speak(DWORD /*dwFlags*/, REFGUID /*rguidFormatId*/, const WAVEFORMATEX* pWaveFormatEx,
                     const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite)
{
    if (!pOutputSite || !pTextFragList) return E_INVALIDARG;

    // Respect abort requests quickly
    if (pOutputSite->GetActions() & SPVES_ABORT) {
        return S_OK;
    }

    // Determine the format we'll pretend to output
    const WAVEFORMATEX* fmt = pWaveFormatEx ? pWaveFormatEx : &m_wfx;

    // Signal start of stream
    SPEVENT startEvt = {};
    startEvt.eEventId = SPEI_START_INPUT_STREAM;
    pOutputSite->AddEvents(&startEvt, 1);

    // Build the text to place on the clipboard
    std::wstring text = BuildTextFromFrags(pTextFragList);

    // Try to copy to clipboard (best-effort)
    CopyTextToClipboard(text);

    // Write a tiny slice of silence so clients that expect audio are satisfied
    WriteTinySilence(pOutputSite, fmt, 0.30); // 300ms

    // End of stream event
    SPEVENT endEvt = {};
    endEvt.eEventId = SPEI_END_INPUT_STREAM;
    pOutputSite->AddEvents(&endEvt, 1);

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
    size_t bytes = static_cast<size_t>(pFmt->nAvgBytesPerSec * seconds);
    if (bytes == 0) bytes = pFmt->nBlockAlign;
    std::vector<BYTE> zero(bytes, 0);
    ULONG written = 0;
    return pSite->Write(zero.data(), static_cast<ULONG>(zero.size()), &written);
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
