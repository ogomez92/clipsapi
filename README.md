# Clipboard Voice

Tired of NVDASapi?

Clipboard Voice is a minimal SAPI 5 TTS engine that writes the spoken text to the Windows clipboard instead of producing audio. It is implemented as a native ATL COM in‑proc DLL that implements `ISpTTSEngine`.

The same DLL also provides **Sapi2Prism**, a SAPI 5 voice that sends the text to your screen reader through [Prism](https://github.com/ethindp/prism) instead. It finds the running screen reader by itself (NVDA, JAWS, ZDSR, PC-Talker, Boy PC Reader, ZoomText, Narrator through UI Automation, and others), so a program that only knows SAPI talks through NVDA like a screen-reader-aware game would. You can install Clipboard Voice, Sapi2Prism or both.

It can register one voice per language, so programs that pick a voice by language (for example a Spanish game looking for a Spanish voice) find one. English keeps the original name, **Clipboard Voice** (or **Sapi2Prism**); other languages are called **Clipboard Voice (Spanish)**, **Sapi2Prism (French)**, and so on. Any language Windows knows that has a Windows language ID can be used, including regional variants such as `es-MX` or `en-GB`.

## Install

### Installer (recommended)

Download `ClipboardVoiceInstaller.exe` from the [latest release](https://github.com/ogomez92/clipsapi/releases/latest) and run it (it asks for administrator rights).

- Check the voices to install: **Clipboard Voice**, **Prism** (the Sapi2Prism voices) or both.
- Search the language list by name or code, check the languages you want (space bar), then press **Install**. Each chosen kind of voice gets every checked language.
- Run it again to change the voices or languages (**Apply changes**) or to uninstall.
- It also adds Clipboard Voice to Settings > Apps, where **Modify** reopens it and **Uninstall** removes everything.

Console mode, for scripts or if you prefer a terminal:

```
ClipboardVoiceInstaller.exe --cli                                   interactive menu
ClipboardVoiceInstaller.exe --cli --install en,es                   install exactly these languages
ClipboardVoiceInstaller.exe --cli --install en,es --output prism    ... as Sapi2Prism voices (clipboard, prism or both;
                                                                    default: the kinds installed now, else clipboard)
ClipboardVoiceInstaller.exe --cli --uninstall                       remove everything
ClipboardVoiceInstaller.exe --cli --list                            list every language code you can use
add --log <file> to also write the output to a file
```

### Scripts

The repository includes the built DLLs. From the repository folder:

```
scripts\install.bat en es
```

Pass any language codes (`en`, `es`, `fr`, `es-MX`, `en-GB`, ...); with none it installs English only. Add `-Output prism` for Sapi2Prism voices or `-Output both` for both kinds (default `clipboard`). The script asks for administrator rights itself, installs both the 64-bit and 32-bit voices, and checks that every voice loads in both. Running it again replaces the installed DLLs and the set of voices.

PowerShell directly: `powershell -ExecutionPolicy Bypass -File scripts\install.ps1 en es -Output both`. To use DLLs from elsewhere: `scripts\install.ps1 en es -X64Dll path\x64.dll -X86Dll path\x86.dll`.

Uninstall: `scripts\uninstall.bat`.

### By hand

```
regsvr32 /i:en,es "C:\Program Files\ClipboardVoice\ClipboardVoice.dll"
regsvr32 /i:clipboard:en,es;prism:en,es "C:\Program Files\ClipboardVoice\ClipboardVoice.dll"
regsvr32 /i:prism:en "C:\Program Files\ClipboardVoice\ClipboardVoice.dll"
```

`regsvr32 dll` alone registers both engines and, if none of our voices exists yet, the English Clipboard Voice. `/i:<languages>` replaces the set of voices: plain languages are Clipboard Voices, and `clipboard:` / `prism:` choose the kind for the languages after them. `regsvr32 /u dll` removes the engines and all their voices. Use `C:\Windows\SysWOW64\regsvr32.exe` for the 32-bit DLL. Sapi2Prism needs `prism.dll` of the same architecture (from `vendor\prism\x64` or `vendor\prism\x86`) next to `ClipboardVoice.dll`.

## Test

```
powershell -ExecutionPolicy Bypass -File test\say.ps1 "This will land on the clipboard."
powershell -ExecutionPolicy Bypass -File test\say.ps1 "Esto va al portapapeles." -Voice "Clipboard Voice (Spanish)"
```

The text should be on the clipboard afterward. For Sapi2Prism, `test\say.ps1 "Hello" -Voice Sapi2Prism` should be spoken by your screen reader. To see what Sapi2Prism does (which screen reader it picked, every text sent and the result), set the environment variable `SAPI2PRISM_LOG` to a file path before starting the program.

`test\eventdump.cpp` is a small console program that speaks with a voice and prints every SAPI event it receives (word and sentence boundaries, bookmarks, end of stream) plus the clipboard contents. Build instructions are at the top of the file.

## How It Works

- `ISpTTSEngine::Speak` joins the text fragments and a background thread writes them to the clipboard as UTF‑16, retrying while another program has the clipboard open. Empty or blank requests leave the clipboard alone.
- Each word and each whitespace character produces 20 ms of silence, and every utterance lasts at least one second, because some programs (Jim Kitchen's games, for example) need speech to take time. Word, sentence and bookmark events are raised against the caller's text as SAPI expects.
- If the program interrupts speech (purge/abort), text still waiting for the clipboard is dropped; the next text is written normally.
- Each voice token declares its language (for example `C0A;A` for Spanish) and points at its engine: one COM class for Clipboard Voice and one for Sapi2Prism, both in `ClipboardVoice.dll`.

### Sapi2Prism

- The text goes to the screen reader that is running, through `prism.dll` (loaded on first use from the folder of `ClipboardVoice.dll`). It looks again for a screen reader at most once a second, so starting, closing or switching screen readers while a program runs works, and it reconnects when NVDA is restarted. Braille displays get the text too.
- Prism's own voices (SAPI, OneCore) are never used, so with no screen reader running the voice stays silent instead of talking over nothing (or calling itself).
- Interrupting: when a program cuts speech (purge), the screen reader is stopped. SAPI does not tell an engine about purges directly, so a text that arrives right after the previous one finished is queued after it (the next item of SAPI's queue), and any other text interrupts what the screen reader is saying. An empty purge request stops the screen reader.
- With screen readers that report whether they are speaking (current NVDA), each utterance lasts as long as the screen reader really speaks it, so programs that wait for speech to end (`WaitUntilDone`, synchronous `Speak`, end-of-stream events) wait the right time. With others (JAWS, Narrator, ...) the pacing is the same as Clipboard Voice's.
- Rate, pitch and volume changes from the program are ignored: they would change your screen reader's own settings.

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

- DLLs: open `ClipboardVoice/ClipboardVoice.sln` and build `Release | x64` and `Release | Win32`, or run `installer\build.bat`, which builds the DLLs and then the installer into `dist\ClipboardVoiceInstaller.exe`. The installer embeds the DLLs and `vendor\prism\*\prism.dll`, so build them first.
- The DLLs (ours and Prism's) link the C++ runtime statically, so they don't need the Visual C++ Redistributable.
- Prism is vendored in `vendor/prism` (see its README). The 32-bit `prism.dll` is built from source with `vendor\prism\build-x86.bat`, which needs Visual Studio 2026; the rest of the project builds with Visual Studio 2022.

### Output Paths

- x64: `ClipboardVoice/x64/Release/ClipboardVoice.dll`
- x86 (Win32): `ClipboardVoice/x86/Release/ClipboardVoice.dll`

### Repo Layout

- `ClipboardVoice/` Visual Studio solution and project
- `installer/` the Rust installer (`ClipboardVoiceInstaller.exe`)
- `scripts/` PowerShell install/uninstall scripts and their `.bat` launchers
- `test/` test scripts
- `vendor/prism/` Prism's header, `prism.dll` for x64 and x86, and its licences

### Tips

- Voice names, vendor and language handling: `ClipboardVoice/ClipboardVoice/Registration.cpp`.
- Pacing: `kMsPerStep` and `kMinUtteranceMs` in `ClipboardVoiceEngine.cpp`.
- Sapi2Prism (screen reader choice, interrupting, waiting for the screen reader): `ClipboardVoice/ClipboardVoice/PrismOutput.cpp`.

## Caveats

- Requires Administrator rights to install (writes HKLM and registers COM).
- Clipboard content is replaced by the spoken text (Clipboard Voice only).
- Prism has no official 32-bit Windows build; the vendored one is built from Prism's source and is less tested than the 64-bit one.
- Both x86 and x64 are installed so 32‑bit and 64‑bit programs can use the voices.
- Windows Defender might complain about and delete the DLL. Add it to allowed threats.
- Programs that were already running may need a restart to see newly added voices. If a program has the DLL loaded during an update, the old copy is set aside and the program uses the new one after it restarts.

## Troubleshooting

- Voice not listed:
  - Check the registry: `HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens\ClipboardVoice*` (or `Sapi2Prism*`) and the same under `HKLM\SOFTWARE\WOW6432Node\`.
  - Check the COM classes: `HKCR\CLSID\{F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}` (Clipboard Voice) and `HKCR\CLSID\{8D3B6C1E-2F47-4A5D-9B0E-6C1F2A7D4E95}` (Sapi2Prism) point to the installed DLL.
- Sapi2Prism silent: make sure a screen reader is running and `prism.dll` sits next to `ClipboardVoice.dll`, then set `SAPI2PRISM_LOG` (see Test) and read the log.
- Clipboard not changing:
  - Another program may hold the clipboard open; the voice keeps retrying until the next interruption.
  - Make sure antivirus did not block or remove the DLL (the installer reports "could not load the DLL" in that case).
- A language is rejected: it has no Windows language ID, so SAPI can't describe it. `ClipboardVoiceInstaller.exe --cli --list` shows every code that works.
