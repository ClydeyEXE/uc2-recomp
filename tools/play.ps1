# Play the game in a window, on the GPU renderer. Run from anywhere:
#
#   powershell -File tools/play.ps1
#
# -Software uses the old CPU rasteriser instead. The log goes to play_log.txt in the repo root.
param([switch]$Software, [switch]$DrawLog)
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
$env:RECOMP_FB_WINDOW = "1"
$env:RECOMP_USB = "1"
$env:RECOMP_KEYBOARD = "1"
$env:RECOMP_D3D_PRESENT = "1"
$env:RECOMP_LOG = "play_log.txt"
if ($Software) { $env:RECOMP_GPU = "sw" }
Start-Process build\game\ucc2_recomp.exe
