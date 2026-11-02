# Goes from a game disc image to a built game in one command. Run from anywhere:
#
#   powershell -File tools/setup.ps1 -Iso <path to your Unreal Championship 2 .iso>
#
# Each step is skipped when its output is already there, so rerunning after a failure carries on where it stopped.
# -Regenerate reruns the recompiler even if src/recomp/gen exists (needed after changing hooks in
# src/recomp_manual.c). -AllowOtherDump continues when default.xbe is not the dump this project targets; expect it
# not to work. The whole run takes about 15-30 minutes, most of it the disassembler and the C build.
param([string]$Iso, [switch]$Regenerate, [switch]$AllowOtherDump)
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$repo = Split-Path -Parent $PSScriptRoot
$toolkit = Join-Path $repo "third_party\xboxrecomp"
$assets = Join-Path $repo "assets"
$xbe = Join-Path $assets "default.xbe"
$gen = Join-Path $repo "src\recomp\gen"
$venv = Join-Path $repo "build\venv"
$py = Join-Path $venv "Scripts\python.exe"
$expectedSha1 = "1f6e3417b6a912b5d3503593fd54d9b7a92c4bd0"
$patches = @("xboxrecomp-decode-stream.patch", "xboxrecomp-extvma-buffers.patch", "xboxrecomp-gpu-backend.patch") |
    ForEach-Object { Join-Path $repo "patches\$_" }

function Step([string]$name) { Write-Host ""; Write-Host "== $name" -ForegroundColor Cyan }
function Fail([string]$msg) { Write-Host ""; Write-Host "SETUP FAILED: $msg" -ForegroundColor Red; exit 1 }
function Run {
    $exe, $rest = $args
    & $exe @rest
    if ($LASTEXITCODE) { Fail "$exe $rest exited with $LASTEXITCODE" }
}

$started = Get-Date
# New shells can miss PATH entries added by recent installs.
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")

Step "Checking prerequisites"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git not found. Install it from https://git-scm.com" }
$sysPy = Get-Command python -ErrorAction SilentlyContinue
if (-not $sysPy) { Fail "python not found. Install Python 3.12 from https://www.python.org and tick 'Add to PATH'" }
$pyVer = (& $sysPy.Source -c "import sys; print('%d.%d' % sys.version_info[:2])").Trim()
if ($pyVer -ne "3.12") { Write-Host "warning: Python $pyVer found; this project is tested with 3.12" -ForegroundColor Yellow }
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
# Prefer 2022, the version the project is tested with, as tools/build.cmd does.
$vcReq = @("-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath")
$vs = if (Test-Path $vswhere) {
    $v = & $vswhere -version "[17.0,18.0)" @vcReq
    if (-not $v) { $v = & $vswhere @vcReq }
    $v
}
if (-not $vs) {
    Fail ("MSVC not found. Install Visual Studio 2022 or its Build Tools with the 'Desktop development with C++' " +
          "workload, which includes CMake and Ninja")
}
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$found = cmd /c "call `"$vcvars`" >nul 2>nul && where cmake 2>nul && where ninja 2>nul"
if (-not ($found -match "cmake") -or -not ($found -match "ninja")) {
    Fail "CMake or Ninja not found. Add the 'C++ CMake tools for Windows' component in the Visual Studio Installer"
}
Write-Host "git, Python $pyVer, MSVC at $vs"

Step "Fetching the xboxrecomp toolkit"
Push-Location $repo
Run git submodule update --init
Pop-Location

Step "Applying toolkit patches"
Push-Location $toolkit
# Windows PowerShell turns a native command's stderr into an error under "Stop", so cmd swallows it.
cmd /c "git apply --reverse --check `"$($patches[-1])`" 2>nul"
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
if (-not (Test-Path $py)) { Run $sysPy.Source -m venv $venv }
Run $py -m pip install -q --disable-pip-version-check -r (Join-Path $repo "requirements.txt")

Step "Extracting the game"
if (Test-Path $xbe) {
    Write-Host "assets\default.xbe already there"
} elseif (-not $Iso) {
    Fail "no game found. Pass your disc image: tools/setup.ps1 -Iso <path to .iso>"
} elseif (-not (Test-Path $Iso)) {
    Fail "$Iso does not exist"
} else {
    & (Join-Path $PSScriptRoot "extract_xiso.ps1") -Iso $Iso -Out $assets | Select-Object -Last 1
    if (-not (Test-Path $xbe)) { Fail "no default.xbe in $Iso. Is it an Xbox disc image?" }
}

Step "Checking the dump"
Run $py -I (Join-Path $PSScriptRoot "verify_setup.py") $assets
$sha1 = (Get-FileHash -Algorithm SHA1 $xbe).Hash.ToLower()
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
    Run $py -m tools.xbe_parser $xbe --json (Join-Path $assets "default_analysis.json")
    Run $py -m tools.disasm $xbe --text-only --force -v --extra-sections XMV,D3DX,DSOUND,XNET,XONLINE,WMADEC,XACTENG,XGRPH,D3D,XPP
    Run $py -m tools.func_id $xbe -v
    Run $py -m tools.abi_analysis $xbe -v
    Run $py -I (Join-Path $PSScriptRoot "add_eh_entries.py") tools\disasm\output\functions.json $eh
    Run $py -m tools.recomp $xbe --all --split 250 --game-name "Unreal Championship 2" --gen-dir $gen --functions $eh `
        --exclude-manual (Join-Path $repo "src\recomp_manual.c")
    Pop-Location
}

Step "Building"
$log = Join-Path $repo "build\build.log"
cmd /c "`"$(Join-Path $PSScriptRoot 'build.cmd')`""
$exit = (Select-String -Path $log -Pattern "BUILD_EXIT=(\d+)" | Select-Object -Last 1).Matches.Groups[1].Value
if ($exit -ne "0") {
    Get-Content $log -Tail 20
    Fail "build failed; the full log is build\build.log"
}

$mins = [int]((Get-Date) - $started).TotalMinutes
Write-Host ""
Write-Host "Done in $mins min. Play with: powershell -File tools/play.ps1" -ForegroundColor Green
