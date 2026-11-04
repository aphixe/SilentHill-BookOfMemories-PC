# Silent Hill: Book of Memories PC setup. Sets this folder up to build and play the game from the player's own files.
# Run it through Setup.bat. Running it again after an update rebuilds the game and keeps saves and settings.
# Nothing here downloads a game or firmware: only the Project Nativ tools, from that project's own GitHub release.
param(
    [string]$GameFolder,      # the game: a copy of ux0:app/PCSE00011
    [string]$FirmwareFile,    # Sony's firmware update file (PSP2UPDAT.PUP / PSVUPDAT.PUP)
    [string]$Vita3KFolder,    # or a Vita3K install with its firmware installed
    [string]$NativZip,        # a copy of the Project Nativ zip already on disk
    [switch]$NoShortcut,      # no desktop shortcut
    [switch]$Unattended       # no dialogs: stop if something is missing
)
$ErrorActionPreference = 'Stop'
$Title = 'PCSE00011'
$NativVersion = 'v0.1.5'
$NativFile = "ProjectNativ-$NativVersion-windows.zip"
$NativUrl = "https://github.com/TheLostPantheon/nativ/releases/download/$NativVersion/$NativFile"
$NativSha256 = '847a3ce1a98f3a3ada6aa89a0c40f85698f6ce06cedde0ad79d2e2babc0da00a'
$NeededGB = 5

$Root = Split-Path -Parent $PSScriptRoot
$Dump = Join-Path $Root 'dump'
$Python = Join-Path $Root 'python\python.exe'
$Driver = Join-Path $Root 'pipeline\vitarecomp.py'
$Work = Join-Path $Dump "decrypted\$Title"
$Exe = Join-Path $Work 'recomp\game_host.exe'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing, System.IO.Compression.FileSystem

function Step($text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Fail($text) {
    Write-Host ''; Write-Host "Setup stopped: $text" -ForegroundColor Red
    Write-Host 'Fix that and run Setup.bat again. It picks up where it stopped.'
    exit 1
}
function Pipeline {
    # (no pipe here: Windows PowerShell would turn the tools' progress on stderr into errors)
    $ErrorActionPreference = 'Continue'
    & $Python $Driver @args
    if ($LASTEXITCODE -ne 0) { Fail "'vitarecomp $($args -join ' ')' failed (exit code $LASTEXITCODE). See the messages above." }
}
function PickFolder($prompt) {
    if ($Unattended) { return $null }
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = $prompt
    if ($d.ShowDialog() -eq 'OK') { return $d.SelectedPath }
    return $null
}
function PickFile($prompt, $filter) {
    if ($Unattended) { return $null }
    $d = New-Object System.Windows.Forms.OpenFileDialog
    $d.Title = $prompt; $d.Filter = $filter
    if ($d.ShowDialog() -eq 'OK') { return $d.FileName }
    return $null
}

Write-Host 'Silent Hill: Book of Memories PC setup'
Write-Host "Folder: $Root"
if (-not (Test-Path -LiteralPath $Driver)) { Fail "$Driver is missing. Keep the releases folder inside the unpacked download." }

# ---- 1. the tools: Python and the compiler from the Project Nativ release, nothing the repository tracks
if (-not ((Test-Path -LiteralPath $Python) -and (Test-Path -LiteralPath (Join-Path $Root 'toolchain\llvm-mingw')))) {
    $free = (Get-Item -LiteralPath $Root).PSDrive.Free
    if ($free -lt ($NeededGB * 1GB)) { Fail "this drive has $([math]::Round($free / 1GB, 1)) GB free and setup needs about $NeededGB GB." }

    Step "Getting the Project Nativ $NativVersion tools (about 250 MB)"
    $zip = $NativZip
    $temporary = $false
    if (-not $zip) {
        $zip = Join-Path $env:TEMP $NativFile
        $temporary = $true
        $ErrorActionPreference = 'Continue'
        & curl.exe -L --fail --progress-bar -o $zip $NativUrl
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { Fail "could not download $NativUrl" }
    }
    if (-not (Test-Path -LiteralPath $zip)) { Fail "$zip does not exist." }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $NativSha256) { Fail "$zip does not match the expected checksum. It may be a different version or a damaged download." }

    Step 'Unpacking the tools'
    # Only what the repository does not track: unpacking must never overwrite this port's own source.
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $wanted = '^ProjectNativ/(python/|toolchain/|SDL3\.dll$|Project Nativ\.exe$)'
        $count = 0
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName -notmatch $wanted -or $entry.FullName.EndsWith('/')) { continue }
            $target = Join-Path $Root ($entry.FullName.Substring('ProjectNativ/'.Length).Replace('/', '\'))
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
            if ((++$count % 2000) -eq 0) { Write-Host "  $count files" }
        }
        Write-Host "  $count files"
    } finally { $archive.Dispose() }
    if ($temporary) { Remove-Item -LiteralPath $zip -Force }
}
# (the pipeline uses <folder>\dump only if it already exists; otherwise the library would land in the user profile)
[void][IO.Directory]::CreateDirectory($Dump)

