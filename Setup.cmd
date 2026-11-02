@echo off
rem Double-click to set everything up and build the game: see tools\setup.ps1 for what it does and its options.
rem Started from PowerShell 7, the inherited module path hides Windows PowerShell's own cmdlets; clearing it fixes that.
set "PSModulePath="
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
set "SETUP_EXIT=%ERRORLEVEL%"
rem Keep the window open when double-clicked, so the result can be read.
if "%~1"=="" pause
exit /b %SETUP_EXIT%
