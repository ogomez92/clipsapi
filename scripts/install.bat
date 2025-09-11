@echo off
setlocal EnableExtensions

REM Clipboard Voice installer (.bat)
REM Usage: install.bat ["path\to\x64\ClipboardVoice.dll" "path\to\x86\ClipboardVoice.dll"]
REM If no args are provided, attempts to auto-detect from build output:
REM   ..\ClipboardVoice\x64\Release\ClipboardVoice.dll
REM   ..\ClipboardVoice\x86\Release\ClipboardVoice.dll (fallback: ..\ClipboardVoice\Release\ClipboardVoice.dll)

REM --- Admin check ---
net session >nul 2>&1
if not %errorlevel%==0 (
  echo [ERROR] Please run this installer as Administrator.
  pause
  exit /b 1
)

REM --- Args ---
set "SRC_X64=%~1"
set "SRC_X86=%~2"

REM Auto-detect defaults if not provided
set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%SCRIPT_DIR%.."
for %%F in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fF"
echo [DEBUG] SCRIPT_DIR=%SCRIPT_DIR%
echo [DEBUG] REPO_ROOT=%REPO_ROOT%
if "%SRC_X64%"=="" set "SRC_X64=%REPO_ROOT%\ClipboardVoice\x64\Release\ClipboardVoice.dll"
if "%SRC_X86%"=="" set "SRC_X86=%REPO_ROOT%\ClipboardVoice\x86\Release\ClipboardVoice.dll"
if not exist "%SRC_X86%" set "SRC_X86=%REPO_ROOT%\ClipboardVoice\Release\ClipboardVoice.dll"
REM Resolve short forms for sources to avoid special chars
set "SRC_X64_S=%SRC_X64%"
set "SRC_X86_S=%SRC_X86%"
for %%P in ("%SRC_X64%") do if not "%%~sP"=="" set "SRC_X64_S=%%~sP"
for %%P in ("%SRC_X86%") do if not "%%~sP"=="" set "SRC_X86_S=%%~sP"
echo [DEBUG] SRC_X64=%SRC_X64%
echo [DEBUG] SRC_X64_S=%SRC_X64_S%
echo [DEBUG] SRC_X86=%SRC_X86%
echo [DEBUG] SRC_X86_S=%SRC_X86_S%

if "%SRC_X64%"=="" (
  echo Usage: %~n0 ["^<x64 dll^>" "^<x86 dll^>"]
  echo Auto-detection failed to set defaults.
  pause
  exit /b 2
)

echo [DEBUG] Checking x64 DLL exists...
if not exist "%SRC_X64%" (
  echo [ERROR] x64 DLL not found: %SRC_X64%
  pause
  exit /b 3
)
echo [DEBUG] Checking x86 DLL exists...
if not exist "%SRC_X86%" (
  echo [ERROR] x86 DLL not found: %SRC_X86%
  pause
  exit /b 3
)

REM --- Detect OS bitness ---
set "IS_X64_OS=0"
if defined ProgramFiles(x86) set "IS_X64_OS=1"
echo [DEBUG] IS_X64_OS=%IS_X64_OS%

REM --- Install paths ---
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
echo [DEBUG] INSTALL_DIR32=%INSTALL_DIR32%
echo [DEBUG] INSTALL_DIR64=%INSTALL_DIR64%
echo [DEBUG] REGSVR32_32=%REGSVR32_32%
echo [DEBUG] REGSVR32_64=%REGSVR32_64%

REM Resolve short (8.3) paths to avoid parentheses parsing issues
set "INSTALL_DIR32_S=%INSTALL_DIR32%"
set "INSTALL_DIR64_S=%INSTALL_DIR64%"
for %%P in ("%INSTALL_DIR32%") do (
  if not "%%~sP"=="" set "INSTALL_DIR32_S=%%~sP"
)
for %%P in ("%INSTALL_DIR64%") do (
  if not "%%~sP"=="" set "INSTALL_DIR64_S=%%~sP"
)
echo [DEBUG] INSTALL_DIR32_S=%INSTALL_DIR32_S%
echo [DEBUG] INSTALL_DIR64_S=%INSTALL_DIR64_S%

