@echo off
rem Double-click to play. Options such as -Scale 2 or -Fps 60 pass through to tools\play.ps1.
if not exist "%~dp0build\game\ucc2_recomp.exe" (
    echo The game is not built yet. Run Setup.cmd first.
    pause
    exit /b 1
)
set "PSModulePath="
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\play.ps1" %*
