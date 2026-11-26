# Setup for the Twonkies PC port: asks for your disc image, puts it in the project,
# and runs every build step (extract, verify, analyse, recompile, build).
#
#   Double-click Setup.cmd in the repository root, or:
#   powershell -ExecutionPolicy Bypass -File tools\setup\setup.ps1 [-Headless -Iso <image> [-Copy]] [-Root <dir>]
#   powershell -ExecutionPolicy Bypass -File tools\setup\setup.ps1 -Headless -Art
#
# The image is moved into "Game iso\" (use -Copy, or the Copy option, to keep the original).
# The window's background pictures are screenshots the built game takes from your own
# disc (local\setup_art, never committed); until they exist it shows its own artwork.
param(
    [string]$Root,
    [string]$Iso,
    [switch]$Copy,
    [switch]$Headless,
    [switch]$Art,          # with -Headless: only take the window's pictures
    [string]$Shot,         # development: render the window to this PNG and exit
    [int]$ShotSlide = 0,   # development: which picture the shot shows
    [switch]$ShotNoArt,    # development: shoot the window without pictures
    [switch]$ShotJob       # development: shoot the window while it takes pictures (log open)
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Join-Path $PSScriptRoot '..\..' }
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')

$GameTitle = 'Jimmy Neutron: Attack of the Twonkies'
$ElfName = 'SLUS_208.87'
$ElfSha1 = '3dc783935b4a853ad42f97a6c1697aae128ffca3'
$TwonkExe = Join-Path $Root 'build\msvc\Release\twonk.exe'
$IsoDir = Join-Path $Root 'Game iso'
$LogFile = Join-Path $Root 'local\logs\setup.log'
$ArtDir = Join-Path $Root 'local\setup_art'

# Pictures for the window: game frames to capture, and pad input that gets there with an
# empty memory card (New Game, continue without saving, skip the lab intro).
$ArtTitleFrame = 1200
$ArtFrames = @(1200, 3060, 3180, 3660, 3720, 4080, 4700)
$ArtRoute = '400:start,700:start,1000:start,1300:start,1600:start,1900:cross,2250:down,2320:down,2400:cross,2600:up,2700:cross,4200:start:6,4700:start:6'

# ---------------------------------------------------------------- tool detection

function Update-PathFromRegistry {
    # Pick up tools installed after this window opened.
    $m = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $u = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$m;$u"
}

function Invoke-Quiet([string]$exe, [string[]]$a) {
    # Output lines of a tool, or nothing when it cannot run or fails.
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $exe @a 2>$null
        if ($LASTEXITCODE -eq 0) { $out }
    } catch {}
}

function Get-VsWhere {
    $p = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $p) { $p }
}

function Find-Python {
    $cands = New-Object System.Collections.ArrayList
    $py = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($py) { [void]$cands.Add(@($py.Source, '-3')) }
    foreach ($c in @(Get-Command python.exe -CommandType Application -All -ErrorAction SilentlyContinue)) { [void]$cands.Add(@($c.Source)) }
    $dirs = @("$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe", "$env:ProgramFiles\Python3*\python.exe")
    foreach ($f in @(Get-ChildItem $dirs -ErrorAction SilentlyContinue | Sort-Object FullName -Descending)) { [void]$cands.Add(@($f.FullName)) }
    foreach ($c in $cands) {
        $exe = $c[0]
        $pre = @($c | Select-Object -Skip 1)
        $out = @(Invoke-Quiet $exe ($pre + @('-c', 'import sys; print(sys.version_info >= (3, 8), sys.version.split()[0])')))
        if ($out.Count -and $out[0] -match '^True (\S+)') { return @{ Exe = $exe; Args = $pre; Version = $Matches[1] } }
    }
}

