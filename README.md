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
Run from `third_party/xboxrecomp`, with $XBE the absolute path to `assets/default.xbe`. The disassembler
takes about 35 minutes on this game; the other steps take seconds to a couple of minutes.

    python -m tools.xbe_parser $XBE --json <repo>/assets/default_analysis.json
    python -m tools.disasm $XBE --text-only -v
    python -m tools.func_id $XBE -v
    python -m tools.abi_analysis $XBE -v
    python -m tools.recomp $XBE --all --split 250 --game-name "Unreal Championship 2" --gen-dir <repo>/src/recomp/gen

## Build and run
From a Developer prompt (vcvars64), at the repo root:

    cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/game -j 12

Run from the repo root, because paths are relative. `RECOMP_EXT_VMA=1` is required: without it the game
loops forever walking memory.

    $env:RECOMP_EXT_VMA="1"; build\game\ucc2_recomp.exe 2> stderr.txt

The runtime keeps save data in `%LOCALAPPDATA%\xboxrecomp`.

## Target facts (this dump)
- Title ID 4D570021, retail keys, XDK 1.0.5849 for all linked libraries
- `default.xbe` SHA-1 1f6e3417b6a912b5d3503593fd54d9b7a92c4bd0; all 18 section digests verify
- Image base 0x10000, size 0x4696A0, entry 0x0026867F, kernel thunk table 0x0039D460
- Game code: `.text`, VA 0x11000, 2.8 MB
- XDK code in its own sections: XMV, D3DX, DSOUND, XNET, XONLINE, WMADEC, XACTENG, XGRPH, D3D, XPP
- 156 kernel imports: 133 stdcall, 5 fastcall, 18 data exports

See docs/ROADMAP.md for status and open decisions.
