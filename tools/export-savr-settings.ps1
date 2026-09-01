[CmdletBinding()]
param(
    [string]$Serial = '',
    [string]$OutputDirectory = '',
    [switch]$DrivingOnly,
    [switch]$NoOpen
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ExporterVersion = '1.0'
$PackageName = 'com.rockstargames.gtasa'
$RemoteRoot = "/sdcard/Android/data/$PackageName/files"
$KnownSettings = @(
    'vr_driving.ini',
    'vr_appearance.ini',
    'vr_calib.ini',
    'vr_graphics.ini',
    'vr_holsters.ini',
    'vr_hud.ini',
    'vr_locomotion.ini'
)

function Get-AdbPath {
    $candidates = [System.Collections.Generic.List[string]]::new()

    foreach ($relative in @(
        'adb.exe',
        'platform-tools\adb.exe',
        '..\adb.exe',
        '..\platform-tools\adb.exe'
    )) {
        $candidates.Add((Join-Path $PSScriptRoot $relative))
    }

    foreach ($sdkRoot in @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME)) {
        if (-not [string]::IsNullOrWhiteSpace($sdkRoot)) {
            $candidates.Add((Join-Path $sdkRoot 'platform-tools\adb.exe'))
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $candidates.Add((Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'))
        $candidates.Add((Join-Path $env:LOCALAPPDATA 'SideQuest\platform-tools\adb.exe'))
        $candidates.Add((Join-Path $env:LOCALAPPDATA 'Programs\SideQuest\resources\app.asar.unpacked\build\platform-tools\adb.exe'))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) {
        $candidates.Add((Join-Path $env:APPDATA 'SideQuest\platform-tools\adb.exe'))
    }

    $pathCommand = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($pathCommand) {
        $candidates.Add($pathCommand.Source)
    }

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate) -or
            -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            continue
        }

        # SideQuest installations can leave an adb.exe placeholder that exists
        # but is not executable. Probe every candidate and continue to the next
        # known location instead of failing after selecting the first file.
        try {
            $probe = @(& $candidate version 2>&1)
            if ($LASTEXITCODE -eq 0 -and $probe.Count -gt 0) {
                return [System.IO.Path]::GetFullPath($candidate)
            }
        } catch {
            continue
        }
    }

    throw @'
adb.exe was not found.

Install Android Platform Tools / SideQuest, or put these three files next to
this script:
  adb.exe
  AdbWinApi.dll
  AdbWinUsbApi.dll
'@
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [switch]$AllowFailure
    )

    $output = @(& $script:AdbPath @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        $text = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
        throw "adb failed (exit $exitCode): adb $($Arguments -join ' ')`n$text"
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = @($output | ForEach-Object { [string]$_ })
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

function Get-SafeName {
    param([string]$Text, [string]$Fallback = 'Quest')

    if ([string]::IsNullOrWhiteSpace($Text)) { return $Fallback }
    $safe = [regex]::Replace($Text.Trim(), '[^A-Za-z0-9._-]+', '_').Trim('_')
    if ([string]::IsNullOrWhiteSpace($safe)) { return $Fallback }
    return $safe
}

function Pull-StableFile {
    param(
        [Parameter(Mandatory = $true)][string]$DeviceSerial,
        [Parameter(Mandatory = $true)][string]$RemotePath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ScratchDirectory
    )

    $leaf = Split-Path -Leaf $Destination
    $first = Join-Path $ScratchDirectory ("first_" + $leaf)
    $second = Join-Path $ScratchDirectory ("second_" + $leaf)

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        foreach ($candidate in @($first, $second)) {
            if (Test-Path -LiteralPath $candidate) {
                Remove-Item -LiteralPath $candidate -Force
            }
        }

        $firstPull = Invoke-Adb -Arguments @('-s', $DeviceSerial, 'pull', $RemotePath, $first) -AllowFailure
        if ($firstPull.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $first -PathType Leaf)) {
            Start-Sleep -Milliseconds 200
            continue
        }

        Start-Sleep -Milliseconds 250
        $secondPull = Invoke-Adb -Arguments @('-s', $DeviceSerial, 'pull', $RemotePath, $second) -AllowFailure
        if ($secondPull.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $second -PathType Leaf)) {
            Start-Sleep -Milliseconds 200
            continue
        }

        $firstFile = Get-Item -LiteralPath $first
        $secondFile = Get-Item -LiteralPath $second
        $firstHash = Get-FileSha256 -Path $first
        $secondHash = Get-FileSha256 -Path $second
        if ($firstFile.Length -eq $secondFile.Length -and
            $firstHash -eq $secondHash -and $secondFile.Length -gt 0) {
            Copy-Item -LiteralPath $second -Destination $Destination -Force
            return
        }

        Start-Sleep -Milliseconds 300
    }

    throw "Could not obtain a stable snapshot of $RemotePath. Stop editing settings and run the exporter again."
}

