[CmdletBinding()]
param(
    [string]$GamePackage,
    [string]$AudioSource,
    [string]$WorkDir = 'C:\SAVRBuild',
    [string]$AndroidSdk,
    [string]$JavaHome,
    [string]$Serial,
    [string]$LogPath,
    [switch]$BuildOnly,
    [switch]$NonInteractive
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:PackageName = 'com.rockstargames.gtasa'
$script:ExpectedVersionCode = '4234641'
$script:ExpectedVersionName = '2.11.311'
$script:ExpectedSourceSigner = 'FF5B7B6A083FE5994E3306B30AE19D311951D019A8DE7C3E6914F0E06D130A13'
$script:ExpectedLibGame = '4C6A7445E30B27AFDDA781302E4DB9BAC89C28FC1181B68B1EEF16F84D6A282E'
$script:ExpectedOpenXrLoader = '713E3BB8D955254C670ACC1C4899A65CB8C930E97DD9958BF37EA922D72B7A06'
$script:SelectedSerial = $null
$script:AdbExe = $null
$script:TranscriptStarted = $false

$script:PinnedFiles = @{
    Python = @{
        FileName = 'python-3.12.10-embed-amd64.zip'
        Uri = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip'
        Sha256 = '4ACBED6DD1C744B0376E3B1CF57CE906F9DC9E95E68824584C8099A63025A3C3'
    }
    Java = @{
        FileName = 'OpenJDK21U-jdk_x64_windows_hotspot_21.0.11_10.zip'
        Uri = 'https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/OpenJDK21U-jdk_x64_windows_hotspot_21.0.11_10.zip'
        Sha256 = 'D3625E7CADF23787EA540229544B6E2AB494B3B54DA1801879E583E1DFEE0A64'
    }
    AndroidCommandLineTools = @{
        FileName = 'commandlinetools-win-15859902_latest.zip'
        Uri = 'https://dl.google.com/android/repository/commandlinetools-win-15859902_latest.zip'
        Sha256 = '90AE805D20434428BFFCB699C290860F19BB5F66A67E6B330067E3DE801FB04A'
    }
    PlatformTools = @{
        FileName = 'platform-tools_r37.0.1-win.zip'
        Uri = 'https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip'
        Sha256 = '45F4D63113E895EBDE0C90F194099A4676B6AC653BD28D54314A9E022BBC1A99'
    }
    Apktool = @{
        FileName = 'apktool_3.0.3.jar'
        Uri = 'https://github.com/iBotPeaches/Apktool/releases/download/v3.0.3/apktool_3.0.3.jar'
        Sha256 = 'DBF930B076C6B9BE08D57C449CACEFC3BDD6B71EBD59B3066FC0E1F5B14F9423'
    }
    OpenXr = @{
        FileName = 'openxr_loader_for_android-1.1.43.aar'
        Uri = 'https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.43/openxr_loader_for_android-1.1.43.aar'
        Sha256 = '7E1B36141F9A4F1FA4A7E061936344FD9FCD36BCE6C47EAE2AD09812736167B6'
    }
    SevenZip = @{
        FileName = '7zr-26.02.exe'
        Uri = 'https://github.com/ip7z/7zip/releases/download/26.02/7zr.exe'
        Sha256 = '56B8CC9F4971CEF253644FAFE54063ED7FDCA551D4DEE0F8C6BAA81B855ACD72'
    }
}

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host "`n==> $Text" -ForegroundColor Cyan
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path.Trim().Trim('"').Trim("'"))
}

function Test-IsUnderPath {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $candidateFull = Get-FullPath $Candidate
    $rootFull = (Get-FullPath $Root).TrimEnd('\') + '\'
    return $candidateFull.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeWorkDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-FullPath $Path
    $root = [System.IO.Path]::GetPathRoot($full)
    if ([string]::IsNullOrWhiteSpace($root) -or $full.TrimEnd('\') -eq $root.TrimEnd('\')) {
        throw "WorkDir must be a dedicated folder, not a drive root: $full"
    }
    return $full
}

function Move-AsideIfPresent {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$AllowedRoot
    )
    if (-not (Test-Path -LiteralPath $Path)) { return }
    if (-not (Test-IsUnderPath -Candidate $Path -Root $AllowedRoot)) {
        throw "Refusing to replace a path outside the managed root: $Path"
    }
    $suffix = Get-Date -Format 'yyyyMMdd-HHmmss'
    $aside = "$Path.invalid-$suffix"
    Move-Item -LiteralPath $Path -Destination $aside
    Write-Warning "Preserved an invalid managed item at: $aside"
}

function Format-DownloadSize {
    param([Parameter(Mandatory = $true)][long]$Bytes)
    if ($Bytes -ge 1GB) { return ('{0:N2} GiB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:N1} MiB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:N1} KiB' -f ($Bytes / 1KB)) }
    return "$Bytes bytes"
}

function Invoke-DownloadWithProgress {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$OutFile,
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [ValidateRange(15, 600)][int]$StallTimeoutSeconds = 90
    )

    Add-Type -AssemblyName System.Net.Http
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $true
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [System.Threading.Timeout]::InfiniteTimeSpan
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('GTASAVR-SourceKit/0.1.0')

    $response = $null
    $source = $null
    $target = $null
    $headerTimeout = [System.Threading.CancellationTokenSource]::new()
    $headerTimeout.CancelAfter([TimeSpan]::FromSeconds(60))
    $progressId = 1701
    try {
        Write-Host "Downloading $DisplayName"
        Write-Host "  Source: $Uri"
        Write-Host "  If no bytes arrive for $StallTimeoutSeconds seconds, the download fails instead of hanging forever."

        try {
            $response = $client.GetAsync(
                $Uri,
                [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead,
                $headerTimeout.Token).GetAwaiter().GetResult()
        }
        catch [System.OperationCanceledException] {
            throw "Timed out while connecting to $Uri"
        }
        $response.EnsureSuccessStatusCode() | Out-Null
        $source = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $target = [System.IO.File]::Open(
            $OutFile,
            [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None)

        $total = $response.Content.Headers.ContentLength
        $buffer = New-Object byte[] (1MB)
        [long]$written = 0
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        $nextTextReport = 5.0

        while ($true) {
            $readTimeout = [System.Threading.CancellationTokenSource]::new()
            $readTimeout.CancelAfter([TimeSpan]::FromSeconds($StallTimeoutSeconds))
            try {
                try {
                    $read = $source.ReadAsync(
                        $buffer, 0, $buffer.Length,
                        $readTimeout.Token).GetAwaiter().GetResult()
                }
                catch [System.OperationCanceledException] {
                    throw "Download stalled: no data received for $StallTimeoutSeconds seconds while downloading $DisplayName. Run the master again to retry."
                }
            }
            finally {
                $readTimeout.Dispose()
            }
            if ($read -eq 0) { break }

            $target.Write($buffer, 0, $read)
            $written += $read
            $seconds = [Math]::Max($timer.Elapsed.TotalSeconds, 0.001)
            $bytesPerSecond = $written / $seconds
            $rateText = "$(Format-DownloadSize ([long]$bytesPerSecond))/s"
            $writtenText = Format-DownloadSize $written

            if ($null -ne $total -and $total -gt 0) {
                $percent = [Math]::Min(100, [int](100.0 * $written / $total))
                $remainingSeconds = if ($bytesPerSecond -gt 0) {
                    [Math]::Max(0, [int](($total - $written) / $bytesPerSecond))
                } else { 0 }
                $status = "$writtenText / $(Format-DownloadSize $total) - $percent% - $rateText - ETA $([TimeSpan]::FromSeconds($remainingSeconds).ToString('mm\:ss'))"
                Write-Progress -Id $progressId -Activity "Downloading $DisplayName" `
                    -Status $status -PercentComplete $percent
            }
            else {
                $status = "$writtenText received - $rateText"
                Write-Progress -Id $progressId -Activity "Downloading $DisplayName" `
                    -Status $status
            }

            if ($timer.Elapsed.TotalSeconds -ge $nextTextReport) {
                Write-Host "  $status"
                $nextTextReport = $timer.Elapsed.TotalSeconds + 5.0
            }
        }

        $target.Flush()
        if ($null -ne $total -and $total -ge 0 -and $written -ne $total) {
            throw "Incomplete download for $DisplayName. Expected $total bytes, received $written."
        }
        $timer.Stop()
        $averageRate = if ($timer.Elapsed.TotalSeconds -gt 0) {
            Format-DownloadSize ([long]($written / $timer.Elapsed.TotalSeconds))
        } else { Format-DownloadSize $written }
        Write-Host "Downloaded ${DisplayName}: $(Format-DownloadSize $written) at $averageRate/s."
    }
    finally {
        Write-Progress -Id $progressId -Activity "Downloading $DisplayName" -Completed
        if ($null -ne $target) { $target.Dispose() }
        if ($null -ne $source) { $source.Dispose() }
        if ($null -ne $response) { $response.Dispose() }
        $headerTimeout.Dispose()
        $client.Dispose()
        $handler.Dispose()
    }
}

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Definition,
        [Parameter(Mandatory = $true)][string]$DownloadRoot
    )
    New-Item -ItemType Directory -Force -Path $DownloadRoot | Out-Null
    $destination = Join-Path $DownloadRoot $Definition.FileName
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $existingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
        if ($existingHash -eq $Definition.Sha256) {
            Write-Host "Using verified cache: $destination"
            return $destination
        }
        Move-AsideIfPresent -Path $destination -AllowedRoot $DownloadRoot
    }

    $temporary = "$destination.download-$([Guid]::NewGuid().ToString('N'))"
    try {
        Invoke-DownloadWithProgress -Uri $Definition.Uri -OutFile $temporary `
            -DisplayName $Definition.FileName
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash
        if ($actualHash -ne $Definition.Sha256) {
            throw "SHA256 mismatch for $($Definition.FileName). Expected $($Definition.Sha256), got $actualHash"
        }
        Move-Item -LiteralPath $temporary -Destination $destination
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    return $destination
}

function Expand-ZipToFreshDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Archive,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ManagedRoot
    )
    if (Test-Path -LiteralPath $Destination) {
        Move-AsideIfPresent -Path $Destination -AllowedRoot $ManagedRoot
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Destination)
}

function Invoke-NativeCapture {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure,
        [switch]$EchoOutput
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $raw = @(& $FilePath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $lines = @($raw | ForEach-Object { $_.ToString() })
    $text = $lines -join [Environment]::NewLine
    if ($EchoOutput.IsPresent -and -not [string]::IsNullOrWhiteSpace($text)) {
        Write-Host $text
    }
    if ($exitCode -ne 0 -and -not $AllowFailure.IsPresent) {
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')`n$text"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Lines = $lines
        Text = $text
    }
}

