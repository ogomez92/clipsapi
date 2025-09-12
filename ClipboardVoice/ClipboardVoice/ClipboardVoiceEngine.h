#pragma once

#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <sapi.h>
#include <sapiddk.h>
#include "resource.h"
#include <string>
#include <vector>

// {F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}
static const CLSID CLSID_ClipboardVoiceEngine = 
{ 0xf4abf2e3, 0x9f21, 0x4b8c, { 0xb4, 0x10, 0x4a, 0x0a, 0x9e, 0x5c, 0x5b, 0xd3 } };

class ATL_NO_VTABLE CClipboardVoiceEngine
    : public CComObjectRootEx<CComMultiThreadModel>
    , public CComCoClass<CClipboardVoiceEngine, &CLSID_ClipboardVoiceEngine>
    , public ISpTTSEngine
    , public ISpObjectWithToken
{
public:
    CClipboardVoiceEngine();
    ~CClipboardVoiceEngine() = default;

    DECLARE_REGISTRY_RESOURCEID(IDR_CLIPBOARDVOICEENGINE)
    DECLARE_CLASSFACTORY()
    DECLARE_NOT_AGGREGATABLE(CClipboardVoiceEngine)

    BEGIN_COM_MAP(CClipboardVoiceEngine)
        COM_INTERFACE_ENTRY(ISpTTSEngine)
        COM_INTERFACE_ENTRY(ISpObjectWithToken)
    END_COM_MAP()

    // ISpObjectWithToken
    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

    // ISpTTSEngine
    STDMETHOD(Speak)(DWORD dwFlags, REFGUID rguidFormatId, const WAVEFORMATEX* pWaveFormatEx,
                     const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFormatId, const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pDesiredFormatId, WAVEFORMATEX** ppCoMemDesiredWaveFormatEx) override;

private:
    CComPtr<ISpObjectToken> m_cpToken;
    WAVEFORMATEX m_wfx{};

    static WAVEFORMATEX* CreatePcmWaveFormat(WORD samplesPerSec);
    static void SleepMs(DWORD ms);
    static bool CopyTextToClipboard(const std::wstring& text);
    static std::wstring BuildTextFromFrags(const SPVTEXTFRAG* pList);
    HRESULT WriteTinySilence(ISpTTSEngineSite* pSite, const WAVEFORMATEX* pFmt, double seconds);

    // Progress helpers
    static bool IsWhitespace(wchar_t ch);
    static void EmitWordAndSentenceEvents(const std::wstring& text,
                                          ISpTTSEngineSite* pSite,
                                          const WAVEFORMATEX* pFmt,
                                          size_t& audioBytesWritten,
                                          double msPerWord,
                                          ULONGLONG interestMask);
    static void AddEvent(ISpTTSEngineSite* pSite, SPEVENTENUM evtId,
                         ULONGLONG audioOffset, ULONG wParam, LONG lParam);
};

// Helpers used by DllRegisterServer
HRESULT RegisterSapiToken();
HRESULT UnregisterSapiToken();
