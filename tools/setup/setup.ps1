# Setup for the Twonkies PC port: asks for your disc image, puts it in the project,
# and runs every build step (extract, verify, analyse, recompile, build).
#
#   Double-click Setup.cmd in the repository root, or:
#   powershell -ExecutionPolicy Bypass -File tools\setup\setup.ps1 [-Headless -Iso <image> [-Copy]] [-Root <dir>]
#
# The image is moved into "Game iso\" (use -Copy, or the Copy option, to keep the original).
param(
    [string]$Root,
    [string]$Iso,
    [switch]$Copy,
    [switch]$Headless,
    [string]$Shot    # development: render the window to this PNG and exit
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

    try {
        if ($S.Mode -eq 'tools') {
            $S.Total = $S.Install.Count
            foreach ($k in $S.Install) {
                Step ('Installing ' + $S.ToolNames[$k])
                Run $S.Winget ($S.WingetArgs[$k] + @('--accept-package-agreements', '--accept-source-agreements'))
            }
            return
        }

        $root = $S.Root
        $py = $S.PyExe; $pa = @($S.PyArgs)
        $cmake = $S.CMake
        $disc = Join-Path $root 'extracted\disc'
        $staging = Join-Path $root 'extracted\disc.new'
        $gen = Join-Path $root 'build\generated'
        $S.Total = 9

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

        Step 'Building the game (this is the long one)'
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
    if ($script:S.Pid) { & taskkill.exe /T /F /PID $script:S.Pid 2>&1 | Out-Null }
    [void]$script:PS.BeginStop($null, $null)
}

function New-InstallJob([hashtable]$tools, [string]$iso, [bool]$move) {
    @{
        Mode = 'install'; Iso = $iso; MoveIso = $move; IsoDir = $IsoDir; TwonkExe = $TwonkExe
        ElfName = $ElfName; ElfSha1 = $ElfSha1
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
    $tools = Get-Tools
    $missing = @($ToolNames.Keys | Where-Object { -not $tools[$_] } | ForEach-Object { $ToolNames[$_] })
    if ($missing.Count) { Write-Host "Missing: $($missing -join ', ')"; exit 1 }
    if (-not $Iso -or -not (Test-IsoImage $Iso)) { Write-Host 'Give -Iso <your disc image (.iso)>.'; exit 1 }
    $S = Start-Worker (New-InstallJob $tools $Iso (-not $Copy))
    while (-not $S.Done) { Start-Sleep -Milliseconds 250; [Console]::Out.Write((Read-Queue)) }
    [Console]::Out.Write((Read-Queue))
    if ($S.Error) { exit 1 }
    exit 0
}

# ---------------------------------------------------------------- window

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
try {
    Add-Type -Namespace TwonkSetup -Name Native -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
    [void][TwonkSetup.Native]::SetProcessDPIAware()
} catch {}

$Green = [Drawing.Color]::FromArgb(0, 128, 0)
$Red = [Drawing.Color]::FromArgb(192, 0, 0)
$Gray = [Drawing.Color]::FromArgb(96, 96, 96)

function Add-Control($parent, [string]$type, [int]$x, [int]$y, [int]$w, [int]$h, [string]$text) {
    $c = New-Object "System.Windows.Forms.$type"
    $c.Location = New-Object Drawing.Point($x, $y)
    $c.Size = New-Object Drawing.Size($w, $h)
    if ($text) { $c.Text = $text }
    $parent.Controls.Add($c)
    $c
}

$form = New-Object Windows.Forms.Form
$form.SuspendLayout()
$form.AutoScaleDimensions = New-Object Drawing.SizeF(96, 96)
$form.AutoScaleMode = 'Dpi'
$form.Font = New-Object Drawing.Font('Segoe UI', 9)
$form.Text = "$GameTitle - PC Setup"
$form.ClientSize = New-Object Drawing.Size(720, 650)
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'

$title = Add-Control $form Label 16 12 690 26 $GameTitle
$title.Font = New-Object Drawing.Font('Segoe UI', 13, [Drawing.FontStyle]::Bold)
$sub = Add-Control $form Label 18 40 690 18 'PC port setup. You need your own copy of the USA PS2 disc (SLUS-208.87) as an .iso image.'
$sub.ForeColor = $Gray

# 1. tools
$gTools = Add-Control $form GroupBox 16 66 688 132 '1. Build tools'
$toolRows = @{}
$urls = @{
    VS     = 'https://visualstudio.microsoft.com/downloads/'
    CMake  = 'https://cmake.org/download/'
    Python = 'https://www.python.org/downloads/'
}
$yy = 24
foreach ($k in @('VS', 'CMake', 'Python')) {
    [void](Add-Control $gTools Label 12 $yy 200 20 $ToolNames[$k])
    $st = Add-Control $gTools Label 214 $yy 380 20 'Checking...'
    $ln = Add-Control $gTools LinkLabel 600 $yy 80 20 'Download'
    $ln.Tag = $urls[$k]
    $ln.Add_LinkClicked({ param($s, $e) Start-Process $s.Tag })
    $toolRows[$k] = @{ Status = $st; Link = $ln }
    $yy += 24
}
$btnTools = Add-Control $gTools Button 12 96 190 28 'Install missing tools'
$btnRecheck = Add-Control $gTools Button 208 96 100 28 'Check again'
$toolsNote = Add-Control $gTools Label 316 102 364 20 ''
$toolsNote.ForeColor = $Gray

# 2. disc image
$gIso = Add-Control $form GroupBox 16 206 688 102 '2. Your disc image'
$isoBox = Add-Control $gIso TextBox 12 26 560 24 ''
$btnBrowse = Add-Control $gIso Button 580 24 96 28 'Browse...'
$rbMove = Add-Control $gIso RadioButton 12 58 260 22 'Move it into the project folder'
$rbCopy = Add-Control $gIso RadioButton 280 58 300 22 'Copy it (keeps the original, uses 1.2 GB more)'
$rbMove.Checked = -not $Copy
$rbCopy.Checked = [bool]$Copy
$isoNote = Add-Control $gIso Label 12 80 664 18 ''
$isoNote.ForeColor = $Gray

# 3. install
$gRun = Add-Control $form GroupBox 16 316 688 114 '3. Install'
$cbShortcut = Add-Control $gRun CheckBox 12 26 300 22 'Create a desktop shortcut'
$cbShortcut.Checked = $true
$btnInstall = Add-Control $gRun Button 548 20 128 34 'Install'
$btnInstall.Font = New-Object Drawing.Font('Segoe UI', 10, [Drawing.FontStyle]::Bold)
$bar = Add-Control $gRun ProgressBar 12 62 664 18 ''
$status = Add-Control $gRun Label 12 86 664 20 'Pick your disc image, then click Install. The build takes a while (around 10-30 minutes).'

$logBox = Add-Control $form TextBox 16 440 688 160 ''
$logBox.Multiline = $true
$logBox.ScrollBars = 'Vertical'
$logBox.ReadOnly = $true
$logBox.WordWrap = $false
$logBox.MaxLength = 0
$logBox.BackColor = [Drawing.Color]::White
$logBox.Font = New-Object Drawing.Font('Consolas', 8.5)

$btnPlay = Add-Control $form Button 16 610 110 30 'Play'
$btnFolder = Add-Control $form Button 134 610 150 30 'Open project folder'
$btnLog = Add-Control $form Button 292 610 100 30 'Open log'
$btnClose = Add-Control $form Button 594 610 110 30 'Close'

$form.ResumeLayout($false)

$script:Tools = @{}
$script:S = $null
$script:PS = $null

function Update-Buttons {
    $busy = $script:S -and -not $script:S.Done
    $haveAll = $script:Tools.VS -and $script:Tools.CMake -and $script:Tools.Python
    $btnInstall.Enabled = (-not $busy) -and $haveAll -and $isoBox.Text -and (Test-Path -LiteralPath $isoBox.Text)
    $btnTools.Enabled = (-not $busy) -and (-not $haveAll)
    $btnRecheck.Enabled = -not $busy
    $btnBrowse.Enabled = -not $busy
    $isoBox.Enabled = -not $busy
    $rbMove.Enabled = -not $busy
    $rbCopy.Enabled = -not $busy
    $btnPlay.Enabled = (-not $busy) -and (Test-Path $TwonkExe) -and [bool](Get-ProjectIso)
    $btnLog.Enabled = Test-Path $LogFile
}

function Update-Tools {
    $form.Cursor = 'WaitCursor'
    $script:Tools = Get-Tools
    $form.Cursor = 'Default'
    $show = @{
        VS     = { param($t) $t.Name }
        CMake  = { param($t) "CMake $($t.Version)" }
        Python = { param($t) "Python $($t.Version)" }
    }
    foreach ($k in $toolRows.Keys) {
        $t = $script:Tools[$k]
        $row = $toolRows[$k]
        if ($t) { $row.Status.Text = 'OK - ' + (& $show[$k] $t); $row.Status.ForeColor = $Green; $row.Link.Visible = $false }
        else { $row.Status.Text = 'Not found (needed)'; $row.Status.ForeColor = $Red; $row.Link.Visible = $true }
    }
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { $toolsNote.Text = 'winget is not available: use the Download links.' }
    elseif ($script:Tools.VS -and $script:Tools.CMake -and $script:Tools.Python) { $toolsNote.Text = '' }
    else { $toolsNote.Text = 'Installs with winget; Windows may ask for permission.' }
    Update-Buttons
}

function Update-IsoNote {
    $p = $isoBox.Text
    if (-not $p) { $isoNote.Text = ''; Update-Buttons; return }
    if (-not (Test-Path -LiteralPath $p)) { $isoNote.Text = 'File not found.'; $isoNote.ForeColor = $Red }
    elseif (-not (Test-IsoImage $p)) { $isoNote.Text = 'This does not look like a disc image (.iso, ISO9660).'; $isoNote.ForeColor = $Red }
    else {
        $gb = (Get-Item -LiteralPath $p).Length / 1GB
        $isoNote.Text = ('Disc image, {0:N2} GB. The game version is checked during install.' -f $gb)
        $isoNote.ForeColor = $Gray
    }
    Update-Buttons
}

function Add-Log([string]$t) {
    if (-not $t) { return }
    if ($logBox.TextLength -gt 1500000) { $logBox.Text = $logBox.Text.Substring($logBox.TextLength - 500000) }
    $logBox.AppendText($t)
}

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 150
$timer.Add_Tick({
    $S = $script:S
    if (-not $S) { return }
    $done = $S.Done
    Add-Log (Read-Queue)
    $bar.Maximum = [Math]::Max(1, $S.Total)
    $bar.Value = [Math]::Min($S.Step, $bar.Maximum)
    if (-not $done) { $status.Text = $S.StepName; return }

    $timer.Stop()
    if ($S.Mode -eq 'tools') {
        Update-Tools
        if ($S.Error) { $status.Text = 'A tool install failed; see the log, or use the Download links.'; $status.ForeColor = $Red }
        else { $status.Text = 'Tools installed. If one still shows Not found, restart Windows and run Setup again.'; $status.ForeColor = $Gray }
    }
    elseif ($S.Error) {
        $status.Text = 'Setup failed: ' + $S.Error
        $status.ForeColor = $Red
        [void][Windows.Forms.MessageBox]::Show($form, $S.Error, 'Setup failed', 'OK', 'Error')
    }
    else {
        $msg = 'Done. Click Play to start the game.'
        if ($cbShortcut.Checked) {
            try { [void](New-GameShortcut $S.IsoFinal); $msg = 'Done. Start the game with Play or the desktop shortcut.' }
            catch { Add-Log "Could not create the shortcut: $($_.Exception.Message)`r`n" }
        }
        $isoBox.Text = $S.IsoFinal
        $status.Text = $msg
        $status.ForeColor = $Green
        [void][Windows.Forms.MessageBox]::Show($form, "$msg`n`nF4 opens the video settings in game.", 'Setup finished', 'OK', 'Information')
    }
    Update-Buttons
})

$btnBrowse.Add_Click({
    $d = New-Object Windows.Forms.OpenFileDialog
    $d.Title = 'Select your Attack of the Twonkies disc image'
    $d.Filter = 'Disc images (*.iso)|*.iso|All files (*.*)|*.*'
    if ($isoBox.Text -and (Test-Path -LiteralPath $isoBox.Text)) { $d.InitialDirectory = Split-Path $isoBox.Text }
    if ($d.ShowDialog($form) -eq 'OK') { $isoBox.Text = $d.FileName }
})
$isoBox.Add_TextChanged({ Update-IsoNote })
$btnRecheck.Add_Click({ Update-Tools })

$btnTools.Add_Click({
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) {
        [void][Windows.Forms.MessageBox]::Show($form, 'winget is not available on this PC. Use the Download links, then click Check again.', 'Install tools')
        return
    }
    $need = @(@('VS', 'CMake', 'Python') | Where-Object { -not $script:Tools[$_] })
    $list = ($need | ForEach-Object { '  - ' + $ToolNames[$_] }) -join "`n"
    $ok = [Windows.Forms.MessageBox]::Show($form, "Install these with winget?`n`n$list`n`nVisual Studio Build Tools is a large download (several GB).", 'Install tools', 'OKCancel', 'Question')
    if ($ok -ne 'OK') { return }
    $logBox.Clear()
    $status.ForeColor = [Drawing.SystemColors]::ControlText
    [void](Start-Worker @{ Mode = 'tools'; Install = $need; Winget = $winget.Source; WingetArgs = $WingetArgs; ToolNames = $ToolNames })
    Update-Buttons
    $timer.Start()
})

$btnInstall.Add_Click({
    $iso = [IO.Path]::GetFullPath($isoBox.Text)
    if (-not (Test-IsoImage $iso)) {
        [void][Windows.Forms.MessageBox]::Show($form, 'That file is not a disc image (.iso).', 'Install', 'OK', 'Warning')
        return
    }
    $logBox.Clear()
    $status.ForeColor = [Drawing.SystemColors]::ControlText
    [void](Start-Worker (New-InstallJob $script:Tools $iso $rbMove.Checked))
    Update-Buttons
    $timer.Start()
})

$btnPlay.Add_Click({
    $iso = Get-ProjectIso
    Start-Process -FilePath $TwonkExe -ArgumentList (Get-GameArgs $iso) -WorkingDirectory $Root
})
$btnFolder.Add_Click({ Start-Process explorer.exe $Root })
$btnLog.Add_Click({ if (Test-Path $LogFile) { Start-Process notepad.exe $LogFile } })
$btnClose.Add_Click({ $form.Close() })

$form.Add_FormClosing({
    param($s, $e)
    if ($script:S -and -not $script:S.Done) {
        $r = [Windows.Forms.MessageBox]::Show($form, 'Setup is still running. Stop it and close?', 'Close', 'YesNo', 'Warning')
        if ($r -ne 'Yes') { $e.Cancel = $true; return }
        Stop-Worker
    }
})

$form.Add_Shown({
    Update-Tools
    if ($Iso) { $isoBox.Text = $Iso }
    else {
        $existing = Get-ProjectIso
        if ($existing) { $isoBox.Text = $existing }
    }
    Update-IsoNote
    if ($Shot) {
        $form.Refresh()
        $bmp = New-Object Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($bmp, (New-Object Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $bmp.Save($Shot, [Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $form.Close()
    }
})

if ($Shot) { $form.StartPosition = 'Manual'; $form.Location = New-Object Drawing.Point(-4000, -4000) }

try { [void]$form.ShowDialog() }
catch { [void][Windows.Forms.MessageBox]::Show("Setup error: $($_.Exception.Message)", 'Setup') }
