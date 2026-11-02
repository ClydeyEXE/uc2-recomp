# Fixed so far, and how

Read this when a symptom looks familiar, or before changing the code a fix touched. `HANDOFF.md` has what is still open.

1. **Freeze after the first flip.** The dump's registers fit the spin at `loc_00386C60` in `sub_00386B30`
   (`eax=2`, `ecx=3`, `esi=0xD`, `edi=0xA`), which waits for the semaphore to reach the fence. It is not the
   `0x400B10` loop in `sub_00386AC0`. `RECOMP_WATCH` showed the semaphore at `0x80267000` written only once, to 3, by
   D3D at init. The executor dropped releases because it had no target. `sub_00386840_enter` in `recomp_manual.c` now
   supplies the target from `device+0x30`.
2. **"Dirty disc" fatal screen.** `sub_0001A110` draws a message and calls `Sleep(0xFFFFFED8)`, so it looks like a
   hang. It was reached through `ReadFile beyond EOF` in `sub_00015D20`, because `NtReadFile` rejected a buffer at
   `0x74000008`. `bridge_va_mapped` did not know about `RECOMP_EXT_VMA` memory. This is fixed in
   `patches/xboxrecomp-extvma-buffers.patch`.
3. **Movies stuck or crawling.** XMV (`sub_002C5E0A`) times frames with the vblank count at `device+0x1DE8`, read by
   `sub_00382610`, and adds a skew at `+0xC8` that tracks real time. With no vblanks the count never moved. Tying it
   to flips (21 Hz) made the two clocks disagree, so the skew kept jumping and frames were scheduled further away.
   `RECOMP_VBLANK=paced` delivers real 60 Hz vblanks to D3D's own ISR. Do not register the count with
   `xbox_Nv2aFrameCounter` as well.
4. **Freeze on pressing Start.** The original fence wait's "GPU far behind" path spins on PGRAPH `0x400B10` and sleeps on
   the event at `device+0x1DCC` (`miniport+0x1A4`) that a GPU interrupt sets. `src/recomp_manual.c` replaces
   `sub_00386B30`: it kicks the fence if newest, then waits on the semaphore, giving up after 250 ms with GET not moving.
   It logs `[D3D] fence ...: no GPU progress` then, and the original's callback at `device+0x19FC` is not called.
5. **Pad not seen.** `main.c` never called `xbox_OhciInit`, so `RECOMP_USB` did nothing.
6. **Untranslated CRT x87 targets.** The table at `0x0040AAB8` has targets `0x002B7B7C` and `0x002B7C1E` that were
   never translated, because neighbouring functions started two bytes further in. Skipped calls left values on the
   FPU stack. `recomp_lookup_manual` now supplies both. This did not fix the fatal screen, whatever commit 893e6d1 says.
7. **Fatal screen on Begin Match.** `RtlRaiseException` only logs and returns, so code ran on after every `throw`. Loading
   a match throws "Can't resolve package name" in normal use, and running past it ended in a garbage file name and
   `sub_00016784`'s dirty disc call. `src/recomp_manual.c` now replaces `_CxxThrowException` (`sub_002BA881`): it walks
   `fs:[0]`, reads the MSVC tables, runs destructors, then longjmps to a wrapper that runs the catch funclet and its
   continuation. `[EH]` lines log each throw.

   Limits: only C++ throws are handled. Raises through `RtlRaiseException` still return, and `__except` is not
   dispatched. Each function with a catch needs a wrapper, and each continuation an entry in `tools/add_eh_entries.py`.
   The game has 11 such functions, and all are covered, except one in an XDK section (funclet `0x374BF1`), which was not
   decoded. A throw with no site logs `NO CATCH SITE` and falls through as before.
8. **Frozen map load.** `sub_002043A0`, the wrapper for GetVisibilityTestResult, waited for a report the executor never
   writes. `src/recomp_manual.c` now reports the test done with 640x480 pixels visible, so occlusion culling never
   culls. That may cost speed.
