# Clipboard Voice

Tired of NVDASapi?

Clipboard Voice is a minimal SAPI 5 TTS engine that writes the spoken text to the Windows clipboard instead of producing audio. It is implemented as a native ATL COM in‑proc DLL that implements `ISpTTSEngine`.

It can register one voice per language, so programs that pick a voice by language (for example a Spanish game looking for a Spanish voice) find one. English keeps the original name, **Clipboard Voice**; other languages are called **Clipboard Voice (Spanish)**, **Clipboard Voice (French)**, and so on. Any language Windows knows that has a Windows language ID can be used, including regional variants such as `es-MX` or `en-GB`.

## Install

### Installer (recommended)

Download `ClipboardVoiceInstaller.exe` from the [latest release](https://github.com/ogomez92/clipsapi/releases/latest) and run it (it asks for administrator rights).

- Search the language list by name or code, check the languages you want (space bar), then press **Install**.
- Run it again to add or remove languages (**Apply languages**) or to uninstall.
- It also adds Clipboard Voice to Settings > Apps, where **Modify** reopens it and **Uninstall** removes everything.

Console mode, for scripts or if you prefer a terminal:

```
ClipboardVoiceInstaller.exe --cli                    interactive menu
ClipboardVoiceInstaller.exe --cli --install en,es    install exactly these languages
ClipboardVoiceInstaller.exe --cli --uninstall        remove everything
ClipboardVoiceInstaller.exe --cli --list             list every language code you can use
add --log <file> to also write the output to a file
```

### Scripts

The repository includes the built DLLs. From the repository folder:

```
scripts\install.bat en es
```

Pass any language codes (`en`, `es`, `fr`, `es-MX`, `en-GB`, ...); with none it installs English only. The script asks for administrator rights itself, installs both the 64-bit and 32-bit voices, and checks that every voice loads in both. Running it again replaces the installed DLLs and the set of languages.

PowerShell directly: `powershell -ExecutionPolicy Bypass -File scripts\install.ps1 en es`. To use DLLs from elsewhere: `scripts\install.ps1 en es -X64Dll path\x64.dll -X86Dll path\x86.dll`.

Uninstall: `scripts\uninstall.bat`.

### By hand

```
regsvr32 /i:en,es "C:\Program Files\ClipboardVoice\ClipboardVoice.dll"
```

`regsvr32 dll` alone registers the engine and, if no Clipboard Voice exists yet, the English voice. `/i:<languages>` replaces the set of voices. `regsvr32 /u dll` removes the engine and all its voices. Use `C:\Windows\SysWOW64\regsvr32.exe` for the 32-bit DLL.

## Test

```
powershell -ExecutionPolicy Bypass -File test\say.ps1 "This will land on the clipboard."
powershell -ExecutionPolicy Bypass -File test\say.ps1 "Esto va al portapapeles." -Voice "Clipboard Voice (Spanish)"
```

The text should be on the clipboard afterward.

`test\eventdump.cpp` is a small console program that speaks with a voice and prints every SAPI event it receives (word and sentence boundaries, bookmarks, end of stream) plus the clipboard contents. Build instructions are at the top of the file.

## How It Works

- `ISpTTSEngine::Speak` joins the text fragments and a background thread writes them to the clipboard as UTF‑16, retrying while another program has the clipboard open. Empty or blank requests leave the clipboard alone.
- Each word and each whitespace character produces 20 ms of silence, and every utterance lasts at least one second, because some programs (Jim Kitchen's games, for example) need speech to take time. Word, sentence and bookmark events are raised against the caller's text as SAPI expects.
- If the program interrupts speech (purge/abort), text still waiting for the clipboard is dropped; the next text is written normally.
- Each voice token declares its language (for example `C0A;A` for Spanish) and points at the same engine.

## Release

Bump `version` in `installer/Cargo.toml`, commit, then push a matching tag:

```
git tag v1.0.0
git push origin v1.0.0
```

`.github/workflows/release.yml` builds both DLLs and the installer on GitHub and attaches `ClipboardVoiceInstaller.exe`, a zip with the DLLs and scripts, and SHA-256 sums to the release. It can also be started by hand from the Actions tab for an existing tag.

## Contribute

### Prerequisites

- Visual Studio 2022 with C++ and ATL support (and the Windows SDK, which includes `sapi.h` and `sapiddk.h`)
- For the installer: Rust (stable). wxdragon builds wxWidgets with CMake and Ninja; both ship with Visual Studio.
- Administrator rights to install/register

### Build

- DLLs: open `ClipboardVoice/ClipboardVoice.sln` and build `Release | x64` and `Release | Win32`, or run `installer\build.bat`, which builds the DLLs and then the installer into `dist\ClipboardVoiceInstaller.exe`. The installer embeds the DLLs, so build them first.
- The DLLs link the C++ runtime statically, so they don't need the Visual C++ Redistributable.

### Output Paths

- x64: `ClipboardVoice/x64/Release/ClipboardVoice.dll`
- x86 (Win32): `ClipboardVoice/x86/Release/ClipboardVoice.dll`

### Repo Layout

- `ClipboardVoice/` Visual Studio solution and project
- `installer/` the Rust installer (`ClipboardVoiceInstaller.exe`)
- `scripts/` PowerShell install/uninstall scripts and their `.bat` launchers
- `test/` test scripts

### Tips

- Voice names, vendor and language handling: `ClipboardVoice/ClipboardVoice/ClipboardVoiceEngine.cpp` (registration section at the bottom).
- Pacing: `kMsPerStep` and `kMinUtteranceMs` in `ClipboardVoiceEngine.cpp`.

## Caveats

- Requires Administrator rights to install (writes HKLM and registers COM).
- Clipboard content is replaced by the spoken text.
- Both x86 and x64 are installed so 32‑bit and 64‑bit programs can use the voices.
- Windows Defender might complain about and delete the DLL. Add it to allowed threats.
- Programs that were already running may need a restart to see newly added voices. If a program has the DLL loaded during an update, the old copy is set aside and the program uses the new one after it restarts.

## Troubleshooting

- Voice not listed:
  - Check the registry: `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens\ClipboardVoice*` and `HKLM\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\ClipboardVoice*`.
  - Check the COM class: `HKCR\CLSID\{F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}` points to the installed DLL.
- Clipboard not changing:
  - Another program may hold the clipboard open; the voice keeps retrying until the next interruption.
  - Make sure antivirus did not block or remove the DLL (the installer reports "could not load the DLL" in that case).
- A language is rejected: it has no Windows language ID, so SAPI can't describe it. `ClipboardVoiceInstaller.exe --cli --list` shows every code that works.
