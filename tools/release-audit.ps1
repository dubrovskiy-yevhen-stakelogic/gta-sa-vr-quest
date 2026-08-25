[CmdletBinding()]
param(
    [string]$Root,
    [string]$PythonExe,
    [string]$BashExe
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Path $PSScriptRoot -Parent
}
$Root = [System.IO.Path]::GetFullPath($Root)

$forbiddenDirectoryNames = @(
    'recon', 'deps', 'dist', 'out', '__pycache__',
    '.agents', '.cache', '.codex', '.gradle', '.idea', '.vscode'
)
$forbiddenExtensions = @(
    '.apk', '.apks', '.xapk', '.apkm', '.aab', '.aar', '.obb',
    '.so', '.dex', '.idsig', '.osw', '.idx',
    '.keystore', '.jks', '.p12', '.pfx', '.pem', '.key',
    '.zip', '.7z', '.rar', '.tar', '.gz', '.xz', '.bz2', '.tgz',
    '.log', '.csv', '.trace', '.perfetto-trace', '.tombstone',
    '.pyc', '.o', '.obj', '.a', '.lib', '.exe', '.dll', '.dylib',
    '.class', '.jar', '.wasm', '.bin'
)
$forbiddenFileNames = @('.env', 'local.properties', 'release-signing.properties')

$allItems = @(Get-ChildItem -LiteralPath $Root -Force -Recurse)
$badDirectories = @(
    $allItems | Where-Object {
        $_.PSIsContainer -and (
            $forbiddenDirectoryNames -contains $_.Name.ToLowerInvariant() -or
            $_.Name -like 'build*'
        )
    }
)
if ($badDirectories.Count -gt 0) {
    throw "Forbidden release directories found:`n$($badDirectories.FullName -join "`n")"
}

$files = @($allItems | Where-Object { -not $_.PSIsContainer })
$badFiles = @(
    $files | Where-Object {
        $forbiddenExtensions -contains $_.Extension.ToLowerInvariant() -or
        $forbiddenFileNames -contains $_.Name.ToLowerInvariant()
    }
)
if ($badFiles.Count -gt 0) {
    throw "Forbidden release files found:`n$($badFiles.FullName -join "`n")"
}

$audioReference = Get-Content -LiteralPath (Join-Path $Root 'tools\audio-reference.json') -Raw |
    ConvertFrom-Json
if ($audioReference.fileCount -ne 59 -or $audioReference.files.Count -ne 59) {
    throw 'audio-reference.json must describe exactly 59 files.'
}
$audioBytes = ($audioReference.files | Measure-Object -Property size -Sum).Sum
if ($audioBytes -ne 1218349029 -or $audioReference.totalBytes -ne $audioBytes) {
    throw "audio-reference.json byte total mismatch: $audioBytes"
}

$expectedHands = @{
    'BigHandLeft.uxrh' = '6BB32E6F79ED6DBA401BD36DD39D3178F8259096F889EF01C870D057134EB535'
    'BigHandRight.uxrh' = '48B14A77EFEA68027F5AE6723994CADC06DAC58708ECB7339CC16ECB1ADE2478'
    'BigHandsAlbedo.rgba' = '46BA8B2F08CE1CA420537E060F64BE139DD39A22357D4EA5B9B7B69DA2BA2AD4'
}
foreach ($entry in $expectedHands.GetEnumerator()) {
    $path = Join-Path (Join-Path $Root 'assets\vrhands') $entry.Key
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($actual -ne $entry.Value) {
        throw "VR hand asset hash mismatch: $($entry.Key)"
    }
}

$xrSource = Get-Content -LiteralPath (Join-Path $Root 'native\src\Xr.cpp') -Raw
$versionMatch = [regex]::Match($xrSource, 'kModVersion\[\]\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"')
if (-not $versionMatch.Success) { throw 'Could not read kModVersion from Xr.cpp.' }
$modVersion = $versionMatch.Groups[1].Value
$readmeText = Get-Content -LiteralPath (Join-Path $Root 'README.md') -Raw
$snapshotText = Get-Content -LiteralPath (Join-Path $Root 'SOURCE_SNAPSHOT.md') -Raw
if ($readmeText -notmatch ('Version `' + [regex]::Escape($modVersion) + ' alpha`')) {
    throw "README version does not match Xr.cpp: $modVersion"
}
if ($snapshotText -notmatch ('Mod version: `' + [regex]::Escape($modVersion) + ' alpha`')) {
    throw "SOURCE_SNAPSHOT version does not match Xr.cpp: $modVersion"
}
$documentedActiveSourceManifest = '6861B4187AE1B27D3A8579CBD310E4EBDA024B6A4AE4C6BE3B0E9177007A24A4'
if ($snapshotText -notmatch [regex]::Escape($documentedActiveSourceManifest)) {
    throw 'SOURCE_SNAPSHOT active-tree provenance marker is stale.'
}
$discordChannel = 'https://discord.com/channels/747967102895390741/1540234546182750228'
if ($readmeText -notmatch [regex]::Escape($discordChannel)) {
    throw 'The public Discord channel is missing from README.'
}
$exportToolFiles = @('EXPORT_PLAY_APKS.bat', 'tools\export-play-apks.ps1')
foreach ($relative in $exportToolFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $relative) -PathType Leaf)) {
        throw "The one-click Play APK exporter is incomplete: $relative"
    }
}
if ($readmeText -notmatch [regex]::Escape('EXPORT_PLAY_APKS.bat')) {
    throw 'README does not document the one-click Play APK exporter.'
}

