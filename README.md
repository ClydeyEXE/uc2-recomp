# Unreal Championship 2: The Liandri Conflict, natively on PC

A static recompilation of the original Xbox game into a native Windows program, built with the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit. The game's own code runs, translated to C, with no
emulator.

> [!IMPORTANT]
> This is an unofficial fan project, not affiliated with or endorsed by Epic Games, Midway or Microsoft. It contains
> no game data and no code from the game: you need your own copy, and the build makes the game on your machine. The
> program it makes contains the game's code, so don't share it, and don't ask for game files or builds in Issues.

## Status
**Playable.** Full Instant Action matches play start to finish with a controller or keyboard.

| | |
|---|---|
| Frame rate | 60-130 fps in most of a match; busy scenes still drop to 35-48 |
| Hitches | Occasional 100-160 ms pauses while the game loads files |
| Audio | No dropouts; effects such as reverb are missing |
| Story Mode | Cutscenes play; the rest is untested |
| DLC | Not supported yet |

What's being worked on, in order: the pinned **Roadmap** in [Issues](https://github.com/ClydeyEXE/ucc2-recomp/issues).

## Getting started
You need Windows 10 or 11, about 5 GB free, and a disc image (`.iso`) of your own copy of the game. The supported
release has a `default.xbe` with SHA-1 `1f6e3417b6a912b5d3503593fd54d9b7a92c4bd0`; setup checks it for you.

1. Download this repository (green **Code** button, **Download ZIP**) and unzip it somewhere with space.
2. Double-click **`Setup.cmd`** and choose your `.iso`. It installs anything missing (Git, Python 3.12 and the Visual
   Studio 2022 Build Tools, after asking), then unpacks, recompiles and builds the game. The first run takes about
   30-60 minutes. If it stops, fix what it says and run it again: finished steps are skipped.
3. Double-click **`Play.cmd`**, or the desktop shortcut setup offers to make.

## Playing
A controller (XInput, such as an Xbox pad) works. On the keyboard: Enter is Start, Z/X/C/V are A/B/X/Y, the arrows are
the D-pad, WASD the left stick and IJKL the right stick.

**F1** (or Back + Start) opens a settings panel: internal resolution up to 4K, 4:3 or stretched 16:9, a frame limit,
vsync, scaling, an FPS counter, and skipping the boot screens' fixed waits. **F11** or **Alt+Enter** toggles
fullscreen. Settings and saves are kept in `%LOCALAPPDATA%\xboxrecomp`.

## Contributing
Bug reports are welcome: include your GPU and the end of `play_log.txt`, but no game files. For working on the code,
see [docs/BUILDING.md](docs/BUILDING.md) (the build by hand, run-time settings), [docs/DEVELOPING.md](docs/DEVELOPING.md)
(debugging tools) and [docs/HISTORY.md](docs/HISTORY.md) (past bugs and their fixes).

## Credits and license
Built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz, which does the translation and supplies the
runtime. xboxrecomp is MIT licensed and includes parts of [xemu](https://github.com/xemu-project/xemu) under
LGPL-2.1-or-later (see its `NOTICE`).

The code in this repository is MIT licensed (see [LICENSE](LICENSE)). The patches in `patches/` modify xboxrecomp and
fall under its licenses: the hunks for its `src/apu/` files change xemu code and are LGPL-2.1-or-later, the rest MIT.
Unreal Championship 2 and Unreal are trademarks of Epic Games; Xbox is a trademark of Microsoft.
