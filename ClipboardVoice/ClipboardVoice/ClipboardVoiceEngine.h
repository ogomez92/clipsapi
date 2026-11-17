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

    // Used by the background clipboard writer.
    static bool CopyTextToClipboard(const std::wstring& text);

private:
    // Text of one Speak call, with what we need to raise events against it.
    struct SpeechInput
    {
        std::wstring text;              // what goes on the clipboard
        std::vector<ULONG> srcOffset;   // offset in the caller's original string, per char of text
        struct Bookmark { size_t at; std::wstring name; };
        std::vector<Bookmark> bookmarks; // bookmark fires before text[at] is "spoken"
    };

    static WAVEFORMATEX* CreatePcmWaveFormat(DWORD samplesPerSec);
    static SpeechInput BuildInput(const SPVTEXTFRAG* pList);
    static bool IsWhitespace(wchar_t ch);
    static bool IsBlank(const std::wstring& text);

    // These return false when SAPI asked us to abort.
    static bool EmitProgress(const SpeechInput& in, ISpTTSEngineSite* pSite,
                             const WAVEFORMATEX* pFmt, ULONGLONG& audioBytes);
    static bool WriteSilence(ISpTTSEngineSite* pSite, const WAVEFORMATEX* pFmt,
                             size_t bytes, ULONGLONG& audioBytes);
    static void AddEvent(ISpTTSEngineSite* pSite, SPEVENTENUM evtId, ULONGLONG audioOffset,
                         WPARAM wParam, LPARAM lParam, SPEVENTLPARAMTYPE lParamType = SPET_LPARAM_IS_UNDEFINED);

    CComPtr<ISpObjectToken> m_cpToken;
    WAVEFORMATEX m_wfx{};
};

// Voice token registration, used by DllRegisterServer / DllInstall.
// languages: comma/semicolon/space separated locale names, e.g. L"en,es" or L"en-GB es-MX".
HRESULT RegisterSapiTokens(const wchar_t* languages);
HRESULT UnregisterSapiTokens();
