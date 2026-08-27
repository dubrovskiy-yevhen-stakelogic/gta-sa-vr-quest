# Build the HD weapon model payload from a downloaded pack and push it to a
# connected Quest. Reuses the Python, adb and (if present) 7-Zip that
# BUILD_AND_INSTALL already downloaded. Nothing here modifies the game APK --
# the payload lives in the app's external files dir and the mod loads it when
# GRAPHICS -> WEAPON MODELS is set to HD.
#
# The pack can be an archive OR an already-extracted folder. If the pack is a
# .7z/.rar and no 7-Zip is on the machine, the script asks you to extract it
# yourself and drag the FOLDER in -- that path always works and needs no extra
# tools.
param(
    [string]$Archive,                  # .zip / .7z / .rar pack OR an already-extracted folder (prompted if omitted)
    [string]$WorkDir = 'C:\SAVRBuild', # same default as BUILD_AND_INSTALL
    [string]$Serial                    # adb serial (auto if a single device)
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$RepoRoot  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ToolsRoot = Join-Path $WorkDir '.tools'
# BUILD_AND_INSTALL scatters tools: adb/python land under .tools, but 7-Zip
# (only fetched when its own game/audio input was a .7z) lands under .downloads.
# Search both so a tool is found wherever the master happened to put it.
$SearchRoots = @($ToolsRoot, (Join-Path $WorkDir '.downloads')) |
               Where-Object { Test-Path -LiteralPath $_ }

# --- Tool discovery -------------------------------------------------------
function Find-Tool {
    param([string[]]$Globs, [string[]]$OnPath)
    foreach ($g in $Globs) {
        foreach ($root in $SearchRoots) {
            $hit = Get-ChildItem -LiteralPath $root -Recurse -Filter $g -ErrorAction SilentlyContinue |
                   Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    foreach ($n in $OnPath) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
    }
    return $null
}

# A 7-Zip capable of .7z, if one is already on the machine (BUILD_AND_INSTALL's
# 7zr under .downloads, or a system install). Returns $null if none.
function Find-SevenZip {
    $found = Find-Tool -Globs @('7z.exe','7za.exe','7zr*.exe') -OnPath @('7z','7za','7zr')
    if ($found) { return $found }
    foreach ($p in @(
        "$env:ProgramFiles\7-Zip\7z.exe",
        "${env:ProgramFiles(x86)}\7-Zip\7z.exe")) {
        if ($p -and (Test-Path -LiteralPath $p)) { return $p }
    }
    return $null
}

$Python = Find-Tool -Globs @('python.exe') -OnPath @('python','py')
$Adb    = Find-Tool -Globs @('adb.exe')    -OnPath @('adb')

if (-not $Python) {
    Write-Host ''
    Write-Host 'Python was not found. Run BUILD_AND_INSTALL.bat once first, then retry.' -ForegroundColor Yellow
    exit 1
}
if (-not $Adb) {
    Write-Host ''
    Write-Host 'adb was not found. Run BUILD_AND_INSTALL.bat once first, then retry.' -ForegroundColor Yellow
    exit 1
}

Write-Host "Python: $Python"
Write-Host "adb:    $Adb"

# --- Pack input -----------------------------------------------------------
if (-not $Archive) {
    Write-Host ''
    Write-Host 'Drag the downloaded HD weapons pack here and press Enter.'
    Write-Host 'It can be the .zip / .7z / .rar archive, OR an already-extracted folder:'
    $Archive = (Read-Host '  pack').Trim().Trim('"').Trim()
}
if (-not (Test-Path -LiteralPath $Archive)) { throw "Pack not found: $Archive" }

# --- Device selection (unlock / authorization aware) ----------------------
function Get-AdbDevices {
    $lines = & $Adb devices 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Could not run adb devices.`n$($lines -join "`n")" }
    $result = @()
    foreach ($line in $lines) {
        if ($line -match '^(?<serial>\S+)\s+(?<state>device|unauthorized|offline)$') {
            $result += [PSCustomObject]@{ Serial = $Matches.serial; State = $Matches.state }
        }
    }
    return $result
}

if (-not $Serial) {
    $devices = @(Get-AdbDevices)
    $ready   = @($devices | Where-Object { $_.State -eq 'device' })
    if ($ready.Count -eq 0) {
        if ($devices.Count -gt 0) {
            $details = ($devices | ForEach-Object { "$($_.Serial) [$($_.State)]" }) -join ', '
            throw "No usable ADB device. Current: $details`nUnlock the Quest, accept the USB-debugging prompt, and retry."
        }
        throw 'No Quest detected. Connect it by USB, unlock it, and allow USB debugging.'
    }
    if ($ready.Count -eq 1) {
        $Serial = $ready[0].Serial
    } else {
        Write-Host ''
        Write-Host 'Multiple ADB devices detected:' -ForegroundColor Yellow
        for ($i = 0; $i -lt $ready.Count; $i++) { Write-Host "  [$($i + 1)] $($ready[$i].Serial)" }
        $choice = Read-Host 'Select the Quest to use (number)'
        if ($choice -notmatch '^\d+$' -or [int]$choice -lt 1 -or [int]$choice -gt $ready.Count) {
            throw 'Invalid device selection.'
        }
        $Serial = $ready[[int]$choice - 1].Serial
    }
}
Write-Host "Target device: $Serial"

$state = (& $Adb -s $Serial get-state 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $state -ne 'device') {
    throw "ADB device '$Serial' is not reachable (get-state: $state). Reconnect and authorize USB debugging."
}

$Stage = Join-Path ([IO.Path]::GetTempPath()) ("hdw-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
try {
    # --- Get a folder of .dff/.png (extract if the input is an archive) ----
    if (Test-Path -LiteralPath $Archive -PathType Container) {
        Write-Host 'Using the extracted folder as-is.'
        $PackRoot = $Archive
    } else {
        $ext = [IO.Path]::GetExtension($Archive).ToLowerInvariant()
        Write-Host "Extracting the pack ($ext)..."
        if ($ext -eq '.zip') {
            try {
                Expand-Archive -LiteralPath $Archive -DestinationPath $Stage -Force
            } catch {
                $sz = Find-SevenZip
                if (-not $sz) { throw "Could not extract $Archive. Extract it yourself and drag the FOLDER here instead." }
                & $sz x "-o$Stage" -y -- $Archive | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "Could not extract $Archive. Extract it yourself and drag the FOLDER here instead." }
            }
        } else {
            $sz = Find-SevenZip
            if (-not $sz) {
                throw "No 7-Zip is installed to open $ext. Extract the pack yourself (right-click -> extract) and run this again, dragging the extracted FOLDER in instead of the archive."
            }
            & $sz x "-o$Stage" -y -- $Archive | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "7-Zip could not extract $Archive (a .rar may need the full 7-Zip). Extract the pack yourself and drag the extracted FOLDER in instead."
            }
        }
        $PackRoot = $Stage
    }

    $dffCount = @(Get-ChildItem -LiteralPath $PackRoot -Recurse -Filter *.dff -ErrorAction SilentlyContinue).Count
    if ($dffCount -eq 0) {
        throw "No .dff weapon models were found in the pack. Is this the right archive/folder?"
    }
    Write-Host "Found $dffCount weapon model(s)."

    # --- Build the payload (image + loose-PNG texture database) ------------
    Write-Host 'Building the weapon image and textures...'
    $Payload = Join-Path $Stage '_payload'
    & $Python (Join-Path $RepoRoot 'tools\build_hdweapons.py') $PackRoot --out $Payload
    if ($LASTEXITCODE -ne 0) { throw 'build_hdweapons.py failed.' }

    $img = Join-Path $Payload 'hdweapons\hdweapons.img'
    $txt = Join-Path $Payload 'texdb\hdweapons\hdweapons.txt'
    $src = Join-Path $Payload 'texdb\hdweapons\src'
    if (-not (Test-Path -LiteralPath $img)) { throw 'No weapon image was produced; is this the right pack?' }

    # --- Push to the app's files dir --------------------------------------
    $data = '/sdcard/Android/data/com.rockstargames.gtasa/files'
    Write-Host 'Copying to the headset...'

    & $Adb -s $Serial shell "mkdir -p $data/hdweapons $data/texdb/hdweapons/src"
    if ($LASTEXITCODE -ne 0) { throw "Could not create the HD weapons folders on '$Serial'. Is the game installed on this headset?" }

    & $Adb -s $Serial push $img "$data/hdweapons/hdweapons.img"
    if ($LASTEXITCODE -ne 0) { throw "Failed to copy hdweapons.img to '$Serial'." }

    & $Adb -s $Serial push $txt "$data/texdb/hdweapons/hdweapons.txt"
    if ($LASTEXITCODE -ne 0) { throw "Failed to copy hdweapons.txt to '$Serial'." }

    & $Adb -s $Serial push $src "$data/texdb/hdweapons/"
    if ($LASTEXITCODE -ne 0) { throw "Failed to copy HD weapon textures to '$Serial'." }

    # --- Verify the payload actually landed (drives the in-game
    #     'WEAPON MODELS < NO FILES >' check) --------------------------------
    $imgStat = (& $Adb -s $Serial shell "ls -l $data/hdweapons/hdweapons.img 2>/dev/null" | Out-String).Trim()
    $txtStat = (& $Adb -s $Serial shell "ls -l $data/texdb/hdweapons/hdweapons.txt 2>/dev/null" | Out-String).Trim()
    if (-not $imgStat -or -not $txtStat) {
        throw "Verification failed: the files are not on the headset after the copy. Check free space and that the game is installed for THIS user profile."
    }
    Write-Host ''
    Write-Host 'Verified on headset:' -ForegroundColor Green
    Write-Host "  $imgStat"
    Write-Host "  $txtStat"

    Write-Host ''
    Write-Host 'HD weapons installed.' -ForegroundColor Green
    Write-Host 'In the headset:' -ForegroundColor Green
    Write-Host '  1. Open the VR menu -> GRAPHICS.'
    Write-Host '  2. Set WEAPON MODELS to HD  (it will show a [RESTART] tag).'
    Write-Host '  3. Fully close and reopen the game.'
    Write-Host '  4. Re-open GRAPHICS: the [RESTART] tag is gone once HD is active.'
    Write-Host 'If the row shows < NO FILES >, the payload is not where the mod looks;'
    Write-Host 'rerun this installer. Set it back to ORIGINAL any time to undo.'
} finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $Stage
}
