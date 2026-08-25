@echo off
setlocal
title GTA San Andreas - Export Google Play APK Set
cd /d "%~dp0"

echo ================================================================
echo  GTA SAN ANDREAS - EXPORT YOUR GOOGLE PLAY APK SET
echo ================================================================
echo.
echo Connect the Android phone or tablet where your Google Play copy of
echo GTA San Andreas 2.11.311 is installed, then approve USB debugging.
echo Seeing the phone in Windows File Explorer is not enough by itself.
echo.

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\export-play-apks.ps1"
set "RESULT=%ERRORLEVEL%"

echo.
if "%RESULT%"=="0" (
  echo EXPORT COMPLETED.
) else (
  echo EXPORT FAILED. Read the error above.
)
echo.
pause
exit /b %RESULT%
