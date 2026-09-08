@echo off
setlocal
title Disable SAVR Render Diagnostics
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\collect-render-report.ps1" -DisableDiagnostics %*
set "SAVR_DIAG_EXIT=%ERRORLEVEL%"
echo.
if not "%SAVR_DIAG_EXIT%"=="0" echo Disabling failed. Read the message above and check the USB connection.
pause
endlocal & exit /b %SAVR_DIAG_EXIT%
