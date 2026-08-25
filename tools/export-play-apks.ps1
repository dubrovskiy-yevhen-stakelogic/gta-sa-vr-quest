[CmdletBinding()]
param(
    [string]$WorkDir = 'C:\SAVRBuild',
    [string]$Destination,
    [string]$Serial
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:PackageName = 'com.rockstargames.gtasa'
$script:ExpectedVersionName = '2.11.311'
$script:ExpectedVersionCode = '4234641'
$script:PlatformToolsName = 'platform-tools_r37.0.1-win.zip'
$script:PlatformToolsUri = 'https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip'
$script:PlatformToolsSha256 = '45F4D63113E895EBDE0C90F194099A4676B6AC653BD28D54314A9E022BBC1A99'
$script:Adb = $null
$script:SelectedSerial = $null
$script:TranscriptStarted = $false

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path.Trim().Trim('"').Trim("'"))
}

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure,
        [switch]$Echo
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $raw = @(& $FilePath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldPreference }
    $lines = @($raw | ForEach-Object { $_.ToString() })
    $text = $lines -join [Environment]::NewLine
    if ($Echo.IsPresent -and -not [string]::IsNullOrWhiteSpace($text)) {
        Write-Host $text
    }
    if ($exitCode -ne 0 -and -not $AllowFailure.IsPresent) {
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')`n$text"
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Lines = $lines; Text = $text }
}

function Get-VerifiedPlatformTools {
    param([Parameter(Mandatory = $true)][string]$ResolvedWorkDir)
    $downloads = Join-Path $ResolvedWorkDir '.downloads'
    $tools = Join-Path $ResolvedWorkDir '.tools'
    $toolRoot = Join-Path $tools 'platform-tools-37.0.1'
    $adb = Join-Path $toolRoot 'platform-tools\adb.exe'
    New-Item -ItemType Directory -Force -Path $downloads, $tools | Out-Null

    if (Test-Path -LiteralPath $adb -PathType Leaf) {
        $probe = Invoke-NativeCapture -FilePath $adb -Arguments @('version') -AllowFailure
        if ($probe.ExitCode -eq 0 -and $probe.Text -match 'Android Debug Bridge') {
            Write-Host "Using cached ADB: $adb"
            return $adb
        }
        $aside = "$toolRoot.invalid-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
        Move-Item -LiteralPath $toolRoot -Destination $aside
    }

    $archive = Join-Path $downloads $script:PlatformToolsName
    $archiveReady = $false
    if (Test-Path -LiteralPath $archive -PathType Leaf) {
        $archiveReady = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -eq $script:PlatformToolsSha256
        if (-not $archiveReady) {
            $aside = "$archive.invalid-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            Move-Item -LiteralPath $archive -Destination $aside
        }
    }
    if (-not $archiveReady) {
        $temporary = "$archive.download-$([Guid]::NewGuid().ToString('N'))"
        Write-Host 'Downloading Google Android SDK Platform Tools...'
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $script:PlatformToolsUri -OutFile $temporary
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash
            if ($hash -ne $script:PlatformToolsSha256) {
                throw "Platform Tools SHA256 mismatch. Expected $($script:PlatformToolsSha256), got $hash"
            }
            Move-Item -LiteralPath $temporary -Destination $archive
        }
        finally {
            if (Test-Path -LiteralPath $temporary -PathType Leaf) {
                Remove-Item -LiteralPath $temporary -Force
            }
        }
    }

    $temporaryRoot = "$toolRoot.extract-$([Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    try {
        Expand-Archive -LiteralPath $archive -DestinationPath $temporaryRoot
        $temporaryAdb = Join-Path $temporaryRoot 'platform-tools\adb.exe'
        if (-not (Test-Path -LiteralPath $temporaryAdb -PathType Leaf)) {
            throw 'Downloaded Platform Tools archive does not contain adb.exe.'
        }
        if (Test-Path -LiteralPath $toolRoot) {
            $aside = "$toolRoot.old-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            Move-Item -LiteralPath $toolRoot -Destination $aside
        }
        Move-Item -LiteralPath $temporaryRoot -Destination $toolRoot
    }
    finally {
        if (Test-Path -LiteralPath $temporaryRoot) {
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
        }
    }
    $probe = Invoke-NativeCapture -FilePath $adb -Arguments @('version')
    Write-Host $probe.Text
    return $adb
}