function Write-NativeOutputLine {
    param([Parameter(Mandatory = $true)]$Item)
    $line = if ($Item -is [System.Management.Automation.ErrorRecord]) {
        $Item.Exception.Message
    } else {
        $Item.ToString()
    }
    if ([string]::IsNullOrWhiteSpace($line) -or
        $line -eq 'System.Management.Automation.RemoteException') {
        return
    }
    Write-Host $line
}

function Invoke-NativeLive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $FilePath @Arguments 2>&1 | ForEach-Object {
            Write-NativeOutputLine -Item $_
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($exitCode -ne 0) {
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')"
    }
}

function Invoke-NativeWithInput {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string[]]$InputLines
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $InputLines | & $FilePath @Arguments 2>&1 | ForEach-Object {
            Write-NativeOutputLine -Item $_
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($exitCode -ne 0) {
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')"
    }
}

function Read-PathWithDialog {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$Filter
    )
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $choice = [System.Windows.Forms.MessageBox]::Show(
            "$Title`n`nYes: choose a file`nNo: choose a folder`nCancel: use the text prompt",
            'GTA SA VR source kit',
            [System.Windows.Forms.MessageBoxButtons]::YesNoCancel,
            [System.Windows.Forms.MessageBoxIcon]::Information
        )
        if ($choice -eq [System.Windows.Forms.DialogResult]::Yes) {
            $dialog = New-Object System.Windows.Forms.OpenFileDialog
            $dialog.Title = $Title
            $dialog.Filter = $Filter
            $dialog.CheckFileExists = $true
            if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                return $dialog.FileName
            }
        }
        elseif ($choice -eq [System.Windows.Forms.DialogResult]::No) {
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = $Title
            $dialog.ShowNewFolderButton = $false
            if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                return $dialog.SelectedPath
            }
        }
    }
    catch {
        Write-Verbose "Graphical path prompt is unavailable: $($_.Exception.Message)"
    }
    return $null
}

function Resolve-RequiredInputPath {
    param(
        [string]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Prompt,
        [Parameter(Mandatory = $true)][string]$Filter
    )
    $candidate = $Value
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        if ($NonInteractive.IsPresent) {
            throw "$Name is required with -NonInteractive."
        }
        $candidate = Read-PathWithDialog -Title $Prompt -Filter $Filter
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            $candidate = Read-Host "$Prompt (paste a file or folder path)"
        }
    }
    if ([string]::IsNullOrWhiteSpace($candidate)) { throw "$Name was not provided." }
    $candidate = Get-FullPath $candidate
    if (-not (Test-Path -LiteralPath $candidate)) { throw "$Name does not exist: $candidate" }
    return $candidate
}

function Confirm-ExactText {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [Parameter(Mandatory = $true)][string]$RequiredText,
        [switch]$IgnoreCase
    )
    if ($NonInteractive.IsPresent) { return $false }
    Write-Warning $Message
    $answer = Read-Host "Type $RequiredText to continue"
    if ($IgnoreCase.IsPresent) {
        return $answer.Trim() -ieq $RequiredText
    }
    return $answer -ceq $RequiredText
}

function Resolve-JavaHome {
    param(
        [string]$Requested,
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        $requestedFull = Get-FullPath $Requested
        $javac = Join-Path $requestedFull 'bin\javac.exe'
        if (-not (Test-Path -LiteralPath $javac -PathType Leaf)) {
            throw "JDK javac.exe is missing: $javac"
        }
        $version = Invoke-NativeCapture -FilePath $javac -Arguments @('-version') -AllowFailure
        if ($version.ExitCode -ne 0 -or $version.Text -notmatch 'javac\s+21(?:\.|\s)') {
            throw "-JavaHome must point to JDK 21. Reported: $($version.Text)"
        }
        return $requestedFull
    }

    $destination = Join-Path $ToolsRoot 'temurin-jdk-21.0.11+10'
    $knownJavac = Join-Path $destination 'bin\javac.exe'
    if (-not (Test-Path -LiteralPath $knownJavac -PathType Leaf)) {
        $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.Java -DownloadRoot $DownloadRoot
        $temporary = Join-Path $ToolsRoot ('.extract-jdk-' + [Guid]::NewGuid().ToString('N'))
        Expand-ZipToFreshDirectory -Archive $archive -Destination $temporary -ManagedRoot $ToolsRoot
        try {
            $root = Get-ChildItem -LiteralPath $temporary -Directory |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\javac.exe') -PathType Leaf } |
                Select-Object -First 1
            if ($null -eq $root) { throw 'Pinned JDK archive did not contain bin\javac.exe.' }
            if (Test-Path -LiteralPath $destination) {
                Move-AsideIfPresent -Path $destination -AllowedRoot $ToolsRoot
            }
            Move-Item -LiteralPath $root.FullName -Destination $destination
        }
        finally {
            if (Test-Path -LiteralPath $temporary) {
                Remove-Item -LiteralPath $temporary -Recurse -Force
            }
        }
    }
    $version = Invoke-NativeCapture -FilePath $knownJavac -Arguments @('-version') -AllowFailure
    if ($version.ExitCode -ne 0 -or $version.Text -notmatch 'javac\s+21\.0\.11') {
        throw "Pinned JDK validation failed: $($version.Text)"
    }
    return $destination
}

