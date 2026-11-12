# Building by hand

`Setup.cmd` does all of this for you. This page is for working on the project, or for when a step of
the setup needs doing differently. Debugging tools are in `docs/DEVELOPING.md`.

## Setup
These are the steps `tools/setup.ps1` runs, by hand. Its options: `-Iso <file or folder>`, `-Yes` (ask nothing),
`-NoShortcut`, `-Regenerate` (rerun the recompiler) and `-AllowOtherDump`.

Requires Git, Python 3.12, and Visual Studio 2022 or its Build Tools with the C++ workload and "C++ CMake tools for
Windows". `assets/`, `build/` and `src/recomp/gen/` are gitignored. The XBE parser below prints the certificate's
keys, so keep its output to yourself. Turn on the pre-commit hook that keeps game data and keys out of commits:

    git submodule update --init
    git config core.hooksPath .githooks
    python -m venv build\venv
    build\venv\Scripts\python -m pip install -r requirements.txt
    tools/extract_xiso.ps1 -Iso <path to iso> -Out assets
    build\venv\Scripts\python -I tools/verify_setup.py assets

The commands below say `python`: use `build\venv\Scripts\python`, or activate the venv first.

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
Create `<repo>/build` and `<repo>/src/recomp/gen` first if they do not exist.

    python -m tools.xbe_parser $XBE --json <repo>/assets/default_analysis.json
    python -m tools.disasm $XBE --text-only --force -v --extra-sections "XMV,D3DX,DSOUND,XNET,XONLINE,WMADEC,XACTENG,XGRPH,D3D,XPP"
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

`Play.cmd` (`tools/play.ps1`) sets everything, and logs to `play_log.txt`. Its options: `-Software` for the old CPU
rasteriser, `-Limit60` for flip pacing, `-Scale N` and `-Fps N` for this run only, and `-DrawLog` (see
`docs/DEVELOPING.md`). For one run, `RECOMP_RES_SCALE`, `RECOMP_RES_HEIGHT=<lines>`, `RECOMP_WIDESCREEN=1`,
`RECOMP_WIDE_VIEW=1` (with it, 16:9 widescreen), `RECOMP_FPS_LIMIT`, `RECOMP_PRESENT_VSYNC`,
`RECOMP_PRESENT_FILTER=point`, `RECOMP_ANISOTROPY=<1-16>` and `RECOMP_SKIP_BOOT_WAITS=1` override the settings panel's file, `%LOCALAPPDATA%\xboxrecomp\ucc2_video.ini`.

With vsync off the swap chain may tear (`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`, where Windows supports it), so a window
is not held to the display's refresh rate and a VRR display follows the game. The log's `presenting to the window` line
says whether it is on, and gives the display's refresh rate, from which the panel offers a frame limit just under it
(about 3% under: 160 at 165 Hz, 140 at 144 Hz). `RECOMP_PRESENT_TEARING=0` turns tearing off, which holds a
window to the refresh rate as before.

The first line of the log is the version: `git describe` at build time, with `-modified` if tracked files had changes,
or for a source zip, `tools/version.txt`, which git fills in as it makes the zip.

To run by hand, from the repo root because paths are
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

## Target facts (this dump)
- Title ID 4D570021, retail keys, XDK 1.0.5849 for all linked libraries
- `default.xbe` SHA-1 1f6e3417b6a912b5d3503593fd54d9b7a92c4bd0; all 18 section digests verify
- Image base 0x10000, size 0x4696A0, entry 0x0026867F, kernel thunk table 0x0039D460
- Game code: `.text`, VA 0x11000, 2.8 MB
- XDK code in its own sections: XMV, D3DX, DSOUND, XNET, XONLINE, WMADEC, XACTENG, XGRPH, D3D, XPP
- 156 kernel imports: 133 stdcall, 5 fastcall, 18 data exports
