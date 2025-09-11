#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include "ClipboardVoiceEngine.h"

// Module
class CClipboardVoiceModule : public CAtlDllModuleT< CClipboardVoiceModule > {};

CClipboardVoiceModule _AtlModule;

OBJECT_ENTRY_AUTO(CLSID_ClipboardVoiceEngine, CClipboardVoiceEngine)

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    return _AtlModule.DllMain(dwReason, lpReserved);
}

// Forward decl for helper creating SAPI token keys
HRESULT RegisterSapiToken();
HRESULT UnregisterSapiToken();

// Exported functions
STDAPI DllCanUnloadNow(void)
{
    return _AtlModule.DllCanUnloadNow();
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}

STDAPI DllRegisterServer(void)
{
    // Skip type library registration to avoid TYPE_E_CANTLOADLIBRARY (0x80029C4A)
    // We don't require a registered TLB for this engine.
    HRESULT hr = _AtlModule.DllRegisterServer(FALSE);
    if (SUCCEEDED(hr))
    {
        // Create SAPI Voice Token entries
        hr = RegisterSapiToken();
    }
    return hr;
}

STDAPI DllUnregisterServer(void)
{
    UnregisterSapiToken();
    // Skip type library unregistration to match registration behavior
    return _AtlModule.DllUnregisterServer(FALSE);
}
