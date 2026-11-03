# Developing

Open work is tracked in GitHub Issues, in the order of the pinned Roadmap issue. Past bugs and their fixes are in
`docs/HISTORY.md`: read it when a symptom looks familiar, or before changing code a fix touched.

## Measuring
- Every 10 s the log has a `[FRAMES]` line: p50/p90/p99 frame times and where the walker's time went. "Walker CPU"
  overstates work, as the ack loop polls with short sleeps and clock reads.
- A frame over 100 ms gets a `[HITCH]` line naming where the walker and the title's main thread were. The sampler
  unwinds 10 frames, so a "game thread, inclusive" line counts every function on its sampled stacks. `printf` in
  those lists is probably a mis-named frame (the nearest symbol to an unnamed address), not real printing.
- `python -I tools/prof.py --secs 20` profiles every thread of a live run.
- Scripted matches are much lighter than played ones. Use a real, busy match for performance work.
- `tools/run_match.ps1` can stop in a menu or enter Story Mode, so look at the frames before trusting a number from
  it. With `RECOMP_SKIP_BOOT_WAITS=1` the title comes up 9 s sooner, so move every press 9 s earlier.

## Debugging tools
- `tools/play.ps1 -DrawLog` logs one frame's draws a second, every frame while F7 is held, and F5 steps through kinds
  of blended draw to leave out. It is the way to find out what drew a stray shape.
- `RECOMP_AUDIO_HOST=1` logs `[AUDIO-HOST]` starves, and `RECOMP_AUDIO_WAV=<file>` captures what the APU sends the
  device. The host counters cannot see holes in the sound; the capture can.
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
