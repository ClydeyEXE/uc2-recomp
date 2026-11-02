# Play the game in a window, on the GPU renderer. Run from anywhere:
#
#   powershell -File tools/play.ps1
#
# -Software uses the old CPU rasteriser instead, and -Limit60 holds flips to the 60 Hz vblank
# (RECOMP_FLIP_PACING). -Scale 1..4 and -Fps <n> (0 off) set the internal resolution and the
# frame limit for this run only; F1 in the game sets and keeps them. The log goes to play_log.txt
# in the repo root.
param([switch]$Software, [switch]$DrawLog, [switch]$Limit60, [int]$Scale = 0, [int]$Fps = -1)
Set-Location (Split-Path -Parent $PSScriptRoot)
# -DrawLog logs every draw of one frame a second to the log, for finding out what drew something.
if ($DrawLog) {
    $env:RECOMP_GPU_DRAWLOG = "build\drawlog.flag"
    New-Item -ItemType File -Force $env:RECOMP_GPU_DRAWLOG | Out-Null
}
$env:RECOMP_EXT_VMA = "1"
$env:RECOMP_AC97_READY = "1"
$env:RECOMP_PB_EXEC = "1"
$env:RECOMP_VBLANK = "paced"
$env:RECOMP_PB_WALK = "dma"
$env:RECOMP_GUEST_CPUS = "2"
$env:RECOMP_HEAP_RECLAIM = "1"
$env:RECOMP_DISPATCH_GATE = "1"
if ($Limit60) { $env:RECOMP_FLIP_PACING = "1" } else { Remove-Item env:RECOMP_FLIP_PACING -ErrorAction SilentlyContinue }
$env:RECOMP_FB_WINDOW = "1"
$env:RECOMP_USB = "1"
$env:RECOMP_KEYBOARD = "1"
$env:RECOMP_D3D_PRESENT = "1"
$env:RECOMP_LOG = "play_log.txt"
if ($Software) { $env:RECOMP_GPU = "sw" }
if ($Scale -gt 0) { $env:RECOMP_RES_SCALE = "$Scale" } else { Remove-Item env:RECOMP_RES_SCALE -ErrorAction SilentlyContinue }
if ($Fps -ge 0) { $env:RECOMP_FPS_LIMIT = "$Fps" } else { Remove-Item env:RECOMP_FPS_LIMIT -ErrorAction SilentlyContinue }
Start-Process build\game\ucc2_recomp.exe
