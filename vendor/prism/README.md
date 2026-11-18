# Prism (vendored)

[Prism](https://github.com/ethindp/prism) 0.18.3 by Ethin Probst and contributors, MPL-2.0.
The Sapi2Prism voices load `prism.dll` from the folder of `ClipboardVoice.dll` and speak
through it to the running screen reader. NVDA support is built into `prism.dll`, so no
`nvdaControllerClient.dll` is needed.

| File | Source |
| --- | --- |
| `include/prism.h`, `include/prism_version.h` | official `prism-windows-x64.zip` release v0.18.3 |
| `x64/prism.dll` | official `prism-windows-x64.zip` release v0.18.3 (`dynamic/release/bin`) |
| `x86/prism.dll` | built from tag v0.18.3 with `build-x86.bat` (Prism ships no 32-bit Windows build) |
| `LICENSES/`, `NOTICE` | from the same release |

SHA-256:

- `x64/prism.dll`: `7C7D09C8C7306E8E1A0C46D603CCB2A391C194C77843400C11B746E7DD56A467`
- `x86/prism.dll`: `15EEE4A6A1DCE9458958A8586A8872AAD2D76D2353B7FCA569124FF59643CC6E`
  (built with Visual Studio 2026; Prism needs C23, which Visual Studio 2022's compiler lacks)

Both DLLs link the C runtime statically. The 32-bit build is not an official Prism
build; Prism's maintainer considers 32-bit unsupported, so report problems with it here
first.

To update: replace the x64 files from a newer release, run `build-x86.bat` after changing
its `TAG`, and check that `PrismOutput.cpp` still matches `prism.h` (the header's structs
and error codes change between releases).