function Find-CMake {
    $c = @()
    $g = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($g) { $c += $g.Source }
    $c += Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'
    $vs = Get-VsWhere
    if ($vs) { $c += @(Invoke-Quiet $vs @('-latest', '-products', '*', '-find', 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')) }
    foreach ($exe in $c) {
        if (-not $exe -or -not (Test-Path $exe)) { continue }
        $out = @(Invoke-Quiet $exe @('--version'))
        if ($out.Count -and $out[0] -match '(\d+)\.(\d+)[.\d]*') {
            $ver = $Matches[0]
            if ([int]$Matches[1] -gt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -ge 20)) { return @{ Exe = $exe; Version = $ver } }
        }
    }
}

function Find-VisualStudio {
    $vs = Get-VsWhere
    if (-not $vs) { return }
    $name = @(Invoke-Quiet $vs @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'displayName'))
    if ($name.Count -and $name[0]) { return @{ Name = $name[0] } }
}

function Get-Tools {
    Update-PathFromRegistry
    @{ Python = Find-Python; CMake = Find-CMake; VS = Find-VisualStudio }
}

$WingetArgs = @{
    VS     = @('install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--override',
               '--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    CMake  = @('install', '-e', '--id', 'Kitware.CMake')
    Python = @('install', '-e', '--id', 'Python.Python.3.13')
}
$ToolNames = @{ VS = 'Visual Studio C++ build tools'; CMake = 'CMake'; Python = 'Python 3' }
$ToolUrls = @{
    VS     = 'https://visualstudio.microsoft.com/downloads/'
    CMake  = 'https://cmake.org/download/'
    Python = 'https://www.python.org/downloads/'
}

function Test-IsoImage([string]$path) {
    # ISO9660 primary volume descriptor: "CD001" at sector 16, offset 1.
    try {
        $f = [IO.File]::OpenRead($path)
        try {
            if ($f.Length -lt 40000) { return $false }
            [void]$f.Seek(16 * 2048 + 1, 'Begin')
            $b = New-Object byte[] 5
            [void]$f.Read($b, 0, 5)
            return [Text.Encoding]::ASCII.GetString($b) -eq 'CD001'
        } finally { $f.Dispose() }
    } catch { return $false }
}

function Get-ProjectIso {
    if ($script:S -and $script:S.IsoFinal -and (Test-Path $script:S.IsoFinal)) { return $script:S.IsoFinal }
    $f = @(Get-ChildItem $IsoDir -Filter *.iso -File -ErrorAction SilentlyContinue | Sort-Object Length -Descending)
    if ($f.Count) { $f[0].FullName }
}

function Get-GameArgs([string]$iso) {
    "`"extracted\disc\$ElfName`" --iso `"$iso`" --memcard memcard"
}

function Get-ArtFiles {
    @(Get-ChildItem $ArtDir -Filter 'f*.jpg' -File -ErrorAction SilentlyContinue |
        Sort-Object { [int]($_.BaseName -replace '\D', '') } | ForEach-Object { $_.FullName })
}

# ---------------------------------------------------------------- the work (runs on a background runspace)

$Worker = {
    param($S)
    $ErrorActionPreference = 'Stop'
    $ProgressChars = '[' + [char]0x2588 + [char]0x2592 + ']'   # winget progress bars

    function Log([string]$m) { $S.Queue.Enqueue($m) }
    function Step([string]$name) {
        $S.Step = $S.Step + 1
        $S.StepName = $name
        Log ''
        Log ('== Step {0} of {1}: {2} ==' -f $S.Step, $S.Total, $name)
    }
    function Quote([string]$a) {
        if ($a -eq '' -or $a -match '[\s"&|<>^]') { '"' + $a.Replace('"', '\"') + '"' } else { $a }
    }
    function Run([string]$exe, [string[]]$argv) {
        # stdout and stderr together, line by line, through cmd.exe so nothing interleaves badly
        $cmdline = (@($exe) + $argv | ForEach-Object { Quote $_ }) -join ' '
        Log "> $cmdline"
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $env:ComSpec
        $psi.Arguments = '/d /s /c "' + $cmdline + ' 2>&1"'
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.CreateNoWindow = $true
        $psi.WorkingDirectory = $S.Root
        $p = [Diagnostics.Process]::Start($psi)
        $S.Pid = $p.Id
        while ($null -ne ($l = $p.StandardOutput.ReadLine())) {
            if ($l -match '^\s*[-\\|/]?\s*$' -or $l -match $ProgressChars) { continue }   # blank lines, spinners, progress bars
            Log $l
        }
        $p.WaitForExit()
        $S.Pid = 0
        if ($p.ExitCode -ne 0) { throw ('{0} failed (exit code {1}). The log above says why.' -f [IO.Path]::GetFileName($exe), $p.ExitCode) }
    }
    function Copy-WithProgress([string]$src, [string]$dst) {
        $in = [IO.File]::OpenRead($src)
        $out = [IO.File]::Create($dst)
        try {
            $buf = New-Object byte[] (4MB)
            $total = $in.Length; $done = 0; $next = 10
            while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                $out.Write($buf, 0, $n)
                $done += $n
                if ($done * 100 / $total -ge $next) { Log ('  {0}%' -f $next); $next += 10 }
            }
        } finally { $in.Dispose(); $out.Dispose() }
    }
    function Save-GameArt([string]$iso) {
        # Run the game without a window, scripted, and keep a few frames as JPEGs.
        $raw = Join-Path $S.ArtDir 'raw'
        if (Test-Path $raw) { Remove-Item $raw -Recurse -Force }
        $mc = Join-Path $raw 'memcard'   # empty card: nothing of yours is read or written
        [void](New-Item -ItemType Directory -Force $mc)
        $frames = @($S.ArtFrames)
        $game = '"{0}" "extracted\{1}" --iso "{2}" --memcard "{3}" --no-window --scale 3 --aspect 4:3 --input {4} --shot "{5}\f#.png:{6}"' -f `
            $S.TwonkExe, ('disc\' + $S.ElfName), $iso, $mc, $S.ArtRoute, $raw, ($frames -join ',')
        Log "> $game"
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $env:ComSpec
        $psi.Arguments = '/d /s /c "' + $game + ' > "' + (Join-Path $raw 'game.log') + '" 2>&1"'
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.WorkingDirectory = $S.Root
        $p = [Diagnostics.Process]::Start($psi)
        $S.Pid = $p.Id
        $t0 = Get-Date; $seen = 0
        while (-not $p.HasExited) {
            Start-Sleep -Milliseconds 500
            $n = @(Get-ChildItem $raw -Filter 'f*.png' -File).Count
            if ($n -ne $seen) { $seen = $n; Log ('  picture {0} of {1}' -f $n, $frames.Count) }
            if ($n -ge $frames.Count) {
                # The game keeps running after its last screenshot: wait for the file to settle, then stop it.
                $last = Join-Path $raw ('f{0}.png' -f $frames[-1])
                $size = -1
                for ($i = 0; $i -lt 20 -and (Test-Path $last) -and (Get-Item $last).Length -ne $size; $i++) {
                    $size = (Get-Item $last).Length
                    Start-Sleep -Milliseconds 400
                }
                & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null
                break
            }
            if (((Get-Date) - $t0).TotalSeconds -gt 300) {
                & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null
                Log '  The game took too long; keeping the pictures it made.'
                break
            }
        }
        $S.Pid = 0
        if ($S.Cancel) { return }
        Start-Sleep -Milliseconds 300

        Add-Type -AssemblyName System.Drawing
        $jpeg = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
        $ep = New-Object Drawing.Imaging.EncoderParameters 1
        $ep.Param[0] = New-Object Drawing.Imaging.EncoderParameter ([Drawing.Imaging.Encoder]::Quality), ([long]90)
        $kept = 0
        foreach ($f in $frames) {
            $png = Join-Path $raw "f$f.png"
            if (-not (Test-Path $png)) { continue }
            $b = $null; $dst = $null
            try {
                $b = New-Object Drawing.Bitmap $png
                # Crop letterbox bars: first and last rows with any light in them.
                $top = -1; $bot = -1
                for ($y = 0; $y -lt $b.Height; $y += 2) {
                    $sum = 0
                    for ($x = 0; $x -lt $b.Width; $x += 64) { $c = $b.GetPixel($x, $y); $sum += $c.R + $c.G + $c.B }
                    if ($sum -gt 200) { if ($top -lt 0) { $top = $y }; $bot = $y }
                }
                if ($top -lt 0 -or ($bot - $top) -lt $b.Height / 3) { continue }   # blank frame
                # Cut a 2:1 window (the window's picture band) out of the content, as shown on a 4:3 TV.
                $k = $b.Width * 0.75 / $b.Height         # vertical stretch of a PS2 frame shown 4:3
                $sx = 0; $sw = $b.Width; $sy = $top; $sh = $bot - $top + 1
                if ($sh * $k * 2 -gt $sw) {               # taller than 2:1: keep a band around the focus line
                    $band = $sw / (2 * $k)
                    $sy = $top + [Math]::Max(0, [Math]::Min($sh - $band, 0.44 * $sh - $band / 2))
                    $sh = $band
                } else {                                  # wider: keep the middle
                    $cw = $sh * $k * 2
                    $sx = ($sw - $cw) / 2; $sw = $cw
                }
                $dst = New-Object Drawing.Bitmap 1600, 800
                $g = [Drawing.Graphics]::FromImage($dst)
                $g.InterpolationMode = 'HighQualityBicubic'
                $g.PixelOffsetMode = 'HighQuality'
                $g.DrawImage($b, (New-Object Drawing.RectangleF 0, 0, 1600, 800), (New-Object Drawing.RectangleF $sx, $sy, $sw, $sh), 'Pixel')
                $g.Dispose()
                $dst.Save((Join-Path $raw "f$f.jpg"), $jpeg, $ep)
                $kept++
            } catch { Log "  f$f skipped: $($_.Exception.Message)" }
            finally { if ($b) { $b.Dispose() }; if ($dst) { $dst.Dispose() } }
        }
        if ($kept) {   # replace the old set only when there is a new one
            Get-ChildItem $S.ArtDir -Filter 'f*.jpg' -File -ErrorAction SilentlyContinue | Remove-Item -Force
            Get-ChildItem $raw -Filter 'f*.jpg' -File | Move-Item -Destination $S.ArtDir
        }
        Remove-Item $raw -Recurse -Force -ErrorAction SilentlyContinue
        Log "  Saved $kept pictures to $($S.ArtDir)"
        $S.ArtChanged = $true
    }

    try {
        if ($S.Mode -eq 'tools') {
            $S.Total = $S.Install.Count
            foreach ($k in $S.Install) {
                Step ('Installing ' + $S.ToolNames[$k])
                Run $S.Winget ($S.WingetArgs[$k] + @('--accept-package-agreements', '--accept-source-agreements'))
            }
            return
        }
        if ($S.Mode -eq 'art') {
            $S.Total = 1
            Step 'Taking pictures from your game'
            Save-GameArt $S.Iso
            $S.StepName = 'Pictures ready'
            return
        }

        $root = $S.Root
        $py = $S.PyExe; $pa = @($S.PyArgs)
        $cmake = $S.CMake
        $disc = Join-Path $root 'extracted\disc'
        $staging = Join-Path $root 'extracted\disc.new'
        $gen = Join-Path $root 'build\generated'
        $S.Total = 10

        Step 'Extracting the disc image'
        if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
        Run $py ($pa + @('-I', '-u', (Join-Path $root 'tools\extract_iso.py'), $S.Iso, $staging))

        Step 'Checking the disc version'
        $elf = Join-Path $staging $S.ElfName
        $bad = $null
        if (-not (Test-Path $elf)) { $bad = "This image has no $($S.ElfName)." }
        else {
            $h = (Get-FileHash $elf -Algorithm SHA1).Hash.ToLowerInvariant()
            if ($h -ne $S.ElfSha1) { $bad = "$($S.ElfName) has SHA-1 $h, expected $($S.ElfSha1)." }
        }
        if ($bad) {
            Remove-Item $staging -Recurse -Force
            throw "$bad Only the USA disc, SLUS-208.87 version 1.01, is supported. Your image was not moved."
        }
        Log "$($S.ElfName) SHA-1 matches: this is the supported USA disc."
        if (Test-Path $disc) { Remove-Item $disc -Recurse -Force }
        Rename-Item $staging 'disc'
        $elf = Join-Path $disc $S.ElfName

        if ($S.MoveIso) { Step 'Moving the image into the project' } else { Step 'Copying the image into the project' }
        [void](New-Item -ItemType Directory -Force $S.IsoDir)
        $src = [IO.Path]::GetFullPath($S.Iso)
        $dest = Join-Path $S.IsoDir ([IO.Path]::GetFileName($src))
        if ($src -ieq $dest) { Log 'The image is already in the project.' }
        elseif ((Test-Path $dest) -and (Get-Item $dest).Length -eq (Get-Item $src).Length) {
            Log "Already in the project: $dest (left your original where it is)."
        }
        elseif ($S.MoveIso -and [IO.Path]::GetPathRoot($src) -ieq [IO.Path]::GetPathRoot($dest)) {
            if (Test-Path $dest) { Remove-Item $dest -Force }
            [IO.File]::Move($src, $dest)
            Log "Moved to $dest"
        }
        else {
            $tmp = "$dest.partial"
            Copy-WithProgress $src $tmp
            if (Test-Path $dest) { Remove-Item $dest -Force }
            [IO.File]::Move($tmp, $dest)
            if ($S.MoveIso) { Remove-Item $src -Force; Log "Moved to $dest" } else { Log "Copied to $dest" }
        }
        $S.IsoFinal = $dest

        Step 'Analysing the executable'
        Run $py ($pa + @('-I', '-u', (Join-Path $root 'tools\analysis\elfmap.py'), $elf, (Join-Path $root 'analysis'), '36ef00'))

        Step 'Building the recompiler'
        Run $cmake @('-S', (Join-Path $root 'recomp'), '-B', (Join-Path $root 'recomp\build'))
        Run $cmake @('--build', (Join-Path $root 'recomp\build'), '--config', 'Release', '--parallel')

        Step 'Recompiling the game code'
        [void](New-Item -ItemType Directory -Force $gen)
        Run (Join-Path $root 'recomp\build\Release\twonkrecomp.exe') @('gen', $elf,
            (Join-Path $root 'config\regions.txt'), (Join-Path $root 'analysis\functions.tsv'), $gen, (Join-Path $root 'config\hle.txt'))

        Step 'Configuring the game build'
        Run $cmake @('-S', $root, '-B', (Join-Path $root 'build\msvc'), ('-DTWONK_GENERATED_DIR=' + $gen.Replace('\', '/')))

        Step 'Building the game (the long one)'
        $build = @('--build', (Join-Path $root 'build\msvc'), '--config', 'Release', '--target', 'twonk', '--parallel')
        Run $cmake $build
        if (-not (Test-Path $S.TwonkExe)) { throw "The build finished but $($S.TwonkExe) is missing." }

        Step 'Compiling VU1 microcode (optional)'
        $dumps = Join-Path $root 'analysis\vu_dumps'
        $inc = Join-Path $gen 'vu1_aot_gen.inc'
        if (Test-Path $inc) { Log 'Already compiled.' }
        elseif (-not @(Get-ChildItem $dumps -File -ErrorAction SilentlyContinue).Count) {
            Log 'No VU1 dumps yet, skipped. The game runs them interpreted; see README "Building" to add them later.'
        }
        else {
            Run $S.TwonkExe @('--vu-aot', $dumps, $inc)
            (Get-Item (Join-Path $root 'runtime\src\vu1.cpp')).LastWriteTime = Get-Date   # MSBuild does not see a new .inc
            Run $cmake $build
        }

        Step 'Taking pictures from your game (optional)'
        try { Save-GameArt $dest } catch { Log "Skipped: $($_.Exception.Message)" }

        $S.Step = $S.Total
        $S.StepName = 'Done'
        Log ''
        Log 'Setup finished.'
    }
    catch {
        $S.Error = $_.Exception.Message
        Log ''
        Log "ERROR: $($S.Error)"
    }
    finally { $S.Done = $true }
}

function Start-Worker([hashtable]$init) {
    $S = [hashtable]::Synchronized(@{})
    foreach ($k in $init.Keys) { $S[$k] = $init[$k] }
    $S.Queue = New-Object 'System.Collections.Concurrent.ConcurrentQueue[string]'
    $S.Root = $Root; $S.Step = 0; $S.Total = 1; $S.StepName = 'Starting'; $S.Done = $false; $S.Error = $null; $S.Pid = 0
    $S.ArtDir = $ArtDir; $S.ArtFrames = $ArtFrames; $S.ArtRoute = $ArtRoute; $S.ArtChanged = $false
    $S.TwonkExe = $TwonkExe; $S.ElfName = $ElfName
    [void](New-Item -ItemType Directory -Force $ArtDir)
    $ps = [powershell]::Create()
    [void]$ps.AddScript($Worker.ToString()).AddArgument($S)
    [void]$ps.BeginInvoke()
    $script:PS = $ps
    $script:S = $S
    [void](New-Item -ItemType Directory -Force (Split-Path $LogFile))
    [IO.File]::WriteAllText($LogFile, "Setup started $(Get-Date -Format s)`r`n")
    $S
}

function Read-Queue {
    $sb = New-Object System.Text.StringBuilder
    $line = $null
    while ($script:S.Queue.TryDequeue([ref]$line)) { [void]$sb.AppendLine($line) }
    $t = $sb.ToString()
    if ($t) { [IO.File]::AppendAllText($LogFile, $t) }
    $t
}

function Stop-Worker {
    if (-not $script:S -or $script:S.Done) { return }
    $script:S.Cancel = $true
    if ($script:S.Pid) { & taskkill.exe /T /F /PID $script:S.Pid 2>&1 | Out-Null }
    [void]$script:PS.BeginStop($null, $null)
}

function New-InstallJob([hashtable]$tools, [string]$iso, [bool]$move) {
    @{
        Mode = 'install'; Iso = $iso; MoveIso = $move; IsoDir = $IsoDir; ElfSha1 = $ElfSha1
        PyExe = $tools.Python.Exe; PyArgs = $tools.Python.Args; CMake = $tools.CMake.Exe
    }
}

function New-GameShortcut([string]$iso) {
    $sh = New-Object -ComObject WScript.Shell
    $path = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Jimmy Neutron - Attack of the Twonkies.lnk'
    $lnk = $sh.CreateShortcut($path)
    $lnk.TargetPath = $TwonkExe
    $lnk.Arguments = Get-GameArgs $iso
    $lnk.WorkingDirectory = $Root
    $lnk.IconLocation = "$TwonkExe,0"
    $lnk.Description = "$GameTitle (PC port)"
    $lnk.Save()
    $path
}

# ---------------------------------------------------------------- headless

if ($Headless) {
    if ($Art) {
        $iso = Get-ProjectIso
        if (-not (Test-Path $TwonkExe) -or -not $iso) { Write-Host 'Build the game first (setup.ps1 -Headless -Iso <image>).'; exit 1 }
        $S = Start-Worker @{ Mode = 'art'; Iso = $iso }
    } else {
        $tools = Get-Tools
        $missing = @($ToolNames.Keys | Where-Object { -not $tools[$_] } | ForEach-Object { $ToolNames[$_] })
        if ($missing.Count) { Write-Host "Missing: $($missing -join ', ')"; exit 1 }
        if (-not $Iso -or -not (Test-IsoImage $Iso)) { Write-Host 'Give -Iso <your disc image (.iso)>.'; exit 1 }
        $S = Start-Worker (New-InstallJob $tools $Iso (-not $Copy))
    }
    while (-not $S.Done) { Start-Sleep -Milliseconds 250; [Console]::Out.Write((Read-Queue)) }
    [Console]::Out.Write((Read-Queue))
    if ($S.Error) { exit 1 }
    exit 0
}

# ---------------------------------------------------------------- window (WPF)

Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Xaml

$Xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Attack of the Twonkies - PC Setup" Width="1080" Height="760"
        WindowStyle="None" ResizeMode="CanMinimize" WindowStartupLocation="CenterScreen"
        Background="#0A0E16" FontFamily="Segoe UI" Foreground="White" AllowDrop="True"
        UseLayoutRounding="True" SnapsToDevicePixels="True">
  <Window.Resources>
    <SolidColorBrush x:Key="Glass" Color="#D20F1420"/>
    <SolidColorBrush x:Key="GlassBorder" Color="#30FFFFFF"/>
    <SolidColorBrush x:Key="Muted" Color="#A9B3C7"/>
    <LinearGradientBrush x:Key="Accent" StartPoint="0,0" EndPoint="1,1">
      <GradientStop Color="#FFB21A" Offset="0"/>
      <GradientStop Color="#FF5A1F" Offset="1"/>
    </LinearGradientBrush>
    <LinearGradientBrush x:Key="Go" StartPoint="0,0" EndPoint="1,1">
      <GradientStop Color="#5BE36B" Offset="0"/>
      <GradientStop Color="#0FA45A" Offset="1"/>
    </LinearGradientBrush>

    <Style x:Key="Btn" TargetType="Button">
      <Setter Property="Foreground" Value="White"/>
      <Setter Property="Background" Value="#22FFFFFF"/>
      <Setter Property="BorderBrush" Value="#30FFFFFF"/>
      <Setter Property="BorderThickness" Value="1"/>
      <Setter Property="Padding" Value="16,7"/>
      <Setter Property="FontSize" Value="13"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Focusable" Value="False"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Grid>
              <Border CornerRadius="10" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="{TemplateBinding BorderThickness}"/>
              <Border x:Name="hl" CornerRadius="10" Background="White" Opacity="0"/>
              <ContentPresenter Margin="{TemplateBinding Padding}" HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Grid>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="hl" Property="Opacity" Value="0.13"/></Trigger>
              <Trigger Property="IsPressed" Value="True"><Setter TargetName="hl" Property="Opacity" Value="0.24"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.35"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Chrome" TargetType="Button" BasedOn="{StaticResource Btn}">
      <Setter Property="Background" Value="Transparent"/>
      <Setter Property="BorderThickness" Value="0"/>
      <Setter Property="Width" Value="44"/>
      <Setter Property="Height" Value="32"/>
      <Setter Property="Padding" Value="0"/>
    </Style>
    <Style x:Key="Link" TargetType="Button" BasedOn="{StaticResource Btn}">
      <Setter Property="Background" Value="Transparent"/>
      <Setter Property="BorderThickness" Value="0"/>
      <Setter Property="Padding" Value="6,3"/>
      <Setter Property="Foreground" Value="#FFC266"/>
      <Setter Property="FontSize" Value="12"/>
    </Style>

    <Style x:Key="Pill" TargetType="RadioButton">
      <Setter Property="Foreground" Value="White"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Focusable" Value="False"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="RadioButton">
            <Border x:Name="bd" CornerRadius="9" Padding="12,6" Background="#12FFFFFF" BorderBrush="#26FFFFFF" BorderThickness="1">
              <ContentPresenter HorizontalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsChecked" Value="True">
                <Setter TargetName="bd" Property="Background" Value="#3DFF8A1F"/>
                <Setter TargetName="bd" Property="BorderBrush" Value="#FF9A3C"/>
              </Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.4"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="Check" TargetType="CheckBox">
      <Setter Property="Foreground" Value="White"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Focusable" Value="False"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="CheckBox">
            <StackPanel Orientation="Horizontal" Background="Transparent">
              <Border x:Name="box" Width="18" Height="18" CornerRadius="5" Background="#12FFFFFF" BorderBrush="#45FFFFFF" BorderThickness="1">
                <Path x:Name="mark" Data="M3.5,8.5 L7,12 L13,4.5" Stroke="White" StrokeThickness="2.2" Visibility="Collapsed"/>
              </Border>
              <ContentPresenter Margin="9,0,0,0" VerticalAlignment="Center"/>
            </StackPanel>
            <ControlTemplate.Triggers>
              <Trigger Property="IsChecked" Value="True">
                <Setter TargetName="mark" Property="Visibility" Value="Visible"/>
                <Setter TargetName="box" Property="Background" Value="#FF7A1F"/>
                <Setter TargetName="box" Property="BorderBrush" Value="#FF7A1F"/>
              </Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.4"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="Bar" TargetType="ProgressBar">
      <Setter Property="Height" Value="8"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ProgressBar">
            <Grid>
              <Border x:Name="PART_Track" CornerRadius="4" Background="#1CFFFFFF"/>
              <Border x:Name="PART_Indicator" CornerRadius="4" HorizontalAlignment="Left" Background="{StaticResource Accent}"/>
            </Grid>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style TargetType="ScrollBar">
      <Setter Property="Width" Value="8"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ScrollBar">
            <Track x:Name="PART_Track" IsDirectionReversed="True">
              <Track.Thumb>
                <Thumb>
                  <Thumb.Template>
                    <ControlTemplate TargetType="Thumb"><Border CornerRadius="4" Background="#50FFFFFF"/></ControlTemplate>
                  </Thumb.Template>
                </Thumb>
              </Track.Thumb>
            </Track>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="Card" TargetType="Border">
      <Setter Property="Background" Value="{StaticResource Glass}"/>
      <Setter Property="BorderBrush" Value="{StaticResource GlassBorder}"/>
      <Setter Property="BorderThickness" Value="1"/>
      <Setter Property="CornerRadius" Value="16"/>
      <Setter Property="Padding" Value="20,16"/>
    </Style>
    <Style x:Key="CardTitle" TargetType="TextBlock">
      <Setter Property="FontSize" Value="16"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Margin" Value="11,0,0,0"/>
      <Setter Property="VerticalAlignment" Value="Center"/>
    </Style>
    <Style x:Key="BadgeText" TargetType="TextBlock">
      <Setter Property="FontSize" Value="13"/>
      <Setter Property="FontWeight" Value="Bold"/>
      <Setter Property="HorizontalAlignment" Value="Center"/>
      <Setter Property="VerticalAlignment" Value="Center"/>
    </Style>
    <Style x:Key="Dot" TargetType="Ellipse">
      <Setter Property="Width" Value="9"/>
      <Setter Property="Height" Value="9"/>
      <Setter Property="HorizontalAlignment" Value="Left"/>
      <Setter Property="VerticalAlignment" Value="Center"/>
      <Setter Property="Fill" Value="#55FFFFFF"/>
    </Style>
    <Style x:Key="ToolName" TargetType="TextBlock">
      <Setter Property="FontSize" Value="13"/>
      <Setter Property="Margin" Value="19,0,0,0"/>
      <Setter Property="VerticalAlignment" Value="Center"/>
    </Style>
    <Style x:Key="ToolInfo" TargetType="TextBlock">
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="Foreground" Value="{StaticResource Muted}"/>
      <Setter Property="HorizontalAlignment" Value="Right"/>
      <Setter Property="VerticalAlignment" Value="Center"/>
      <Setter Property="TextTrimming" Value="CharacterEllipsis"/>
      <Setter Property="MaxWidth" Value="130"/>
    </Style>
  </Window.Resources>

  <Border BorderBrush="#40FFFFFF" BorderThickness="1">
  <Grid ClipToBounds="True">
    <!-- Our own artwork, shown until the game pictures exist -->
    <Grid x:Name="Backdrop">
      <Rectangle>
        <Rectangle.Fill>
          <LinearGradientBrush StartPoint="0,0" EndPoint="1,1">
            <GradientStop Color="#0A1A40" Offset="0"/>
            <GradientStop Color="#24135A" Offset="0.55"/>
            <GradientStop Color="#08385E" Offset="1"/>
          </LinearGradientBrush>
        </Rectangle.Fill>
      </Rectangle>
      <Canvas x:Name="Stars"/>
      <Ellipse Width="900" Height="900" HorizontalAlignment="Right" VerticalAlignment="Top" Margin="0,-520,-330,0">
        <Ellipse.Fill>
          <RadialGradientBrush>
            <GradientStop Color="#40FF8A1F" Offset="0"/>
            <GradientStop Color="#00FF8A1F" Offset="1"/>
          </RadialGradientBrush>
        </Ellipse.Fill>
      </Ellipse>
      <Ellipse Width="380" Height="380" HorizontalAlignment="Right" VerticalAlignment="Top" Margin="0,-90,70,0">
        <Ellipse.Fill>
          <RadialGradientBrush GradientOrigin="0.32,0.28" Center="0.4,0.35" RadiusX="0.75" RadiusY="0.75">
            <GradientStop Color="#FFD27A" Offset="0"/>
            <GradientStop Color="#FF7A2E" Offset="0.45"/>
            <GradientStop Color="#7A1E4A" Offset="0.85"/>
            <GradientStop Color="#2A0E33" Offset="1"/>
          </RadialGradientBrush>
        </Ellipse.Fill>
      </Ellipse>
      <Ellipse Width="660" Height="120" HorizontalAlignment="Right" VerticalAlignment="Top" Margin="0,70,-70,0"
               StrokeThickness="3" RenderTransformOrigin="0.5,0.5">
        <Ellipse.Stroke>
          <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
            <GradientStop Color="#00FFFFFF" Offset="0"/>
            <GradientStop Color="#B0FFE2B0" Offset="0.5"/>
            <GradientStop Color="#00FFFFFF" Offset="1"/>
          </LinearGradientBrush>
        </Ellipse.Stroke>
        <Ellipse.RenderTransform><RotateTransform Angle="-14"/></Ellipse.RenderTransform>
      </Ellipse>
      <Path Data="M0,0 L230,92" StrokeThickness="3" HorizontalAlignment="Left" VerticalAlignment="Top" Margin="150,70,0,0">
        <Path.Stroke>
          <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
            <GradientStop Color="#00FFFFFF" Offset="0"/>
            <GradientStop Color="#C0BFE8FF" Offset="1"/>
          </LinearGradientBrush>
        </Path.Stroke>
      </Path>
      <Ellipse Width="9" Height="9" Fill="White" HorizontalAlignment="Left" VerticalAlignment="Top" Margin="376,158,0,0">
        <Ellipse.Effect><DropShadowEffect Color="#9FDBFF" BlurRadius="16" ShadowDepth="0" Opacity="1"/></Ellipse.Effect>
      </Ellipse>
    </Grid>

    <!-- Game pictures, 2:1, across the top (crossfade with a slow zoom) -->
    <Grid x:Name="ArtHost" Height="540" VerticalAlignment="Top" ClipToBounds="True">
      <Image x:Name="ArtA" Stretch="UniformToFill" Opacity="0" RenderTransformOrigin="0.5,0.35">
        <Image.RenderTransform><ScaleTransform/></Image.RenderTransform>
      </Image>
      <Image x:Name="ArtB" Stretch="UniformToFill" Opacity="0" RenderTransformOrigin="0.5,0.35">
        <Image.RenderTransform><ScaleTransform/></Image.RenderTransform>
      </Image>
    </Grid>

    <Rectangle IsHitTestVisible="False">
      <Rectangle.Fill>
        <LinearGradientBrush StartPoint="0,0" EndPoint="0,1">
          <GradientStop Color="#99000000" Offset="0"/>
          <GradientStop Color="#00000000" Offset="0.12"/>
          <GradientStop Color="#00000000" Offset="0.40"/>
          <GradientStop Color="#A00A0E16" Offset="0.58"/>
          <GradientStop Color="#EE0A0E16" Offset="0.70"/>
          <GradientStop Color="#FA0A0E16" Offset="1"/>
        </LinearGradientBrush>
      </Rectangle.Fill>
    </Rectangle>
    <Rectangle IsHitTestVisible="False">
      <Rectangle.Fill>
        <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
          <GradientStop Color="#80000000" Offset="0"/>
          <GradientStop Color="#00000000" Offset="0.55"/>
        </LinearGradientBrush>
      </Rectangle.Fill>
    </Rectangle>

    <Grid>
      <Grid.RowDefinitions>
        <RowDefinition Height="44"/>
        <RowDefinition Height="*"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
      </Grid.RowDefinitions>

      <!-- title bar -->
      <Grid x:Name="TitleBar" Background="Transparent">
        <StackPanel Orientation="Horizontal" Margin="22,0,0,0" VerticalAlignment="Center">
          <Ellipse Width="10" Height="10" Fill="{StaticResource Accent}"/>
          <TextBlock Text="TWONKIES  PC  PORT" Margin="10,0,0,0" FontSize="11" FontWeight="Bold" Foreground="#D0FFFFFF"/>
        </StackPanel>
        <StackPanel Orientation="Horizontal" HorizontalAlignment="Right" VerticalAlignment="Top">
          <Button x:Name="MinBtn" Style="{StaticResource Chrome}">
            <Path Data="M0,0 L10,0" Stroke="White" StrokeThickness="1.3"/>
          </Button>
          <Button x:Name="CloseBtn" Style="{StaticResource Chrome}">
            <Path Data="M0,0 L10,10 M10,0 L0,10" Stroke="White" StrokeThickness="1.3"/>
          </Button>
        </StackPanel>
      </Grid>

      <!-- hero -->
      <Grid Grid.Row="1" x:Name="Hero" Background="Transparent">
        <StackPanel x:Name="HeroText" VerticalAlignment="Bottom" Margin="40,0,0,20">
          <StackPanel.Effect><DropShadowEffect BlurRadius="20" ShadowDepth="0" Opacity="0.9"/></StackPanel.Effect>
          <TextBlock Text="PC PORT SETUP" FontSize="12" FontWeight="Bold" Foreground="#FFC266"/>
          <TextBlock Text="Attack of the Twonkies" FontSize="46" FontFamily="Segoe UI Black" Margin="-2,0,0,0"/>
          <TextBlock Text="Jimmy Neutron: Boy Genius, rebuilt as a native Windows game from your own PS2 disc."
                     FontSize="14" Foreground="#E8FFFFFF" Margin="1,2,0,0"/>
        </StackPanel>
        <Border x:Name="LogPanel" Visibility="Collapsed" HorizontalAlignment="Right" Width="620" Margin="0,2,32,14"
                Style="{StaticResource Card}" Background="#F0070A10" Padding="16,10,8,10">
          <DockPanel>
            <DockPanel DockPanel.Dock="Top" Margin="0,0,6,6">
              <Button x:Name="LogFileBtn" DockPanel.Dock="Right" Style="{StaticResource Link}" Content="Open log file"/>
              <TextBlock Text="Details" FontSize="13" FontWeight="SemiBold" VerticalAlignment="Center"/>
            </DockPanel>
            <TextBox x:Name="LogBox" Background="Transparent" Foreground="#C9D1E0" BorderThickness="0"
                     FontFamily="Consolas" FontSize="11.5" IsReadOnly="True" TextWrapping="Wrap"
                     VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled"/>
          </DockPanel>
        </Border>
      </Grid>

      <!-- the three steps -->
      <Grid Grid.Row="2" Margin="32,0,32,0" Height="180">
        <Grid.ColumnDefinitions>
          <ColumnDefinition/>
          <ColumnDefinition Width="16"/>
          <ColumnDefinition/>
          <ColumnDefinition Width="16"/>
          <ColumnDefinition/>
        </Grid.ColumnDefinitions>

        <Border Style="{StaticResource Card}">
          <DockPanel>
            <DockPanel DockPanel.Dock="Top">
              <Button x:Name="RecheckBtn" DockPanel.Dock="Right" Style="{StaticResource Link}" Content="Check again" VerticalAlignment="Center"/>
              <StackPanel Orientation="Horizontal">
                <Grid Width="26" Height="26"><Ellipse x:Name="Badge1" Fill="#30FFFFFF"/><TextBlock x:Name="Badge1Text" Text="1" Style="{StaticResource BadgeText}"/></Grid>
                <TextBlock Text="Build tools" Style="{StaticResource CardTitle}"/>
              </StackPanel>
            </DockPanel>
            <Button x:Name="ToolsBtn" DockPanel.Dock="Bottom" Style="{StaticResource Btn}" Content="Install missing tools" HorizontalAlignment="Left"/>
            <StackPanel Margin="0,14,0,0">
              <Grid Height="26">
                <Ellipse x:Name="VSDot" Style="{StaticResource Dot}"/>
                <TextBlock Text="Visual Studio C++" Style="{StaticResource ToolName}"/>
                <TextBlock x:Name="VSInfo" Text="checking" Style="{StaticResource ToolInfo}"/>
                <Button x:Name="VSGet" Style="{StaticResource Link}" Content="Download" HorizontalAlignment="Right" Visibility="Collapsed"/>
              </Grid>
              <Grid Height="26">
                <Ellipse x:Name="CMakeDot" Style="{StaticResource Dot}"/>
                <TextBlock Text="CMake" Style="{StaticResource ToolName}"/>
                <TextBlock x:Name="CMakeInfo" Text="checking" Style="{StaticResource ToolInfo}"/>
                <Button x:Name="CMakeGet" Style="{StaticResource Link}" Content="Download" HorizontalAlignment="Right" Visibility="Collapsed"/>
              </Grid>
              <Grid Height="26">
                <Ellipse x:Name="PythonDot" Style="{StaticResource Dot}"/>
                <TextBlock Text="Python 3" Style="{StaticResource ToolName}"/>
                <TextBlock x:Name="PythonInfo" Text="checking" Style="{StaticResource ToolInfo}"/>
                <Button x:Name="PythonGet" Style="{StaticResource Link}" Content="Download" HorizontalAlignment="Right" Visibility="Collapsed"/>
              </Grid>
            </StackPanel>
          </DockPanel>
        </Border>

        <Border Grid.Column="2" Style="{StaticResource Card}">
          <DockPanel>
            <StackPanel DockPanel.Dock="Top" Orientation="Horizontal">
              <Grid Width="26" Height="26"><Ellipse x:Name="Badge2" Fill="#30FFFFFF"/><TextBlock x:Name="Badge2Text" Text="2" Style="{StaticResource BadgeText}"/></Grid>
              <TextBlock Text="Your game disc" Style="{StaticResource CardTitle}"/>
            </StackPanel>
            <StackPanel DockPanel.Dock="Bottom" Orientation="Horizontal">
              <RadioButton x:Name="MoveRb" GroupName="IsoMode" Style="{StaticResource Pill}" Content="Move it in"/>
              <RadioButton x:Name="CopyRb" GroupName="IsoMode" Style="{StaticResource Pill}" Content="Keep my original" Margin="8,0,0,0"/>
            </StackPanel>
            <Border x:Name="DropZone" Margin="0,12,0,10" CornerRadius="12" Background="#0EFFFFFF" Cursor="Hand">
              <Grid>
                <Rectangle x:Name="DropRect" RadiusX="12" RadiusY="12" Stroke="#45FFFFFF" StrokeDashArray="4 3" StrokeThickness="1.3"/>
                <StackPanel Orientation="Horizontal" Margin="14,0,10,0" VerticalAlignment="Center">
                  <Grid Width="42" Height="42">
                    <Ellipse>
                      <Ellipse.Fill>
                        <LinearGradientBrush StartPoint="0,0" EndPoint="1,1">
                          <GradientStop Color="#E8EEF7" Offset="0"/>
                          <GradientStop Color="#9FD8FF" Offset="0.35"/>
                          <GradientStop Color="#D6A6FF" Offset="0.6"/>
                          <GradientStop Color="#FFE08A" Offset="0.8"/>
                          <GradientStop Color="#B8C2D6" Offset="1"/>
                        </LinearGradientBrush>
                      </Ellipse.Fill>
                    </Ellipse>
                    <Ellipse Width="13" Height="13" Fill="#0F1420" Stroke="#70FFFFFF" StrokeThickness="1.5"/>
                  </Grid>
                  <StackPanel Margin="12,0,0,0" VerticalAlignment="Center" Width="190">
                    <TextBlock x:Name="IsoName" Text="Choose your .iso" FontSize="13" FontWeight="SemiBold" TextTrimming="CharacterEllipsis"/>
                    <TextBlock x:Name="IsoInfo" Text="Click to browse, or drop it here" FontSize="11.5" Foreground="{StaticResource Muted}" TextTrimming="CharacterEllipsis" Margin="0,2,0,0"/>
                  </StackPanel>
                </StackPanel>
              </Grid>
            </Border>
          </DockPanel>
        </Border>

        <Border Grid.Column="4" Style="{StaticResource Card}">
          <DockPanel>
            <StackPanel DockPanel.Dock="Top" Orientation="Horizontal">
              <Grid Width="26" Height="26"><Ellipse x:Name="Badge3" Fill="#30FFFFFF"/><TextBlock x:Name="Badge3Text" Text="3" Style="{StaticResource BadgeText}"/></Grid>
              <TextBlock Text="Install" Style="{StaticResource CardTitle}"/>
            </StackPanel>
            <Button x:Name="InstallBtn" DockPanel.Dock="Bottom" Style="{StaticResource Btn}" Background="{StaticResource Accent}"
                    BorderThickness="0" Height="48" FontSize="17" FontWeight="Bold" Content="Install"/>
            <StackPanel Margin="0,10,0,0">
              <TextBlock x:Name="InstallInfo" TextWrapping="Wrap" FontSize="12" Foreground="{StaticResource Muted}"
                         Text="Builds the game from your disc (10-30 min)."/>
              <CheckBox x:Name="ShortcutCb" Style="{StaticResource Check}" Content="Desktop shortcut" IsChecked="True" Margin="0,10,0,0"/>
            </StackPanel>
          </DockPanel>
        </Border>
      </Grid>

      <!-- play, progress, extras -->
      <Border Grid.Row="3" Style="{StaticResource Card}" Margin="32,14,32,18" Padding="10">
        <Grid>
          <Grid.ColumnDefinitions>
            <ColumnDefinition Width="Auto"/>
            <ColumnDefinition/>
            <ColumnDefinition Width="Auto"/>
          </Grid.ColumnDefinitions>
          <Button x:Name="PlayBtn" Style="{StaticResource Btn}" Background="{StaticResource Go}" BorderThickness="0"
                  Height="46" Padding="30,0" FontSize="17" FontWeight="Bold">
            <StackPanel Orientation="Horizontal">
              <Path Data="M0,0 L12,7.5 L0,15 Z" Fill="White" VerticalAlignment="Center"/>
              <TextBlock Text="Play" Margin="11,0,0,1" VerticalAlignment="Center"/>
            </StackPanel>
          </Button>
          <StackPanel Grid.Column="1" VerticalAlignment="Center" Margin="20,0,18,0">
            <Grid>
              <TextBlock x:Name="StatusText" FontSize="13" FontWeight="SemiBold" Text="Ready when you are." TextTrimming="CharacterEllipsis" Margin="0,0,100,0"/>
              <TextBlock x:Name="StepText" HorizontalAlignment="Right" FontSize="12" Foreground="{StaticResource Muted}"/>
            </Grid>
            <ProgressBar x:Name="Bar" Style="{StaticResource Bar}" Margin="0,8,0,0" Minimum="0" Maximum="1" Value="0"/>
          </StackPanel>
          <StackPanel Grid.Column="2" Orientation="Horizontal">
            <Button x:Name="ArtBtn" Style="{StaticResource Btn}" Content="Make pictures" Height="40" Margin="0,0,8,0" Visibility="Collapsed"
                    ToolTip="Runs the game for a minute without a window and keeps a few screenshots for this window"/>
            <Button x:Name="LogBtn" Style="{StaticResource Btn}" Content="Details" Height="40"/>
            <Button x:Name="FolderBtn" Style="{StaticResource Btn}" Content="Open folder" Height="40" Margin="8,0,0,0"/>
          </StackPanel>
        </Grid>
      </Border>
    </Grid>
  </Grid>
  </Border>
</Window>
'@

$window = [Windows.Markup.XamlReader]::Parse($Xaml)
$ui = @{}
foreach ($m in [regex]::Matches($Xaml, 'x:Name="(\w+)"')) {
    $n = $m.Groups[1].Value
    $el = $window.FindName($n)
    if ($el) { $ui[$n] = $el }
}

$BrushConv = New-Object Windows.Media.BrushConverter
function Get-Brush([string]$c) { $BrushConv.ConvertFromString($c) }
$Green = Get-Brush '#4ADE80'
$Red = Get-Brush '#FF6B6B'
$Muted = Get-Brush '#A9B3C7'
$White = Get-Brush '#FFFFFF'
$CheckMark = [string][char]0x2713

function Start-Anim($target, $prop, [double]$to, [double]$secs, $from = $null, [switch]$Linear) {
    $a = New-Object Windows.Media.Animation.DoubleAnimation
    $a.To = $to
    if ($null -ne $from) { $a.From = [double]$from }
    $a.Duration = New-Object Windows.Duration ([TimeSpan]::FromSeconds($secs))
    if (-not $Linear) {
        $e = New-Object Windows.Media.Animation.QuadraticEase
        $e.EasingMode = 'EaseInOut'
        $a.EasingFunction = $e
    }
    $target.BeginAnimation($prop, $a)
}

# Window icon: an orange dot like the one in the title bar.
$iconVisual = New-Object Windows.Media.DrawingVisual
$dc = $iconVisual.RenderOpen()
$dc.DrawEllipse($window.Resources['Accent'], $null, (New-Object Windows.Point 32, 32), 28, 28)
$dc.DrawEllipse($White, $null, (New-Object Windows.Point 32, 32), 9, 9)
$dc.Close()
$iconBmp = New-Object Windows.Media.Imaging.RenderTargetBitmap 64, 64, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
$iconBmp.Render($iconVisual)
$window.Icon = $iconBmp

# Stars for our own backdrop.
$rnd = New-Object Random 11
for ($i = 0; $i -lt 150; $i++) {
    $e = New-Object Windows.Shapes.Ellipse
    $d = 0.8 + $rnd.NextDouble() * 1.9
    $e.Width = $d; $e.Height = $d
    $e.Fill = $White
    $e.Opacity = 0.15 + $rnd.NextDouble() * 0.75
    [Windows.Controls.Canvas]::SetLeft($e, $rnd.NextDouble() * 1080)
    [Windows.Controls.Canvas]::SetTop($e, $rnd.NextDouble() * 520)
    [void]$ui.Stars.Children.Add($e)
}

# ---------------------------------------------------------------- pictures

$script:ArtFiles = @()
$script:ArtIndex = -1
$script:Front = $ui.ArtA
$script:Back = $ui.ArtB

function Get-Bitmap([string]$path) {
    $bi = New-Object Windows.Media.Imaging.BitmapImage
    $bi.BeginInit()
    $bi.CacheOption = [Windows.Media.Imaging.BitmapCacheOption]::OnLoad   # do not keep the file open
    $bi.UriSource = New-Object Uri $path
    $bi.DecodePixelWidth = 1600
    $bi.EndInit()
    $bi.Freeze()
    $bi
}

function Show-NextArt {
    if (-not $script:ArtFiles.Count) { return }
    $script:ArtIndex = ($script:ArtIndex + 1) % $script:ArtFiles.Count
    $file = $script:ArtFiles[$script:ArtIndex]
    $img = $script:Back
    try { $img.Source = Get-Bitmap $file } catch { return }
    Start-Anim $img ([Windows.UIElement]::OpacityProperty) 1 1.8 0
    Start-Anim $script:Front ([Windows.UIElement]::OpacityProperty) 0 1.8
    $st = $img.RenderTransform
    Start-Anim $st ([Windows.Media.ScaleTransform]::ScaleXProperty) 1.09 14 1.0 -Linear
    Start-Anim $st ([Windows.Media.ScaleTransform]::ScaleYProperty) 1.09 14 1.0 -Linear
    # The title screen carries the game's own logo: let it speak for itself.
    $isTitle = [IO.Path]::GetFileNameWithoutExtension($file) -eq "f$ArtTitleFrame"
    Start-Anim $ui.HeroText ([Windows.UIElement]::OpacityProperty) $(if ($isTitle) { 0 } else { 1 }) 1.2
    $script:Back = $script:Front
    $script:Front = $img
}

$artTimer = New-Object Windows.Threading.DispatcherTimer
$artTimer.Interval = [TimeSpan]::FromSeconds(7)
$artTimer.Add_Tick({ Show-NextArt })

function Update-Art {
    $script:ArtFiles = if ($ShotNoArt) { @() } else { Get-ArtFiles }
    if ($script:ArtFiles.Count) {
        Start-Anim $ui.Backdrop ([Windows.UIElement]::OpacityProperty) 0 1.8
        if ($Shot) { $script:ArtIndex = $ShotSlide - 1 }
        Show-NextArt
        if ($script:ArtFiles.Count -gt 1) { $artTimer.Start() }
    }
}

# ---------------------------------------------------------------- state

$script:Tools = @{}
$script:S = $null
$script:PS = $null
$script:IsoPath = $null
$script:IsoOk = $false
$script:Installed = $false

function Test-Busy { [bool]($script:S -and -not $script:S.Done) }

function Set-Badge([int]$n, [string]$state) {
    $e = $ui["Badge$n"]; $t = $ui["Badge$n" + 'Text']
    switch ($state) {
        'done'   { $e.Fill = $Green; $t.Text = $CheckMark; $t.Foreground = Get-Brush '#0B2A15' }
        'active' { $e.Fill = $window.Resources['Accent']; $t.Text = "$n"; $t.Foreground = $White }
        'error'  { $e.Fill = $Red; $t.Text = '!'; $t.Foreground = $White }
        default  { $e.Fill = Get-Brush '#30FFFFFF'; $t.Text = "$n"; $t.Foreground = $White }
    }
}

function Update-Ui {
    $busy = Test-Busy
    $haveAll = [bool]($script:Tools.VS -and $script:Tools.CMake -and $script:Tools.Python)
    $built = Test-Path $TwonkExe
    $ui.InstallBtn.IsEnabled = (-not $busy) -and $haveAll -and $script:IsoOk
    $ui.InstallBtn.Content = $(if ($built) { 'Rebuild' } else { 'Install' })
    $ui.ToolsBtn.Visibility = $(if ($haveAll) { 'Collapsed' } else { 'Visible' })
    $ui.ToolsBtn.IsEnabled = -not $busy
    $ui.RecheckBtn.IsEnabled = -not $busy
    $ui.DropZone.IsEnabled = -not $busy
    $ui.MoveRb.IsEnabled = -not $busy
    $ui.CopyRb.IsEnabled = -not $busy
    $ui.ShortcutCb.IsEnabled = -not $busy
    $ui.PlayBtn.IsEnabled = (-not $busy) -and $built -and [bool](Get-ProjectIso)
    $ui.ArtBtn.Visibility = $(if ($built -and -not $busy -and -not $script:ArtFiles.Count -and (Get-ProjectIso)) { 'Visible' } else { 'Collapsed' })
    if (-not $busy) {
        Set-Badge 1 $(if ($haveAll) { 'done' } else { 'active' })
        Set-Badge 2 $(if ($script:IsoOk) { 'done' } elseif ($haveAll) { 'active' } else { 'todo' })
        Set-Badge 3 $(if ($script:Installed -or ($built -and -not $script:S)) { 'done' } elseif ($haveAll -and $script:IsoOk) { 'active' } else { 'todo' })
    }
}

function Update-Tools {
    $window.Cursor = [Windows.Input.Cursors]::Wait
    $script:Tools = Get-Tools
    $window.Cursor = $null
    $show = @{
        VS     = { param($t) ($t.Name -replace '^Visual Studio ', '') }
        CMake  = { param($t) $t.Version }
        Python = { param($t) $t.Version }
    }
    foreach ($k in @('VS', 'CMake', 'Python')) {
        $t = $script:Tools[$k]
        if ($t) {
            $ui["${k}Dot"].Fill = $Green
            $ui["${k}Info"].Text = & $show[$k] $t
            $ui["${k}Info"].Visibility = 'Visible'
            $ui["${k}Get"].Visibility = 'Collapsed'
        } else {
            $ui["${k}Dot"].Fill = $Red
            $ui["${k}Info"].Visibility = 'Collapsed'
            $ui["${k}Get"].Visibility = 'Visible'
        }
    }
    Update-Ui
}

function Set-Iso([string]$p) {
    $script:IsoPath = $p
    $script:IsoOk = $false
    if (-not $p) {
        $ui.IsoName.Text = 'Choose your .iso'
        $ui.IsoInfo.Text = 'Click to browse, or drop it here'
        $ui.IsoInfo.Foreground = $Muted
    } elseif (-not (Test-Path -LiteralPath $p)) {
        $ui.IsoName.Text = [IO.Path]::GetFileName($p)
        $ui.IsoInfo.Text = 'File not found'
        $ui.IsoInfo.Foreground = $Red
    } elseif (-not (Test-IsoImage $p)) {
        $ui.IsoName.Text = [IO.Path]::GetFileName($p)
        $ui.IsoInfo.Text = 'Not a disc image (.iso)'
        $ui.IsoInfo.Foreground = $Red
    } else {
        $script:IsoOk = $true
        $ui.IsoName.Text = [IO.Path]::GetFileNameWithoutExtension($p)
        $gb = (Get-Item -LiteralPath $p).Length / 1GB
        $inProject = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($p)) -ieq $IsoDir
        $where = $(if ($inProject) { 'in the project' } else { Split-Path $p })
        $ui.IsoInfo.Text = ('{0:N2} GB, {1}' -f $gb, $where)
        $ui.IsoInfo.Foreground = $Muted
        $ui.DropZone.ToolTip = $p
    }
    Update-Ui
}

function Add-Log([string]$t) {
    if (-not $t) { return }
    if ($ui.LogBox.Text.Length -gt 1500000) { $ui.LogBox.Text = $ui.LogBox.Text.Substring($ui.LogBox.Text.Length - 500000) }
    $ui.LogBox.AppendText($t)
    $ui.LogBox.ScrollToEnd()
}

function Set-Status([string]$text, $brush) {
    $ui.StatusText.Text = $text
    $ui.StatusText.Foreground = $(if ($brush) { $brush } else { $White })
}

function Set-Progress([double]$v) {
    Start-Anim $ui.Bar ([Windows.Controls.Primitives.RangeBase]::ValueProperty) $v 0.6
}

function Show-Log([bool]$on) {
    $ui.LogPanel.Visibility = $(if ($on) { 'Visible' } else { 'Collapsed' })
    $ui.LogBtn.Content = $(if ($on) { 'Hide details' } else { 'Details' })
}

function Start-SetupJob([hashtable]$job) {
    $ui.LogBox.Clear()
    Set-Status 'Starting...'
    Set-Progress 0
    $ui.StepText.Text = ''
    [void](Start-Worker $job)
    Update-Ui
    $pollTimer.Start()
}

$pollTimer = New-Object Windows.Threading.DispatcherTimer
$pollTimer.Interval = [TimeSpan]::FromMilliseconds(150)
$pollTimer.Add_Tick({
    $S = $script:S
    if (-not $S) { return }
    $done = $S.Done
    Add-Log (Read-Queue)
    $total = [Math]::Max(1, $S.Total)
    if ($S.Step -gt 0) {
        $ui.StepText.Text = 'Step {0} of {1}' -f [Math]::Min($S.Step, $total), $total
        # a step counts as half done while it runs
        $target = [Math]::Min(1.0, ($S.Step - $(if ($done) { 0 } else { 0.5 })) / $total)
        if ([Math]::Abs($target - $script:LastTarget) -gt 0.001) { $script:LastTarget = $target; Set-Progress $target }
    }
    if (-not $done) { Set-Status $S.StepName; return }

    $pollTimer.Stop()
    $script:LastTarget = -1
    if ($S.ArtChanged) { $artTimer.Stop(); Update-Art }
    if ($S.Mode -eq 'tools') {
        Update-Tools
        if ($S.Error) { Set-Status 'A tool install failed. See Details, or use the Download links.' $Red; Show-Log $true }
        else { Set-Status 'Tools installed. If one still shows Download, restart Windows and run Setup again.' }
    }
    elseif ($S.Mode -eq 'art') {
        if ($S.Error) { Set-Status ('Could not take pictures: ' + $S.Error) $Red; Show-Log $true }
        else { Set-Status 'Pictures ready.' $Green; Set-Progress 1 }
    }
    elseif ($S.Error) {
        Set-Status ('Setup failed: ' + $S.Error) $Red
        Set-Badge 3 'error'
        Show-Log $true
    }
    else {
        $script:Installed = $true
        $msg = 'All done. Press Play!'
        if ($ui.ShortcutCb.IsChecked) {
            try { [void](New-GameShortcut $S.IsoFinal); $msg = 'All done. Press Play, or use the new desktop shortcut.' }
            catch { Add-Log "Could not create the shortcut: $($_.Exception.Message)`r`n" }
        }
        Set-Iso $S.IsoFinal
        Set-Status $msg $Green
        Set-Progress 1
    }
    Update-Ui
})
$script:LastTarget = -1

# ---------------------------------------------------------------- events

$ui.TitleBar.Add_MouseLeftButtonDown({ $window.DragMove() })
$ui.Hero.Add_MouseLeftButtonDown({ $window.DragMove() })
$ui.MinBtn.Add_Click({ $window.WindowState = 'Minimized' })
$ui.CloseBtn.Add_Click({ $window.Close() })

foreach ($k in @('VS', 'CMake', 'Python')) {
    $ui["${k}Get"].Tag = $ToolUrls[$k]
    $ui["${k}Get"].Add_Click({ param($s, $e) Start-Process $s.Tag })
}
$ui.RecheckBtn.Add_Click({ Update-Tools })

$ui.ToolsBtn.Add_Click({
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) {
        [void][Windows.MessageBox]::Show($window, 'winget is not available on this PC. Use the Download links, then click Check again.', 'Install tools')
        return
    }
    $need = @(@('VS', 'CMake', 'Python') | Where-Object { -not $script:Tools[$_] })
    $list = ($need | ForEach-Object { '  - ' + $ToolNames[$_] }) -join "`n"
    $ok = [Windows.MessageBox]::Show($window, "Install these with winget?`n`n$list`n`nVisual Studio Build Tools is a large download (several GB). Windows may ask for permission.", 'Install tools', 'OKCancel', 'Question')
    if ($ok -ne 'OK') { return }
    Start-SetupJob @{ Mode = 'tools'; Install = $need; Winget = $winget.Source; WingetArgs = $WingetArgs; ToolNames = $ToolNames }
})

function Select-Iso {
    if (Test-Busy) { return }
    $d = New-Object Microsoft.Win32.OpenFileDialog
    $d.Title = 'Select your Attack of the Twonkies disc image'
    $d.Filter = 'Disc images (*.iso)|*.iso|All files (*.*)|*.*'
    if ($script:IsoPath -and (Test-Path -LiteralPath $script:IsoPath)) { $d.InitialDirectory = Split-Path $script:IsoPath }
    if ($d.ShowDialog($window)) { Set-Iso $d.FileName }
}
$ui.DropZone.Add_MouseLeftButtonUp({ Select-Iso })
$ui.DropZone.Add_MouseEnter({ if (-not (Test-Busy)) { $ui.DropRect.Stroke = Get-Brush '#FF9A3C' } })
$ui.DropZone.Add_MouseLeave({ $ui.DropRect.Stroke = Get-Brush '#45FFFFFF' })

$window.Add_DragOver({
    param($s, $e)
    $e.Effects = $(if ($e.Data.GetDataPresent([Windows.DataFormats]::FileDrop) -and -not (Test-Busy)) { 'Copy' } else { 'None' })
    $e.Handled = $true
})
$window.Add_Drop({
    param($s, $e)
    if (Test-Busy) { return }
    $files = $e.Data.GetData([Windows.DataFormats]::FileDrop)
    if ($files -and $files.Count) { Set-Iso $files[0] }
})

$ui.InstallBtn.Add_Click({
    $iso = [IO.Path]::GetFullPath($script:IsoPath)
    $script:Installed = $false
    Set-Badge 3 'active'
    Start-SetupJob (New-InstallJob $script:Tools $iso ([bool]$ui.MoveRb.IsChecked))
})

$ui.ArtBtn.Add_Click({ Start-SetupJob @{ Mode = 'art'; Iso = (Get-ProjectIso) } })

$ui.PlayBtn.Add_Click({
    Start-Process -FilePath $TwonkExe -ArgumentList (Get-GameArgs (Get-ProjectIso)) -WorkingDirectory $Root
})
$ui.FolderBtn.Add_Click({ Start-Process explorer.exe $Root })
$ui.LogBtn.Add_Click({ Show-Log ($ui.LogPanel.Visibility -ne 'Visible') })
$ui.LogFileBtn.Add_Click({ if (Test-Path $LogFile) { Start-Process notepad.exe $LogFile } })

$window.Add_Closing({
    param($s, $e)
    if ((Test-Busy) -and -not $script:ForceClose) {
        $r = [Windows.MessageBox]::Show($window, 'Setup is still running. Stop it and close?', 'Close', 'YesNo', 'Warning')
        if ($r -ne 'Yes') { $e.Cancel = $true; return }
        Stop-Worker
    }
})

$window.Add_ContentRendered({
    Update-Tools
    if ($Iso) { Set-Iso $Iso } else { Set-Iso (Get-ProjectIso) }
    Update-Art
    Update-Ui
    if ($Shot) {
        $t = New-Object Windows.Threading.DispatcherTimer
        $t.Interval = [TimeSpan]::FromSeconds($(if ($ShotJob) { 25 } else { 2.5 }))
        if ($ShotJob) { Show-Log $true; Start-SetupJob @{ Mode = 'art'; Iso = (Get-ProjectIso) } }
        $t.Add_Tick({
            $this.Stop()
            $artTimer.Stop()
            $rtb = New-Object Windows.Media.Imaging.RenderTargetBitmap ([int]$window.ActualWidth), ([int]$window.ActualHeight), 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
            $rtb.Render($window)
            $enc = New-Object Windows.Media.Imaging.PngBitmapEncoder
            $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($rtb))
            $fs = [IO.File]::Create($Shot)
            try { $enc.Save($fs) } finally { $fs.Dispose() }
            Stop-Worker
            $script:ForceClose = $true
            $window.Close()
        })
        $t.Start()
    }
})

$ui.MoveRb.IsChecked = -not $Copy
$ui.CopyRb.IsChecked = [bool]$Copy
if ($Shot) { $window.WindowStartupLocation = 'Manual'; $window.Left = -6000; $window.Top = 0; $window.ShowInTaskbar = $false }

$script:ForceClose = $false
try { [void]$window.ShowDialog() }
catch { [void][Windows.MessageBox]::Show("Setup error: $($_.Exception.Message)", 'Setup') }
# A stopped job's runspace can keep this hidden process alive: end it with the window.
Stop-Worker
[Environment]::Exit(0)
