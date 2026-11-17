#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include "ClipboardVoiceEngine.h"

// Module
class CClipboardVoiceModule : public CAtlDllModuleT< CClipboardVoiceModule > {};

CClipboardVoiceModule _AtlModule;

OBJECT_ENTRY_AUTO(CLSID_ClipboardVoiceEngine, CClipboardVoiceEngine)

bool AnySapiTokenRegistered();

extern "C" BOOL WINAPI DllMain(HINSTANCE /*hInstance*/, DWORD dwReason, LPVOID lpReserved)
{
    // The clipboard worker thread pins the DLL once started, so there is
    // nothing to tear down here (and joining a thread under the loader lock
    // would deadlock).
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
// Registers the COM class. Adds the English voice only when no Clipboard Voice
// token exists yet, so re-registering keeps the languages chosen earlier.
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

// regsvr32 /i:en,es ClipboardVoice.dll   -> one voice per language (replaces the previous set)
// regsvr32 /u /n /i ClipboardVoice.dll   -> removes the voices only
// Languages are locale names: "en", "es", "es-MX", "fr", ...
STDAPI DllInstall(BOOL bInstall, LPCWSTR pszCmdLine)
{
    return bInstall ? RegisterSapiTokens(pszCmdLine) : UnregisterSapiTokens();
}