# ---- 2. firmware
$haveFirmware = [bool](Get-ChildItem -LiteralPath $Dump -Recurse -Filter 'libgxm_es4.suprx.elf' -ErrorAction SilentlyContinue)
if (-not $haveFirmware) {
    Step 'Firmware'
    if (-not $FirmwareFile -and -not $Vita3KFolder -and -not $Unattended) {
        $answer = [Windows.Forms.MessageBox]::Show(
            "The game needs Sony's Vita system libraries.`n`nYes: choose Sony's firmware update file (PSP2UPDAT.PUP or PSVUPDAT.PUP).`nNo: choose a Vita3K folder with the firmware installed.",
            'Firmware', 'YesNoCancel', 'Question')
        if ($answer -eq 'Yes') { $FirmwareFile = PickFile "Choose Sony's Vita firmware update file" 'Vita firmware (*.PUP)|*.PUP|All files (*.*)|*.*' }
        elseif ($answer -eq 'No') { $Vita3KFolder = PickFolder 'Choose the Vita3K folder that holds os0 and vs0' }
    }
    if ($FirmwareFile) {
        if (-not (Test-Path -LiteralPath $FirmwareFile)) { Fail "$FirmwareFile does not exist." }
        Pipeline firmware $FirmwareFile
    } elseif ($Vita3KFolder) {
        $ErrorActionPreference = 'Continue'
        & $Python (Join-Path $PSScriptRoot 'vita3k_firmware.py') $Vita3KFolder $Dump
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { Fail 'could not read the firmware from that Vita3K folder.' }
    } else { Fail 'no firmware chosen. README.md says where to get it.' }
}

# ---- 3. the game: decrypted, recompiled and compiled on this machine
if (Test-Path -LiteralPath (Join-Path $Work 'app0\eboot.bin.elf')) {
    Step 'Bringing the build up to date (your saves and settings stay)'
    Pipeline ready $Title
    Pipeline build $Title
} else {
    Step 'Your game files'
    if (-not $GameFolder) { $GameFolder = PickFolder "Choose your copy of Silent Hill: Book of Memories, the $Title folder that holds eboot.bin" }
    if (-not $GameFolder) { Fail 'no game folder chosen.' }
    if (-not (Test-Path -LiteralPath (Join-Path $GameFolder 'eboot.bin'))) {
        $inner = Join-Path $GameFolder $Title
        if (Test-Path -LiteralPath (Join-Path $inner 'eboot.bin')) { $GameFolder = $inner }
        else { Fail "$GameFolder has no eboot.bin. Choose the $Title folder copied from ux0:app." }
    }
    Step 'Decrypting and building the game. This takes several minutes and uses all CPU cores.'
    Pipeline add $GameFolder
}
if (-not (Test-Path -LiteralPath $Exe)) { Fail 'the build finished without producing the game. See the messages above.' }

# ---- 4. default controls, and a shortcut with the icon from the player's own copy
$settings = Join-Path $Work 'recomp\settings.ini'
if (-not (Test-Path -LiteralPath $settings)) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'default-settings.ini') -Destination $settings }

if (-not $NoShortcut) {
    Step 'Desktop shortcut'
    $icon = Join-Path $Root 'SilentHillBoM.ico'
    $png = Join-Path $Work 'app0\sce_sys\icon0.png'
    if (Test-Path -LiteralPath $png) {
        $source = [Drawing.Image]::FromFile($png)
        $bitmap = New-Object Drawing.Bitmap 256, 256
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $graphics.InterpolationMode = 'HighQualityBicubic'
        $graphics.DrawImage($source, 0, 0, 256, 256)
        $stream = New-Object IO.MemoryStream
        $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        $data = $stream.ToArray()
        $out = [IO.File]::Create($icon)
        $writer = New-Object IO.BinaryWriter $out
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]1)                     # an icon with one image
        $writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([byte]0)   # 256 x 256
        $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]$data.Length); $writer.Write([uint32]22)
        $writer.Write($data)
        $writer.Close(); $graphics.Dispose(); $bitmap.Dispose(); $source.Dispose()
    }
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Silent Hill Book of Memories.lnk'))
    $link.TargetPath = $Python
    $link.Arguments = "pipeline\vitarecomp.py run $Title"
    $link.WorkingDirectory = $Root
    $link.WindowStyle = 7
    if (Test-Path -LiteralPath $icon) { $link.IconLocation = "$icon,0" }
    $link.Save()
}

Write-Host ''
Write-Host 'Setup finished.' -ForegroundColor Green
if ($NoShortcut) { Write-Host "To play: `"$Python`" pipeline\vitarecomp.py run $Title" } else { Write-Host 'To play, use the Silent Hill Book of Memories shortcut on your desktop.' }
