@echo off
:: Launch the CEF weave host forced-IPC (#625). The weave service is IPC-only, so
:: XRT_FORCE_MODE=ipc must be set process-level (the runtime DLL has its own
:: static-CRT env block). displayxr-service must already be running.
::
:: From an ELEVATED shell, launch this at MEDIUM integrity (to match the
:: non-elevated service, else the DUP_HANDLE handoff is access-denied) via:
::     explorer.exe "%~dp0run.bat"
set "XRT_FORCE_MODE=ipc"
set "DIR=%~dp0..\build\Release"
start "" "%DIR%\displayxr_cef_host.exe" %*
