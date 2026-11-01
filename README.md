# Unreal Championship 2: The Liandri Conflict - native PC recompilation

Static recompilation of the original Xbox `default.xbe` into C that builds and runs natively on
Windows, using the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit
(submodule at `third_party/xboxrecomp`, pinned).

No game data or game-derived code belongs in this repo. Supply your own dump. `assets/` and
`src/recomp/gen/` are gitignored. `assets/default_analysis.json` holds keys from the XBE certificate.

## Setup
Requires CMake, Ninja, Python 3.12, and MSVC (VS 2022 Build Tools, C++ workload).

    git submodule update --init
    python -m pip install -r requirements.txt
    tools/extract_xiso.ps1 -Iso <path to iso> -Out assets
    python -I tools/verify_setup.py assets

## Recompile
Apply the local toolkit patches once, from `third_party/xboxrecomp`. The first makes the disassembler about 6x faster
and its output was byte-identical on `--text-only`. The second lets kernel calls use buffers in `RECOMP_EXT_VMA` memory:

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
    cmake --build build/game -j 12

Run from the repo root, because paths are relative. All four settings are required:

- `RECOMP_EXT_VMA=1`: without it the game loops forever walking memory.
- `RECOMP_AC97_READY=1`: without it `DirectSoundCreate` fails and the game skips D3D device creation.
- `RECOMP_PB_EXEC=1`: executes the GPU pushbuffer, so frames are actually drawn.
- `RECOMP_VBLANK=paced`: delivers 60 Hz vblanks to D3D. Without it, movies never advance past their first frame.

`RECOMP_FB_DUMP=<prefix>` writes frames to BMP files for checking. `RECOMP_USB=1` attaches an emulated pad, and
`RECOMP_PAD_SCRIPT` drives it, for example `70000:start:500,100000:a:500` (time in ms, button, hold in ms).

    $env:RECOMP_EXT_VMA="1"; $env:RECOMP_AC97_READY="1"; $env:RECOMP_PB_EXEC="1"; $env:RECOMP_VBLANK="paced"; Start-Process build\game\ucc2_recomp.exe -NoNewWindow -RedirectStandardError stderr.txt

To play in a window, run `powershell -File tools/play.ps1`, which also sets `RECOMP_HEAP_RECLAIM=1` and logs to
`play_log.txt`. It draws on the GPU; add `-Software` for the old CPU rasteriser. Keys work while the window
has focus: Enter is Start, Z is A, X is B, C is X, V is Y, arrows are the D-pad, WASD is the left stick and IJKL is the
right stick. A connected XInput controller should also work.

The runtime keeps save data in `%LOCALAPPDATA%\xboxrecomp`.

## Target facts (this dump)
- Title ID 4D570021, retail keys, XDK 1.0.5849 for all linked libraries
- `default.xbe` SHA-1 1f6e3417b6a912b5d3503593fd54d9b7a92c4bd0; all 18 section digests verify
- Image base 0x10000, size 0x4696A0, entry 0x0026867F, kernel thunk table 0x0039D460
- Game code: `.text`, VA 0x11000, 2.8 MB
- XDK code in its own sections: XMV, D3DX, DSOUND, XNET, XONLINE, WMADEC, XACTENG, XGRPH, D3D, XPP
- 156 kernel imports: 133 stdcall, 5 fastcall, 18 data exports

See docs/ROADMAP.md for status and open decisions.