$nativeRoot = Join-Path $Root 'native'
$nativeSourceRoot = Join-Path $nativeRoot 'src'
$cmakeText = Get-Content -LiteralPath (Join-Path $nativeRoot 'CMakeLists.txt') -Raw
$listedCompileUnits = @(
    [regex]::Matches(
        $cmakeText,
        '(?m)^\s*src/([A-Za-z0-9_.-]+\.(?:cpp|S))\s*$'
    ) | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
)
$actualCompileUnits = @(
    Get-ChildItem -LiteralPath $nativeSourceRoot -File |
        Where-Object { $_.Extension.ToLowerInvariant() -in @('.cpp', '.s') } |
        ForEach-Object { $_.Name } | Sort-Object -Unique
)
$missingCompileUnits = @(
    $listedCompileUnits | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $nativeSourceRoot $_) -PathType Leaf)
    }
)
$unlistedCompileUnits = @(
    $actualCompileUnits | Where-Object { $listedCompileUnits -notcontains $_ }
)
if ($missingCompileUnits.Count -gt 0 -or $unlistedCompileUnits.Count -gt 0) {
    throw "Native CMake source closure failed.`nMissing:`n$($missingCompileUnits -join "`n")`nUnlisted:`n$($unlistedCompileUnits -join "`n")"
}

$localSourceFiles = @(
    Get-ChildItem -LiteralPath $nativeSourceRoot -File |
        Where-Object { $_.Extension.ToLowerInvariant() -in @('.cpp', '.h', '.s') }
)
foreach ($sourceFile in $localSourceFiles) {
    $sourceText = Get-Content -LiteralPath $sourceFile.FullName -Raw
    foreach ($include in [regex]::Matches($sourceText, '(?m)^\s*#include\s+"([^"]+)"')) {
        $includePath = Join-Path $nativeSourceRoot $include.Groups[1].Value
        if (-not (Test-Path -LiteralPath $includePath -PathType Leaf)) {
            throw "Missing local include in $($sourceFile.Name): $($include.Groups[1].Value)"
        }
    }
}

foreach ($script in Get-ChildItem -LiteralPath (Join-Path $Root 'tools') -Filter '*.ps1') {
    [void][scriptblock]::Create((Get-Content -LiteralPath $script.FullName -Raw))
}

if ([string]::IsNullOrWhiteSpace($BashExe)) {
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if ($null -ne $bash) { $BashExe = $bash.Source }
    elseif ($env:OS -eq 'Windows_NT') {
        foreach ($candidate in @(
            (Join-Path $env:ProgramFiles 'Git\bin\bash.exe'),
            (Join-Path $env:ProgramFiles 'Git\usr\bin\bash.exe'),
            (Join-Path $env:LOCALAPPDATA 'Programs\Git\bin\bash.exe')
        )) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $BashExe = $candidate
                break
            }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($BashExe) -or
    -not (Test-Path -LiteralPath $BashExe -PathType Leaf)) {
    throw 'Bash was not found; pass -BashExe so every shipped shell script can be syntax-checked.'
}
foreach ($script in Get-ChildItem -LiteralPath $Root -Filter '*.sh' -File -Recurse) {
    $bashPath = $script.FullName
    if ($env:OS -eq 'Windows_NT' -and $bashPath -match '^([A-Za-z]):\\(.*)$') {
        $bashPath = '/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2].Replace('\', '/')
    }
    & $BashExe -n $bashPath
    if ($LASTEXITCODE -ne 0) { throw "Bash syntax check failed: $($script.FullName)" }
}

if ([string]::IsNullOrWhiteSpace($PythonExe)) {
    foreach ($pythonName in @('python', 'python3')) {
        $python = Get-Command $pythonName -ErrorAction SilentlyContinue
        if ($null -ne $python) {
            $PythonExe = $python.Source
            break
        }
    }
}
if ([string]::IsNullOrWhiteSpace($PythonExe)) {
    throw 'Python was not found; pass -PythonExe so every shipped Python script can be parsed.'
}
foreach ($script in Get-ChildItem -LiteralPath (Join-Path $Root 'tools') -Filter '*.py') {
    & $PythonExe -c 'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_bytes().decode(), filename=sys.argv[1])' $script.FullName
    if ($LASTEXITCODE -ne 0) { throw "Python syntax check failed: $($script.FullName)" }
}

