# Clipboard Voice

Tired of NVDASapi?

Clipboard Voice is a minimal SAPI 5 TTS engine that writes the spoken text to the Windows clipboard instead of producing audio. It is implemented as a native ATL COM in‑proc DLL that implements `ISpTTSEngine`.

## Install via Scripts

The scripts install both x64 and x86 voices for maximum compatibility.

The repository already includes the dlls, just run the install script and boom! Just pay attention to the regsvr32 messages, let me know if something fails.

### Batch (auto-detect)

Run Command Prompt as Administrator:

```
scripts\install.bat
```

The batch script auto-detects the same default locations. You can also specify both paths explicitly:

```
scripts\install.bat "C:\path\to\ClipboardVoice\x64\Release\ClipboardVoice.dll" "C:\path\to\ClipboardVoice\x86\Release\ClipboardVoice.dll"
```

Uninstall (Batch, Admin):

```
scripts\uninstall.bat
```

## Test

```
./test/say.ps1 "This will land on the clipboard."
```

It selects the “Clipboard Voice” token and speaks; the text should be on the clipboard afterward.

## How It Works

- `ISpTTSEngine::Speak` concatenates input text fragments and writes the result to the clipboard as UTF‑16.
- The engine writes ~50 ms of PCM silence to satisfy clients that expect audio.
- The voice token declares language 409 and basic attributes.

## Contribute

 ### Prerequisites

- Visual Studio 2019/2022 with C++ and ATL support
- Windows 10/11 SDK (headers/libs: `sapi.h`, `sapiddk.h`) (might come included with VS studio already)
- Administrator rights to install/register

### Build

1. Open `ClipboardVoice/ClipboardVoice.sln` in Visual Studio.
2. Build these configurations:
   - `Release | x64`
   - `Release | Win32`

### Output Paths

- x64: `ClipboardVoice/x64/Release/ClipboardVoice.dll`
- x86 (Win32): `ClipboardVoice/Release/ClipboardVoice.dll`

## Caveats

- Requires Administrator rights to install (writes HKLM and registers COM).
- Clipboard content is replaced by the spoken text.
- Keep both x86 and x64 installed so 32‑bit and 64‑bit apps can use the voice.
- Windows defender might complain and delete the dll. Add it to allowed threats or whatever. Funny how the stuff that you are compiling yourself are throwing windows defender into a fit.

## Contributing

### Repo Layout

- `ClipboardVoice/` Visual Studio solution and project
- `scripts/` batch install/uninstall scripts
- `test/` PowerShell test script

### Tips

- Display name/vendor: update in:
  - `ClipboardVoice/ClipboardVoice/ClipboardVoiceEngine.cpp`
- Silence duration: adjust `WriteTinySilence(...)` in `ClipboardVoiceEngine.cpp`.

## Troubleshooting

- Voice not listed:
  - Verify install succeeded; check registry:
    - `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens\ClipboardVoice`
    - `HKLM\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\ClipboardVoice`
  - Check COM class: `HKCR\CLSID\{F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}` points to the installed DLL.
- Clipboard not changing:
  - Another process may hold the clipboard; try again with a shorter string.
  - Try the test script; ensure AV/EDR did not block registration.
- regsvr32: entry point not found / no DllRegisterServer:
  - Ensure you rebuilt after pulling latest changes. The DLL now
    explicitly exports `DllRegisterServer`/`DllUnregisterServer` via a
    module definition file (`ClipboardVoice/ClipboardVoice/ClipboardVoice.def`).
  - Use the correct regsvr32 for the DLL bitness:
    - 32-bit DLL → `C:\Windows\SysWOW64\regsvr32.exe`
    - 64-bit DLL → `C:\Windows\System32\regsvr32.exe`
- regsvr32 error 0x80029C4A (TYPE_E_CANTLOADLIBRARY):
  - Rebuild after latest changes. The DLL now skips ATL type library
    registration, which avoids this error when no embedded TLB exists.
  - If you need a TLB, embed it by adding a `TYPELIB` resource in
    `ClipboardVoice.rc` and switch the ATL calls back to default.
