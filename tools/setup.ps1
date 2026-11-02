# Goes from a game disc image to a built game in one command. Double-click Setup.cmd in the repo root, or run:
#
#   powershell -ExecutionPolicy Bypass -File tools/setup.ps1 [-Iso <path to your .iso, or an extracted disc folder>]
#
# With no -Iso it asks for the disc image in a file dialog. Missing tools (Git, Python 3.12, the Visual Studio 2022
# Build Tools with CMake and Ninja) are installed with winget after one question. Each step is skipped when its
# output is already there, so rerunning after a failure carries on where it stopped.
#
#   -Yes             ask nothing: install what is missing and make the desktop shortcut
#   -NoShortcut      do not make the desktop shortcut
#   -Regenerate      rerun the recompiler even if src/recomp/gen exists (after changing hooks in src/recomp_manual.c)
#   -AllowOtherDump  carry on when default.xbe is not the dump this project targets; expect it not to work
#
# Progress lines read "[n/9] What it's doing", and a failure ends with "Setup stopped: <why>" and exit code 1.
param([Alias("GameDir")][string]$Iso, [switch]$Yes, [switch]$NoShortcut, [switch]$Regenerate, [switch]$AllowOtherDump)
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$repo = Split-Path -Parent $PSScriptRoot
$toolkit = Join-Path $repo "third_party\xboxrecomp"
# Keep in step with the submodule. Used when the repo came as a zip, which leaves third_party/xboxrecomp empty.
$toolkitUrl = "https://github.com/sp00nznet/xboxrecomp.git"
$toolkitPin = "f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b"
$assets = Join-Path $repo "assets"
$xbe = Join-Path $assets "default.xbe"
$gen = Join-Path $repo "src\recomp\gen"
$venv = Join-Path $repo "build\venv"
$py = Join-Path $venv "Scripts\python.exe"
$exePath = Join-Path $repo "build\game\ucc2_recomp.exe"
$expectedSha1 = "1f6e3417b6a912b5d3503593fd54d9b7a92c4bd0"
$patches = @("xboxrecomp-decode-stream.patch", "xboxrecomp-extvma-buffers.patch", "xboxrecomp-gpu-backend.patch") |
    ForEach-Object { Join-Path $repo "patches\$_" }
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

$script:step = 0
function Step([string]$name) { $script:step++; Write-Host ""; Write-Host "[$script:step/9] $name" -ForegroundColor Cyan }
function Fail([string]$msg) { Write-Host ""; Write-Host "Setup stopped: $msg" -ForegroundColor Red; exit 1 }
function Run {
    $exe, $rest = $args
    & $exe @rest
    if ($LASTEXITCODE) { Fail "$(Split-Path -Leaf $exe) $rest exited with $LASTEXITCODE" }
}
# Windows PowerShell turns a native command's stderr into an error under "Stop", so cmd swallows it.
function Quiet([string]$line) { cmd /c "$line 2>nul" }
function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}
function Ask([string]$question) {
    if ($Yes) { return $true }
    $a = Read-Host "$question [Y/n]"
    return ($a -eq "" -or $a -match "^[yY]")
}

