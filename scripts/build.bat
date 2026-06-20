@echo off
setlocal enabledelayedexpansion
:: ============================================================
:: displayxr-cef-host build (#625, Step A)
::
:: Configures + builds the CEF OSR weave host. Run scripts\setup-deps.bat
:: first (downloads CEF + the OpenXR loader to C:\dev\...).
::
:: Usage: scripts\build.bat [generate]
::   (no arg) - configure (if needed) + build
::   generate - CMake configure only
:: ============================================================

set "REPO=%~dp0.."
:: Forward slashes: CEF's cmake macros treat backslashes in CEF_ROOT as escapes.
set "CEF_ROOT=C:/dev/cef/149.0.4"
set "OPENXR_SDK_SHORT=C:/dev/openxr_sdk_1.1.43"
set "BUILD_DIR=%REPO%\build"

:: --- deps present? ---
if not exist "%CEF_ROOT%\libcef_dll\CMakeLists.txt" (
    echo ERROR: CEF not found at %CEF_ROOT%. Run scripts\setup-deps.bat first.
    exit /b 1
)
if not exist "%OPENXR_SDK_SHORT%\x64\lib\openxr_loader.lib" (
    echo ERROR: OpenXR loader not found at %OPENXR_SDK_SHORT%. Run scripts\setup-deps.bat first.
    exit /b 1
)

:: --- MSVC environment (vswhere, any edition) ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%i"
)
set "VCVARS="
if defined VS_INSTALL if exist "%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS (
    echo ERROR: Visual Studio 2022 with the C++ workload not found.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1

:: --- Ninja on PATH (winget location, same as the runtime build) ---
set "NINJA_DIR=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe"
if exist "%NINJA_DIR%\ninja.exe" set "PATH=%NINJA_DIR%;%PATH%"
where ninja >nul 2>&1
if errorlevel 1 ( echo ERROR: ninja not found. Install via: winget install Ninja-build.Ninja & exit /b 1 )

:: --- configure ---
if not exist "%BUILD_DIR%\build.ninja" (
    echo === Configuring ^(Ninja, Release^) ===
    cmake -S "%REPO%" -B "%BUILD_DIR%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCEF_ROOT="%CEF_ROOT%" ^
        -DOpenXR_ROOT="%OPENXR_SDK_SHORT%"
    if errorlevel 1 ( echo ERROR: CMake configure failed & exit /b 1 )
)

if /i "%~1"=="generate" (
    echo === Configure complete ===
    exit /b 0
)

:: --- build ---
echo === Building ===
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 ( echo ERROR: build failed & exit /b 1 )

echo.
echo === Build complete ===
echo   Exe + CEF payload: %BUILD_DIR%
endlocal
exit /b 0