function Select-SourceDevice {
    Invoke-NativeCapture -FilePath $script:Adb -Arguments @('start-server') -AllowFailure | Out-Null
    $result = Invoke-NativeCapture -FilePath $script:Adb -Arguments @('devices', '-l')
    $devices = @()
    foreach ($line in $result.Lines) {
        if ($line -match '^([^\s]+)\s+(device|unauthorized|offline|no permissions)\b\s*(.*)$') {
            $devices += [pscustomobject]@{
                Serial = $Matches[1]
                State = $Matches[2]
                Detail = $Matches[3]
            }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($Serial)) {
        $match = @($devices | Where-Object Serial -eq $Serial)
        if ($match.Count -ne 1) { throw "Requested Android device was not found: $Serial" }
        if ($match[0].State -ne 'device') { throw "Requested Android device is $($match[0].State): $Serial" }
        $script:SelectedSerial = $Serial
    }
    else {
        $ready = @($devices | Where-Object State -eq 'device')
        if ($ready.Count -eq 0) {
            if (@($devices | Where-Object State -eq 'unauthorized').Count -gt 0) {
                throw 'The Android device is unauthorized. Unlock it, approve the USB debugging prompt, and run EXPORT_PLAY_APKS.bat again.'
            }
            if (@($devices | Where-Object State -eq 'offline').Count -gt 0) {
                throw 'The Android device is offline. Unlock it, reconnect the USB cable, and run EXPORT_PLAY_APKS.bat again.'
            }
            throw 'No Android device is connected. Use a data-capable USB cable, unlock the phone/tablet, enable USB debugging, and approve the prompt.'
        }
        if ($ready.Count -eq 1) { $script:SelectedSerial = $ready[0].Serial }
        else {
            Write-Host 'Connected Android devices:'
            for ($index = 0; $index -lt $ready.Count; $index++) {
                Write-Host "  [$($index + 1)] $($ready[$index].Serial) $($ready[$index].Detail)"
            }
            $answer = Read-Host 'Choose the phone/tablet containing the Google Play game'
            [int]$number = 0
            if (-not [int]::TryParse($answer, [ref]$number) -or
                $number -lt 1 -or $number -gt $ready.Count) {
                throw 'Invalid device selection.'
            }
            $script:SelectedSerial = $ready[$number - 1].Serial
        }
    }
    $model = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'getprop', 'ro.product.model'
    )
    $manufacturer = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'getprop', 'ro.product.manufacturer'
    )
    if (("$($manufacturer.Text) $($model.Text)") -match '(?i)\b(?:oculus|quest)\b') {
        throw 'The selected device is a Quest headset. Disconnect it and connect the Android phone/tablet containing your official Google Play copy.'
    }
    Write-Host "Selected source device: $($script:SelectedSerial) ($($model.Text.Trim()))" -ForegroundColor Green
}

function Read-InstalledPackage {
    $dump = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'dumpsys', 'package', $script:PackageName
    ) -AllowFailure
    if ($dump.ExitCode -ne 0) {
        throw "Could not query GTA SA on the selected device. ADB reported:`n$($dump.Text)"
    }
    $versionName = [regex]::Match($dump.Text, '(?m)^\s*versionName=([^\s]+)').Groups[1].Value
    $versionCode = [regex]::Match($dump.Text, '(?m)^\s*versionCode=(\d+)').Groups[1].Value
    if ([string]::IsNullOrWhiteSpace($versionName) -or [string]::IsNullOrWhiteSpace($versionCode)) {
        throw "GTA San Andreas is not installed on the selected device. Install your Google Play copy first.`nDevice output:`n$($dump.Text)"
    }
    if ($versionName -ne $script:ExpectedVersionName -or $versionCode -ne $script:ExpectedVersionCode) {
        throw "Unsupported GTA SA version: $versionName ($versionCode). Required: $($script:ExpectedVersionName) ($($script:ExpectedVersionCode))."
    }

    $pathResult = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'pm', 'path', $script:PackageName
    ) -AllowFailure
    if ($pathResult.ExitCode -ne 0) {
        throw "ADB connection failed while reading the installed APK paths. Keep the device unlocked, reconnect USB, approve debugging, and retry.`nADB reported:`n$($pathResult.Text)"
    }
    $paths = @($pathResult.Lines | ForEach-Object {
        if ($_ -match '^package:(/.+\.apk)\s*$') { $Matches[1].Trim() }
    } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($paths.Count -lt 3) {
        throw "The installed game did not expose a complete Google Play split set. Found $($paths.Count) APK path(s). Reinstall GTA SA $($script:ExpectedVersionName) from Google Play on this phone/tablet and retry.`nRaw output:`n$($pathResult.Text)"
    }
    Write-Host "Verified GTA SA $versionName ($versionCode), $($paths.Count) installed APK splits." -ForegroundColor Green
    return $paths
}

