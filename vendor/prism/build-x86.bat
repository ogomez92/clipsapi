@echo off
setlocal
rem Builds the 32-bit prism.dll (Prism has no official 32-bit Windows release)
rem from the tagged source and copies it to vendor\prism\x86\prism.dll.
rem Needs git and Visual Studio 2026 (18) with C++: Prism is C23, which the
rem Visual Studio 2022 compiler doesn't support. CMake and Ninja ship with it.
rem Usage: vendor\prism\build-x86.bat [work folder]   (default: %TEMP%\prism-x86-build)

set "TAG=v0.18.3"
set "HERE=%~dp0"
set "WORK=%~1"
if "%WORK%"=="" set "WORK=%TEMP%\prism-x86-build"
set "VS="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio with C++ not found & exit /b 1)
echo Using %VS%

call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 || exit /b 1
rem Visual Studio's own CMake and Ninja, so the 32-bit MSVC librarian is used.
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

if not exist "%WORK%\src\.git" (
    git clone --depth 1 --branch %TAG% https://github.com/ethindp/prism.git "%WORK%\src" || exit /b 1
)

rem Same options as Prism's official Windows release, minus the compatibility shims.
cmake -S "%WORK%\src" -B "%WORK%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON ^
    -DPRISM_ENABLE_TESTS=OFF -DPRISM_ENABLE_DEMOS=OFF -DPRISM_ENABLE_SHIMS=OFF ^
    -DPRISM_ENABLE_LEGACY_BACKENDS=ON -DPRISM_ENABLE_GDEXTENSION=OFF || exit /b 1
cmake --build "%WORK%\build" || exit /b 1
cmake --install "%WORK%\build" --prefix "%WORK%\dist" --config Release || exit /b 1

if not exist "%HERE%x86" mkdir "%HERE%x86"
copy /y "%WORK%\dist\bin\prism.dll" "%HERE%x86\prism.dll" >nul || exit /b 1
echo Built %HERE%x86\prism.dll from Prism %TAG%
exit /b 0
