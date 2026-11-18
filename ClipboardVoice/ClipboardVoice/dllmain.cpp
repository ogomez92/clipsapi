#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include "ClipboardVoiceEngine.h"

// Module
class CClipboardVoiceModule : public CAtlDllModuleT< CClipboardVoiceModule > {};

CClipboardVoiceModule _AtlModule;

OBJECT_ENTRY_AUTO(CLSID_ClipboardVoiceEngine, CClipboardVoiceEngine)
OBJECT_ENTRY_AUTO(CLSID_PrismVoiceEngine, CPrismVoiceEngine)

extern "C" BOOL WINAPI DllMain(HINSTANCE /*hInstance*/, DWORD dwReason, LPVOID lpReserved)
{
    // The clipboard and Prism worker threads pin the DLL once started, so
    // there is nothing to tear down here (and joining a thread under the
    // loader lock would deadlock).
    return _AtlModule.DllMain(dwReason, lpReserved);
}

STDAPI DllCanUnloadNow(void)
{
    return _AtlModule.DllCanUnloadNow();
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}

// regsvr32 ClipboardVoice.dll
// Registers both COM classes. Adds the English Clipboard Voice only when none of
// our voices exists yet, so re-registering keeps the voices chosen earlier.
STDAPI DllRegisterServer(void)
{
    // Skip type library registration to avoid TYPE_E_CANTLOADLIBRARY (0x80029C4A);
    // the engine doesn't need a registered TLB.
    HRESULT hr = _AtlModule.DllRegisterServer(FALSE);
    if (SUCCEEDED(hr) && !AnySapiTokenRegistered())
        hr = RegisterSapiTokens(L"en");
    return hr;
}

// regsvr32 /u ClipboardVoice.dll
STDAPI DllUnregisterServer(void)
{
    UnregisterSapiTokens();
    return _AtlModule.DllUnregisterServer(FALSE);
}

// regsvr32 /i:en,es ClipboardVoice.dll                      -> Clipboard Voice in these languages
// regsvr32 /i:clipboard:en,es;prism:en,es ClipboardVoice.dll -> Clipboard Voice and Sapi2Prism voices
// regsvr32 /i:prism:en ClipboardVoice.dll                    -> only the English Sapi2Prism voice
// regsvr32 /u /n /i ClipboardVoice.dll                       -> removes the voices only
// Each call replaces the previous set. Languages are locale names: "en", "es", "es-MX", "fr", ...
STDAPI DllInstall(BOOL bInstall, LPCWSTR pszCmdLine)
{
    return bInstall ? RegisterSapiTokens(pszCmdLine) : UnregisterSapiTokens();
}
