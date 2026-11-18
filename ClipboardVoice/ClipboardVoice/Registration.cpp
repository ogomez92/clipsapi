// ---------- SAPI voice token registration ----------
//
// One token per language and kind of voice. Clipboard Voice in English (en-US)
// keeps the original key ("ClipboardVoice") so programs that saved the token
// keep working; other languages get "ClipboardVoice_<locale>". Every voice is
// named "Clipboard Voice (<Language>)", English included: System.Speech's
// SelectVoice("X (Spanish)") takes an installed voice named plain "X" listed
// before it as the match and then fails, so no name may be a word-prefix of
// another. Sapi2Prism voices follow the same rule with "Sapi2Prism".

#include "ClipboardVoiceEngine.h"
#include <cwctype>

static const wchar_t* kTokensKey = L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens";
static const REGSAM kViews[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

namespace {
    struct VoiceKind
    {
        const wchar_t* prefix;  // token key prefix
        const wchar_t* name;    // voice name (and vendor)
        const wchar_t* clsid;   // engine class
    };

    const VoiceKind kClipboardKind = { L"ClipboardVoice", L"Clipboard Voice", L"{F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}" };
    const VoiceKind kPrismKind = { L"Sapi2Prism", L"Sapi2Prism", L"{8D3B6C1E-2F47-4A5D-9B0E-6C1F2A7D4E95}" };
    const VoiceKind* const kKinds[] = { &kClipboardKind, &kPrismKind };

    struct VoiceLanguage
    {
        const VoiceKind* kind = nullptr;
        std::wstring keyName;   // token key under ...\Voices\Tokens
        std::wstring name;      // voice name shown to users
        std::wstring langAttr;  // SAPI "Language" attribute, e.g. "C0A;A"
    };

    struct VoiceRequest
    {
        const VoiceKind* kind;
        std::wstring code;
    };
}

static HRESULT ResolveVoiceLanguage(const VoiceKind& kind, const std::wstring& requested, VoiceLanguage& out)
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
    out.kind = &kind;
    out.langAttr = buf;

    if (_wcsicmp(specific, L"en-US") == 0)
    {
        out.keyName = kind.prefix;
        out.name = std::wstring(kind.name) + L" (English)";
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
    out.keyName = std::wstring(kind.prefix) + L"_" + id;
    out.name = std::wstring(kind.name) + L" (" + display + L")";
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
    if (r == ERROR_SUCCESS) r = SetString(hKey, L"CLSID", lang.kind->clsid);
    if (r == ERROR_SUCCESS) r = SetString(hKey, L"Version", L"1.0");

    HKEY hAttr = nullptr;
    if (r == ERROR_SUCCESS)
        r = ::RegCreateKeyExW(hKey, L"Attributes", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | view,
                              nullptr, &hAttr, nullptr);
    if (r == ERROR_SUCCESS)
    {
        r = SetString(hAttr, L"Language", lang.langAttr);
        if (r == ERROR_SUCCESS) r = SetString(hAttr, L"Name", lang.name);
        if (r == ERROR_SUCCESS) r = SetString(hAttr, L"Vendor", lang.kind->name);
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

// "<prefix>" or "<prefix>_<locale>".
static bool IsTokenKeyOf(const VoiceKind& kind, const wchar_t* name)
{
    const size_t n = wcslen(kind.prefix);
    return _wcsnicmp(name, kind.prefix, n) == 0 && (name[n] == L'\0' || name[n] == L'_');
}

static bool IsOurTokenKey(const wchar_t* name)
{
    for (const VoiceKind* kind : kKinds)
        if (IsTokenKeyOf(*kind, name)) return true;
    return false;
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

// "en,es" -> Clipboard Voice en, es. "clipboard:en prism:en,es" -> by kind.
// "prism:" switches the kind for the codes after it and can be glued to the
// first code ("prism:en"). Returns false on an unknown kind.
static bool ParseSpec(const wchar_t* spec, std::vector<VoiceRequest>& out, bool& kindGiven)
{
    const VoiceKind* kind = &kClipboardKind;
    std::wstring cur;
    bool ok = true;
    kindGiven = false;
    auto flush = [&] {
        if (cur.empty()) return;
        const size_t colon = cur.find(L':');
        if (colon != std::wstring::npos)
        {
            const std::wstring head = cur.substr(0, colon);
            if (_wcsicmp(head.c_str(), L"clipboard") == 0) kind = &kClipboardKind;
            else if (_wcsicmp(head.c_str(), L"prism") == 0) kind = &kPrismKind;
            else ok = false;
            kindGiven = true;
            cur.erase(0, colon + 1);
        }
        if (!cur.empty())
        {
            for (auto& ch : cur) if (ch == L'_') ch = L'-';
            bool dup = false;
            for (const auto& r : out) dup = dup || (r.kind == kind && _wcsicmp(r.code.c_str(), cur.c_str()) == 0);
            if (!dup) out.push_back({ kind, cur });
        }
        cur.clear();
    };
    for (const wchar_t* p = spec ? spec : L""; *p; ++p)
    {
        if (*p == L',' || *p == L';' || *p == L'"' || std::iswspace(*p)) flush();
        else cur.push_back(*p);
    }
    flush();
    return ok;
}

HRESULT RegisterSapiTokens(const wchar_t* spec)
{
    std::vector<VoiceRequest> requested;
    bool kindGiven = false;
    if (!ParseSpec(spec, requested, kindGiven)) return E_INVALIDARG;
    // Nothing at all: the English Clipboard Voice, as before.
    if (requested.empty() && !kindGiven) requested.push_back({ &kClipboardKind, L"en" });

    // Validate everything before touching the registry.
    std::vector<VoiceLanguage> voices;
    for (const auto& req : requested)
    {
        VoiceLanguage v;
        const HRESULT hr = ResolveVoiceLanguage(*req.kind, req.code, v);
        if (FAILED(hr)) return hr;
        bool dup = false;
        for (const auto& existing : voices) dup = dup || _wcsicmp(existing.keyName.c_str(), v.keyName.c_str()) == 0;
        if (!dup) voices.push_back(v);
    }

    // Replace the previous set so removed languages disappear.
    UnregisterSapiTokens();
    if (voices.empty()) return S_OK;

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
