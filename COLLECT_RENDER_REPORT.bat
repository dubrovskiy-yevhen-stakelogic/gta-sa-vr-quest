@echo off
setlocal
title SAVR Render Report
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\collect-render-report.ps1" %*
set "SAVR_REPORT_EXIT=%ERRORLEVEL%"
echo.
if not "%SAVR_REPORT_EXIT%"=="0" echo Collection failed. Read the message above and check the USB connection.
pause
endlocal & exit /b %SAVR_REPORT_EXIT%
