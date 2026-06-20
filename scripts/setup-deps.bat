@echo off
setlocal enabledelayedexpansion
:: ============================================================
:: displayxr-cef-host dependency setup (#625, Step A)
::
:: Downloads + extracts the two binary deps the host links against:
::   1. CEF (Chromium Embedded Framework) binary distribution, windows64
::      "standard" build (libcef.dll + libcef_dll_wrapper sources + payload).
::   2. The OpenXR loader (reuses the runtime repo's short-path copy if present).
::
:: Both land under short, space-free paths (C:\dev\...) to dodge the MSVC
:: spaces-in-path linker bug (this box's GitHub clone path contains a space) --
:: same idiom as the runtime's scripts\build_windows.bat.
::
:: Idempotent: re-running skips anything already present.
:: ============================================================

:: --- Pinned CEF build (latest stable as of 2026-06; Chromium 149) ----------
:: Reproducible pin. To bump: pick a newer "standard" windows64 build from
::   https://cef-builds.spotifycdn.com/index.json  (j.windows64.versions[].files[type=standard])
:: and update CEF_VER + CEF_ARCHIVE + CEF_SHA1 together.
set "CEF_VER=149.0.4+g2f1bfd8+chromium-149.0.7827.156"
set "CEF_ARCHIVE=cef_binary_%CEF_VER%_windows64.tar.bz2"
set "CEF_SHA1=90bbfdd4d6da76c6a96865e3d4f1b88e5302a1dc"
:: URL needs the '+' chars percent-encoded as %%2B.
set "CEF_URL=https://cef-builds.spotifycdn.com/cef_binary_149.0.4%%2Bg2f1bfd8%%2Bchromium-149.0.7827.156_windows64.tar.bz2"
:: Short, space-free, stable extraction root (top dir stripped on extract).
set "CEF_ROOT=C:\dev\cef\149.0.4"

:: --- OpenXR loader (match the runtime's pinned version + short path) --------
set "OPENXR_VERSION=1.1.43"
set "OPENXR_SDK_SHORT=C:\dev\openxr_sdk_%OPENXR_VERSION%"
set "OPENXR_URL=https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-%OPENXR_VERSION%/openxr_loader_windows-%OPENXR_VERSION%.zip"

echo === displayxr-cef-host dependency setup ===

:: ------------------------------------------------------------------
:: 1. CEF
:: ------------------------------------------------------------------
if exist "%CEF_ROOT%\libcef_dll\CMakeLists.txt" (
    echo [CEF] already present at %CEF_ROOT%
) else (
    echo [CEF] downloading %CEF_ARCHIVE% ^(~340 MB^)...
    if not exist "%TEMP%\%CEF_ARCHIVE%" (
        powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '%CEF_URL%' -OutFile '%TEMP%\%CEF_ARCHIVE%'"
        if errorlevel 1 ( echo ERROR: CEF download failed & exit /b 1 )
    )
    echo [CEF] verifying sha1...
    for /f "usebackq delims=" %%H in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA1 '%TEMP%\%CEF_ARCHIVE%').Hash.ToLower()"`) do set "GOTSHA=%%H"
    if /i not "!GOTSHA!"=="%CEF_SHA1%" (
        echo ERROR: CEF sha1 mismatch ^(got !GOTSHA!, want %CEF_SHA1%^). Delete %TEMP%\%CEF_ARCHIVE% and retry.
        exit /b 1
    )
    echo [CEF] extracting to %CEF_ROOT% ...
    if not exist "%CEF_ROOT%" mkdir "%CEF_ROOT%"
    :: bsdtar (Windows tar.exe) autodetects bzip2; strip the top dir.
    tar -xf "%TEMP%\%CEF_ARCHIVE%" -C "%CEF_ROOT%" --strip-components=1
    if errorlevel 1 ( echo ERROR: CEF extract failed & exit /b 1 )
    del "%TEMP%\%CEF_ARCHIVE%" 2>nul
    echo [CEF] ready.
)

:: ------------------------------------------------------------------
:: 2. OpenXR loader
:: ------------------------------------------------------------------
if exist "%OPENXR_SDK_SHORT%\x64\lib\openxr_loader.lib" (
    echo [OpenXR] already present at %OPENXR_SDK_SHORT%
) else (
    echo [OpenXR] downloading loader %OPENXR_VERSION% ...
    powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '%OPENXR_URL%' -OutFile '%TEMP%\openxr_loader.zip'"
    if errorlevel 1 ( echo ERROR: OpenXR download failed & exit /b 1 )
    if not exist "%OPENXR_SDK_SHORT%" mkdir "%OPENXR_SDK_SHORT%"
    powershell -NoProfile -Command "Expand-Archive -Path '%TEMP%\openxr_loader.zip' -DestinationPath '%OPENXR_SDK_SHORT%' -Force"
    del "%TEMP%\openxr_loader.zip" 2>nul
    echo [OpenXR] ready.
)

echo.
echo === Dependencies ready ===
echo   CEF_ROOT=%CEF_ROOT%
echo   OPENXR_SDK_SHORT=%OPENXR_SDK_SHORT%
echo Next: scripts\build.bat
endlocal
exit /b 0
