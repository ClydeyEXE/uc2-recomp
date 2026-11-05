@echo off
rem Builds the game from any shell. The log goes to build\build.log; its last line is BUILD_EXIT=<code>.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq tokens=2*" %%a in (`reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path`) do set "PATH=%%b;%PATH%"
for /f "usebackq tokens=2*" %%a in (`reg query "HKCU\Environment" /v Path`) do set "PATH=%%b;%PATH%"
rem Any edition of Visual Studio 2022 or its Build Tools with the C++ workload. 2022 is what the project is tested
rem with, so it wins over a newer version; the newest is used only when there is no 2022.
set "VSDIR="
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
cd /d "%~dp0.."
if not exist build mkdir build
rem A running game locks the exe, and the link would fail halfway; say so instead.
tasklist /fi "imagename eq ucc2_recomp.exe" /nh 2>nul | find /i "ucc2_recomp.exe" >nul
if not errorlevel 1 (
    echo The game is running: close it before building. > build\build.log
    echo BUILD_EXIT=1 >> build\build.log
    echo The game is running: close it before building.
    exit /b 1
)
if not defined VSDIR (
    echo MSVC not found: install Visual Studio 2022 or its Build Tools with the C++ workload > build\build.log
    echo BUILD_EXIT=1 >> build\build.log
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake -S . -B build\game -G Ninja -DCMAKE_BUILD_TYPE=Release > build\build.log 2>&1
cmake --build build\game -- -k 0 >> build\build.log 2>&1
echo BUILD_EXIT=%ERRORLEVEL% >> build\build.log
