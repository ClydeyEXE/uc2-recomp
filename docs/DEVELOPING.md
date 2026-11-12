# Developing

Open work is tracked in GitHub Issues, in the order of the pinned Roadmap issue. Past bugs and their fixes are in
`docs/HISTORY.md`: read it when a symptom looks familiar, or before changing code a fix touched.

## Measuring
- Every 10 s the log has a `[FRAMES]` line: p50/p90/p99 frame times and where the walker's time went. "Walker CPU"
  overstates work, as the ack loop polls with short sleeps and clock reads.
- Every 10 s the log also has a `[FENCE]` line: how long the title's main thread waited for the walker in D3D's
  fence wait (`sub_00386B30`), by return address. `0x003809BE` is present waiting for the frame before last, and
  `0x003870B7` a Lock waiting for the GPU to finish with a resource. The wait spins, so the main thread shows as
  busy in a profile while it waits: its work a frame is the frame time less this.
- A frame over 100 ms gets a `[HITCH]` line naming where the walker and the title's main thread were. The sampler
  unwinds 10 frames, so a "game thread, inclusive" line counts every function on its sampled stacks. `printf` in
  those lists is probably a mis-named frame (the nearest symbol to an unnamed address), not real printing.
- `python -I tools/prof.py --secs 20` profiles every thread of a live run.
- Scripted matches are much lighter than played ones. Use a real, busy match for performance work.
- `python -I tools/drive_match.py --secs 120 --timeout 400` drives the game into a CTF-Aqueduct match by watching it:
  each press waits for the screen it expects (file opens in the log, or the game window matched against reference
  frames), and it backs out of a wrong menu and retries. It ends with `ROUTE reached match at <t> s` or
  `ROUTE failed: <state>`, then runs `check_run.py`, and exits 0 only if both pass. A run's log, route and match
  frames go in `build/runs/<name>/`. Boot waits are skipped unless `--boot-waits`; `--map` picks another CTF arena.
  - `--direct` skips the menus: `RECOMP_CMDLINE` hands the game a start URL (`CTF-Aqueduct?Game=UCGameplay.CTFGame?
    NumBots=0?GamepadID=0?...`), it presses A through the boot logos (any pad button ends them) and once on the
    "press fire" scoreboard, and the match is up about 9 s after launch. `GamepadID=0` is needed: without the title's
    Start no pad is bound to the player, who then cannot press fire, and `PlayersMustBeReady=false` does not skip that
    screen in a standalone game. It runs 4:3 with no bots unless `--bots N`, and with `RECOMP_BENCH_SPAWN=<--seed>`.
- `RECOMP_BENCH_SPAWN=<seed>` fixes where the player spawns. The script picks a start with `RatePlayerStart`, which
  adds a random term through `RandRange`, so the wrapped `FRand` and `Rand` natives (`sub_0003E890`, `sub_0003D930` in
  `src/recomp_manual.c`) return values from a private sequence when `RatePlayerStart` is one of the three script
  frames running. It finds the caller's frame through `CallFunction`'s argument, and logs the first chains as
  `[BENCH]`. That walk takes about 2 ms (its `VirtualQuery` checks), so it stops for the rest of the run once the
  spawn is rated (`[BENCH] spawn chosen, hook off`); bots' first spawns are rated in the same burst, and respawns
  are random. Script `FRand`/`Rand` run only about once a second in an idle match (`[BENCH] ... calls in 10 s`). Seed 1 is CTF-Aqueduct's corridor view; 2 spawns elsewhere and 3 one start further along the corridor.
  A fixed `srand` would not do: every frame's emitters draw from the same `rand()` before the spawn.
  - It needs reference frames in `build/route/` (`main_menu.png`, `match.png` and others), captured on this machine
    at 2x window scale. They come from the game, so they stay local; a new machine has to capture its own.
  - It reads frames from the window with PrintWindow, so the window may be covered but not minimised. That costs the
    game nothing measurable: in one match, 30 s blocks with no grabs, a grab every 5 s and a grab every 0.5 s ran
    85.0, 82.0 and 85.9 fps (each grab takes the driver about 21 ms). Do not use
    `RECOMP_FB_DUMP` (or `--dumps`/`--no-window`) on runs whose fps matters: reading every frame back costs 10-13 ms a
    frame, and a match drops from about 105 to 37 fps.
