@echo off
setlocal
REM Clipboard Voice installer. Runs install.ps1, which asks for admin rights itself.
REM Usage: install.bat [languages...]
REM   install.bat            English only
REM   install.bat en es      English and Spanish
REM   install.bat en-GB es-MX
REM   install.bat en es -Output prism    Sapi2Prism voices (speak through the screen reader)
REM   install.bat en es -Output both     Clipboard Voice and Sapi2Prism voices
REM Advanced: install.bat en es -X64Dll "path\to\x64.dll" -X86Dll "path\to\x86.dll"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set "RC=%errorlevel%"
REM Keep the window open when started by double-click from Explorer.
echo %cmdcmdline% | find /i "%~nx0" >nul && pause
exit /b %RC%