function Resolve-Python {
    param(
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destination = Join-Path $ToolsRoot 'python-3.12.10-embed-amd64'
    $python = Join-Path $destination 'python.exe'
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
        $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.Python -DownloadRoot $DownloadRoot
        Expand-ZipToFreshDirectory -Archive $archive -Destination $destination -ManagedRoot $ToolsRoot
    }
    $version = Invoke-NativeCapture -FilePath $python -Arguments @('--version') -AllowFailure
    if ($version.ExitCode -ne 0 -or $version.Text -notmatch 'Python\s+3\.12\.10') {
        throw "Pinned Python validation failed: $($version.Text)"
    }
    return $python
}

function Resolve-Apktool {
    param([Parameter(Mandatory = $true)][string]$DownloadRoot)
    return Get-VerifiedDownload -Definition $script:PinnedFiles.Apktool -DownloadRoot $DownloadRoot
}

function Test-IsSevenZipArchive {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and
        [System.IO.Path]::GetExtension($Path).Equals('.7z', [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-SevenZip {
    param([Parameter(Mandatory = $true)][string]$DownloadRoot)
    $sevenZip = Get-VerifiedDownload -Definition $script:PinnedFiles.SevenZip -DownloadRoot $DownloadRoot
    $version = Invoke-NativeCapture -FilePath $sevenZip -Arguments @('i') -AllowFailure
    if ($version.ExitCode -ne 0 -or $version.Text -notmatch '7-Zip.*26\.02') {
        throw "Pinned 7-Zip validation failed: $($version.Text)"
    }
    return $sevenZip
}

function Resolve-PlatformTools {
    param(
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destination = Join-Path $ToolsRoot 'platform-tools-37.0.1'
    $adb = Join-Path $destination 'platform-tools\adb.exe'
    if (-not (Test-Path -LiteralPath $adb -PathType Leaf)) {
        $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.PlatformTools -DownloadRoot $DownloadRoot
        Expand-ZipToFreshDirectory -Archive $archive -Destination $destination -ManagedRoot $ToolsRoot
    }
    $version = Invoke-NativeCapture -FilePath $adb -Arguments @('version') -AllowFailure
    if ($version.ExitCode -ne 0 -or $version.Text -notmatch 'Version\s+37\.0\.1-') {
        throw "Pinned ADB validation failed: $($version.Text)"
    }
    return $adb
}

function Resolve-OpenXrLoader {
    param(
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destinationRoot = Join-Path $ToolsRoot 'openxr-loader-1.1.43'
    $destination = Join-Path $destinationRoot 'libopenxr_loader.so'
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
        if ($hash -eq $script:ExpectedOpenXrLoader) { return $destination }
        Move-AsideIfPresent -Path $destinationRoot -AllowedRoot $ToolsRoot
    }

    $archivePath = Get-VerifiedDownload -Definition $script:PinnedFiles.OpenXr -DownloadRoot $DownloadRoot
    New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entryName = 'prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so'
        $entry = $archive.GetEntry($entryName)
        if ($null -eq $entry) { throw "Pinned OpenXR AAR is missing $entryName" }
        $input = $entry.Open()
        try {
            $output = [System.IO.File]::Open($destination, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
            try { $input.CopyTo($output) }
            finally { $output.Dispose() }
        }
        finally { $input.Dispose() }
    }
    finally { $archive.Dispose() }
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    if ($actualHash -ne $script:ExpectedOpenXrLoader) {
        throw "Extracted OpenXR loader hash mismatch. Expected $($script:ExpectedOpenXrLoader), got $actualHash"
    }
    return $destination
}

function Install-AndroidCommandLineTools {
    param(
        [Parameter(Mandatory = $true)][string]$SdkRoot,
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destination = Join-Path $SdkRoot 'cmdline-tools\15859902'
    $sdkManagerClasspath = Join-Path $destination 'lib\sdkmanager-classpath.jar'
    if (Test-Path -LiteralPath $sdkManagerClasspath -PathType Leaf) { return $sdkManagerClasspath }

    $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.AndroidCommandLineTools -DownloadRoot $DownloadRoot
    $temporary = Join-Path $ToolsRoot ('.extract-android-cli-' + [Guid]::NewGuid().ToString('N'))
    Expand-ZipToFreshDirectory -Archive $archive -Destination $temporary -ManagedRoot $ToolsRoot
    try {
        $source = Join-Path $temporary 'cmdline-tools'
        if (-not (Test-Path -LiteralPath (Join-Path $source 'lib\sdkmanager-classpath.jar') -PathType Leaf)) {
            throw 'Pinned Android command-line tools archive is missing sdkmanager-classpath.jar.'
        }
        $destinationParent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Force -Path $destinationParent | Out-Null
        if (Test-Path -LiteralPath $destination) {
            $aside = "$destination.invalid-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            Move-Item -LiteralPath $destination -Destination $aside
            Write-Warning "Preserved an invalid Android CLI directory at: $aside"
        }
        Move-Item -LiteralPath $source -Destination $destination
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Recurse -Force
        }
    }
    return $sdkManagerClasspath
}

function Test-AndroidPackages {
    param([Parameter(Mandatory = $true)][string]$SdkRoot)
    $requiredFiles = @(
        (Join-Path $SdkRoot 'platforms\android-35\android.jar'),
        (Join-Path $SdkRoot 'build-tools\35.0.0\aapt2.exe'),
        (Join-Path $SdkRoot 'build-tools\35.0.0\lib\apksigner.jar'),
        (Join-Path $SdkRoot 'ndk\27.2.12479018\build\cmake\android.toolchain.cmake'),
        (Join-Path $SdkRoot 'cmake\3.22.1\bin\cmake.exe'),
        (Join-Path $SdkRoot 'cmake\3.22.1\bin\ninja.exe')
    )
    return @($requiredFiles | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
}

function Resolve-AndroidSdk {
    param(
        [string]$Requested,
        [Parameter(Mandatory = $true)][string]$DefaultRoot,
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedJavaHome
    )
    $sdkRoot = if ([string]::IsNullOrWhiteSpace($Requested)) { $DefaultRoot } else { Get-FullPath $Requested }
    New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null
    $missing = @(Test-AndroidPackages -SdkRoot $sdkRoot)
    if ($missing.Count -eq 0) { return $sdkRoot }

    $sdkManagerClasspath = Install-AndroidCommandLineTools -SdkRoot $sdkRoot -DownloadRoot $DownloadRoot -ToolsRoot $ToolsRoot
    $javaExe = Join-Path $ResolvedJavaHome 'bin\java.exe'
    $sdkManagerMain = 'com.android.sdklib.tool.sdkmanager.SdkManagerCli'
    $oldJavaHome = $env:JAVA_HOME
    $oldSdkRoot = $env:ANDROID_SDK_ROOT
    try {
        $env:JAVA_HOME = $ResolvedJavaHome
        $env:ANDROID_SDK_ROOT = $sdkRoot
        $licenseMarker = Join-Path $sdkRoot 'licenses\android-sdk-license'
        if ($NonInteractive.IsPresent -and -not (Test-Path -LiteralPath $licenseMarker -PathType Leaf)) {
            throw "Android SDK licenses have not been accepted in $sdkRoot. Run this master once without -NonInteractive."
        }
        if (-not $NonInteractive.IsPresent -and
            -not (Test-Path -LiteralPath $licenseMarker -PathType Leaf)) {
            $accepted = Confirm-ExactText `
                -Message 'Android components require the Google Android SDK licenses. Type ACCEPT only if you agree to those licenses; the master will answer the repetitive sdkmanager prompts for you.' `
                -RequiredText 'ACCEPT' `
                -IgnoreCase
            if (-not $accepted) {
                throw 'Android SDK licenses were not accepted. No Android components were installed.'
            }
            Write-Host 'Recording Android SDK license acceptance...' -ForegroundColor Yellow
            Invoke-NativeWithInput -FilePath $javaExe -Arguments @(
                '-cp', $sdkManagerClasspath, $sdkManagerMain, "--sdk_root=$sdkRoot", '--licenses'
            ) -InputLines (@('y') * 32)
            if (-not (Test-Path -LiteralPath $licenseMarker -PathType Leaf)) {
                throw "sdkmanager finished without recording license acceptance in $sdkRoot."
            }
            Write-Host 'Android SDK licenses accepted.' -ForegroundColor Green
        }
        $packages = @(
            'platforms;android-35',
            'build-tools;35.0.0',
            'ndk;27.2.12479018',
            'cmake;3.22.1'
        )
        Invoke-NativeLive -FilePath $javaExe -Arguments (
            @('-cp', $sdkManagerClasspath, $sdkManagerMain, "--sdk_root=$sdkRoot") + $packages
        )
    }
    finally {
        $env:JAVA_HOME = $oldJavaHome
        $env:ANDROID_SDK_ROOT = $oldSdkRoot
    }
    $missing = @(Test-AndroidPackages -SdkRoot $sdkRoot)
    if ($missing.Count -ne 0) {
        throw "Android SDK installation is incomplete:`n$($missing -join [Environment]::NewLine)"
    }
    return $sdkRoot
}

function Get-PersistentKeystorePath {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedWorkDir
    )
    $localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($localAppData)) { $localAppData = $env:LOCALAPPDATA }
    if ([string]::IsNullOrWhiteSpace($localAppData)) { throw 'LOCALAPPDATA is unavailable; cannot place the persistent signing key safely.' }
    $path = Join-Path $localAppData 'GTASAVRBuilder\signing\savr.keystore'
    if ((Test-IsUnderPath -Candidate $path -Root $ProjectRoot) -or
        (Test-IsUnderPath -Candidate $path -Root $ResolvedWorkDir)) {
        $roaming = [Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)
        $path = Join-Path $roaming 'GTASAVRBuilder\signing\savr.keystore'
    }
    if ((Test-IsUnderPath -Candidate $path -Root $ProjectRoot) -or
        (Test-IsUnderPath -Candidate $path -Root $ResolvedWorkDir)) {
        throw 'Cannot choose a persistent keystore path outside the repository and WorkDir.'
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    return $path
}

function Resolve-ManifestFile {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )
    $normalized = $RelativePath.Replace('/', '\')
    $full = Get-FullPath (Join-Path $Root $normalized)
    if (-not (Test-IsUnderPath -Candidate $full -Root $Root)) {
        throw "Manifest path escapes its root: $RelativePath"
    }
    return $full
}

function Assert-BuildArtifacts {
    param([Parameter(Mandatory = $true)][string]$BuildRoot)
    $manifestPath = Join-Path $BuildRoot 'build-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Build manifest is missing: $manifestPath"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([string]$manifest.package -ne $script:PackageName) { throw 'Build manifest package is unexpected.' }
    if ([string]$manifest.versionCode -ne $script:ExpectedVersionCode) { throw 'Build manifest versionCode is unexpected.' }
    if ([string]$manifest.versionName -ne $script:ExpectedVersionName) { throw 'Build manifest versionName is unexpected.' }
    if (-not [bool]$manifest.officialSource) { throw 'The selected APK set is not the verified official retail source.' }
    if ([string]$manifest.sourceSignerSha256 -ne $script:ExpectedSourceSigner) { throw 'Official source signer hash changed.' }
    if ([string]$manifest.libGameSha256 -ne $script:ExpectedLibGame) { throw 'Official libGame.so hash changed.' }
    if ([string]::IsNullOrWhiteSpace([string]$manifest.outputSignerSha256)) { throw 'Output signer is missing from the build manifest.' }

    $outputRoot = Join-Path $BuildRoot 'out'
    $outputs = @($manifest.outputs)
    if ($outputs.Count -eq 0) { throw 'Build manifest contains no APK outputs.' }
    foreach ($record in $outputs) {
        $path = Resolve-ManifestFile -Root $outputRoot -RelativePath ([string]$record.file)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Built APK is missing: $path" }
        $item = Get-Item -LiteralPath $path
        if ([int64]$item.Length -ne [int64]$record.size) { throw "Built APK size mismatch: $path" }
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        if ($hash -ne ([string]$record.sha256).ToUpperInvariant()) { throw "Built APK hash mismatch: $path" }
    }

    $payloadRoot = Join-Path $BuildRoot 'payload'
    foreach ($treeName in @('data_main', 'audio', 'vrhands')) {
        $treeRoot = Join-Path $payloadRoot $treeName
        foreach ($metadataName in @('SHA256SUMS', 'manifest.json')) {
            $metadataPath = Join-Path $treeRoot $metadataName
            if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
                throw "Payload metadata is missing: $metadataPath"
            }
        }
        $treeProperty = $manifest.payload.PSObject.Properties[$treeName]
        if ($null -eq $treeProperty) { throw "Payload manifest section is missing: $treeName" }
        $treeManifest = $treeProperty.Value
        $files = @($treeManifest.files)
        if ($files.Count -ne [int]$treeManifest.fileCount) { throw "Payload file count mismatch: $treeName" }
        [int64]$totalBytes = 0
        foreach ($record in $files) {
            $path = Resolve-ManifestFile -Root $treeRoot -RelativePath ([string]$record.path)
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Payload file is missing: $path" }
            $item = Get-Item -LiteralPath $path
            if ([int64]$item.Length -ne [int64]$record.size) { throw "Payload size mismatch: $path" }
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
            if ($hash -ne ([string]$record.sha256).ToUpperInvariant()) { throw "Payload hash mismatch: $path" }
            $totalBytes += [int64]$item.Length
        }
        if ($totalBytes -ne [int64]$treeManifest.totalBytes) { throw "Payload total size mismatch: $treeName" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot 'payload-manifest.json') -PathType Leaf)) {
        throw 'Top-level payload-manifest.json is missing.'
    }
    return [pscustomobject]@{
        Manifest = $manifest
        ManifestPath = $manifestPath
        OutputRoot = $outputRoot
        PayloadRoot = $payloadRoot
    }
}

function Invoke-AdbCapture {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure,
        [switch]$EchoOutput,
        [switch]$WithoutSerial
    )
    if ([string]::IsNullOrWhiteSpace([string]$script:AdbExe)) { throw 'ADB has not been initialized.' }
    $allArguments = @()
    if (-not $WithoutSerial.IsPresent) {
        if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) { throw 'No Quest serial has been selected.' }
        $allArguments += @('-s', $script:SelectedSerial)
    }
    $allArguments += $Arguments
    return Invoke-NativeCapture -FilePath $script:AdbExe -Arguments $allArguments -AllowFailure:$AllowFailure -EchoOutput:$EchoOutput
}

function Select-QuestDevice {
    param([string]$RequestedSerial)
    Invoke-AdbCapture -Arguments @('start-server') -WithoutSerial -AllowFailure | Out-Null
    $result = Invoke-AdbCapture -Arguments @('devices', '-l') -WithoutSerial
    $devices = @()
    foreach ($line in $result.Lines) {
        if ($line -match '^([^\s]+)\s+(device|unauthorized|offline)\s*(.*)$') {
            $devices += [pscustomobject]@{ Serial = $Matches[1]; State = $Matches[2]; Detail = $Matches[3] }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($RequestedSerial)) {
        $match = @($devices | Where-Object { $_.Serial -eq $RequestedSerial })
        if ($match.Count -ne 1) { throw "Requested ADB device was not found: $RequestedSerial" }
        if ($match[0].State -ne 'device') { throw "Requested ADB device is $($match[0].State): $RequestedSerial" }
        $script:SelectedSerial = $RequestedSerial
    }
    else {
        $ready = @($devices | Where-Object { $_.State -eq 'device' })
        if ($ready.Count -eq 0) {
            $blocked = @($devices | Where-Object { $_.State -ne 'device' })
            if ($blocked.Count -gt 0) { throw 'Quest is connected but not authorized. Put on the headset and allow USB debugging.' }
            throw 'No authorized ADB device was found.'
        }
        if ($ready.Count -eq 1) {
            $script:SelectedSerial = $ready[0].Serial
        }
        elseif ($NonInteractive.IsPresent) {
            throw 'More than one ADB device is connected. Use -Serial with -NonInteractive.'
        }
        else {
            Write-Host 'Connected ADB devices:'
            for ($index = 0; $index -lt $ready.Count; $index++) {
                Write-Host "  [$($index + 1)] $($ready[$index].Serial) $($ready[$index].Detail)"
            }
            $selection = Read-Host 'Choose a device number'
            [int]$number = 0
            if (-not [int]::TryParse($selection, [ref]$number) -or $number -lt 1 -or $number -gt $ready.Count) {
                throw 'Invalid device selection.'
            }
            $script:SelectedSerial = $ready[$number - 1].Serial
        }
    }

    $model = (Invoke-AdbCapture -Arguments @('shell', 'getprop', 'ro.product.model')).Text.Trim()
    $manufacturer = (Invoke-AdbCapture -Arguments @('shell', 'getprop', 'ro.product.manufacturer')).Text.Trim()
    Write-Host "Selected device: $($script:SelectedSerial) ($manufacturer $model)"
    $identity = "$manufacturer $model"
    if ($identity -notmatch '(?i)quest|oculus|meta') {
        $message = "The selected device does not identify itself as a Meta/Oculus Quest: '$identity'. Installation is blocked unless you explicitly approve it."
        if (-not (Confirm-ExactText -Message $message -RequiredText 'INSTALL')) {
            throw 'Installation was not approved for the selected non-Quest device.'
        }
    }
}

function Stop-GameAndVerify {
    param([switch]$BestEffort)
    if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial) -or
        [string]::IsNullOrWhiteSpace([string]$script:AdbExe)) { return }
    $stop = Invoke-AdbCapture -Arguments @('shell', 'am', 'force-stop', $script:PackageName) -AllowFailure
    $pidResult = Invoke-AdbCapture -Arguments @('shell', 'pidof', $script:PackageName) -AllowFailure
    if (-not [string]::IsNullOrWhiteSpace($pidResult.Text)) {
        if ($BestEffort.IsPresent) {
            Write-Warning "Could not confirm that $($script:PackageName) is stopped. PID: $($pidResult.Text.Trim())"
            return
        }
        throw "$($script:PackageName) is still running after force-stop. PID: $($pidResult.Text.Trim())"
    }
    if ($stop.ExitCode -ne 0 -and -not $BestEffort.IsPresent) {
        throw "Failed to force-stop $($script:PackageName): $($stop.Text)"
    }
}