- `python -I tools/ab.py --direct --runs 3 --secs 60 --a "" --b "RECOMP_X=1"` compares two settings (or two builds,
  with `--a-exe`/`--b-exe`) over interleaved runs. An idle match's fps depends on where the player spawns (65 to 158
  fps on CTF-Aqueduct), so `--direct` boots each run straight into the match at the spawn looking down the long
  corridor (#1's heavy view, checked against `build/route/corridor.png`), with no bots. It measures only windows with
  nothing moving in view and reports medians, spread and a 95% interval. A run takes about 85 s, and runs vary
  about 1% (A/A, 85-87.5 fps), so 3 a side detects about 3%. `--bots N` adds bots for real load. Every run is 4:3
  whatever the settings panel says, as the reference frames are. Without `--direct` it goes through the menus and
  forfeits until the spawn is the corridor (about one spawn in ten since toolkit v0.17.0), several minutes a run.
- `python -I tools/check_run.py <log>` says whether a run worked: maps loaded, match reached, crash, watchdog as timer
  or hang, untranslated instructions reached, menu and match fps (`settled` leaves out the window with the load),
  hitches and failed opens. It exits 0 on pass, 1 on fail; `--min-fps`, `--no-expect-match` and `--json` adjust it.
  Its tests are `python -I tools/test_check_run.py`. `[UNIMPL]` stops after 50 lines, so later addresses don't show.
- `tools/run_match.ps1` presses on a timer and can stop in a menu or enter Story Mode; prefer `drive_match.py`.

## Debugging tools
- `tools/play.ps1 -DrawLog` logs one frame's draws a second, every frame while F7 is held, and F5 steps through kinds
  of blended draw to leave out. It is the way to find out what drew a stray shape. Each draw names its depth buffer
  (`zeta`) and how many host pixels passed the depth and stencil tests (`passed`), so something that should be hidden
  and is not shows there. Logged frames wait on the GPU after every draw, so they hitch.
- `RECOMP_ZETA_COPY=0` stops carrying depth between depth buffers and colour surfaces at the same address (see
  `zeta_encode` in `src/gpu_d3d11.c`), which brings back distortion effects drawing through walls.
- `RECOMP_AUDIO_HOST=1` logs `[AUDIO-HOST]` starves, and `RECOMP_AUDIO_WAV=<file>` captures what the APU sends the
  device. The host counters cannot see holes in the sound; the capture can.
- `RECOMP_APU_VOICE_LOG=1` logs every voice start as `[APU-ON]` with its time, handle, format and volume. A sound that
  logs no start was never handed to the APU, so the fault is on the game's side, not in mixing.
- Visibility tests are D3D11 occlusion queries, answered a frame or so later; `RECOMP_GPU_STATS=1` counts them.
  `RECOMP_VIS_PIXELS=<n>` answers n pixels at once instead (307200, a full 640x480, was the old behaviour) and logs
  each caller as `[VIS]`. With 0, everything the game culls by these tests disappears.
- 16:9 widescreen (#9) wraps three functions in `src/recomp_manual.c`: the scene's perspective matrix
  (`sub_001379C0`), the canvas's per-frame update (`sub_00099350`, which makes a 640x480 canvas 853 wide) and the
  canvas flush (`sub_00142960`, which stretches full-width pieces, centres menus and spreads the match HUD's
  widgets by thirds). Full-screen movies are shown 4:3: CreateFile (`sub_00262187`) says the last `.xmv` opened was
  from the root `Movies` folder, or the boot code is playing one, while the decoder (`sub_00030570`) runs. The loading
  screen, from `Textures/Movies`, is stretched.
  `[WIDE]` lines log each projection once. `RECOMP_WIDE_LOG=1` logs each flush's widgets, where they sit and how
  far they moved, once a second per flush site (2: every flush but the menus'): have the owner play with it to place
  a misplaced HUD piece.
  `RECOMP_WIDE_VIEW_FLIP=<ms>` turns the wider view on and off every ms, so consecutive frames compare both at
  one spot, and `RECOMP_WIDE_ZONES=0` leaves the HUD and menus laid out from the left.
  `drive_match.py` saves frames as shown and compares only the middle 4:3 when the HUD is kept there.
- `RECOMP_LOG=<file>` writes the log straight to a file. Do not use `-RedirectStandardError`: its pipe slows the game.
- `RECOMP_GPU=sw` restores the software rasteriser, and `RECOMP_GPU_STATS=1` logs renderer counts every 5 s.
- `RECOMP_FRAME_TRACE_ALL=1` with `RECOMP_FRAME_TRACE=<flag file>` logs every frame while the file exists, including
  each walk and jump, and `tools/frame_trace.py` summarises it. `run_match.ps1 -CaptureAt` frames are consecutive;
  report dumps are not, and `tools/sheet.py` makes a contact sheet of either.
- Dumps often catch a bare background between menu frames. That may be a fade or a frame caught mid-draw, not a hang.
  Compare several dumps, or a live window, first.
- `RECOMP_WATCHDOG_SECS=N` dumps registers, PUT/GET and the guest stack, then exits with code 3.
  `python -I tools/map_watchdog.py <log> third_party/xboxrecomp/tools/disasm/output/functions.json` maps the dump to
  function names. Stack words can be stale: only those at or above the reported esp are live.
- `RECOMP_WATCH=<va>` reports writes to a guest address. `RECOMP_FB_DUMP=<prefix>` writes frames as BMP files, and
  `RECOMP_PB_REPORT_MS=<ms>` sets how often. `RECOMP_PEEK=<va,va>` prints guest words at each report and in the
  watchdog dump, but shows `??` for `RECOMP_EXT_VMA` addresses.
- `RECOMP_UNIMPL_TRAP=1` stops at the first untranslated instruction actually reached.
- A temporary `void sub_XXXXXXXX_enter(void)` in `src/recomp_manual.c` is the quickest way to read guest state when a
  function is entered. Guest registers in a dump can identify the exact loop, so compare them with the lifted code.
- Before treating a stop as a hang, check for a deliberate one: search the log for `not mapped` and read the strings
  the code passes. The title shows a "dirty disc" screen and sleeps forever on some errors.
- Generated functions are `void sub_XXXXXXXX(void)` in `src/recomp/gen/recomp_*.c`.
- `faulthandler.dump_traceback_later` crashed the Python pipeline with an access violation, so do not sample the
  disassembler that way. Native crash logs repeat, so read the first `[CRASH]`, not the last.
- The runtime writes saves to `%LOCALAPPDATA%\xboxrecomp`, and `xbox_kernel.log` to the repo root (ignored).
- `third_party/xboxrecomp/docs/technical/xemu-debugging.md` describes comparing guest state with a working run in
  xemu. Keep xemu's files, BIOS and disc images out of this repo.

## Notes on the toolkit's output
- `func_id` category names are keyword guesses (for example 725 `game_vehicle`). Do not trust them.
- `tools/discover.py <default.xbe> build/discover` writes an earlier, independent function list. In `.text` the
  toolkit finds 10,736 functions, including 9,029 of its 9,317. Of the 288 only it found, 258 sit inside the
  toolkit's function bodies.
- Assets are cooked for Xbox and read by the game itself through the runtime's file layer.
- Vulkan was considered and set aside: D3D11 already draws on the GPU, and the remaining limits are on the CPU.
