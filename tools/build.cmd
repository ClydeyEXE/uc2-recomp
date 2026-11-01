@echo off
rem Builds the game from any shell. The log goes to build\build.log; its last line is BUILD_EXIT=<code>.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq tokens=2*" %%a in (`reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path`) do set "PATH=%%b;%PATH%"
for /f "usebackq tokens=2*" %%a in (`reg query "HKCU\Environment" /v Path`) do set "PATH=%%b;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
cmake -S . -B build\game -G Ninja -DCMAKE_BUILD_TYPE=Release > build\build.log 2>&1
cmake --build build\game -j 12 -- -k 0 >> build\build.log 2>&1
echo BUILD_EXIT=%ERRORLEVEL% >> build\build.log
