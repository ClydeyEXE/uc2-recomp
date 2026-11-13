# Unreal Championship 2: The Liandri Conflict, natively on PC

A static recompilation of the original Xbox game into a native Windows program, built with the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit. The game's own code runs, translated to C, with no
emulator.

> [!IMPORTANT]
> This is an unofficial fan project, not affiliated with or endorsed by Epic Games, Midway or Microsoft. It contains
> no game data and no code from the game: you need your own copy, and the build makes the game on your machine. The
> program it makes contains the game's code, so don't share it, and don't ask for game files or builds in Issues.

## Getting started

**You need**
- Windows 10 or 11
- About 3 GB of free space, plus about 6 GB if setup needs to install the Visual Studio Build Tools
- A disc image (`.iso`) of your own copy of the game: the USA/Europe release, which is one disc for both regions.
  A full dump or a trimmed `.xiso.iso` both work, as setup checks the game's `default.xbe`, not the image.

### 1. Download
Open the [latest release](https://github.com/ClydeyEXE/uc2-recomp/releases/latest) and download **Source code
(zip)** under Assets. Unzip it somewhere with space, such as `C:\Games\uc2`.

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
| Aspect | 4:3, 16:9 (stretched), or 16:9 (widescreen): the game sees more to the sides, the HUD keeps its shape and spreads to the screen's edges, menus keep their shape in the middle, and movies and cutscenes stay 4:3 |
| Frame limit | Off (up to 800 fps), 30, 60, 120, 144 or 240 fps, or just under your display's refresh rate (160 on a 165 Hz screen), which keeps a G-Sync or FreeSync display in its range |
| Vsync | On, or off: each frame is shown as soon as it is drawn, so with no frame limit it can tear on a display without G-Sync or FreeSync |
| Scaling | Smooth or sharp |
| Anisotropic | Off, 2x, 4x, 8x or 16x: sharper floors and walls at an angle |
| FPS counter | On or off |
| Boot waits | Keep, or skip the 9 seconds the startup screens hold for (from the next start) |

**Up / Down** picks a setting, **Left / Right** (or A, Enter) changes it, and **F1**, **Esc** or **B** closes the menu.
Settings are saved, along with your game saves, in `%LOCALAPPDATA%\xboxrecomp`. The menu's bottom line shows which
version you're running.

If the game hitches for about a quarter of a second every 9 seconds or so, check for RivaTuner Statistics Server: its
overlay hook stalls the game even with its frame limit off. Set the game's detection level to None in RivaTuner, or
close it, and use the frame limit above instead.

## Contributing
Bug reports are welcome: the [bug report form](https://github.com/ClydeyEXE/uc2-recomp/issues/new?template=bug_report.yml)
asks for your version, your GPU and the end of `play_log.txt` (in the folder you unzipped to). Please attach no game files.
Code contributions aren't taken; see [CONTRIBUTING.md](CONTRIBUTING.md). For working on the code yourself,
see [docs/BUILDING.md](docs/BUILDING.md) (the build by hand, run-time settings), [docs/DEVELOPING.md](docs/DEVELOPING.md)
(debugging tools) and [docs/HISTORY.md](docs/HISTORY.md) (past bugs and their fixes).

## Credits and license
Built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz, which does the translation and supplies the
runtime. xboxrecomp is MIT licensed and includes parts of [xemu](https://github.com/xemu-project/xemu) under
LGPL-2.1-or-later (see its `NOTICE`).

The code in this repository is MIT licensed (see [LICENSE](LICENSE)), except `tools/xboxkrnl.exe.def`, which is
CC0-1.0, © 2017 Stefan Schmidt. The patches in `patches/` modify xboxrecomp and
fall under its licenses: the hunks for its `src/apu/` files change xemu code and are LGPL-2.1-or-later, the rest MIT.
Unreal Championship 2 and Unreal are trademarks of Epic Games; Xbox is a trademark of Microsoft.
