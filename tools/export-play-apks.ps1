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
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    $guidanceShown = $false
    $devices = @()
    $ready = @()
    while ($true) {
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
        $ready = @($devices | Where-Object State -eq 'device')
        if (-not [string]::IsNullOrWhiteSpace($Serial)) {
            $requested = @($ready | Where-Object Serial -eq $Serial)
            if ($requested.Count -eq 1) {
                $script:SelectedSerial = $Serial
                break
            }
        }
        elseif ($ready.Count -gt 0) { break }

        if (-not $guidanceShown) {
            Write-Host ''
            Write-Host 'WAITING FOR USB DEBUGGING...' -ForegroundColor Yellow
            Write-Host 'Seeing the phone in Windows File Explorer is not enough; that is only the MTP file connection.' -ForegroundColor Yellow
            Write-Host 'On the phone: Settings > About phone > Software information > tap Build number 7 times.'
            Write-Host 'Then: Settings > System > Developer options > enable USB debugging.'
            Write-Host 'Keep the phone unlocked and tap Always allow from this computer, then Allow.'
            Write-Host 'The exporter will continue automatically when ADB becomes available.' -ForegroundColor Cyan
            $guidanceShown = $true
        }
        $remaining = [math]::Max(0, [int][math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds))
        if ($remaining -le 0) {
            Write-Progress -Id 7 -Activity 'Waiting for the Android phone/tablet' -Completed
            if (@($devices | Where-Object State -eq 'unauthorized').Count -gt 0) {
                throw 'USB debugging is still unauthorized. Unlock the phone and approve the Allow USB debugging prompt.'
            }
            if (@($devices | Where-Object State -eq 'offline').Count -gt 0) {
                throw 'The Android ADB connection is offline. Reconnect the USB cable, unlock the phone, and retry.'
            }
            throw 'Windows may see the phone for file transfer, but ADB still cannot see it. Enable USB debugging, approve this computer on the phone, and retry. If no prompt appears, use another data-capable USB cable or install the phone manufacturer ADB driver.'
        }
        $stateText = if (@($devices | Where-Object State -eq 'unauthorized').Count -gt 0) {
            'Phone found; approve the USB debugging prompt on it'
        } else { 'No ADB device yet; enable USB debugging on the unlocked phone' }
        Write-Progress -Id 7 -Activity 'Waiting for the Android phone/tablet' `
            -Status "$stateText ($remaining seconds remaining)" `
            -PercentComplete ([math]::Min(100, [math]::Max(0, (120 - $remaining) * 100 / 120)))
        Start-Sleep -Seconds 2
    }
    Write-Progress -Id 7 -Activity 'Waiting for the Android phone/tablet' -Completed

    if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) {
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
    $remoteNames = @($paths | ForEach-Object { ($_ -split '/')[-1] })
    if ($remoteNames -notcontains 'split_config.arm64_v8a.apk') {
        throw 'Google Play did not install the required ARM64 split_config.arm64_v8a.apk on this device. Quest needs the ARM64 game library. Use a real 64-bit ARM Android phone/tablet, not an emulator or Windows Android subsystem, install GTA SA 2.11.311 there, and export again.'
    }
    if ($remoteNames -notcontains 'split_data_main.apk') {
        throw 'Google Play did not expose split_data_main.apk on this device. Reinstall GTA SA 2.11.311 from Google Play and export again.'
    }
    Write-Host "Verified GTA SA $versionName ($versionCode), $($paths.Count) installed APK splits." -ForegroundColor Green
    return $paths
}

function Get-ApkEntryNames {
    param([Parameter(Mandatory = $true)][string]$Path)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    try { $archive = [System.IO.Compression.ZipFile]::OpenRead($Path) }
    catch { throw "Exported file is not a readable APK: $Path`n$($_.Exception.Message)" }
    try { return @($archive.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) } | ForEach-Object FullName) }
    finally { $archive.Dispose() }
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
    $arm64Apks = @()
    $dataApks = @()
    $dataEntries = @('assets/data/gta.dat', 'assets/anim/anim.img', 'assets/texdb/gta3.img')
    foreach ($apk in $localApks) {
        $entries = @(Get-ApkEntryNames -Path $apk.FullName)
        if ($entries -contains 'lib/arm64-v8a/libGame.so') { $arm64Apks += $apk }
        if (@($dataEntries | Where-Object { $entries -notcontains $_ }).Count -eq 0) { $dataApks += $apk }
    }
    if ($arm64Apks.Count -ne 1) {
        throw "Exported split set is not Quest-compatible: expected exactly one APK containing lib/arm64-v8a/libGame.so, found $($arm64Apks.Count). Use a real 64-bit ARM Android phone/tablet and export its complete Google Play installation."
    }
    if ($dataApks.Count -ne 1) {
        throw "Exported split set is incomplete: expected exactly one split_data_main APK containing the GTA assets, found $($dataApks.Count)."
    }
    Write-Host ''
    Write-Host 'APK EXPORT COMPLETED.' -ForegroundColor Green
    Write-Host "Folder: $resolvedDestination"
    Write-Host 'Correct APK filename: base.apk. Do not rename it, and keep every split_*.apk beside it.' -ForegroundColor Green
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