$shippedScripts = @(
    $files | Where-Object { $_.Extension.ToLowerInvariant() -in @('.ps1', '.sh', '.bat') }
)
$scriptText = ($shippedScripts | ForEach-Object {
    Get-Content -LiteralPath $_.FullName -Raw
}) -join "`n"
$normalizedScriptText = (($scriptText -replace '[\x27\x22`,()@]', ' ') -replace '\s+', ' ')
if ($normalizedScriptText -match '(?i)\bam\s+start(?:service)?\b|\bmonkey\b') {
    throw 'A shipped script contains an application launch command.'
}

$resetFiles = @(
    'RESET_VR_SETTINGS.bat', 'RESET_VR_SETTINGS.sh',
    'tools\reset-vr-settings.ps1', 'tools\reset-vr-settings.sh'
)
$resetText = @()
foreach ($relative in $resetFiles) {
    $path = Join-Path $Root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "VR-settings reset tool is incomplete: $relative"
    }
    $resetText += Get-Content -LiteralPath $path -Raw
}
$resetText = $resetText -join "`n"
$expectedSettings = @(
    'vr_appearance.ini', 'vr_calib.ini', 'vr_calib.ini.tmp',
    'vr_driving.ini', 'vr_graphics.ini', 'vr_holsters.ini',
    'vr_hud.ini', 'vr_locomotion.ini'
)
$nativeSettings = @(
    Get-ChildItem -LiteralPath (Join-Path $Root 'native\src') -Filter '*.cpp' -File |
        ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw } |
        ForEach-Object { [regex]::Matches($_, '/files/(vr_[a-z0-9_.-]+)') } |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique
)
$expectedSorted = @($expectedSettings | Sort-Object -Unique)
if (($nativeSettings -join "`n") -ne ($expectedSorted -join "`n")) {
    throw "Native VR-settings list differs from the audited reset list.`nNative:`n$($nativeSettings -join "`n")"
}
foreach ($relative in @('tools\reset-vr-settings.ps1', 'tools\reset-vr-settings.sh')) {
    $implementation = Get-Content -LiteralPath (Join-Path $Root $relative) -Raw
    $implementedSettings = @(
        [regex]::Matches($implementation, '\bvr_[a-z0-9_.-]+') |
            ForEach-Object { $_.Value } |
            Sort-Object -Unique
    )
    if (($implementedSettings -join "`n") -ne ($expectedSorted -join "`n")) {
        throw "VR-settings reset list is stale in ${relative}."
    }
}
$normalizedResetText = (($resetText -replace '[\x27\x22`,()@]', ' ') -replace '\s+', ' ')
if ($normalizedResetText -match '(?i)\bpm\s+clear\b|\buninstall\b|vr_\*') {
    throw 'VR-settings reset tool contains an unsafe broad-removal operation.'
}

$textExtensions = @(
    '.md', '.txt', '.json', '.py', '.ps1', '.sh', '.bat',
    '.cpp', '.h', '.s', '.java', '.cmake'
)
$textFileNames = @('CMakeLists.txt', 'LICENSE', '.gitattributes', '.gitignore')
$textFiles = @($files | Where-Object {
    $textExtensions -contains $_.Extension.ToLowerInvariant() -or
    $textFileNames -contains $_.Name
})
$forbiddenTextPatterns = @(
    '(?i)\bC:\\Dev\\', '(?i)\b[A-Z]:\\Users\\[^\\\s]+\\',
    '(?i)\bH:\\', '(?i)/Users/[^/\s]+/',
    '(?i)/home/[^/\s]+/', '(?i)promptPreview|ChatGPT|Codex|Claude',
    '(?i)\bOpenAI\b|\bGemini\b|\bCopilot\b|generated[- ]by(?: an? )?(?:AI|LLM)',
    '(?i)another agent|concurrently-owned|see memory|qbuild',
    '(?m)^(<<<<<<<|=======|>>>>>>>)',
    '-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    '\bAKIA[0-9A-Z]{16}\b', '\bgh[pousr]_[A-Za-z0-9_]{20,}\b'
)
foreach ($file in $textFiles) {
    if ($file.FullName -eq $MyInvocation.MyCommand.Path) { continue }
    $content = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($pattern in $forbiddenTextPatterns) {
        if ($content -match $pattern) {
            throw "Forbidden private, AI-session, conflict, or secret marker in $($file.FullName): $($Matches[0])"
        }
    }
}

$totalBytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host "Source-kit audit passed: $($files.Count) files, $totalBytes bytes" -ForegroundColor Green