# Python 3.12 through the py launcher, else a python on PATH that is 3.12. The Store's python stub prints nothing.
function Find-Python {
    $p = Quiet "py -3.12 -c `"import sys; print(sys.executable)`""
    if (-not $LASTEXITCODE -and $p) { return "$p".Trim() }
    if (Get-Command python -ErrorAction SilentlyContinue) {
        $v = Quiet "python -c `"import sys; print('%d.%d' % sys.version_info[:2], sys.executable)`""
        if (-not $LASTEXITCODE -and "$v" -match "^3\.12 (.+)$") { return $Matches[1].Trim() }
    }
    return $null
}
# Visual Studio 2022 with the C++ workload, preferred over a newer version as tools/build.cmd does.
function Find-VS {
    if (-not (Test-Path $vswhere)) { return $null }
    $req = @("-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath")
    $v = & $vswhere -version "[17.0,18.0)" @req
    if (-not $v) { $v = & $vswhere @req }
    return $v
}
function Has-CMakeNinja([string]$vs) {
    $found = Quiet "call `"$(Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat')`" >nul && where cmake && where ninja"
    return (("$found" -match "cmake") -and ("$found" -match "ninja"))
}

$started = Get-Date
Refresh-Path   # new shells can miss PATH entries added by recent installs

Step "Checking for Git, Python 3.12 and Visual Studio"
$hasGit = [bool](Get-Command git -ErrorAction SilentlyContinue)
$basePy = Find-Python
$vs = Find-VS
$missing = @()
if (-not $hasGit) { $missing += "Git" }
if (-not $basePy) { $missing += "Python 3.12" }
if (-not $vs) { $missing += "Visual Studio 2022 Build Tools with C++, CMake and Ninja (about 6 GB)" }
if ($vs -and -not (Has-CMakeNinja $vs)) {
    Fail ("Visual Studio at $vs has no CMake or Ninja. Open the Visual Studio Installer, choose Modify, and add " +
          "'C++ CMake tools for Windows'")
}
if ($missing) { $missing | ForEach-Object { Write-Host "missing: $_" } } else { Write-Host "all found" }

Step "Installing missing tools"
if (-not $missing) {
    Write-Host "nothing to install"
} else {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Fail ("winget is not available to install $($missing -join ', '). Get winget as 'App Installer' from the " +
              "Microsoft Store, or install them yourself: https://git-scm.com, https://www.python.org, and " +
              "https://visualstudio.microsoft.com/downloads/ (Build Tools, with the C++ workload)")
    }
    if (-not (Ask "Install $($missing -join ', ') with winget now? Windows will ask for permission")) {
        Fail "these are needed: $($missing -join ', ')"
    }
    $common = @("-e", "--accept-package-agreements", "--accept-source-agreements")
    if (-not $hasGit) { Run winget install --id Git.Git --silent @common }
    if (-not $basePy) { Run winget install --id Python.Python.3.12 --silent @common }
    if (-not $vs) {
        Write-Host "Visual Studio Build Tools take 10-20 minutes to install; its window shows progress"
        Run winget install --id Microsoft.VisualStudio.2022.BuildTools @common --override `
            "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"
    }
    Refresh-Path
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "Git still not found after installing it. Open a new window and run setup again" }
    $basePy = Find-Python
    if (-not $basePy) { Fail "Python 3.12 still not found after installing it. Open a new window and run setup again" }
    $vs = Find-VS
    if (-not $vs -or -not (Has-CMakeNinja $vs)) { Fail "Visual Studio Build Tools still not found after installing them. Restart Windows and run setup again" }
}
Write-Host "Python $basePy, Visual Studio at $vs"

Step "Fetching the xboxrecomp toolkit"
if (Test-Path (Join-Path $toolkit "tools\recomp")) {
    Write-Host "already there"
} elseif (Test-Path (Join-Path $repo ".git")) {
    Push-Location $repo
    Run git submodule update --init
    Pop-Location
} else {
    # A zip of this repo has no submodule contents, so clone the pinned toolkit commit instead.
    if (Test-Path $toolkit) { Remove-Item -Recurse -Force $toolkit }
    Run git clone -q $toolkitUrl $toolkit
    Run git -C $toolkit checkout -q $toolkitPin
}

Step "Applying toolkit patches"
Push-Location $toolkit
Quiet "git apply --reverse --check `"$($patches[-1])`"" | Out-Null
if (-not $LASTEXITCODE) {
    Write-Host "already applied"
} elseif (-not (git status --porcelain)) {
    foreach ($p in $patches) { Run git apply $p; Write-Host "applied $(Split-Path -Leaf $p)" }
} else {
    Pop-Location
    Fail ("third_party/xboxrecomp has changes that are not these patches. To start it clean (this discards them): " +
          "git -C third_party/xboxrecomp checkout . ; git -C third_party/xboxrecomp clean -fd")
}
Pop-Location

Step "Installing Python packages"
if (-not (Test-Path $py)) { Run $basePy -m venv $venv }
Run $py -m pip install -q --disable-pip-version-check -r (Join-Path $repo "requirements.txt")

Step "Extracting the game"
if (Test-Path $xbe) {
    Write-Host "assets\default.xbe already there"
} else {
    if (-not $Iso -and -not $Yes) {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = "Choose your Unreal Championship 2 disc image"
        $dlg.Filter = "Xbox disc image (*.iso)|*.iso|All files (*.*)|*.*"
        if ($dlg.ShowDialog() -eq "OK") { $Iso = $dlg.FileName }
    }
    if (-not $Iso) { Fail "no game chosen. Run setup again and choose your .iso, or pass -Iso <path>" }
    if (-not (Test-Path $Iso)) { Fail "$Iso does not exist" }
    if (Test-Path $Iso -PathType Container) {
        if (-not (Test-Path (Join-Path $Iso "default.xbe"))) { Fail "$Iso has no default.xbe. Choose the disc image or the folder it was extracted to" }
        Write-Host "copying $Iso"
        New-Item -ItemType Directory -Force $assets | Out-Null
        Copy-Item -Recurse -Force (Join-Path $Iso "*") $assets
    } else {
        Write-Host "extracting $Iso"
        & (Join-Path $PSScriptRoot "extract_xiso.ps1") -Iso $Iso -Out $assets | Select-Object -Last 1
        if (-not (Test-Path $xbe)) { Fail "no default.xbe in $Iso. Is it an Xbox disc image?" }
    }
}

Step "Checking the dump"
Run $py -I (Join-Path $PSScriptRoot "verify_setup.py") $assets
$stream = [IO.File]::OpenRead($xbe)
$sha1 = -join ([Security.Cryptography.SHA1]::Create().ComputeHash($stream) | ForEach-Object { $_.ToString("x2") })
$stream.Dispose()
if ($sha1 -ne $expectedSha1) {
    $msg = "default.xbe has SHA-1 $sha1, not $expectedSha1. This project's hooks are for that exact release"
    if (-not $AllowOtherDump) { Fail "$msg. Pass -AllowOtherDump to try anyway." }
    Write-Host "warning: $msg" -ForegroundColor Yellow
}

Step "Recompiling (about 10 minutes)"
if ((Test-Path (Join-Path $gen "recomp_funcs.h")) -and -not $Regenerate) {
    Write-Host "src\recomp\gen already there; pass -Regenerate to rebuild it"
} else {
    New-Item -ItemType Directory -Force (Join-Path $repo "build"), $gen | Out-Null
    $eh = Join-Path $repo "build\functions_eh.json"
    Push-Location $toolkit
    # The parser prints the certificate's keys, so its output goes to a file rather than a window people paste
    # into issues.
    & $py -m tools.xbe_parser $xbe --json (Join-Path $assets "default_analysis.json") > (Join-Path $repo "build\xbe_parser.log")
    if ($LASTEXITCODE) { Fail "the XBE parser failed; its output is in build\xbe_parser.log" }
    Run $py -m tools.disasm $xbe --text-only --force -v --extra-sections "XMV,D3DX,DSOUND,XNET,XONLINE,WMADEC,XACTENG,XGRPH,D3D,XPP"
    Run $py -m tools.func_id $xbe -v
    Run $py -m tools.abi_analysis $xbe -v
    Run $py -I (Join-Path $PSScriptRoot "add_eh_entries.py") tools\disasm\output\functions.json $eh
    Run $py -m tools.recomp $xbe --all --split 250 --game-name "Unreal Championship 2" --gen-dir $gen --functions $eh `
        --exclude-manual (Join-Path $repo "src\recomp_manual.c")
    Pop-Location
}

Step "Building (10-20 minutes the first time)"
$log = Join-Path $repo "build\build.log"
cmd /c "`"$(Join-Path $PSScriptRoot 'build.cmd')`""
$exit = (Select-String -Path $log -Pattern "BUILD_EXIT=(\d+)" | Select-Object -Last 1).Matches.Groups[1].Value
if ($exit -ne "0" -or -not (Test-Path $exePath)) {
    Get-Content $log -Tail 20
    Fail "the build failed; the full log is build\build.log"
}

if (-not $NoShortcut -and (Ask "Put a shortcut to the game on the desktop?")) {
    $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut(
        (Join-Path ([Environment]::GetFolderPath("Desktop")) "Unreal Championship 2.lnk"))
    $lnk.TargetPath = Join-Path $PSHOME "powershell.exe"
    $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$(Join-Path $PSScriptRoot 'play.ps1')`""
    $lnk.WorkingDirectory = $repo
    $lnk.IconLocation = "$exePath,0"
    $lnk.Save()
    Write-Host "made the desktop shortcut"
}

$mins = [int]((Get-Date) - $started).TotalMinutes
Write-Host ""
Write-Host "Done in $mins min. Play with Play.cmd in this folder, or the desktop shortcut." -ForegroundColor Green