function Get-QuestFreeBytes {
    $result = Invoke-AdbCapture -Arguments @('shell', 'toybox', 'df', '-k', '/sdcard')
    $lines = @($result.Lines | Where-Object { $_ -match '^/|/sdcard|/storage' })
    if ($lines.Count -eq 0) { throw "Could not parse Quest storage information:`n$($result.Text)" }
    $fields = @($lines[$lines.Count - 1].Trim() -split '\s+')
    if ($fields.Count -lt 4) { throw "Could not parse Quest free space line: $($lines[$lines.Count - 1])" }
    [int64]$kilobytes = 0
    if (-not [int64]::TryParse($fields[3], [ref]$kilobytes)) {
        throw "Could not parse Quest free kilobytes: $($fields[3])"
    }
    return $kilobytes * 1024
}

function Assert-QuestFreeSpace {
    param([Parameter(Mandatory = $true)]$BuildInfo)
    [int64]$apkBytes = 0
    foreach ($record in @($BuildInfo.Manifest.outputs)) { $apkBytes += [int64]$record.size }
    [int64]$payloadBytes = [int64]$BuildInfo.Manifest.payload.totalBytes
    [int64]$required = (2 * $apkBytes) + $payloadBytes + 536870912
    [int64]$available = Get-QuestFreeBytes
    Write-Host ('Quest free space: {0:N2} GiB; conservative requirement: {1:N2} GiB' -f ($available / 1GB), ($required / 1GB))
    if ($available -lt $required) {
        throw 'Quest does not have enough free space for verified staging and rollback. Free storage and try again.'
    }
}

