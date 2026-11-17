@echo off
setlocal
REM Clipboard Voice uninstaller. Runs uninstall.ps1, which asks for admin rights itself.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
set "RC=%errorlevel%"
echo %cmdcmdline% | find /i "%~nx0" >nul && pause
exit /b %RC%
