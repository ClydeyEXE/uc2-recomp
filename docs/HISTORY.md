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