function Test-RemotePath {
    param([Parameter(Mandatory = $true)][string]$RemotePath)
    if ($RemotePath -match "['`r`n]") { throw "Unsafe remote path: $RemotePath" }
    $result = Invoke-AdbCapture -Arguments @('shell', 'test', '-e', $RemotePath) -AllowFailure
    return $result.ExitCode -eq 0
}

function Assert-SafeGeneratedRemotePath {
    param([Parameter(Mandatory = $true)][string]$RemotePath)
    if ($RemotePath -match "['`r`n]" -or
        $RemotePath -notmatch '^/sdcard/(savr|Android/data/com\.rockstargames\.gtasa/files)/\.savr-(stage|backup)-[a-f0-9]{32}(/[A-Za-z0-9_.-]+)?$') {
        throw "Refusing unsafe generated remote path: $RemotePath"
    }
}

function Remove-GeneratedRemotePath {
    param([Parameter(Mandatory = $true)][string]$RemotePath)
    Assert-SafeGeneratedRemotePath -RemotePath $RemotePath
    Invoke-AdbCapture -Arguments @('shell', 'rm', '-rf', $RemotePath) | Out-Null
}

function Assert-RemotePayloadHashes {
    param([Parameter(Mandatory = $true)][string]$RemoteRoot)
    if ($RemoteRoot -match "['`r`n]" -or
        $RemoteRoot -notmatch '^/sdcard/(savr/data_main|Android/data/com\.rockstargames\.gtasa/files/(audio|vrhands)|savr/\.savr-stage-[a-f0-9]{32}/(data_main|audio|vrhands))$') {
        throw "Refusing to hash an unexpected remote root: $RemoteRoot"
    }
    $command = "cd '$RemoteRoot' && toybox sha256sum -c SHA256SUMS >/dev/null"
    # `adb shell` already invokes the remote shell. Passing this as its single
    # command avoids a nested `sh -c` whose argument boundary ADB can discard.
    $result = Invoke-AdbCapture -Arguments @('shell', $command) -AllowFailure
    if ($result.ExitCode -ne 0) {
        throw "Quest payload hash verification failed at $RemoteRoot`n$($result.Text)"
    }
}

