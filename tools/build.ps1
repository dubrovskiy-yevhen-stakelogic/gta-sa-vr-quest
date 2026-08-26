[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',
    [string]$AndroidSdk,
    [string]$JavaHome,
    [Parameter(Mandatory = $true)][string]$OpenXrLoader,
    [string]$BuildRoot,
    [string]$PythonExe,
    [string]$Apktool,
    [string]$GamePackage,
    [string]$AudioSource,
    [string]$Keystore,
    [switch]$Package,
    [switch]$AllowUnofficialSource
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

if ([string]::IsNullOrWhiteSpace($AndroidSdk)) {
    $AndroidSdk = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { $env:ANDROID_HOME }
}
if ([string]::IsNullOrWhiteSpace($JavaHome)) { $JavaHome = $env:JAVA_HOME }
if ([string]::IsNullOrWhiteSpace($AndroidSdk)) { throw 'Android SDK path is required.' }
if ([string]::IsNullOrWhiteSpace($JavaHome)) { throw 'JDK 21 path is required.' }
if ([string]::IsNullOrWhiteSpace($BuildRoot)) { $BuildRoot = Join-Path $ProjectRoot 'build' }

$AndroidSdk = [System.IO.Path]::GetFullPath($AndroidSdk)
$JavaHome = [System.IO.Path]::GetFullPath($JavaHome)
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$OpenXrLoader = [System.IO.Path]::GetFullPath($OpenXrLoader)

$Ndk = Join-Path $AndroidSdk 'ndk\27.2.12479018'
$CMake = Join-Path $AndroidSdk 'cmake\3.22.1\bin\cmake.exe'
$Ninja = Join-Path $AndroidSdk 'cmake\3.22.1\bin\ninja.exe'
$Javac = Join-Path $JavaHome 'bin\javac.exe'
$Java = Join-Path $JavaHome 'bin\java.exe'
$BuildTools = Join-Path $AndroidSdk 'build-tools\35.0.0'
$D8Jar = Join-Path $BuildTools 'lib\d8.jar'
$AndroidJar = Join-Path $AndroidSdk 'platforms\android-35\android.jar'
$NativeBuild = Join-Path $BuildRoot 'native'
$LoaderClasses = Join-Path $BuildRoot 'loader-classes'

foreach ($RequiredTool in @($CMake, $Ninja, $Javac, $Java, $D8Jar, $AndroidJar, $OpenXrLoader)) {
    if (-not (Test-Path -LiteralPath $RequiredTool -PathType Leaf)) {
        throw "Required build input not found: $RequiredTool"
    }
}

$loaderHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $OpenXrLoader).Hash
$expectedLoaderHash = '713E3BB8D955254C670ACC1C4899A65CB8C930E97DD9958BF37EA922D72B7A06'
if ($loaderHash -ne $expectedLoaderHash) {
    throw "OpenXR loader hash mismatch. Expected $expectedLoaderHash, got $loaderHash"
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

Write-Host "==> libsavr.so ($Configuration, arm64-v8a)"
& $CMake `
    -S (Join-Path $ProjectRoot 'native') `
    -B $NativeBuild `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $Ndk 'build\cmake\android.toolchain.cmake')" `
    "-DOPENXR_LOADER_SO=$OpenXrLoader" `
    '-DANDROID_ABI=arm64-v8a' `
    '-DANDROID_PLATFORM=android-28' `
    "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

& $CMake --build $NativeBuild
if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }

Write-Host '==> loader DEX'
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
if ($LoaderClassFiles.Count -eq 0) { throw 'Java loader produced no class files.' }

$DexFile = Join-Path $BuildRoot 'classes.dex'
if (Test-Path -LiteralPath $DexFile) { Remove-Item -LiteralPath $DexFile -Force }
& $Java -cp $D8Jar com.android.tools.r8.D8 `
    --release --min-api 28 --lib $AndroidJar --output $BuildRoot @LoaderClassFiles
if ($LASTEXITCODE -ne 0) { throw "D8 failed: $LASTEXITCODE" }
for ($attempt = 0; $attempt -lt 30 -and -not (Test-Path -LiteralPath $DexFile); $attempt++) {
    Start-Sleep -Milliseconds 100
}
if (-not (Test-Path -LiteralPath $DexFile)) {
    throw "D8 completed but did not produce $DexFile"
}

$NativeLibrary = Join-Path $NativeBuild 'libsavr.so'
Write-Host "libsavr.so : $NativeLibrary"
Write-Host "SHA256      : $((Get-FileHash -Algorithm SHA256 -LiteralPath $NativeLibrary).Hash)"
Write-Host "loader DEX  : $DexFile"
Write-Host "SHA256      : $((Get-FileHash -Algorithm SHA256 -LiteralPath $DexFile).Hash)"

if ($Package.IsPresent) {
    foreach ($requiredValue in @{
        GamePackage = $GamePackage
        AudioSource = $AudioSource
        Apktool = $Apktool
    }.GetEnumerator()) {
        if ([string]::IsNullOrWhiteSpace([string]$requiredValue.Value)) {
            throw "$($requiredValue.Key) is required with -Package."
        }
    }
    if ([string]::IsNullOrWhiteSpace($PythonExe)) {
        $python = Get-Command python.exe -ErrorAction SilentlyContinue
        if ($null -ne $python) { $PythonExe = $python.Source }
    }
    if ([string]::IsNullOrWhiteSpace($PythonExe) -or
        -not (Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
        throw 'Python 3 is required for APK assembly. Use BUILD_AND_INSTALL or pass -PythonExe.'
    }
    & $PythonExe -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)'
    if ($LASTEXITCODE -ne 0) {
        throw 'Python 3.10 or newer is required for APK assembly.'
    }
    $expectedApktoolHash = 'DBF930B076C6B9BE08D57C449CACEFC3BDD6B71EBD59B3066FC0E1F5B14F9423'
    $actualApktoolHash = (Get-FileHash -LiteralPath $Apktool -Algorithm SHA256).Hash
    if ($actualApktoolHash -ne $expectedApktoolHash) {
        throw "Apktool hash mismatch. Expected $expectedApktoolHash, got $actualApktoolHash"
    }
    if ([string]::IsNullOrWhiteSpace($Keystore)) {
        $Keystore = Join-Path $BuildRoot 'signing\savr.keystore'
    }

    $assembleArgs = @(
        (Join-Path $ProjectRoot 'tools\assemble.py'),
        '--game-package', $GamePackage,
        '--audio-source', $AudioSource,
        '--build-dir', $BuildRoot,
        '--sdk', $AndroidSdk,
        '--java-home', $JavaHome,
        '--apktool', $Apktool,
        '--native-lib', $NativeLibrary,
        '--loader-dex', $DexFile,
        '--openxr-loader', $OpenXrLoader,
        '--keystore', $Keystore
    )
    if ($AllowUnofficialSource.IsPresent) { $assembleArgs += '--allow-unofficial-source' }
    & $PythonExe @assembleArgs
    if ($LASTEXITCODE -ne 0) { throw "APK assembly failed: $LASTEXITCODE" }
}