function Export-Apks {
    param([Parameter(Mandatory = $true)][string[]]$RemotePaths)
    $resolvedDestination = if ([string]::IsNullOrWhiteSpace($Destination)) {
        Join-Path $resolvedWorkDir ("GTA-SA-Play-export-{0}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    } else { Get-FullPath $Destination }
    if (Test-Path -LiteralPath $resolvedDestination) {
        if (@(Get-ChildItem -LiteralPath $resolvedDestination -Force).Count -ne 0) {
            throw "Export destination is not empty: $resolvedDestination"
        }
    }
    else { New-Item -ItemType Directory -Path $resolvedDestination | Out-Null }

    foreach ($remotePath in $RemotePaths) {
        Write-Host "Exporting $remotePath"
        $pull = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
            '-s', $script:SelectedSerial, 'pull', $remotePath, $resolvedDestination
        ) -AllowFailure -Echo
        if ($pull.ExitCode -ne 0) {
            throw "APK export failed. Keep the device unlocked and connected, then retry.`n$($pull.Text)"
        }
    }
    $localApks = @(Get-ChildItem -LiteralPath $resolvedDestination -Filter '*.apk' -File)
    if ($localApks.Count -ne $RemotePaths.Count -or @($localApks | Where-Object Length -le 0).Count -ne 0) {
        throw "Export verification failed: expected $($RemotePaths.Count) APK files, found $($localApks.Count)."
    }
    $base = @($localApks | Where-Object Name -eq 'base.apk')
    if ($base.Count -ne 1) { throw 'Exported split set does not contain exactly one base.apk.' }
    Write-Host ''
    Write-Host 'APK EXPORT COMPLETED.' -ForegroundColor Green
    Write-Host "Folder: $resolvedDestination"
    Write-Host 'Next: disconnect the phone/tablet, run BUILD_AND_INSTALL.bat, and select base.apk from this folder.' -ForegroundColor Cyan
    Start-Process -FilePath 'explorer.exe' -ArgumentList @($resolvedDestination)
    return $resolvedDestination
}

$failure = $null
try {
    $resolvedWorkDir = Get-FullPath $WorkDir
    $root = [System.IO.Path]::GetPathRoot($resolvedWorkDir)
    if ($resolvedWorkDir.TrimEnd('\') -eq $root.TrimEnd('\')) {
        throw 'WorkDir must not be a drive root.'
    }
    New-Item -ItemType Directory -Force -Path $resolvedWorkDir | Out-Null
    $logs = Join-Path $resolvedWorkDir 'logs'
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs ("export-play-apks-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Start-Transcript -LiteralPath $log -Force | Out-Null
    $script:TranscriptStarted = $true
    Write-Host "Log: $log"
    $script:Adb = Get-VerifiedPlatformTools -ResolvedWorkDir $resolvedWorkDir
    Select-SourceDevice
    $paths = @(Read-InstalledPackage)
    Export-Apks -RemotePaths $paths | Out-Null
}
catch {
    $failure = $_
    Write-Host ''
    Write-Host "FAILED: $($failure.Exception.Message)" -ForegroundColor Red
}
finally {
    if ($script:TranscriptStarted) { try { Stop-Transcript | Out-Null } catch { } }
}

if ($null -ne $failure) { exit 1 }
exit 0
