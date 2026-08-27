@echo off
setlocal enabledelayedexpansion
title GTA San Andreas VR - Get and Install

rem ============================================================================
rem  One-click bootstrap: installs a private copy of Git if needed, clones a
rem  FRESH copy of the mod source next to this file, then runs the normal
rem  BUILD_AND_INSTALL wizard. Re-running updates the clone instead of leaving
rem  people to hand-merge ZIPs (which is how stale/mixed files creep in).
rem
rem  Put this .bat in an EMPTY folder and double-click it. Nothing is installed
rem  system-wide and no admin rights are needed.
rem ============================================================================

cd /d "%~dp0"

set "REPO_URL=https://github.com/dubrovskiy-yevhen-stakelogic/gta-sa-vr-quest.git"
set "REPO_DIR=%~dp0gta-sa-vr-quest"
set "TOOLS_DIR=%~dp0.tools"
set "GIT_VER=2.47.1"
set "GIT_URL=https://github.com/git-for-windows/git/releases/download/v%GIT_VER%.windows.1/PortableGit-%GIT_VER%-64-bit.7z.exe"
set "GIT_EXE=git"

echo(
echo === GTA San Andreas VR - Get ^& Install ===
echo(

rem --- 1. Make sure we have git ------------------------------------------------
where git >nul 2>nul
if %errorlevel%==0 (
    echo [1/3] Git found on this PC.
) else (
    if exist "%TOOLS_DIR%\PortableGit\cmd\git.exe" (
        echo [1/3] Using the private Git copy from a previous run.
        set "GIT_EXE=%TOOLS_DIR%\PortableGit\cmd\git.exe"
    ) else (
        echo [1/3] Git is not installed. Downloading a private copy ^(no admin needed^)...
        if not exist "%TOOLS_DIR%" mkdir "%TOOLS_DIR%"
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
            "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; try { Invoke-WebRequest -Uri '%GIT_URL%' -OutFile '%TOOLS_DIR%\PortableGit.7z.exe' } catch { Write-Host $_.Exception.Message; exit 1 }"
        if not exist "%TOOLS_DIR%\PortableGit.7z.exe" (
            echo(
            echo ERROR: could not download Git. Check your internet connection and try again.
            echo You can also install Git manually from https://git-scm.com and re-run this file.
            goto :fail
        )
        echo       Extracting Git...
        "%TOOLS_DIR%\PortableGit.7z.exe" -y -o"%TOOLS_DIR%\PortableGit" >nul
        del "%TOOLS_DIR%\PortableGit.7z.exe" >nul 2>nul
        if not exist "%TOOLS_DIR%\PortableGit\cmd\git.exe" (
            echo ERROR: Git extraction failed.
            goto :fail
        )
        set "GIT_EXE=%TOOLS_DIR%\PortableGit\cmd\git.exe"
    )
)

rem --- 2. Clone fresh, or update an existing clone ----------------------------
if exist "%REPO_DIR%\.git" (
    echo [2/3] Updating the existing copy to the latest version...
    "%GIT_EXE%" -C "%REPO_DIR%" fetch --depth 1 origin main
    if errorlevel 1 goto :clonefail
    "%GIT_EXE%" -C "%REPO_DIR%" reset --hard origin/main
    if errorlevel 1 goto :clonefail
) else (
    echo [2/3] Downloading a fresh copy of the mod source...
    "%GIT_EXE%" clone --depth 1 "%REPO_URL%" "%REPO_DIR%"
    if errorlevel 1 goto :clonefail
)

rem --- 3. Hand off to the normal wizard ---------------------------------------
if not exist "%REPO_DIR%\BUILD_AND_INSTALL.bat" (
    echo ERROR: BUILD_AND_INSTALL.bat was not found in the downloaded copy.
    goto :fail
)
echo [3/3] Launching the build ^& install wizard...
echo(
cd /d "%REPO_DIR%"
call "%REPO_DIR%\BUILD_AND_INSTALL.bat"
echo(
echo Done. This window can be closed.
goto :end

:clonefail
echo(
echo ERROR: could not download the mod source. Check your internet connection
echo and that the repository is reachable, then run this file again.
goto :fail

:fail
echo(
pause
exit /b 1

:end
pause
exit /b 0
