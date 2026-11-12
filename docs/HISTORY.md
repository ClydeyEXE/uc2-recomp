# Fixed so far, and how

Read this when a symptom looks familiar, or before changing the code a fix touched. GitHub Issues have what is still open.

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
   FPU stack. `recomp_lookup_manual` now supplies both.
   This did not fix the fatal screen, whatever the commit "Supply missing CRT x87 entries to reach map loading" says.
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
    `[OHCI0] STALL: 21 09` before the crash in a played match did not appear in any gated run, so it looks like a symptom of the
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
16. **Black boxes on explosions.** Explosions draw sprites into a 320x240 distortion map (`0x801CD000`), then a full-screen
    pass with stage program `0xA621` reads the scene through it: `DOT_PRODUCT` then `DOT_ST`, with an alpha test on the map.
    The shader returned zero for those modes, so wherever a sprite had written alpha the pass drew black, blocky from the
    low-resolution map. A colour grading pass (`0x2621`) uses the same pair and was losing about a quarter of its light.
    The modes are implemented for stage 1 followed by stage 2. Blending can't darken here, so the clue was that hiding
    additive draws removed the box: they fed the map.
17. **Cutscenes hitched.** In Story Mode videos 1-8 frames a second took 25-80 ms and audio underran. The guest core
    spent half its time in `sub_002C6B80`, the XMV player's YUV 4:2:0 to BGRA converter for a 16x16 macroblock, because
    lifted MMX/SSE does every op lane by lane on thread-local register globals. It is now hand-written in
    `recomp_manual.c` with SSE2 intrinsics on locals, instruction for instruction, reading mm4-mm7 and xmm4-xmm7 from
    the guest and writing back the registers the original leaves. A harness running both bodies on 5000 random
    macroblocks (including saturating inputs and non-standard constants) matched every output byte and register, and
    it is about 12x faster (17.0 to 1.45 us a macroblock). A cutscene then held 157-159 fps with no underruns.
18. **Boot movies stalled now and then.** In about one boot in five the logo movie froze for up to a second at a time,
    `[IRQ] wait ... gate holder has reached no safe point` lines appeared and audio underran. The holder was the timer
    thread inside D3D's GPU interrupt DPC (`sub_0038CFA0`), which writes the vblank ack to PCRTC_INTR_0 and spins until
    PMC_INTR_0's PCRTC bit drops. The suspected cycle (a vblank posted to the DPC with `tick_busy` up) was wrong: logged
    while it spun, PMC_INTR_EN_0 was 0, `tick_busy` 0 and the ack written, but the vblank-ack thread that takes it was
    not running. Its stack showed it inside `SwitchToThread`, from the watch's first 2 ms. That call yields only to a
    thread ready on the same processor, and that was the timer thread, just woken to run the DPC: the watch handed it
    its core and queued behind the spin for 60-600 ms. The watch now spins on a pause for those 2 ms, which cost no more
    CPU (the yield returned at once whenever nothing else was ready). That alone left 1 boot in 15 with a 340 ms ack, the
    thread woken from its 1 ms wait into the same queue, so the thread also runs time-critical; it works for
    microseconds a vblank. With both, 15 boots in a row had no `[IRQ] wait`, no underruns, vblank gaps of 12-21 ms and
    no ack later than 7 ms, against gaps of up to 686 ms before.
19. **Distortion effects showed through walls.** The purple barrier on Sovereign, and cloaked players, were visible
    through anything in front of them. Both are distortion effects. A full-screen pass reads the scene's Z24S8 depth
    buffer (`0x80620000`) as an A8R8G8B8 texture and draws it into a half-size colour surface (`0x807FA000` on
    Sovereign, `0x8074C000` on Aqueduct); the effect's sprites then draw into the distortion map with that surface as
    their depth buffer, so walls hide them. Depth lived only in D3D11 depth buffers and colour in render targets, so
    the read found nothing and the sprites tested against an empty depth buffer. Depth buffers are now sampleable,
    and two passes carry the bytes across in the console's layout: `zeta_encode` when a draw samples a depth buffer's
    address, `zeta_decode` when a depth buffer's address was last written as colour. Visibility tests were the
    first suspect (they always answered "all visible") and now return real counts too, but did not change the glow:
    the draw log showed the glow covering a pillar in front of it, which no culling decision can do.
20. **The countdown before a match was silent.** "The match is about to begin... 3, 2, 1" plays
    `A_Gameplay.Countdown` once a second, and the game did ask XACT for it each time, but no voice started. Its wave
    is in `A_Weapon.xwb`, the one streaming wave bank: XACT fills each of a voice's two packets with an overlapped
    read. The tick fits one packet, so on the second XACT finds no data left and asks `sub_00362D65` whether anything
    is in flight before ending the stream. A packet whose read had finished but which had not gone to DirectSound
    counted as done, so the stream was ended empty and the packet dropped. On the Xbox the read is still running at
    that point; the runtime completes reads before they return. At that one call (returning to `0x00369C08`) such a
    packet now counts as in flight, and XACT hands it over the next time it services the voice. Other short sounds in
    that bank, such as `A_Weapon.SN_Raise`, were silent for the same reason.
21. **Game clock 0.2-0.4% off real time, different every boot.** The main loop (`sub_0001A310`) times frames with
    `rdtsc` times seconds per cycle (`0x0040C590`), which `sub_00068010` calibrates at boot by counting cycles while
    `KeTickCount` moves 200 ms. The runtime wrote `KeTickCount` from `GetTickCount()`, which moves in steps of about
    15.6 ms, so the calibration came out 0.996-1.002x the emulated clock's 733 MHz rate and the level clock and match
    timer ran that much off (#20). `KeTickCount` now counts milliseconds from the performance counter
    (`patches/xboxrecomp-gpu-backend.patch`), and the calibration comes out exact. The frame rate never mattered: the
    delta is unclamped up to the level's 0.0005-0.4 s, and `tools/tick_rates.py` measured the same movement, fire and
    timers at 60, 144, 240 and no limit.
