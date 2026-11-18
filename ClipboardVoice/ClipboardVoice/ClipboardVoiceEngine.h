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

// {8D3B6C1E-2F47-4A5D-9B0E-6C1F2A7D4E95}
static const CLSID CLSID_PrismVoiceEngine =
{ 0x8d3b6c1e, 0x2f47, 0x4a5d, { 0x9b, 0x0e, 0x6c, 0x1f, 0x2a, 0x7d, 0x4e, 0x95 } };

// The SAPI side shared by every voice: parses the text, raises word, sentence
// and bookmark events, and paces the utterance with silence. Subclasses decide
// where the text goes (the clipboard, a screen reader through Prism).
class CVoiceEngineBase
    : public ISpTTSEngine
    , public ISpObjectWithToken
{
public:
    CVoiceEngineBase();
    virtual ~CVoiceEngineBase() = default;

    // ISpObjectWithToken
    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

    // ISpTTSEngine
    STDMETHOD(Speak)(DWORD dwFlags, REFGUID rguidFormatId, const WAVEFORMATEX* pWaveFormatEx,
                     const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFormatId, const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pDesiredFormatId, WAVEFORMATEX** ppCoMemDesiredWaveFormatEx) override;

    static bool IsWhitespace(wchar_t ch);
    static bool IsBlank(const std::wstring& text);

protected:
    // Called once per Speak with the joined text (possibly blank). `purge` is
    // set when the caller asked to drop what was being said.
    virtual void OnText(const std::wstring& text, bool purge) = 0;
    // SAPI aborted the utterance (the program interrupted speech).
    virtual void OnAbort() = 0;
    // After the words are paced: keep the utterance going while the output is
    // still busy (writing silence through WriteSilence). Return false if SAPI aborted.
    virtual bool HoldWhileBusy(const std::wstring& /*text*/, ISpTTSEngineSite* /*pSite*/,
                               const WAVEFORMATEX* /*pFmt*/, ULONGLONG& /*audioBytes*/) { return true; }
    // Speak finished; `completed` is false when it was aborted.
    virtual void OnDone(bool /*completed*/) {}

    // Writes `bytes` of silence in small chunks; false when SAPI asked to abort.
    static bool WriteSilence(ISpTTSEngineSite* pSite, const WAVEFORMATEX* pFmt,
                             size_t bytes, ULONGLONG& audioBytes);
    static bool Aborted(ISpTTSEngineSite* pSite);

private:
    // Text of one Speak call, with what we need to raise events against it.
    struct SpeechInput
    {
        std::wstring text;              // what goes to the output
        std::vector<ULONG> srcOffset;   // offset in the caller's original string, per char of text
        struct Bookmark { size_t at; std::wstring name; };
        std::vector<Bookmark> bookmarks; // bookmark fires before text[at] is "spoken"
    };

    static WAVEFORMATEX* CreatePcmWaveFormat(DWORD samplesPerSec);
    static SpeechInput BuildInput(const SPVTEXTFRAG* pList);

    // Returns false when SAPI asked us to abort.
    static bool EmitProgress(const SpeechInput& in, ISpTTSEngineSite* pSite,
                             const WAVEFORMATEX* pFmt, ULONGLONG& audioBytes);
    static void AddEvent(ISpTTSEngineSite* pSite, SPEVENTENUM evtId, ULONGLONG audioOffset,
                         WPARAM wParam, LPARAM lParam, SPEVENTLPARAMTYPE lParamType = SPET_LPARAM_IS_UNDEFINED);

    CComPtr<ISpObjectToken> m_cpToken;
    WAVEFORMATEX m_wfx{};
};

// Clipboard Voice: writes the spoken text to the clipboard.
class ATL_NO_VTABLE CClipboardVoiceEngine
    : public CComObjectRootEx<CComMultiThreadModel>
    , public CComCoClass<CClipboardVoiceEngine, &CLSID_ClipboardVoiceEngine>
    , public CVoiceEngineBase
{
public:
    DECLARE_REGISTRY_RESOURCEID(IDR_CLIPBOARDVOICEENGINE)
    DECLARE_CLASSFACTORY()
    DECLARE_NOT_AGGREGATABLE(CClipboardVoiceEngine)

    BEGIN_COM_MAP(CClipboardVoiceEngine)
        COM_INTERFACE_ENTRY(ISpTTSEngine)
        COM_INTERFACE_ENTRY(ISpObjectWithToken)
    END_COM_MAP()

    // Used by the background clipboard writer.
    static bool CopyTextToClipboard(const std::wstring& text);

protected:
    void OnText(const std::wstring& text, bool purge) override;
    void OnAbort() override;
};

// Sapi2Prism: sends the spoken text to the running screen reader through Prism
// (prism.dll next to this DLL). See PrismOutput.cpp.
class ATL_NO_VTABLE CPrismVoiceEngine
    : public CComObjectRootEx<CComMultiThreadModel>
    , public CComCoClass<CPrismVoiceEngine, &CLSID_PrismVoiceEngine>
    , public CVoiceEngineBase
{
public:
    DECLARE_REGISTRY_RESOURCEID(IDR_PRISMVOICEENGINE)
    DECLARE_CLASSFACTORY()
    DECLARE_NOT_AGGREGATABLE(CPrismVoiceEngine)

    BEGIN_COM_MAP(CPrismVoiceEngine)
        COM_INTERFACE_ENTRY(ISpTTSEngine)
        COM_INTERFACE_ENTRY(ISpObjectWithToken)
    END_COM_MAP()

protected:
    void OnText(const std::wstring& text, bool purge) override;
    void OnAbort() override;
    bool HoldWhileBusy(const std::wstring& text, ISpTTSEngineSite* pSite,
                       const WAVEFORMATEX* pFmt, ULONGLONG& audioBytes) override;
    void OnDone(bool completed) override;
};

// Voice token registration, used by DllRegisterServer / DllInstall.
// spec: locale names separated by commas, semicolons or spaces, optionally
// grouped by output: L"en,es" (Clipboard Voice only), L"clipboard:en;prism:en,es",
// L"prism:es-MX". "clipboard:" or "prism:" alone means none of that kind.
HRESULT RegisterSapiTokens(const wchar_t* spec);
HRESULT UnregisterSapiTokens();
bool AnySapiTokenRegistered();
