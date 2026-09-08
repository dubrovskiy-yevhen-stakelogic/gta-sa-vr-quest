[CmdletBinding()]
param(
    [string]$AdbPath = '',
    [string]$Serial = '',
    [string]$OutputDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PackageName = 'com.rockstargames.gtasa'
$RemoteRoot = "/sdcard/Android/data/$PackageName/files"
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$script:ResolvedAdb = ''
$script:Commands = New-Object 'System.Collections.Generic.List[object]'
$script:Warnings = New-Object 'System.Collections.Generic.List[string]'

function Write-Utf8 {
    param([string]$Path, [AllowEmptyString()][string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, $script:Utf8)
}

function ConvertTo-NativeArgument {
    param([AllowEmptyString()][string]$Value)
    # Windows CommandLineToArgvW quoting, including trailing backslashes.
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    return '"' + [regex]::Replace(
        [regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Invoke-Adb {
    param([string[]]$Arguments, [switch]$AllowFailure, [switch]$Probe)
    $started = [DateTimeOffset]::UtcNow
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $script:ResolvedAdb
    $process.StartInfo.Arguments = ($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.StandardOutputEncoding = $script:Utf8
    $process.StartInfo.StandardErrorEncoding = $script:Utf8
    $code = -1
    $stdout = ''
    $stderr = ''
    try {
        $null = $process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            # Only the PC-side adb client is terminated; no device process is stopped.
            $process.Kill()
            $process.WaitForExit()
            $stderr = 'Collector timed out after 30 seconds. '
        } else {
            $code = $process.ExitCode
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr += $stderrTask.GetAwaiter().GetResult()
    } catch {
        $stderr += $_.Exception.Message
    } finally {
        $process.Dispose()
    }
    $displayArguments = @($Arguments)
    if ($displayArguments.Count -ge 2 -and $displayArguments[0] -eq '-s') {
        $displayArguments[1] = '<selected-device>'
    }
    if (-not $Probe) {
        $script:Commands.Add([pscustomobject]@{
            started_utc = $started.ToString('o')
            command = 'adb ' + ($displayArguments -join ' ')
            exit_code = $code
            stderr = $stderr.Trim()
        })
    }
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "adb failed (exit $code): $($displayArguments -join ' ')`n$stderr`n$stdout"
    }
    return [pscustomobject]@{
        ExitCode = $code
        Output = @($stdout -split '\r?\n' | Where-Object { $_.Length -gt 0 })
        Error = $stderr.Trim()
    }
}

function Find-Adb {
    if (-not [string]::IsNullOrWhiteSpace($AdbPath)) {
        if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "adb was not found at: $AdbPath" }
        $script:ResolvedAdb = [System.IO.Path]::GetFullPath($AdbPath)
        $null = Invoke-Adb -Arguments @('version') -Probe
        return
    }
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    foreach ($relative in @(
        '..\.tools\platform-tools\adb.exe', '.tools\platform-tools\adb.exe',
        'adb.exe', 'platform-tools\adb.exe', '..\adb.exe', '..\platform-tools\adb.exe',
        '..\android-toolchain\sdk\platform-tools\adb.exe',
        '..\..\android-toolchain\sdk\platform-tools\adb.exe'
    )) { $candidates.Add((Join-Path $PSScriptRoot $relative)) }
    foreach ($sdkRoot in @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME)) {
        if (-not [string]::IsNullOrWhiteSpace($sdkRoot)) {
            $candidates.Add((Join-Path $sdkRoot 'platform-tools\adb.exe'))
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        foreach ($relative in @(
            'Android\Sdk\platform-tools\adb.exe', 'SideQuest\platform-tools\adb.exe',
            'Programs\SideQuest\resources\app.asar.unpacked\build\platform-tools\adb.exe'
        )) { $candidates.Add((Join-Path $env:LOCALAPPDATA $relative)) }
    }
    if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) {
        $candidates.Add((Join-Path $env:APPDATA 'SideQuest\platform-tools\adb.exe'))
    }
    $command = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($command) { $candidates.Add($command.Source) }
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $script:ResolvedAdb = [System.IO.Path]::GetFullPath($candidate)
        $probe = Invoke-Adb -Arguments @('version') -AllowFailure -Probe
        if ($probe.ExitCode -eq 0 -and $probe.Output.Count -gt 0) { return }
    }
    throw 'adb.exe was not found. Install Android Platform Tools / SideQuest, or run with -AdbPath "C:\path\to\adb.exe". The public kit .tools\platform-tools folder is also supported.'
}

function Save-Capture {
    param([string]$Name, [string[]]$Arguments, [string]$Pattern = '', [int]$MaxLines = 0)
    Write-Host "Reading $Name ..."
    $result = Invoke-Adb -Arguments (@('-s', $Serial) + $Arguments) -AllowFailure
    $lines = @($result.Output)
    if ($Pattern.Length -gt 0) { $lines = @($lines | Where-Object { $_ -match $Pattern }) }
    if ($MaxLines -gt 0 -and $lines.Count -gt $MaxLines) {
        $lines = @($lines | Select-Object -First $MaxLines)
        $script:Warnings.Add("$Name was limited to $MaxLines lines.")
    }
    if ($result.ExitCode -ne 0) {
        $script:Warnings.Add("$Name unavailable or incomplete (adb exit $($result.ExitCode)); see commands.json.")
    }
    Write-Utf8 -Path (Join-Path $bundleRoot $Name) -Text ($lines -join "`r`n")
    return $result
}

Write-Host 'SAVR render report collector 1.0' -ForegroundColor Cyan
Write-Host 'Read-only: leave the game at the black screen, with the headset awake.'
Find-Adb
$devicesResult = Invoke-Adb -Arguments @('devices', '-l')
$devices = @(
    foreach ($line in $devicesResult.Output) {
        if ($line -match '^(\S+)\s+(device|unauthorized|offline)(?:\s|$)') {
            [pscustomobject]@{ Serial = $Matches[1]; State = $Matches[2] }
        }
    }
)
$authorized = @($devices | Where-Object { $_.State -eq 'device' })
if ([string]::IsNullOrWhiteSpace($Serial)) {
    if ($authorized.Count -eq 0) {
        throw 'No authorized Quest found. Connect USB, enable developer mode and accept USB debugging inside the headset. Check adb devices for unauthorized/offline devices.'
    }
    if ($authorized.Count -gt 1) {
        throw "Multiple authorized devices: $(($authorized | ForEach-Object { $_.Serial }) -join ', '). Run again with -Serial <serial>."
    }
    $Serial = $authorized[0].Serial
} elseif (-not ($authorized | Where-Object { $_.Serial -eq $Serial })) {
    throw "Device '$Serial' is not connected and authorized. Check adb devices and the USB debugging prompt."
}

$startedUtc = [DateTimeOffset]::UtcNow
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    if ([string]::IsNullOrWhiteSpace($desktop)) { $desktop = Split-Path -Parent $PSScriptRoot }
    $OutputDirectory = Join-Path $desktop 'SAVR Render Reports'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$bundleName = 'SAVR_Render_' + $startedUtc.ToString('yyyyMMdd-HHmmssfff') + '_' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$bundleRoot = Join-Path $OutputDirectory $bundleName
$filesRoot = Join-Path $bundleRoot 'game-files'
$null = New-Item -ItemType Directory -Path $filesRoot -Force
$zipPath = Join-Path $OutputDirectory ($bundleName + '.zip')

# Query all properties, but retain only this small allowlist. Device serials,
# network properties and account-related values are not deliberately collected.
$propertyPattern = '^\[(?:debug\.(?:savr|oculus)\.[^\]]+|ro\.(?:product\.(?:model|device|manufacturer|brand|board)|build\.(?:fingerprint|display\.id|version\.(?:release|sdk|incremental|security_patch))|hardware(?:\.(?:egl|vulkan))?|board\.platform|soc\.(?:manufacturer|model)|opengles\.version)|vendor\.(?:gpu|qti\.gpu)\.[^\]]+)\]:'
$properties = Save-Capture -Name 'device-properties.txt' -Arguments @('shell', 'getprop') -Pattern $propertyPattern
$null = Save-Capture -Name 'device-time.txt' -Arguments @('shell', 'date', '+%Y-%m-%dT%H:%M:%S%z')
$package = Save-Capture -Name 'package.txt' -Arguments @('shell', 'dumpsys', 'package', $PackageName) -MaxLines 1200
$running = Save-Capture -Name 'game-pid.txt' -Arguments @('shell', 'pidof', $PackageName)
$null = Save-Capture -Name 'memory.txt' -Arguments @('shell', 'dumpsys', 'meminfo', $PackageName) -MaxLines 400

$logPattern = '(?i)(\bSAVR\b|savr\.|libsavr|com\.rockstargames\.gtasa|OpenXR|\bxr[A-Z]\w+|VrApi|VrRuntime|Oculus|\bEGL\b|libEGL|GLES|OpenGL|Adreno|kgsl|vulkan|GpuFault|GPU fault|Fatal signal|Abort message|tombstoned|lowmemorykiller|lmkd)'
$null = Save-Capture -Name 'logcat-render.txt' -Arguments @('logcat', '-d', '-b', 'main', '-b', 'system', '-v', 'threadtime', '-t', '20000') -Pattern $logPattern
$null = Save-Capture -Name 'logcat-crash.txt' -Arguments @('logcat', '-d', '-b', 'crash', '-v', 'threadtime', '-t', '4000')

$copied = New-Object 'System.Collections.Generic.List[object]'
$missing = New-Object 'System.Collections.Generic.List[string]'
$names = New-Object 'System.Collections.Generic.List[string]'
foreach ($name in @('savr_debug.txt', 'savr_profile.csv')) { $names.Add($name) }
$listing = Invoke-Adb -Arguments @('-s', $Serial, 'shell', 'ls', '-1', $RemoteRoot) -AllowFailure
if ($listing.ExitCode -ne 0) {
    $script:Warnings.Add('Could not list the game settings directory; trying the known settings filenames.')
    foreach ($name in @('vr_appearance.ini', 'vr_basketball.ini', 'vr_calib.ini', 'vr_driving.ini', 'vr_graphics.ini', 'vr_holsters.ini', 'vr_hud.ini', 'vr_locomotion.ini')) { $names.Add($name) }
} else {
    foreach ($name in $listing.Output) {
        # Only plain filenames from this directory are accepted. Never pull a
        # directory, save, APK, nested path or arbitrary device file.
        if ($name -cmatch '^vr_[A-Za-z0-9_]+\.ini$') { $names.Add($name) }
    }
}
foreach ($name in @($names | Select-Object -Unique)) {
    $remote = "$RemoteRoot/$name"
    $exists = Invoke-Adb -Arguments @('-s', $Serial, 'shell', 'test', '-f', $remote) -AllowFailure
    if ($exists.ExitCode -ne 0) { $missing.Add($name); continue }
    Write-Host "Copying $name ..."
    $destination = Join-Path $filesRoot $name
    $pull = Invoke-Adb -Arguments @('-s', $Serial, 'pull', $remote, $destination) -AllowFailure
    if ($pull.ExitCode -eq 0 -and (Test-Path -LiteralPath $destination -PathType Leaf)) {
        $copied.Add([pscustomobject]@{
            path = 'game-files/' + $name
            bytes = (Get-Item -LiteralPath $destination).Length
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    } else {
        $script:Warnings.Add("Failed to copy $name (adb exit $($pull.ExitCode)); a partial local file may be present.")
    }
}

$finishedUtc = [DateTimeOffset]::UtcNow
$modelLine = @($properties.Output | Where-Object { $_ -match '^\[ro\.product\.model\]:' })
$model = if ($modelLine.Count -gt 0) { $modelLine[0] -replace '^\[ro\.product\.model\]: \[(.*)\]$', '$1' } else { 'unknown' }
$packageText = $package.Output -join "`n"
$versionMatch = [regex]::Match($packageText, '(?m)^\s*versionName=(\S+)')
$gameRunning = $running.ExitCode -eq 0 -and ($running.Output -join '') -match '\d+'
$summary = @(
    'SAVR RENDER REPORT', '',
    "Collection started UTC: $($startedUtc.ToString('o'))",
    "Collection finished UTC: $($finishedUtc.ToString('o'))",
    "PC local time at finish: $([DateTimeOffset]::Now.ToString('o'))",
    "Device model: $model",
    "Android package versionName: $(if ($versionMatch.Success) { $versionMatch.Groups[1].Value } else { 'unknown' })",
    'The Android package version can differ from the mod version. Check SAVR build/version lines in the logs.',
    "Game process found: $gameRunning", '',
    'Please tell the developer: mod version; Quest model; new game or loaded save;',
    'the exact moment the picture disappeared; whether menus/HUD and CJ movement still work;',
    'and whether Quest Games Optimizer or other graphics tuning was active.', '',
    'Coverage: last 20,000 main/system logcat lines, filtered for game/graphics/runtime',
    'diagnostics, plus last 4,000 crash-buffer lines. No log buffers were cleared.',
    'Logs are a snapshot of retained history and can contain earlier runs or other apps.',
    'Older release builds can compile out SAVR logging. Empty logs or missing files',
    'do not prove that the renderer is healthy. A diagnostic build may be needed.',
    'Files were copied once while the game may be running; they are not an atomic snapshot.',
    'getprop GPU fields are device-reported metadata; OpenGL renderer/capabilities require game logs.', '',
    'The collector does not launch/stop the game, install anything, change properties',
    'or settings, clear logs, or upload the report. No saves, APKs or game assets are requested.',
    'Review the report before sharing: logs can contain paths or other app diagnostics.', '',
    "Copied files: $($copied.Count)",
    "Missing or inaccessible optional files: $($missing -join ', ')",
    "Collection warnings: $($script:Warnings.Count)"
) + @($script:Warnings | ForEach-Object { '- ' + $_ })
Write-Utf8 -Path (Join-Path $bundleRoot 'README_REPORT.txt') -Text ($summary -join "`r`n")
Write-Utf8 -Path (Join-Path $bundleRoot 'commands.json') -Text (ConvertTo-Json -InputObject @($script:Commands.ToArray()) -Depth 4)
$manifest = [ordered]@{
    format = 'savr-render-report'; format_version = 1; collector_version = '1.0'
    collected_started_utc = $startedUtc.ToString('o'); collected_finished_utc = $finishedUtc.ToString('o')
    package = $PackageName; device_model = $model; game_running = [bool]$gameRunning
    files = @($copied.ToArray()); missing_optional_files = @($missing.ToArray())
    warnings = @($script:Warnings.ToArray()); device_actions = 'read-only'; uploads = $false
}
Write-Utf8 -Path (Join-Path $bundleRoot 'manifest.json') -Text ($manifest | ConvertTo-Json -Depth 5)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($bundleRoot, $zipPath)
$archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    if (-not ($archive.Entries.FullName -contains 'README_REPORT.txt') -or -not ($archive.Entries.FullName -contains 'manifest.json')) {
        throw 'ZIP verification failed: report summary is missing.'
    }
} finally { $archive.Dispose() }
Write-Host ''
Write-Host "Report saved ($($script:Warnings.Count) collection warnings):" -ForegroundColor Green
Write-Host $zipPath -ForegroundColor Yellow
Write-Host "Reviewable folder: $bundleRoot"
Write-Host 'Send the ZIP and a short description of the failure to the mod developer.'
