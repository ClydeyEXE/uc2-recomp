# Unreal Championship 2: The Liandri Conflict - native PC recompilation

Goal: statically recompile the original Xbox `default.xbe` (32-bit x86) into C++ that builds and
runs natively on Windows, with a runtime replacing the Xbox kernel, D3D8/NV2A, audio, input and storage.

No game data or game-derived code belongs in this repo. Supply your own dump. `assets/` and
`generated/` (future lifted code) are gitignored.

## Setup
Requires CMake, Ninja, Python 3.12, MSVC (VS 2022 Build Tools, C++ workload) and JDK 21 + Ghidra for analysis.

    python -m pip install -r requirements.txt
    tools/extract_xiso.ps1 -Iso <path to iso> -Out assets
    python -I tools/verify_setup.py assets
    python -I tools/xbeinfo.py assets/default.xbe --json docs/xbe.json
    python -I tools/discover.py assets/default.xbe docs

Build from a Developer prompt (vcvars64):

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

Keep the checkout on a short path. MSVC cannot open CMake's build files when the full path passes 260 characters.

## Target facts (this dump)
- Title ID 4D570021, retail keys, XDK 1.0.5849 for all linked libraries
- `default.xbe` SHA-1 1f6e3417b6a912b5d3503593fd54d9b7a92c4bd0; all 18 section digests verify
- Image base 0x10000, size 0x4696A0, entry 0x0026867F, kernel thunk table 0x0039D460
- Game code: `.text`, VA 0x11000, 2.8 MB
- XDK code in its own sections: XMV, D3DX, DSOUND, XNET, XONLINE, WMADEC, XACTENG, XGRPH, D3D, XPP
- TLS: no init data, 8 bytes zero fill, index at 0x004183B8; code does use `fs:`
- 156 kernel imports: 133 stdcall, 5 fastcall, 18 data exports (the runtime must supply real data)
- `update.xbe`, `downloader.xbe`, `dashupdate.xbe` are separate utility titles, not needed for the game

See docs/ROADMAP.md for the plan and open decisions.
