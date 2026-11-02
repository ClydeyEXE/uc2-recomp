# Unreal Championship 2: The Liandri Conflict - native PC recompilation

Static recompilation of the original Xbox `default.xbe` into C that builds and runs natively on
Windows, using the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit
(submodule at `third_party/xboxrecomp`, pinned).

> [!IMPORTANT]
> This is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Epic Games, Midway or
> Microsoft. Unreal Championship 2 and Unreal are trademarks of Epic Games, and Xbox is a trademark of Microsoft.
>
> This repository contains no game data and no code from the game. To use it you need your own copy of the game,
> dumped from a disc you own. The build turns that dump into an executable on your machine. The executable it makes
> contains the game's code, so do not share it, and do not ask for game files, dumps or builds in Issues.

No game data or game-derived code belongs in this repo. `assets/` and `src/recomp/gen/` are gitignored.
`assets/default_analysis.json` holds keys from the XBE certificate. The supported dump is listed under "Target facts"
below; check yours against its SHA-1.

## Status
Full Instant Action matches play from start to finish with a controller or keyboard, on a Direct3D 11 renderer.
Typical matches run 60-130 fps, but busy scenes still drop to 35-48 fps, and there are occasional 100-160 ms hitches
while the game loads files. Audio plays without dropouts, but effects such as reverb are missing. Story Mode cutscenes
play; the rest of Story Mode, and the DLC, are untested.