9. **Flicker and popping.** D3D runs recorded pushbuffers by jumping into them. A non-zero `NO_OPERATION` before the jump
   (argument `<< 5` | code) traps to D3D's interrupt handler `sub_0038D600`, whose codes 12-14 write the recording's
   return jump and fixups (`sub_003863F0`). `d3d_software_method` in `recomp_manual.c` does that from the walk. Unhandled,
   each tail returned to its first caller and old commands replayed, which was also why `RECOMP_PB_WALK=dma` livelocked.
   D3D patches those `NO_OPERATION`s while GET shows the GPU has not reached them, so GET must be exact, which only the
   DMA walk gives.
10. **Low frame rate.** `src/gpu_d3d11.c` draws on the GPU through `nv2a_backend.h`. The kick's PFB flush spin is gone
    (`sub_00386FA0` in `recomp_manual.c`), so the game thread no longer waits while the executor runs vertex programs.
    Those run on the raster pool with duplicate indices removed. `RECOMP_LOG` replaces PowerShell's slow stderr pipe.
    SSX3-recomp (GPL, Vulkan) was a useful reference for this toolkit's GPU path; its code was read for approach, not
    copied.
11. **Checkerboard of missing triangles.** Strips alternate winding, and culling added for the GPU path did not allow for
    it. `raster_batch_program` now swaps every other triangle's first two vertices.
12. **Arena exhausted.** Contiguous frees were ignored without `RECOMP_HEAP_RECLAIM`, so the arena ran out about two
    minutes into a match. Reusing blocks then crashed on pages the last owner had protected, so `xbox_ContiguousAllocEx`
    now resets their protection.
13. **Crashes in XPP.** `sub_00399A84`, `sub_003998CA` and the others faulted on `0xFFFFFFFF`, reached through a transfer
    descriptor that had been freed: `sub_00398505` sets a freed descriptor's link to that value. USB submits run on a game
    thread at DISPATCH and completions in a DPC on the timer thread, with nothing keeping them apart, so the free list
    and the queues raced. `RECOMP_DISPATCH_GATE=1` serializes them as the single CPU does. A scripted match crashed at
    about 3.5 minutes without it, and did not crash in four runs of 400 s or in a longer soak with it. The stray
    `[OHCI0] STALL: 21 09` before the owner's crash did not appear in any gated run, so it looks like a symptom of the
    race, not a gap in `src/usb`.
14. **Chopped audio and a 10 ms stall per update.** `sub_002F96C7`, on every DirectSound DoWork, tests `0xFE85A018`, word 6
    of EP program memory, for `0xCCCCCC`, and otherwise takes the DSP to have crashed and resets it (`sub_002F9487`). No EP
    program runs here, so the word stayed 0: about 50 resets a second, each stopping the sequencer (SECTL XCNTMODE off) and
    stalling 10 ms. The frame thread filled that time with silent periods, so about half the output was digital zero, and
    the voices resumed where they stopped. `mcpx_apu_dsp_ack_poll` now writes the word, and a stopped sequencer produces no
    frames for up to 50 ms, as in xemu. In a 400 s capture, runs of zero with sound on both sides went from 17,020 to 3,
    click candidates from 3,993 to 2, and resets to 0. Match frame rate rose from the 40s to about 60 at the same point.
    The GP and EP memories are plain RAM (only `0xFE800000..0xFE830000` traps), so `mcpx_apu_mmio_*` never sees them.
15. **Black boxes.** UC2 draws its projected shadows into 256x256 B8 surfaces (format `0x119`, pitch 256), then multiplies
    the scene by them with blend `DST_COLOR, SRC_COLOR` and a combiner that lerps from 0.5 grey to the texture. The
    vertex-program path dropped triangles for 1 byte a pixel, so the map stayed all zero and the projector darkened its
    whole frustum. Those triangles now reach the back end, and `gpu_d3d11.c` reads a target sampled as format `0x13` from
    blue, as luminance. `tools/play.ps1 -DrawLog` found this: it logs one frame's draws each second.
