@echo off
setlocal EnableExtensions

rem Keep a double-clicked window open so success and errors remain visible.
if not defined SAVR_RESET_KEEP_OPEN (
  set "SAVR_RESET_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title GTA San Andreas VR - Reset VR Settings
echo.
echo ============================================================
echo   GTA SAN ANDREAS VR - RESET HEADSET VR SETTINGS
echo ============================================================
echo.
echo This removes only the mod's VR configuration files.
echo Saves, audio, game data, hand assets, and the APK are preserved.
echo.

set "SAVR_RESET=%~dp0tools\reset-vr-settings.ps1"
if not exist "%SAVR_RESET%" (
  echo ERROR: The source kit is incomplete. Missing:
  echo   %SAVR_RESET%
  set "SAVR_EXIT=1"
  goto :finished
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "SAVR_EXIT=1"
  goto :finished
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SAVR_RESET%" %*
set "SAVR_EXIT=%ERRORLEVEL%"

:finished
echo.
if not "%SAVR_EXIT%"=="0" (
  echo FAILED. No unreported files were removed.
) else (
  echo FINISHED. The game was not launched.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %SAVR_EXIT%
