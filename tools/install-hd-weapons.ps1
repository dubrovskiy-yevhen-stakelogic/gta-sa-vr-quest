# Build the HD weapon model payload from a downloaded pack and push it to a
# connected Quest. Reuses the Python and adb that BUILD_AND_INSTALL already
# downloaded. Nothing here modifies the game APK — the payload lives in the
# app's external files dir and the mod loads it when WEAPON MODELS is set to HD.
param(
    [string]$Archive,                 # .zip / .7z pack (prompted if omitted)
    [string]$WorkDir = 'C:\SAVRBuild', # same default as BUILD_AND_INSTALL
    [string]$Serial                    # adb serial (auto if a single device)
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ToolsRoot = Join-Path $WorkDir '.tools'

function Find-Tool([string]$underTools, [string]$onPath) {
    $p = Join-Path $ToolsRoot $underTools
    if (Test-Path -LiteralPath $p) { return $p }
    $c = Get-Command $onPath -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

$Python = Find-Tool 'python-3.12.10-embed-amd64\python.exe' 'python'
$Adb    = Find-Tool 'platform-tools-37.0.1\platform-tools\adb.exe' 'adb'
$SevenZip = Join-Path $ToolsRoot '7zr-26.02.exe'

if (-not $Python -or -not $Adb) {
    Write-Host ''
    Write-Host 'Python or adb was not found. Run BUILD_AND_INSTALL.bat once first'
    Write-Host '(it downloads them), then run this again.' -ForegroundColor Yellow
    exit 1
}

if (-not $Archive) {
    Write-Host ''
    Write-Host 'Drag the downloaded HD weapons archive here and press Enter'
    Write-Host '(a .zip or .7z that contains the weapon .dff files):'
    $Archive = (Read-Host '  archive').Trim('"').Trim()
}
if (-not (Test-Path -LiteralPath $Archive)) { throw "Archive not found: $Archive" }

# Pick the target Quest.
if (-not $Serial) {
    $devices = & $Adb devices |
        Where-Object { $_ -match "`tdevice$" } |
        ForEach-Object { ($_ -split "`t")[0] }
    if (-not $devices) { throw 'No Quest detected. Connect it and allow USB debugging.' }
    if ($devices.Count -gt 1) { throw "More than one device; pass -Serial. Found: $($devices -join ', ')" }
    $Serial = $devices[0]
}
Write-Host "Target device: $Serial"

# Extract the pack.
$Stage = Join-Path ([IO.Path]::GetTempPath()) ("hdw-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
try {
    $ext = [IO.Path]::GetExtension($Archive).ToLowerInvariant()
    Write-Host 'Extracting the pack...'
    if ($ext -eq '.zip') {
        Expand-Archive -LiteralPath $Archive -DestinationPath $Stage -Force
    } elseif (Test-Path -LiteralPath $SevenZip) {
        & $SevenZip x "-o$Stage" -y -- $Archive | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip could not extract $Archive" }
    } else {
        throw "Cannot extract $ext here. Unzip it yourself and pass the folder to build_hdweapons.py, or run BUILD_AND_INSTALL to fetch 7-Zip."
    }

    # Build the payload (image + loose-PNG texture database).
    Write-Host 'Building the weapon image and textures...'
    $Payload = Join-Path $Stage '_payload'
    & $Python (Join-Path $RepoRoot 'tools\build_hdweapons.py') $Stage --out $Payload
    if ($LASTEXITCODE -ne 0) { throw 'build_hdweapons.py failed.' }

    $img = Join-Path $Payload 'hdweapons\hdweapons.img'
    $txt = Join-Path $Payload 'texdb\hdweapons\hdweapons.txt'
    $src = Join-Path $Payload 'texdb\hdweapons\src'
    if (-not (Test-Path -LiteralPath $img)) { throw 'No weapon image was produced; is this the right pack?' }

    # Push to the app's files dir.
    $data = '/sdcard/Android/data/com.rockstargames.gtasa/files'
    Write-Host 'Copying to the headset...'
    & $Adb -s $Serial shell "mkdir -p $data/hdweapons $data/texdb/hdweapons/src" | Out-Null
    & $Adb -s $Serial push $img "$data/hdweapons/hdweapons.img"
    & $Adb -s $Serial push $txt "$data/texdb/hdweapons/hdweapons.txt"
    & $Adb -s $Serial push $src "$data/texdb/hdweapons/"

    Write-Host ''
    Write-Host 'HD weapons installed.' -ForegroundColor Green
    Write-Host 'In the headset: open the VR menu -> GRAPHICS -> set WEAPON MODELS to HD,'
    Write-Host 'then fully restart the game. Set it back to ORIGINAL any time to undo.'
} finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $Stage
}