function New-RemotePayloadDirectoryTree {
    param(
        [Parameter(Mandatory = $true)][string]$LocalRoot,
        [Parameter(Mandatory = $true)][string]$RemoteRoot
    )
    $localFull = (Get-FullPath $LocalRoot).TrimEnd('\', '/')
    $localPrefix = $localFull + [System.IO.Path]::DirectorySeparatorChar
    foreach ($directory in Get-ChildItem -LiteralPath $localFull -Directory -Recurse) {
        if (-not $directory.FullName.StartsWith($localPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Payload directory escaped its local root: $($directory.FullName)"
        }
        $relative = $directory.FullName.Substring($localPrefix.Length).Replace('\', '/')
        if ($relative -notmatch '^[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$') {
            throw "Payload contains a directory name that is unsafe for ADB: $relative"
        }
        Invoke-AdbCapture -Arguments @('shell', 'mkdir', '-p', "$RemoteRoot/$relative") | Out-Null
    }
}

function Publish-PayloadTree {
    param(
        [Parameter(Mandatory = $true)][string]$LocalRoot,
        [Parameter(Mandatory = $true)][string]$RemoteParent,
        [Parameter(Mandatory = $true)][string]$RemoteFinal
    )
    $id = [Guid]::NewGuid().ToString('N')
    $treeName = Split-Path -Leaf (Get-FullPath $LocalRoot)
    if ($treeName -notin @('data_main', 'audio', 'vrhands')) {
        throw "Refusing an unexpected local payload tree: $LocalRoot"
    }
    # Never use `adb push` below Android/data. On some Quest firmware, adbd's
    # scoped-storage secure_mkdirs fails even when shell-created directories
    # already exist. Upload and verify on ordinary shared storage, then let the
    # device shell atomically move the verified tree to its final location.
    $stageContainer = "/sdcard/savr/.savr-stage-$id"
    $stage = "$stageContainer/$treeName"
    $backup = "$RemoteParent/.savr-backup-$id"
    Assert-SafeGeneratedRemotePath -RemotePath $stageContainer
    Assert-SafeGeneratedRemotePath -RemotePath $stage
    Assert-SafeGeneratedRemotePath -RemotePath $backup
    $validFinals = @(
        '/sdcard/savr/data_main',
        "/sdcard/Android/data/$($script:PackageName)/files/audio",
        "/sdcard/Android/data/$($script:PackageName)/files/vrhands"
    )
    if ($validFinals -notcontains $RemoteFinal) { throw "Refusing unexpected final payload path: $RemoteFinal" }
    if ([System.IO.Path]::GetFileName($RemoteFinal) -ne $treeName) {
        throw "Local payload tree '$treeName' does not match its remote destination: $RemoteFinal"
    }

    Invoke-AdbCapture -Arguments @('shell', 'mkdir', '-p', $RemoteParent) | Out-Null
    if (Test-RemotePath -RemotePath $stageContainer) { Remove-GeneratedRemotePath -RemotePath $stageContainer }
    # Pre-create the directory shape so transfer behavior is deterministic on
    # both old and new adbd versions. This staging root is outside Android/data.
    Invoke-AdbCapture -Arguments @('shell', 'mkdir', '-p', $stage) | Out-Null
    New-RemotePayloadDirectoryTree -LocalRoot $LocalRoot -RemoteRoot $stage
    Write-Host "Staging $LocalRoot -> $stage"
    try {
        # Pushing a directory into an existing remote directory creates
        # <remote>/<local-leaf>. Verify that exact layout before hashing or moving it.
        Invoke-AdbCapture -Arguments @('push', $LocalRoot, $stageContainer) -EchoOutput | Out-Null
        if (-not (Test-RemotePath -RemotePath "$stage/SHA256SUMS")) {
            throw "ADB push produced an unexpected remote directory layout under $stageContainer"
        }
        Assert-RemotePayloadHashes -RemoteRoot $stage
    }
    catch {
        $stageError = $_
        if (Test-RemotePath -RemotePath $stageContainer) {
            Remove-GeneratedRemotePath -RemotePath $stageContainer
        }
        throw $stageError
    }

    $oldMoved = $false
    $newPublished = $false
    try {
        if (Test-RemotePath -RemotePath $RemoteFinal) {
            Invoke-AdbCapture -Arguments @('shell', 'mv', $RemoteFinal, $backup) | Out-Null
            $oldMoved = $true
        }
        Invoke-AdbCapture -Arguments @('shell', 'mv', $stage, $RemoteFinal) | Out-Null
        $newPublished = $true
        # Files written by the ADB shell can retain shell ownership after the
        # move into Android/data. Grant traversal/read bits so the app UID can
        # consume its own verified payload on scoped-storage firmware.
        Invoke-AdbCapture -Arguments @('shell', 'chmod', '-R', 'a+rX', $RemoteFinal) | Out-Null
        Assert-RemotePayloadHashes -RemoteRoot $RemoteFinal
        if (Test-RemotePath -RemotePath $stageContainer) {
            try { Remove-GeneratedRemotePath -RemotePath $stageContainer }
            catch { Write-Warning "Verified payload was published, but the empty staging container remains: $stageContainer" }
        }
        if ($oldMoved -and (Test-RemotePath -RemotePath $backup)) {
            Remove-GeneratedRemotePath -RemotePath $backup
        }
    }
    catch {
        $publishError = $_
        if ($newPublished -and (Test-RemotePath -RemotePath $RemoteFinal)) {
            Invoke-AdbCapture -Arguments @('shell', 'rm', '-rf', $RemoteFinal) -AllowFailure | Out-Null
        }
        if ($oldMoved -and (Test-RemotePath -RemotePath $backup)) {
            Invoke-AdbCapture -Arguments @('shell', 'mv', $backup, $RemoteFinal) -AllowFailure | Out-Null
        }
        if (Test-RemotePath -RemotePath $stageContainer) {
            Remove-GeneratedRemotePath -RemotePath $stageContainer
        }
        throw $publishError
    }
}

function Backup-QuestSavesAndSettings {
    param([Parameter(Mandatory = $true)][string]$BackupRoot)
    $remoteRoot = "/sdcard/Android/data/$($script:PackageName)/files"
    New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null
    $filesRoot = Join-Path $BackupRoot 'files'
    New-Item -ItemType Directory -Force -Path $filesRoot | Out-Null
    $copiedNames = @()

    if (Test-RemotePath -RemotePath $remoteRoot) {
        $list = Invoke-AdbCapture -Arguments @('shell', 'ls', '-1A', $remoteRoot) -AllowFailure
        if ($list.ExitCode -ne 0) { throw "Could not enumerate Quest saves/settings: $($list.Text)" }
        foreach ($rawName in $list.Lines) {
            [string]$name = [string]$rawName
            if ([string]::IsNullOrEmpty($name)) { continue }
            if ($name -in @('audio', 'vrhands') -or $name -like '.savr-stage-*' -or $name -like '.savr-backup-*') { continue }
            if ($name -notmatch '^[A-Za-z0-9_.-]+$' -or $name -in @('.', '..')) {
                throw "Unsafe filename in Quest files directory: $name"
            }
            $remoteChild = "$remoteRoot/$name"
            Invoke-AdbCapture -Arguments @('pull', $remoteChild, $filesRoot) -EchoOutput | Out-Null
            $copiedNames += $name
        }
    }

    $records = @()
    if (Test-Path -LiteralPath $filesRoot) {
        foreach ($file in @(Get-ChildItem -LiteralPath $filesRoot -File -Recurse)) {
            $relative = $file.FullName.Substring($filesRoot.Length).TrimStart('\').Replace('\', '/')
            if ($relative -match "['`r`n]" -or (($relative -split '/') -contains '..')) {
                throw "Unsafe path in the save/settings backup: $relative"
            }
            $localHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
            $remoteFile = "$remoteRoot/$relative"
            $remoteResult = Invoke-AdbCapture -Arguments @(
                'shell', 'toybox', 'sha256sum', $remoteFile
            ) -AllowFailure
            if ($remoteResult.ExitCode -ne 0) {
                throw "Could not hash the remote save/settings file before uninstall: $remoteFile`n$($remoteResult.Text)"
            }
            $remoteHash = (($remoteResult.Text.Trim() -split '\s+')[0]).ToUpperInvariant()
            if ($remoteHash -ne $localHash) {
                throw "Save/settings backup hash mismatch before uninstall: $relative"
            }
            $records += [pscustomobject]@{
                path = $relative
                size = [int64]$file.Length
                sha256 = $localHash
            }
        }
    }
    $manifest = [ordered]@{
        formatVersion = 1
        package = $script:PackageName
        deviceSerial = $script:SelectedSerial
        createdUtc = [DateTime]::UtcNow.ToString('o')
        remoteRoot = $remoteRoot
        excluded = @('audio', 'vrhands', '.savr-stage-*', '.savr-backup-*')
        topLevelItems = $copiedNames
        files = $records
    }
    $manifestPath = Join-Path $BackupRoot 'backup-manifest.json'
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Write-Host "Save/settings backup: $manifestPath ($($records.Count) files)"
    return [pscustomobject]@{ Root = $BackupRoot; FilesRoot = $filesRoot; Manifest = $manifest; ManifestPath = $manifestPath }
}

function Restore-QuestSavesAndSettings {
    param([Parameter(Mandatory = $true)]$Backup)
    $remoteRoot = "/sdcard/Android/data/$($script:PackageName)/files"
    Invoke-AdbCapture -Arguments @('shell', 'mkdir', '-p', $remoteRoot) | Out-Null
    foreach ($name in @($Backup.Manifest.topLevelItems)) {
        if ([string]$name -notmatch '^[A-Za-z0-9_.-]+$' -or [string]$name -in @('.', '..')) {
            throw "Cannot safely restore a top-level Quest filename: $name"
        }
        $localChild = Join-Path $Backup.FilesRoot ([string]$name)
        if (Test-Path -LiteralPath $localChild) {
            $remoteChild = "$remoteRoot/$name"
            # ADB appends a directory's basename when the destination already exists.
            # Remove only this verified backup target so the restored layout is exact.
            Invoke-AdbCapture -Arguments @('shell', 'rm', '-rf', $remoteChild) | Out-Null
            Invoke-AdbCapture -Arguments @('push', $localChild, $remoteChild) -EchoOutput | Out-Null
        }
    }
    foreach ($record in @($Backup.Manifest.files)) {
        $remoteFile = "$remoteRoot/$($record.path)"
        if ($remoteFile -match "['`r`n]") { throw "Cannot verify restored path: $remoteFile" }
        $result = Invoke-AdbCapture -Arguments @('shell', 'toybox', 'sha256sum', $remoteFile)
        $remoteHash = (($result.Text.Trim() -split '\s+')[0]).ToUpperInvariant()
        if ($remoteHash -ne ([string]$record.sha256).ToUpperInvariant()) {
            throw "Restored save/settings hash mismatch: $($record.path)"
        }
    }
    Write-Host "Restored and verified $(@($Backup.Manifest.files).Count) save/settings files."
}

function Get-OrderedApkPaths {
    param([Parameter(Mandatory = $true)]$BuildInfo)
    $records = @($BuildInfo.Manifest.outputs)
    $base = @($records | Where-Object { $null -eq $_.split })
    if ($base.Count -ne 1) { throw "Expected one base APK, found $($base.Count)." }
    $ordered = @($base[0]) + @($records | Where-Object { $null -ne $_.split })
    return @($ordered | ForEach-Object { Resolve-ManifestFile -Root $BuildInfo.OutputRoot -RelativePath ([string]$_.file) })
}

function Get-ExpectedInstalledApkLeaf {
    param([Parameter(Mandatory = $true)]$Record)
    if ($null -eq $Record.split) { return 'base.apk' }
    $safeSplit = ([string]$Record.split) -replace '[^A-Za-z0-9_.-]+', '_'
    if ([string]::IsNullOrWhiteSpace($safeSplit)) { throw 'APK manifest contains an empty split id.' }
    return "split_$safeSplit.apk"
}

function Install-ApkSetSafely {
    param(
        [Parameter(Mandatory = $true)]$BuildInfo,
        [Parameter(Mandatory = $true)][string]$BackupRootBase
    )
    $apkPaths = @(Get-OrderedApkPaths -BuildInfo $BuildInfo)
    $arguments = @('install-multiple', '-r') + $apkPaths
    $attempt = Invoke-AdbCapture -Arguments $arguments -AllowFailure -EchoOutput
    if ($attempt.ExitCode -eq 0 -and $attempt.Text -match '(?im)^Success\s*$') { return $null }

    $signatureMismatch = $attempt.Text -match '(?i)INSTALL_FAILED_UPDATE_INCOMPATIBLE|signatures? do not match|inconsistent certificates|existing package.*signature'
    if (-not $signatureMismatch) {
        throw "Quest APK installation failed without a signature mismatch:`n$($attempt.Text)"
    }

    $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backup = Backup-QuestSavesAndSettings -BackupRoot (Join-Path $BackupRootBase $timestamp)
    $warning = @"
Android rejected the update because the installed GTA SA and this personal build have different signing certificates.
The official APK cannot be re-signed in place. Continuing requires uninstalling $($script:PackageName), which can erase its app data.
An ADB-accessible backup of saves/settings was created at:
$($backup.ManifestPath)
Private internal app data that Android does not expose to ADB cannot be backed up by this installer.
"@
    if (-not (Confirm-ExactText -Message $warning -RequiredText 'UNINSTALL')) {
        throw 'Signature mismatch: uninstall was not explicitly approved. The installed game was left in place.'
    }
    Stop-GameAndVerify
    $uninstall = Invoke-AdbCapture -Arguments @('uninstall', $script:PackageName) -AllowFailure -EchoOutput
    if ($uninstall.ExitCode -ne 0 -or $uninstall.Text -notmatch '(?im)^Success\s*$') {
        throw "Approved uninstall failed:`n$($uninstall.Text)"
    }
    $install = Invoke-AdbCapture -Arguments $arguments -AllowFailure -EchoOutput
    if ($install.ExitCode -ne 0 -or $install.Text -notmatch '(?im)^Success\s*$') {
        throw "Fresh APK installation failed after uninstall. Backup remains at $($backup.ManifestPath)`n$($install.Text)"
    }
    return $backup
}

function Assert-InstalledApks {
    param(
        [Parameter(Mandatory = $true)]$BuildInfo,
        [Parameter(Mandatory = $true)][string]$VerificationRoot
    )
    $pathsResult = Invoke-AdbCapture -Arguments @('shell', 'pm', 'path', $script:PackageName)
    $remotePaths = @($pathsResult.Lines | ForEach-Object {
        if ($_ -match '^package:(.+\.apk)$') { $Matches[1].Trim() }
    } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $remoteByLeaf = @{}
    foreach ($remotePath in $remotePaths) {
        $leaf = [System.IO.Path]::GetFileName($remotePath)
        if ($remoteByLeaf.ContainsKey($leaf)) { throw "Installed package reports duplicate APK leaf name: $leaf" }
        $remoteByLeaf[$leaf] = $remotePath
    }
    $records = @($BuildInfo.Manifest.outputs)
    $expectedLeaves = @{}
    foreach ($record in $records) {
        $expectedLeaf = Get-ExpectedInstalledApkLeaf -Record $record
        if ([string]$record.file -ne $expectedLeaf) {
            throw "Build manifest filename does not match split id '$($record.split)': $($record.file)"
        }
        if ($expectedLeaves.ContainsKey($expectedLeaf)) { throw "Build manifest contains duplicate APK leaf name: $expectedLeaf" }
        $expectedLeaves[$expectedLeaf] = $true
        if (-not $remoteByLeaf.ContainsKey($expectedLeaf)) {
            throw "Installed package is missing expected APK split: $expectedLeaf"
        }
    }
    $unexpectedLeaves = @($remoteByLeaf.Keys | Where-Object { -not $expectedLeaves.ContainsKey($_) })
    if ($unexpectedLeaves.Count -ne 0) {
        throw "Installed package exposes unexpected APK splits: $($unexpectedLeaves -join ', ')"
    }
    $packageDump = Invoke-AdbCapture -Arguments @('shell', 'dumpsys', 'package', $script:PackageName)
    if ($packageDump.Text -notmatch "versionCode=$($script:ExpectedVersionCode)\b") {
        throw 'Installed GTA SA versionCode is not the verified source-kit version.'
    }

    New-Item -ItemType Directory -Force -Path $VerificationRoot | Out-Null
    $important = @($records | Where-Object {
        $null -eq $_.split -or ([string]$_.split) -eq 'config.arm64_v8a' -or ([string]$_.file) -match '(?i)arm64'
    })
    $seen = @{}
    foreach ($record in $important) {
        if ($seen.ContainsKey([string]$record.file)) { continue }
        $seen[[string]$record.file] = $true
        $expectedLeaf = Get-ExpectedInstalledApkLeaf -Record $record
        $remote = [string]$remoteByLeaf[$expectedLeaf]
        $local = Join-Path $VerificationRoot ([string]$record.file)
        Invoke-AdbCapture -Arguments @('pull', $remote, $local) -EchoOutput | Out-Null
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $local).Hash
        if ($hash -ne ([string]$record.sha256).ToUpperInvariant()) {
            throw "Installed APK hash mismatch: $($record.file)"
        }
    }
    Write-Host "Installed package paths verified; patched base/arm64 APK hashes saved at $VerificationRoot"
}

function Invoke-Main {
    $projectRoot = Get-FullPath (Join-Path $PSScriptRoot '..')
    $buildScript = Join-Path $PSScriptRoot 'build.ps1'
    $assembleScript = Join-Path $PSScriptRoot 'assemble.py'
    foreach ($required in @($buildScript, $assembleScript)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Source-kit file is missing: $required" }
    }

    $resolvedWorkDir = Assert-SafeWorkDirectory -Path $WorkDir
    if ($resolvedWorkDir.TrimEnd('\') -ieq $projectRoot.TrimEnd('\') -or
        (Test-IsUnderPath -Candidate $resolvedWorkDir -Root $projectRoot) -or
        (Test-IsUnderPath -Candidate $projectRoot -Root $resolvedWorkDir)) {
        throw "WorkDir must be separate from the public source-kit repository: $projectRoot"
    }
    New-Item -ItemType Directory -Force -Path $resolvedWorkDir | Out-Null
    $downloads = Join-Path $resolvedWorkDir '.downloads'
    $toolsRoot = Join-Path $resolvedWorkDir '.tools'
    $defaultSdk = Join-Path $resolvedWorkDir '.android-sdk'
    $runsRoot = Join-Path $resolvedWorkDir 'runs'
    $runId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), ([Guid]::NewGuid().ToString('N').Substring(0, 8))
    $buildRoot = Join-Path $runsRoot $runId
    $backupRoot = Join-Path (Join-Path $resolvedWorkDir 'device-backups') $runId
    New-Item -ItemType Directory -Force -Path $downloads, $toolsRoot, $runsRoot, $buildRoot, $backupRoot | Out-Null

    $resolvedLogPath = $LogPath
    if ([string]::IsNullOrWhiteSpace($resolvedLogPath)) {
        $resolvedLogPath = Join-Path (Join-Path $resolvedWorkDir 'logs') ("build-and-install-{0}.log" -f $runId)
    }
    $resolvedLogPath = Get-FullPath $resolvedLogPath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedLogPath) | Out-Null
    Start-Transcript -LiteralPath $resolvedLogPath -Force | Out-Null
    $script:TranscriptStarted = $true

    Write-Host 'GTA SA VR Quest personal source-kit builder' -ForegroundColor Green
    Write-Host 'This tool requires your own legally obtained GTA SA 2.11.311 APK set and the separately supplied sound mod.'
    Write-Host 'It does not download or redistribute Rockstar APKs, game data, or audio.'
    Write-Host "Work directory: $resolvedWorkDir"
    Write-Host "Log: $resolvedLogPath"

    $resolvedGamePackage = Resolve-RequiredInputPath -Value $GamePackage -Name 'GamePackage' `
        -Prompt 'Select your legally obtained GTA SA APK, split-APK folder, or APK archive' `
        -Filter 'GTA SA package (*.apk;*.zip;*.apks;*.xapk;*.apkm;*.7z;*.rar)|*.apk;*.zip;*.apks;*.xapk;*.apkm;*.7z;*.rar|All files (*.*)|*.*'
    Write-Host ''
    Write-Host 'Required sound archive: gta-sa-ps2-style-mod-pack_1786856007_737162.7z' -ForegroundColor Yellow
    Write-Host 'Download page: https://libertycity.net/files/gta-san-andreas-ios-android/241069-gta-sa-classic-avanced-mod-pack.html'
    Write-Host 'On that page select Original plan mod pack (16 August 2026, 1.41 GB), NOT CLASSIC ADVANCED v1.0.' -ForegroundColor Yellow
    $resolvedAudioSource = Resolve-RequiredInputPath -Value $AudioSource -Name 'AudioSource' `
        -Prompt 'Select gta-sa-ps2-style-mod-pack_1786856007_737162.7z (Original plan mod pack) or its extracted audio folder' `
        -Filter 'Sound mod (*.zip;*.7z;*.rar)|*.zip;*.7z;*.rar|All files (*.*)|*.*'

    Write-Step 'Preparing pinned build tools'
    $resolvedJavaHome = Resolve-JavaHome -Requested $JavaHome -DownloadRoot $downloads -ToolsRoot $toolsRoot
    $python = Resolve-Python -DownloadRoot $downloads -ToolsRoot $toolsRoot
    $apktool = Resolve-Apktool -DownloadRoot $downloads
    if ((Test-IsSevenZipArchive -Path $resolvedGamePackage) -or
        (Test-IsSevenZipArchive -Path $resolvedAudioSource)) {
        $archiveTool = Resolve-SevenZip -DownloadRoot $downloads
        $env:SAVR_ARCHIVE_TOOL = $archiveTool
        Write-Host "Pinned 7-Zip archive tool: $archiveTool"
    }
    $openXrLoader = Resolve-OpenXrLoader -DownloadRoot $downloads -ToolsRoot $toolsRoot
    $resolvedSdk = Resolve-AndroidSdk -Requested $AndroidSdk -DefaultRoot $defaultSdk -DownloadRoot $downloads `
        -ToolsRoot $toolsRoot -ResolvedJavaHome $resolvedJavaHome
    $keystore = Get-PersistentKeystorePath -ProjectRoot $projectRoot -ResolvedWorkDir $resolvedWorkDir
    Write-Host "Persistent personal signing key: $keystore"

    Write-Step 'Validating the selected GTA SA package and audio before compilation'
    Invoke-NativeLive -FilePath $python -Arguments @(
        $assembleScript,
        '--game-package', $resolvedGamePackage,
        '--audio-source', $resolvedAudioSource,
        '--build-dir', $buildRoot,
        '--sdk', $resolvedSdk,
        '--java-home', $resolvedJavaHome,
        '--apktool', $apktool,
        '--validate-only'
    )

    Write-Step 'Building and assembling the verified personal APK set'
    & $buildScript `
        -Configuration RelWithDebInfo `
        -AndroidSdk $resolvedSdk `
        -JavaHome $resolvedJavaHome `
        -OpenXrLoader $openXrLoader `
        -BuildRoot $buildRoot `
        -PythonExe $python `
        -Apktool $apktool `
        -GamePackage $resolvedGamePackage `
        -AudioSource $resolvedAudioSource `
        -Keystore $keystore `
        -Package

    Write-Step 'Hash-checking build outputs and staged Quest payloads'
    $buildInfo = Assert-BuildArtifacts -BuildRoot $buildRoot
    Write-Host "Verified build manifest: $($buildInfo.ManifestPath)"
    if ($BuildOnly.IsPresent) {
        Write-Host "BuildOnly complete. APKs: $($buildInfo.OutputRoot)" -ForegroundColor Green
        Write-Host "Quest payload: $($buildInfo.PayloadRoot)"
        Write-Host 'No device was selected or modified.'
        return
    }

    Write-Step 'Selecting an authorized Quest'
    $pinnedAdb = Resolve-PlatformTools -DownloadRoot $downloads -ToolsRoot $toolsRoot
    $script:AdbExe = $pinnedAdb
    Select-QuestDevice -RequestedSerial $Serial
    Stop-GameAndVerify
    Assert-QuestFreeSpace -BuildInfo $buildInfo

    Write-Step 'Installing the personally signed APK set'
    $backup = Install-ApkSetSafely -BuildInfo $buildInfo -BackupRootBase $backupRoot

    if ($null -ne $backup) {
        Write-Step 'Restoring the verified save/settings backup'
        Restore-QuestSavesAndSettings -Backup $backup
    }

    Write-Step 'Publishing hash-verified Quest data'
    Publish-PayloadTree -LocalRoot (Join-Path $buildInfo.PayloadRoot 'data_main') `
        -RemoteParent '/sdcard/savr' -RemoteFinal '/sdcard/savr/data_main'
    $packageFiles = "/sdcard/Android/data/$($script:PackageName)/files"
    Publish-PayloadTree -LocalRoot (Join-Path $buildInfo.PayloadRoot 'audio') `
        -RemoteParent $packageFiles -RemoteFinal "$packageFiles/audio"
    Publish-PayloadTree -LocalRoot (Join-Path $buildInfo.PayloadRoot 'vrhands') `
        -RemoteParent $packageFiles -RemoteFinal "$packageFiles/vrhands"

    Write-Step 'Verifying the installed APKs and leaving the game stopped'
    $verificationRoot = Join-Path (Join-Path $resolvedWorkDir 'device-verification') $runId
    Assert-InstalledApks -BuildInfo $buildInfo -VerificationRoot $verificationRoot
    Stop-GameAndVerify

    Write-Host "File-level installation verified on Quest $($script:SelectedSerial)." -ForegroundColor Green
    Write-Host 'The game is force-stopped. This installer never starts it; launch it yourself from the Quest library when ready.'
    Write-Host "Build manifest: $($buildInfo.ManifestPath)"
    if ($null -ne $backup) { Write-Host "Save/settings backup retained at: $($backup.ManifestPath)" }
}

$failure = $null
try {
    Invoke-Main
}
catch {
    $failure = $_
    if (-not [string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) {
        try { Stop-GameAndVerify -BestEffort }
        catch { Write-Warning "Final best-effort force-stop failed: $($_.Exception.Message)" }
    }
    Write-Host "`nFAILED: $($failure.Exception.Message)" -ForegroundColor Red
}
finally {
    if ($script:TranscriptStarted) {
        try { Stop-Transcript | Out-Null }
        catch { }
    }
}
if ($null -ne $failure) { throw $failure }
