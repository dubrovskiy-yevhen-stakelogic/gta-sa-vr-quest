@echo off
setlocal EnableExtensions

rem A double-clicked CMD window normally disappears on an early PowerShell
rem failure. Re-enter through cmd /k once so the result always stays visible.
if not defined SAVR_KEEP_OPEN (
  set "SAVR_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title GTA San Andreas VR - Personal Quest Builder
echo.
echo ============================================================
echo   GTA SAN ANDREAS VR - PERSONAL BUILD AND INSTALL WIZARD
echo ============================================================
echo.
echo No APK, GTA data, or audio mod is included in this source kit.
echo.

set "SAVR_MASTER=%~dp0tools\build-and-install.ps1"
if not exist "%SAVR_MASTER%" (
  echo ERROR: The source kit is incomplete. Missing:
  echo   %SAVR_MASTER%
  set "SAVR_EXIT=1"
  goto :finished
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "SAVR_EXIT=1"
  goto :finished
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SAVR_MASTER%" %*
set "SAVR_EXIT=%ERRORLEVEL%"

:finished
echo.
if not "%SAVR_EXIT%"=="0" (
  echo FAILED. Read the reported error and diagnostic log above.
) else (
  echo SUCCESS. Every requested build/install step completed.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %SAVR_EXIT%