function Get-DeviceRecords {
    $result = Invoke-Adb -Arguments @('devices', '-l')
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($line in $result.Output) {
        $match = [regex]::Match($line, '^(\S+)\s+(device|unauthorized|offline)(?:\s+(.*))?$')
        if ($match.Success) {
            $records.Add([pscustomobject]@{
                Serial = $match.Groups[1].Value
                State = $match.Groups[2].Value
                Details = $match.Groups[3].Value
            })
        }
    }
    return @($records)
}

function Get-DrivingSummary {
    param([Parameter(Mandatory = $true)][string]$Path)

    $lines = @(Get-Content -LiteralPath $Path)
    $configVersion = $null
    $stats = @{}

    foreach ($line in $lines) {
        if ($line -match '^DrivingConfigVersion=(\d+)$') {
            $configVersion = [int]$Matches[1]
            continue
        }

        $modelId = $null
        $category = $null
        if ($line -match '^ModelSeat\.(\d+)\.') {
            $modelId = [int]$Matches[1]
            $category = 'SeatKeys'
        } elseif ($line -match '^BikeAccelerator\.(\d+)=') {
            $modelId = [int]$Matches[1]
            $category = 'AcceleratorKeys'
        } elseif ($line -match '^Control\.(\d+)\.') {
            $modelId = [int]$Matches[1]
            $category = 'ControlKeys'
        } elseif ($line -match '^WheelCalib\.(\d+)\.') {
            $modelId = [int]$Matches[1]
            $category = 'WheelKeys'
        }

        if ($null -ne $modelId) {
            if (-not $stats.ContainsKey($modelId)) {
                $stats[$modelId] = @{
                    ModelId = $modelId
                    SeatKeys = 0
                    AcceleratorKeys = 0
                    ControlKeys = 0
                    WheelKeys = 0
                }
            }
            $stats[$modelId][$category] = [int]$stats[$modelId][$category] + 1
        }
    }

    if ($null -eq $configVersion) {
        throw 'vr_driving.ini is incomplete: DrivingConfigVersion is missing.'
    }

    $rows = @(
        foreach ($entry in ($stats.GetEnumerator() | Sort-Object { [int]$_.Key })) {
            $item = $entry.Value
            $total = [int]$item.SeatKeys + [int]$item.AcceleratorKeys +
                [int]$item.ControlKeys + [int]$item.WheelKeys
            [pscustomobject]@{
                ModelId = [int]$item.ModelId
                SeatKeys = [int]$item.SeatKeys
                AcceleratorKeys = [int]$item.AcceleratorKeys
                ControlKeys = [int]$item.ControlKeys
                WheelKeys = [int]$item.WheelKeys
                TotalKeys = $total
                CompleteCurrentSchema = (
                    [int]$item.SeatKeys -eq 4 -and
                    [int]$item.AcceleratorKeys -eq 1 -and
                    [int]$item.ControlKeys -eq 12 -and
                    [int]$item.WheelKeys -eq 7
                )
            }
        }
    )

    $modelKeyCount = @($rows | Measure-Object -Property TotalKeys -Sum).Sum
    if ($null -eq $modelKeyCount) { $modelKeyCount = 0 }
    $globalKeyCount = $lines.Count - [int]$modelKeyCount
    $completeModelCount = @($rows | Where-Object CompleteCurrentSchema).Count
    $completeCurrentSchema = $configVersion -eq 3 -and
        $globalKeyCount -eq 41 -and
        $completeModelCount -eq $rows.Count -and
        $lines.Count -eq (41 + 24 * $rows.Count)

    return [pscustomobject]@{
        ConfigVersion = $configVersion
        LineCount = $lines.Count
        GlobalKeyCount = $globalKeyCount
        ModelCount = $rows.Count
        CompleteModelCount = $completeModelCount
        CompleteCurrentSchema = $completeCurrentSchema
        Models = $rows
    }
}