set ERRORS=0

echo.
echo [INFO] Installing to:
echo        x86: "%INSTALL_DIR32%"
if "%IS_X64_OS%"=="1" echo        x64: "%INSTALL_DIR64%"
echo.

REM --- Create directories ---
echo [DEBUG] Creating install directories if needed...
if not exist "%INSTALL_DIR32%" mkdir "%INSTALL_DIR32%" 2>nul
if "%IS_X64_OS%"=="1" if not exist "%INSTALL_DIR64%" mkdir "%INSTALL_DIR64%" 2>nul

REM --- Copy DLLs ---
echo [INFO] Copying x86 DLL...
echo [DEBUG] copy /Y "%SRC_X86_S%" "%INSTALL_DIR32_S%\ClipboardVoice.dll"
copy /Y "%SRC_X86_S%" "%INSTALL_DIR32_S%\ClipboardVoice.dll" >nul
if not %errorlevel%==0 (
  echo [ERROR] Failed to copy x86 DLL to "%INSTALL_DIR32%" ^(error %errorlevel%^).
  set /a ERRORS+=1
)

if "%IS_X64_OS%"=="1" goto :CopyX64
goto :PostCopyX64

:CopyX64
echo [INFO] Copying x64 DLL...
echo [DEBUG] copy /Y "%SRC_X64_S%" "%INSTALL_DIR64_S%\ClipboardVoice.dll"
copy /Y "%SRC_X64_S%" "%INSTALL_DIR64_S%\ClipboardVoice.dll" >nul
if errorlevel 1 echo [ERROR] Failed to copy x64 DLL to "%INSTALL_DIR64%" ^(error %errorlevel%^).
if errorlevel 1 set /a ERRORS+=1
:PostCopyX64

REM --- Register (COM) ---
set "DLL32=%INSTALL_DIR32%\ClipboardVoice.dll"
set "DLL64=%INSTALL_DIR64%\ClipboardVoice.dll"
for %%F in ("%DLL32%") do set "DLL32=%%~fF"
if "%IS_X64_OS%"=="1" for %%F in ("%DLL64%") do set "DLL64=%%~fF"

echo [INFO] Registering x86 DLL...
echo [DEBUG] "%REGSVR32_32%" "%DLL32%"
if exist "%DLL32%" (
  "%REGSVR32_32%" "%DLL32%"
  if not %errorlevel%==0 (
    echo [ERROR] regsvr32 ^(x86^) failed with code %errorlevel%.
    set /a ERRORS+=1
  )
) else (
  echo [ERROR] x86 DLL not found at "%DLL32%" ^- skipping registration.
  set /a ERRORS+=1
)

if "%IS_X64_OS%"=="1" goto :RegX64
goto :PostRegX64

:RegX64
echo [INFO] Registering x64 DLL...
echo [DEBUG] "%REGSVR32_64%" "%DLL64%"
if exist "%DLL64%" (
  "%REGSVR32_64%" "%DLL64%"
  if errorlevel 1 echo [ERROR] regsvr32 ^(x64^) failed with code %errorlevel%.
  if errorlevel 1 set /a ERRORS+=1
) else (
  echo [ERROR] x64 DLL not found at "%DLL64%" ^- skipping registration.
  set /a ERRORS+=1
)
:PostRegX64

echo.
if %ERRORS% NEQ 0 (
  echo [FAIL] Installation finished with %ERRORS% error^(s^).
  echo        Check the messages above. Ensure you ran as Administrator.
  pause
  exit /b 1
)

echo [SUCCESS] Clipboard Voice installed and registered.
echo           The voice should appear as "Clipboard Voice" in apps matching installed bitness.
echo           You can test with: powershell -Command ^".^\test^\say.ps1 'Hello' ^"
pause
exit /b 0
