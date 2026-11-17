@echo off
setlocal
rem Builds the voice DLLs and the installer; writes dist\ClipboardVoiceInstaller.exe.
pushd "%~dp0.." || exit /b 1

set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
rem Ninja ships with Visual Studio 2022; wxdragon's build needs it on PATH.
set "PATH=%PATH%;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

echo === Building the voice DLLs ===
"%MSBUILD%" ClipboardVoice\ClipboardVoice.sln /t:Build /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal || goto :fail
"%MSBUILD%" ClipboardVoice\ClipboardVoice.sln /t:Build /p:Configuration=Release /p:Platform=Win32 /m /nologo /v:minimal || goto :fail

echo === Building the installer ===
pushd installer || goto :fail
cargo build --release || (popd & goto :fail)
popd
if not exist dist mkdir dist
copy /y installer\target\release\ClipboardVoiceInstaller.exe dist\ClipboardVoiceInstaller.exe >nul || goto :fail
echo Built %CD%\dist\ClipboardVoiceInstaller.exe
popd
exit /b 0

:fail
set "exit_code=%errorlevel%"
popd
exit /b %exit_code%
