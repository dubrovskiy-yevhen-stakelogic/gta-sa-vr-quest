@echo off
rem One-click HD weapon models: builds the payload from a downloaded pack and
rem pushes it to your connected Quest. Run BUILD_AND_INSTALL.bat once first so
rem Python and adb are available. Then double-click this and drag the pack in
rem when asked -- either the .zip/.7z archive OR an already-extracted folder.
setlocal
cd /d "%~dp0"
where powershell.exe >nul 2>nul
if errorlevel 1 (
    echo PowerShell was not found. This installer needs Windows PowerShell.
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install-hdweapons.ps1" %*
echo(
pause
