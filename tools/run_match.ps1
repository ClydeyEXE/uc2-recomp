# Scripted run into an Instant Action match on CTF-Aqueduct, for testing without a player.
#
#   powershell -File tools/run_match.ps1 -Secs 420 -Log run.txt -Env "RECOMP_PB_WALK=dma;RECOMP_HEAP_RECLAIM=1"
#
# Runs from the repo root and stops the game after -Secs. -Env adds or overrides settings.
# -Dump <prefix> writes frames as BMP files: one per report, and every frame for -CaptureFor
# seconds from -CaptureAt, which is how to see frame-to-frame flicker.
param(
    [int]$Secs = 420,
    [string]$Log = "stderr.txt",
    [string]$Env = "",
    [string]$Dump = "",
    [int]$CaptureAt = 0,
    [int]$CaptureFor = 8,
    [switch]$NoWindow
)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# On the GPU renderer the menus keep up with presses a few seconds apart.
$press = @("45000:start:500", "60000:down:300")
for ($t = 70000; $t -le 260000; $t += 8000) { $press += "${t}:a:400" }

$settings = @{
    RECOMP_EXT_VMA       = "1"
    RECOMP_AC97_READY    = "1"
    RECOMP_PB_EXEC       = "1"
    RECOMP_VBLANK        = "paced"
    RECOMP_PB_WALK       = "dma"
    RECOMP_GUEST_CPUS    = "2"
    RECOMP_HEAP_RECLAIM  = "1"
    RECOMP_USB           = "1"
    RECOMP_WATCHDOG_SECS = "$Secs"
    RECOMP_PB_REPORT_MS  = "15000"
    RECOMP_PAD_SCRIPT    = ($press -join ",")
}
if (-not $NoWindow) { $settings.RECOMP_FB_WINDOW = "1" }
$flag = ""
if ($Dump) {
    $settings.RECOMP_FB_DUMP = $Dump
    if ($CaptureAt) {
        $flag = "$Dump.capture"
        Remove-Item $flag -ErrorAction SilentlyContinue
        $settings.RECOMP_FB_DUMP_FLIPS = $flag
    }
}
foreach ($pair in ($Env -split ";")) {
    if ($pair) { $k, $v = $pair -split "=", 2; $settings[$k] = $v }
}
foreach ($k in $settings.Keys) { Set-Item -Path "env:$k" -Value $settings[$k] }

# Not -RedirectStandardError: that sends every line through a pipe PowerShell drains
# slowly, which was costing the game thread 15% of its time.
$env:RECOMP_LOG = $Log
$p = Start-Process build\game\ucc2_recomp.exe -PassThru
if ($flag) {
    if (-not $p.WaitForExit($CaptureAt * 1000)) {
        New-Item -ItemType File $flag | Out-Null
        Start-Sleep -Seconds $CaptureFor
        Remove-Item $flag
    }
}
if (-not $p.WaitForExit(($Secs + 30) * 1000)) { $p.Kill() }
"exit $($p.ExitCode)"