function New-PortableZip {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $sourceRoot = [System.IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\', '/')
    $stream = [System.IO.File]::Open(
        $DestinationPath,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
    try {
        $archive = [System.IO.Compression.ZipArchive]::new(
            $stream,
            [System.IO.Compression.ZipArchiveMode]::Create,
            $false)
        try {
            foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -File -Recurse | Sort-Object FullName) {
                $relativeName = $file.FullName.Substring($sourceRoot.Length + 1).Replace('\', '/')
                $entry = $archive.CreateEntry(
                    $relativeName,
                    [System.IO.Compression.CompressionLevel]::Optimal)
                $input = [System.IO.File]::OpenRead($file.FullName)
                try {
                    $output = $entry.Open()
                    try {
                        $input.CopyTo($output)
                    } finally {
                        $output.Dispose()
                    }
                } finally {
                    $input.Dispose()
                }
            }
        } finally {
            $archive.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

$script:AdbPath = Get-AdbPath
Write-Host "SAVR Settings Exporter $ExporterVersion" -ForegroundColor Cyan
Write-Host "adb: $script:AdbPath"

$null = Invoke-Adb -Arguments @('start-server')
$devices = @(Get-DeviceRecords)
$authorized = @($devices | Where-Object State -eq 'device')

if (-not [string]::IsNullOrWhiteSpace($Serial)) {
    $selected = $authorized | Where-Object Serial -eq $Serial | Select-Object -First 1
    if (-not $selected) {
        throw "Quest '$Serial' is not connected and authorized. Check USB debugging inside the headset."
    }
} else {
    if ($authorized.Count -eq 0) {
        $states = if ($devices.Count -gt 0) {
            ($devices | ForEach-Object { "$($_.Serial):$($_.State)" }) -join ', '
        } else {
            'no devices'
        }
        throw "No authorized Quest found ($states). Connect USB, enable developer mode, and accept the USB debugging prompt."
    }
    if ($authorized.Count -gt 1) {
        $names = ($authorized | ForEach-Object Serial) -join ', '
        throw "More than one device is connected ($names). Run again with -Serial <serial>."
    }
    $selected = $authorized[0]
}

$Serial = $selected.Serial
$packageResult = Invoke-Adb -Arguments @('-s', $Serial, 'shell', 'pm', 'path', $PackageName) -AllowFailure
if ($packageResult.ExitCode -ne 0 -or
    -not ($packageResult.Output | Where-Object { $_ -match '^package:' })) {
    throw "$PackageName is not installed on the selected Quest."
}

$modelResult = Invoke-Adb -Arguments @('-s', $Serial, 'shell', 'getprop', 'ro.product.model') -AllowFailure
$deviceModel = Get-SafeName (($modelResult.Output -join '').Trim())
$archiveId = [Guid]::NewGuid().ToString('N')
$shortArchiveId = $archiveId.Substring(0, 12)
$timestampUtc = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfff')

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    if ([string]::IsNullOrWhiteSpace($desktop)) {
        $OutputDirectory = Join-Path $PSScriptRoot 'SAVR Settings Exports'
    } else {
        $OutputDirectory = Join-Path $desktop 'SAVR Settings Exports'
    }
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$bundleName = "SAVR_Settings_${deviceModel}_${shortArchiveId}_${timestampUtc}"
$scratchRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("SAVR-settings-export-" + [Guid]::NewGuid().ToString('N'))
$bundleRoot = Join-Path $scratchRoot $bundleName
$settingsRoot = Join-Path $bundleRoot 'settings'
$zipPath = Join-Path $OutputDirectory ($bundleName + '.zip')

New-Item -ItemType Directory -Force -Path $settingsRoot | Out-Null

try {
    $requestedSettings = if ($DrivingOnly) { @('vr_driving.ini') } else { $KnownSettings }
    $exportedFiles = [System.Collections.Generic.List[object]]::new()
    $missingFiles = [System.Collections.Generic.List[string]]::new()

    foreach ($name in $requestedSettings) {
        $remotePath = "$RemoteRoot/$name"
        $exists = Invoke-Adb -Arguments @('-s', $Serial, 'shell', 'test', '-f', $remotePath) -AllowFailure
        if ($exists.ExitCode -ne 0) {
            $missingFiles.Add($name)
            continue
        }

        Write-Host "Pulling $name ..."
        $destination = Join-Path $settingsRoot $name
        Pull-StableFile -DeviceSerial $Serial -RemotePath $remotePath `
            -Destination $destination -ScratchDirectory $scratchRoot
        $item = Get-Item -LiteralPath $destination
        $exportedFiles.Add([pscustomobject]@{
            path = "settings/$name"
            bytes = $item.Length
            sha256 = Get-FileSha256 -Path $destination
        })
    }

    $drivingPath = Join-Path $settingsRoot 'vr_driving.ini'
    if (-not (Test-Path -LiteralPath $drivingPath -PathType Leaf)) {
        throw 'vr_driving.ini was not found. Save at least one Vehicle Settings adjustment in SAVR, then run the exporter again.'
    }

    $driving = Get-DrivingSummary -Path $drivingPath
    if ($driving.ConfigVersion -eq 3 -and -not $driving.CompleteCurrentSchema) {
        throw "vr_driving.ini failed schema-3 validation: lines=$($driving.LineCount), global=$($driving.GlobalKeyCount), models=$($driving.ModelCount), complete=$($driving.CompleteModelCount)."
    }
    $modelsCsv = Join-Path $bundleRoot 'models.csv'
    if ($driving.Models.Count -gt 0) {
        $driving.Models | Export-Csv -LiteralPath $modelsCsv -NoTypeInformation -Encoding UTF8
    } else {
        [System.IO.File]::WriteAllText(
            $modelsCsv,
            '"ModelId","SeatKeys","AcceleratorKeys","ControlKeys","WheelKeys","TotalKeys","CompleteCurrentSchema"' + [Environment]::NewLine,
            [System.Text.UTF8Encoding]::new($false))
    }

    $gameRunningResult = Invoke-Adb -Arguments @('-s', $Serial, 'shell', 'pidof', $PackageName) -AllowFailure
    $gameRunning = $gameRunningResult.ExitCode -eq 0 -and
        -not [string]::IsNullOrWhiteSpace(($gameRunningResult.Output -join '').Trim())

    $packageDump = Invoke-Adb -Arguments @('-s', $Serial, 'shell', 'dumpsys', 'package', $PackageName) -AllowFailure
    $packageText = $packageDump.Output -join "`n"
    $versionNameMatch = [regex]::Match($packageText, '(?m)^\s*versionName=(\S+)')
    $versionCodeMatch = [regex]::Match($packageText, '(?m)^\s*versionCode=(\d+)')

    $savrVersion = $null
    $savrVersionSource = 'unavailable'
    $logResult = Invoke-Adb -Arguments @('-s', $Serial, 'logcat', '-d', '-v', 'brief', 'SAVR:I', '*:S') -AllowFailure
    if ($logResult.ExitCode -eq 0) {
        $versionMatches = [regex]::Matches(($logResult.Output -join "`n"), 'SAVR version\s+([0-9A-Za-z._-]+)')
        if ($versionMatches.Count -gt 0) {
            $savrVersion = $versionMatches[$versionMatches.Count - 1].Groups[1].Value
            $savrVersionSource = 'recent_logcat_unverified'
        }
    }

    $manifest = [ordered]@{
        format = 'savr-settings'
        format_version = 1
        schema_version = 1
        exporter_version = $ExporterVersion
        archive_id = $archiveId
        exported_utc = [DateTime]::UtcNow.ToString('o')
        package = $PackageName
        game_version_name = if ($versionNameMatch.Success) { $versionNameMatch.Groups[1].Value } else { 'unknown' }
        game_version_code = if ($versionCodeMatch.Success) { $versionCodeMatch.Groups[1].Value } else { 'unknown' }
        savr_version = $savrVersion
        savr_version_source = $savrVersionSource
        device_model = $deviceModel
        game_running_during_export = $gameRunning
        capture_consistency = 'two_identical_reads_per_file'
        source = $RemoteRoot
        driving = [ordered]@{
            config_version = $driving.ConfigVersion
            line_count = $driving.LineCount
            global_key_count = $driving.GlobalKeyCount
            model_count = $driving.ModelCount
            complete_model_count = $driving.CompleteModelCount
            complete_current_schema = $driving.CompleteCurrentSchema
            expected_schema_3_shape = 'global=41; per model: seat=4, accelerator=1, control=12, wheel=7'
        }
        files = @($exportedFiles)
        missing_optional_files = @($missingFiles | Where-Object { $_ -ne 'vr_driving.ini' })
        privacy_profile = 'settings-only-v1'
        privacy = 'No device serial or serial-derived identifier, saves, logs, screenshots, or game data are included.'
    }

    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $bundleRoot 'manifest.json') -Encoding UTF8

    $readme = @(
        'SAVR SETTINGS EXPORT',
        '',
        'Send this entire ZIP to the mod developer.',
        'The primary vehicle calibration file is settings/vr_driving.ini.',
        'models.csv lists every per-model calibration block and whether it is complete.',
        '',
        "Vehicle models: $($driving.ModelCount)",
        "Complete model blocks: $($driving.CompleteModelCount)",
        "Driving config version: $($driving.ConfigVersion)",
        "Archive id: $archiveId",
        '',
        'The archive contains SAVR INI settings only. It does not contain saves, logs,',
        'screenshots, game assets, or the raw Quest serial number.'
    )
    [System.IO.File]::WriteAllLines(
        (Join-Path $bundleRoot 'README_SEND_THIS_ZIP.txt'),
        $readme,
        [System.Text.UTF8Encoding]::new($false))

    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    New-PortableZip -SourceDirectory $bundleRoot -DestinationPath $zipPath

    # Verify the ZIP is readable and contains the mandatory calibration before
    # presenting it as a successful export.
    $zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entryNames = @($zip.Entries | ForEach-Object FullName)
        if (-not ($entryNames -contains 'settings/vr_driving.ini') -or
            -not ($entryNames -contains 'manifest.json') -or
            -not ($entryNames -contains 'models.csv')) {
            throw 'ZIP verification failed: mandatory entries are missing.'
        }
    } finally {
        $zip.Dispose()
    }

    $zipHash = Get-FileSha256 -Path $zipPath
    Write-Host ''
    Write-Host 'EXPORT COMPLETE' -ForegroundColor Green
    Write-Host "Vehicle models: $($driving.ModelCount) ($($driving.CompleteModelCount) complete blocks)"
    Write-Host "ZIP SHA256: $zipHash"
    Write-Host 'Send this file:' -ForegroundColor Yellow
    Write-Host $zipPath -ForegroundColor Yellow

    if (-not $NoOpen) {
        try {
            Start-Process -FilePath explorer.exe -ArgumentList ('/select,"{0}"' -f $zipPath)
        } catch {
            Write-Warning "Could not open Explorer: $($_.Exception.Message)"
        }
    }
} finally {
    if ((Test-Path -LiteralPath $scratchRoot -PathType Container) -and
        ((Split-Path -Leaf $scratchRoot) -like 'SAVR-settings-export-*')) {
        Remove-Item -LiteralPath $scratchRoot -Recurse -Force
    }
}
