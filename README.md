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
| Story Mode | The first quarter plays through with no issues, cutscenes included; the rest is untested |
| DLC | Not supported yet |

What's being worked on, in order: the pinned **Roadmap** in [Issues](https://github.com/ClydeyEXE/ucc2-recomp/issues).

## Getting started

**You need**
- Windows 10 or 11
- About 5 GB of free space
- A disc image (`.iso`) of your own copy of the game

### 1. Download
Click the green **Code** button above, then **Download ZIP**. Unzip it somewhere with space, such as `C:\Games\ucc2`.

### 2. Set up
Double-click **`Setup.cmd`**, then choose your `.iso` when it asks.

- If Git, Python or the Visual Studio Build Tools are missing, it asks once, then installs them.
- It then unpacks, checks, recompiles and builds the game.
- The first run takes **30-60 minutes**. Leave the window open until it says **Done**.
- If it stops with an error, fix what it says and run `Setup.cmd` again. It carries on where it stopped.

### 3. Play
Double-click **`Play.cmd`**, or use the desktop shortcut that setup offers to make.

## Playing

> [!TIP]
> Press **F1** in the game to open the **settings menu**: resolution, widescreen, frame limit and more.

### Controls
An Xbox controller, or any XInput controller, works as on the console. On the keyboard:

| Controller | Keyboard |
|---|---|
| Start | Enter |
| A / B / X / Y | Z / X / C / V |
| D-pad | Arrow keys |
| Left stick | W A S D |
| Right stick | I J K L |

**F11** or **Alt+Enter** switches between a window and fullscreen.

### Settings menu
Open it with **F1**, or **Back + Start** on a controller. The game pauses its input while the menu is open.

| Setting | Choices |
|---|---|
| Resolution | 640x480 (the console's) up to 4K |
| Aspect | 4:3, or 16:9 (stretched) |
| Frame limit | Off, 30, 60, 120, 144 or 240 fps |
| Vsync | On or off |
| Scaling | Smooth or sharp |
| FPS counter | On or off |
| Boot waits | Skip the 9 seconds the startup screens hold for |

**Up / Down** picks a setting, **Left / Right** (or A, Enter) changes it, and **F1**, **Esc** or **B** closes the menu.
Settings are saved, along with your game saves, in `%LOCALAPPDATA%\xboxrecomp`.

## Contributing
Bug reports are welcome: include your GPU and the end of `play_log.txt` (in the folder you unzipped to), but no game files. For working on the code,
see [docs/BUILDING.md](docs/BUILDING.md) (the build by hand, run-time settings), [docs/DEVELOPING.md](docs/DEVELOPING.md)
(debugging tools) and [docs/HISTORY.md](docs/HISTORY.md) (past bugs and their fixes).

## Credits and license
Built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz, which does the translation and supplies the
runtime. xboxrecomp is MIT licensed and includes parts of [xemu](https://github.com/xemu-project/xemu) under
LGPL-2.1-or-later (see its `NOTICE`).

The code in this repository is MIT licensed (see [LICENSE](LICENSE)), except `docs/xboxkrnl.exe.def`, which is
CC0-1.0, © 2017 Stefan Schmidt. The patches in `patches/` modify xboxrecomp and
fall under its licenses: the hunks for its `src/apu/` files change xemu code and are LGPL-2.1-or-later, the rest MIT.
Unreal Championship 2 and Unreal are trademarks of Epic Games; Xbox is a trademark of Microsoft.
