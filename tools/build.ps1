[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',
    [string]$ToolchainRoot = 'C:\Dev\android-toolchain',
    [switch]$Package,
    [string]$InstallSerial = ''
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Sdk = Join-Path $ToolchainRoot 'sdk'
$Ndk = Join-Path $Sdk 'ndk\27.2.12479018'
$CMake = Join-Path $Sdk 'cmake\3.22.1\bin\cmake.exe'
$Ninja = Join-Path $Sdk 'cmake\3.22.1\bin\ninja.exe'
$JavaRoot = Join-Path $ToolchainRoot 'jdk21'
$Javac = Join-Path $JavaRoot 'bin\javac.exe'
$BuildTools = Join-Path $Sdk 'build-tools\35.0.0'
$D8 = Join-Path $BuildTools 'd8.bat'
$AndroidJar = Join-Path $Sdk 'platforms\android-35\android.jar'
$BuildRoot = Join-Path $ProjectRoot 'build'
$NativeBuild = Join-Path $BuildRoot 'native'
$LoaderClasses = Join-Path $BuildRoot 'loader-classes'

foreach ($RequiredTool in @($CMake, $Ninja, $Javac, $D8, $AndroidJar)) {
    if (-not (Test-Path -LiteralPath $RequiredTool)) {
        throw "Required build tool not found: $RequiredTool"
    }
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

Write-Host "==> libsavr.so ($Configuration, arm64-v8a)"
& $CMake `
    -S (Join-Path $ProjectRoot 'native') `
    -B $NativeBuild `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $Ndk 'build\cmake\android.toolchain.cmake')" `
    '-DANDROID_ABI=arm64-v8a' `
    '-DANDROID_PLATFORM=android-28' `
    "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

& $CMake --build $NativeBuild
if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }

Write-Host '==> loader dex'
if (Test-Path -LiteralPath $LoaderClasses) {
    Remove-Item -LiteralPath $LoaderClasses -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $LoaderClasses | Out-Null

& $Javac `
    -source 8 -target 8 -nowarn `
    -bootclasspath $AndroidJar -classpath $AndroidJar `
    -d $LoaderClasses `
    (Join-Path $ProjectRoot 'loader\src\com\savr\SavrApplication.java')
if ($LASTEXITCODE -ne 0) { throw "Java loader build failed: $LASTEXITCODE" }

$LoaderClassFiles = @(
    Get-ChildItem -LiteralPath (Join-Path $LoaderClasses 'com\savr') -Filter '*.class' |
        ForEach-Object { $_.FullName }
)
if ($LoaderClassFiles.Count -eq 0) { throw 'Java loader produced no class files' }

$DexFile = Join-Path $BuildRoot 'classes.dex'
if (Test-Path -LiteralPath $DexFile) {
    Remove-Item -LiteralPath $DexFile -Force
}
& $D8 --release --min-api 28 --lib $AndroidJar --output $BuildRoot @LoaderClassFiles
if ($LASTEXITCODE -ne 0) { throw "D8 failed: $LASTEXITCODE" }

# d8.bat can return before the freshly written dex is visible to the next
# process on Windows.  Give the file system a short bounded window instead of
# letting the packager fail later with a misleading FileNotFoundError.
for ($attempt = 0; $attempt -lt 30 -and -not (Test-Path -LiteralPath $DexFile); $attempt++) {
    Start-Sleep -Milliseconds 100
}
if (-not (Test-Path -LiteralPath $DexFile)) {
    throw "D8 completed but did not produce the loader dex: $DexFile"
}

Write-Host "libsavr.so : $(Join-Path $NativeBuild 'libsavr.so')"
Write-Host "loader dex : $DexFile (stored in the APK as classes4.dex)"

if ($Package.IsPresent -or $InstallSerial) {
    $PythonExe = $null
    $PythonArgs = @()
    $PythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($PythonCommand) {
        $PythonExe = $PythonCommand.Source
    } else {
        $BundledPython = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
        if (Test-Path -LiteralPath $BundledPython) {
            $PythonExe = $BundledPython
        } else {
            $PythonLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
            if ($PythonLauncher) {
                $PythonExe = $PythonLauncher.Source
                $PythonArgs += '-3'
            }
        }
    }
    if (-not $PythonExe) {
        throw 'Python 3 is required for APK assembly. Install it or put python.exe on PATH.'
    }

    $PythonArgs += (Join-Path $ProjectRoot 'tools\assemble.py')
    $PythonArgs += '--vr'
    if ($InstallSerial) {
        $PythonArgs += '--install'
        $PythonArgs += $InstallSerial
    }
    & $PythonExe @PythonArgs
    if ($LASTEXITCODE -ne 0) { throw "APK assembly failed: $LASTEXITCODE" }
}