Open work is in [Issues](https://github.com/ClydeyEXE/ucc2-recomp/issues), in the order of the pinned Roadmap issue.
`docs/DEVELOPING.md` covers debugging tools, and `docs/HISTORY.md` records past bugs and their fixes.

## Quick start
You need Windows, about 5 GB free besides the disc image, and:
- [Git](https://git-scm.com)
- [Python 3.12](https://www.python.org), with "Add to PATH" ticked
- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) (or any VS 2022 edition) with the
  "Desktop development with C++" workload, which includes CMake and Ninja
- A disc image (`.iso`) of your own copy of the game

Then, in PowerShell:

    git clone --recursive https://github.com/ClydeyEXE/ucc2-recomp.git
    cd ucc2-recomp
    powershell -File tools/setup.ps1 -Iso <path to your .iso>
    powershell -File tools/play.ps1

`setup.ps1` checks the prerequisites, applies the toolkit patches, installs Python packages into `build\venv`,
extracts and verifies the game, recompiles it and builds it. It takes about 15-30 minutes, most of it the
disassembler and the C build. If a step fails, fix what it says and run it again: finished steps are skipped.

## Setup by hand
These are the steps `setup.ps1` runs.

    git submodule update --init
    python -m pip install -r requirements.txt
    tools/extract_xiso.ps1 -Iso <path to iso> -Out assets
    python -I tools/verify_setup.py assets

## Recompile
Apply the local toolkit patches once, from `third_party/xboxrecomp`. The first makes the disassembler about 6x faster
and its output was byte-identical on `--text-only`. The second lets kernel calls use buffers in `RECOMP_EXT_VMA` memory.
The third holds later toolkit work, and `tools/update_patches.ps1` regenerates it:

    git apply ../../patches/xboxrecomp-decode-stream.patch
    git apply ../../patches/xboxrecomp-extvma-buffers.patch
    git apply ../../patches/xboxrecomp-gpu-backend.patch

Then run these from the same folder, with $XBE the absolute path to `assets/default.xbe`. Without
`--extra-sections`, 446 XDK call targets become stubs and boot crashes. The disassembler takes about 5 minutes.
`--exclude-manual` makes the lifter use the hooks, wrappers and hand-written functions in `src/recomp_manual.c`, and
`tools/add_eh_entries.py` adds the entry points its exception handling needs. Without them the game does not get far.

    python -m tools.xbe_parser $XBE --json <repo>/assets/default_analysis.json
    python -m tools.disasm $XBE --text-only --force -v --extra-sections XMV,D3DX,DSOUND,XNET,XONLINE,WMADEC,XACTENG,XGRPH,D3D,XPP
    python -m tools.func_id $XBE -v
    python -m tools.abi_analysis $XBE -v
    python -I <repo>/tools/add_eh_entries.py tools/disasm/output/functions.json <repo>/build/functions_eh.json
    python -m tools.recomp $XBE --all --split 250 --game-name "Unreal Championship 2" --gen-dir <repo>/src/recomp/gen --functions <repo>/build/functions_eh.json --exclude-manual <repo>/src/recomp_manual.c

## Build and run
From a Developer prompt (vcvars64), at the repo root:

    cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/game

`tools/build.cmd` does the same from any shell. Rerun `tools.recomp` first (or `setup.ps1 -Regenerate`) after adding
or removing a hand-written function in `src/recomp_manual.c`.

To play, use `tools/play.ps1` below, which sets everything. To run by hand, from the repo root because paths are
relative, these settings are required:

- `RECOMP_EXT_VMA=1`: without it the game loops forever walking memory.
- `RECOMP_AC97_READY=1`: without it `DirectSoundCreate` fails and the game skips D3D device creation.
- `RECOMP_PB_EXEC=1`: executes the GPU pushbuffer, so frames are actually drawn.
- `RECOMP_VBLANK=paced`: delivers 60 Hz vblanks to D3D. Without it, movies never advance past their first frame.
- `RECOMP_PB_WALK=dma`: reads the pushbuffer as the GPU does. D3D patches commands based on the read position it reports.
- `RECOMP_GUEST_CPUS=2`: the game's threads share one core, as on the single-CPU console.
- `RECOMP_HEAP_RECLAIM=1`: reuses freed contiguous memory. Without it the arena runs out about two minutes into a match.
- `RECOMP_DISPATCH_GATE=1`: keeps DPCs and raised-IRQL code apart, as on the single CPU. Without it USB races and crashes.

`RECOMP_FLIP_PACING=1` holds each flip to the next vblank, capping the rate near 60 fps. `run_match.ps1` sets it, and
`play.ps1 -Limit60` does. It leaks: one GPU walk can carry several frames, and matches still reached 64-79 fps.
`edge` also makes a late frame wait for the vblank after, as D3D does, which turns a 50 fps stretch into 30.

`RECOMP_FB_DUMP=<prefix>` writes frames to BMP files for checking. `RECOMP_USB=1` attaches an emulated pad, and
`RECOMP_PAD_SCRIPT` drives it, for example `70000:start:500,100000:a:500` (time in ms, button, hold in ms).

    $env:RECOMP_EXT_VMA="1"; $env:RECOMP_AC97_READY="1"; $env:RECOMP_PB_EXEC="1"; $env:RECOMP_VBLANK="paced"; $env:RECOMP_PB_WALK="dma"; $env:RECOMP_GUEST_CPUS="2"; $env:RECOMP_HEAP_RECLAIM="1"; $env:RECOMP_DISPATCH_GATE="1"; $env:RECOMP_LOG="stderr.txt"; Start-Process build\game\ucc2_recomp.exe

To play in a window, run `powershell -File tools/play.ps1`, which also sets `RECOMP_HEAP_RECLAIM=1` and logs to
`play_log.txt`. It draws on the GPU; add `-Software` for the old CPU rasteriser. Keys work while the window
has focus: Enter is Start, Z is A, X is B, C is X, V is Y, arrows are the D-pad, WASD is the left stick and IJKL is the
right stick. A connected XInput controller should also work.

F1 (or Back and Start together on the pad) opens a settings panel over the game: internal resolution (480 to 2160 lines:
the console's 640x480, its multiples, and 720p, 1080p, 1440p and 4K heights), an aspect of 4:3 or 16:9 (stretched, as
the game's camera is 4:3; at 2160 lines this renders 3840x2160), a frame limit (off, 30, 60, 120, 144 or 240 fps), vsync, smooth or sharp scaling to the window, and
an FPS counter, and "Boot waits": skip, from the next start, the 9 s boot holds its first screens for (or
`RECOMP_SKIP_BOOT_WAITS=1`). Up and down choose, left and right (or A, Enter) change, and F1, Esc or B close it. The game sees the
pad at rest while it is open. Choices are kept in `%LOCALAPPDATA%\xboxrecomp\ucc2_video.ini`. For one run,
`play.ps1 -Scale 2 -Fps 60` (or `RECOMP_RES_SCALE`, `RECOMP_RES_HEIGHT=<lines>`, `RECOMP_WIDESCREEN=1`, `RECOMP_FPS_LIMIT`, `RECOMP_PRESENT_VSYNC`,
`RECOMP_PRESENT_FILTER=point`) override the file. F11 or Alt+Enter toggles borderless fullscreen.

The runtime keeps save data in `%LOCALAPPDATA%\xboxrecomp`.

## Target facts (this dump)
- Title ID 4D570021, retail keys, XDK 1.0.5849 for all linked libraries
- `default.xbe` SHA-1 1f6e3417b6a912b5d3503593fd54d9b7a92c4bd0; all 18 section digests verify
- Image base 0x10000, size 0x4696A0, entry 0x0026867F, kernel thunk table 0x0039D460
- Game code: `.text`, VA 0x11000, 2.8 MB
- XDK code in its own sections: XMV, D3DX, DSOUND, XNET, XONLINE, WMADEC, XACTENG, XGRPH, D3D, XPP
- 156 kernel imports: 133 stdcall, 5 fastcall, 18 data exports

## Credits and license
Built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz, which does the lifting and supplies the
runtime. xboxrecomp is MIT licensed, and includes parts of [xemu](https://github.com/xemu-project/xemu) under
LGPL-2.1-or-later (see its `NOTICE`).

The code in this repository is under the MIT license in `LICENSE`. The patches in `patches/` modify xboxrecomp and
fall under its licenses: the hunks for its `src/apu/` files change xemu code and are LGPL-2.1-or-later, and the rest
are MIT.
