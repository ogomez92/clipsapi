@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Clipboard Voice uninstaller (.bat)
REM Unregisters x86/x64 COM DLLs (if present) and removes install folders.

REM --- Admin check ---
net session >nul 2>&1
if not %errorlevel%==0 (
  echo [ERROR] Please run this uninstaller as Administrator.
  pause
  exit /b 1
)

REM --- Paths and tools ---
set "IS_X64_OS=0"
if defined ProgramFiles(x86) set "IS_X64_OS=1"

set "INSTALL_DIR64=%ProgramFiles%\ClipboardVoice"
if "%IS_X64_OS%"=="1" (
  set "INSTALL_DIR32=%ProgramFiles(x86)%\ClipboardVoice"
  set "REGSVR32_32=%SystemRoot%\SysWOW64\regsvr32.exe"
  set "REGSVR32_64=%SystemRoot%\System32\regsvr32.exe"
)
if not "%IS_X64_OS%"=="1" (
  set "INSTALL_DIR32=%ProgramFiles%\ClipboardVoice"
  set "REGSVR32_32=%SystemRoot%\System32\regsvr32.exe"
  set "REGSVR32_64="
)

set ERRORS=0

REM --- Unregister ---
set "DLL32=%INSTALL_DIR32%\ClipboardVoice.dll"
if exist "%DLL32%" (
  echo [INFO] Unregistering x86 DLL...
  "%REGSVR32_32%" /s /u "%DLL32%"
  if not %errorlevel%==0 (
    echo [WARN] regsvr32 ^(x86^) returned %errorlevel%.
    set /a ERRORS+=1
  )
)

if "%IS_X64_OS%"=="1" (
  set "DLL64=%INSTALL_DIR64%\ClipboardVoice.dll"
  if exist "%DLL64%" (
    echo [INFO] Unregistering x64 DLL...
    "%REGSVR32_64%" /s /u "%DLL64%"
    if not %errorlevel%==0 (
      echo [WARN] regsvr32 ^(x64^) returned %errorlevel%.
      set /a ERRORS+=1
    )
  )
)

REM --- Remove folders ---
echo [INFO] Removing install folders...
if exist "%INSTALL_DIR32%" rmdir /S /Q "%INSTALL_DIR32%" 2>nul
if "%IS_X64_OS%"=="1" if exist "%INSTALL_DIR64%" rmdir /S /Q "%INSTALL_DIR64%" 2>nul

echo.
if %ERRORS% NEQ 0 (
  echo [DONE] Uninstall finished with %ERRORS% warning/error^(s^). Some registry entries may remain.
  pause
  exit /b 1
)

echo [SUCCESS] Clipboard Voice uninstalled.
pause
exit /b 0
