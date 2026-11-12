/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

typedef void (*recomp_func_t)(void);

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;


/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern __declspec(thread) uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

/* ── GPU semaphore ─────────────────────────────────────────── */

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#define D3D_DEVICE_PTR_VA     0x00393888u  /* D3D's global device pointer */
#define D3D_DEV_WRITE_SEQ     0x2Cu        /* device field: next fence to issue */
#define D3D_DEV_SEMAPHORE_PTR 0x30u        /* device field: semaphore address */
#define D3D_DEV_REGISTERS     0x1C20u      /* device field: USER register block */
#define D3D_DEV_FLAGS         0x08u        /* device field: state flags */
#define D3D_DEV_ALT_PUT       0x00000004u  /* flag: the put slot is at device+0x770 */
#define D3D_DEV_SUSPENDED     0x00002000u  /* flag: kicks do nothing */
#define D3D_FENCE_WAIT_MS     250u         /* longest a wait lasts with GET not moving */

/*
 * Entry hook on D3D's fence insert, the only code that pushes the semaphore
 * release (method 0x1D70). The lifter calls this at the top of sub_00386840
 * when tools.recomp runs with --exclude-manual on this file.
 *
 * D3D's fence wait, sub_00386B30, spins until the semaphore reaches its fence.
 * The executor drops releases until it is told where the semaphore is, so the
 * word kept the 3 D3D gave it at init and the title froze after its first
 * flip. Telling it here comes before any release can be pushed. Re-read on each
 * call because D3D rewrites the field on device reset.
 */
void sub_00386840_enter(void)
{
    static uint32_t current;
    const char *mem = (const char *)g_xbox_mem_offset;
    uint32_t dev = *(const uint32_t *)(mem + D3D_DEVICE_PTR_VA);
    uint32_t sem = *(const uint32_t *)(mem + dev + D3D_DEV_SEMAPHORE_PTR);

    if (sem != current) {
        current = sem;
        nv2a_pb_set_semaphore_target(sem);
        fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
    }
}

/*
 * D3D's fence wait, BlockOnTime(fence, flags), replacing the generated body.
 *
 * The original has a "GPU far behind" path that patches a software method into
 * the pushbuffer, spins on PGRAPH 0x400B10 and sleeps on an event set by the GPU
 * interrupt. The executor's default segment scan models neither, so the game
 * froze there when Start was pressed. RECOMP_PB_WALK=dma models the register but
 * rendered the title screen wrongly and still hung on the event.
 *
 * This keeps what the wait means: kick the fence if it is the newest one, then
 * wait for the real semaphore to reach it. It gives up only when the GPU has made
 * no progress for a while, as a lost release must not freeze the title. Giving up
 * on a GPU that is just busy lets D3D reuse pushbuffer space the executor has not
 * read, which desynchronises the walk. The original's callback at device+0x19FC is
 * not called.
 */
extern void sub_00386840(void);
extern __declspec(thread) uint32_t g_esp;
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);

#define NV2A_USER_DMA_GET 0x800044u

/*
 * Time the title spends here, by return address, logged every 10 s as [FENCE]:
 * how long the game waits for the GPU (the walker) a second. Present waits for
 * the frame before last (return 0x0038097E or 0x003809BE); Lock waits when a
 * resource the GPU still reads is locked (0x003870A0 for the newest fence,
 * 0x003870B7 for the resource's own). Only waits that loop are counted.
 */
#define FENCE_SITES 8
static struct { uint32_t ret, calls, waits; LONGLONG ticks; } s_fence[FENCE_SITES];
static LONGLONG s_fence_window, s_fence_freq;

static void fence_account(uint32_t ret, int waited, LONGLONG t0, LONGLONG t1)
{
    int i;

    for (i = 0; i < FENCE_SITES - 1 && s_fence[i].ret && s_fence[i].ret != ret; i++)
        ;
    s_fence[i].ret = ret;
    s_fence[i].calls++;
    if (waited) {
        s_fence[i].waits++;
        s_fence[i].ticks += t1 - t0;
    }
    if (!s_fence_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_fence_freq = f.QuadPart;
        s_fence_window = t1;
        return;
    }
    if (t1 - s_fence_window >= 10 * s_fence_freq) {
        char line[512];
        int n = 0;
        double span = (double)(t1 - s_fence_window) / s_fence_freq, all = 0;

        for (i = 0; i < FENCE_SITES && s_fence[i].ret; i++) {
            double ms = 1000.0 * s_fence[i].ticks / s_fence_freq;
            all += ms;
            n += snprintf(line + n, sizeof line - n, " | 0x%08X %u calls, %u waits, %.0f ms",
                          s_fence[i].ret, s_fence[i].calls, s_fence[i].waits, ms);
            if (n >= (int)sizeof line)
                n = (int)sizeof line - 1;
        }
        fprintf(stderr, "[FENCE] %.1f s: waited %.0f ms (%.1f%%)%s\n", span, all,
                100.0 * all / (1000.0 * span), line);
        memset(s_fence, 0, sizeof s_fence);
        s_fence_window = t1;
    }
}

void sub_00386B30(void)
{
    const char *mem = (const char *)g_xbox_mem_offset;
    uint32_t fence = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t dev = *(const uint32_t *)(mem + D3D_DEVICE_PTR_VA);
    const volatile uint32_t *seq =
        (const volatile uint32_t *)(mem + dev + D3D_DEV_WRITE_SEQ);
    const volatile uint32_t *sem = (const volatile uint32_t *)
        (mem + *(const uint32_t *)(mem + dev + D3D_DEV_SEMAPHORE_PTR));
    const volatile uint32_t *get = xbox_Nv2aRegPtr(NV2A_USER_DMA_GET);
    uint32_t last_get = get ? *get : 0;
    ULONGLONG start = 0;
    int kicked = 0, waited = 0;
    uint32_t ret = *(const uint32_t *)(mem + g_esp);
    LARGE_INTEGER t0, t1;

    QueryPerformanceCounter(&t0);
    if (fence == *seq) {
        g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0;
        g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0x00386B5Fu;
        sub_00386840();
    }
    while (*seq - fence < *seq - *sem) {
        waited = 1;
        if (get && *get != last_get) {
            last_get = *get;
            start = 0;
        }
        if (!start) {
            start = GetTickCount64();
        } else if (!kicked && GetTickCount64() - start >= 2) {
            /* The release may still be in the part of the pushbuffer D3D has not
             * kicked, in which case no amount of waiting brings it. This is D3D's
             * own KickOff (sub_00386EA0) less its wait for the GPU to go idle. */
            uint32_t flags = *(const uint32_t *)(mem + dev + D3D_DEV_FLAGS);
            kicked = 1;
            if (!(flags & D3D_DEV_SUSPENDED)) {
                uint32_t put = *(const uint32_t *)(mem + dev + ((flags & D3D_DEV_ALT_PUT) ? 0x770u : 0u));
                uint32_t regs = *(const uint32_t *)(mem + dev + D3D_DEV_REGISTERS);
                *(volatile uint32_t *)(mem + regs + 0x40) = put & 0x0FFFFFFFu;
            }
        } else if (GetTickCount64() - start >= D3D_FENCE_WAIT_MS) {
            static unsigned timeouts;
            if (timeouts++ < 8)
                fprintf(stderr, "[D3D] fence 0x%X: no GPU progress in %u ms (semaphore 0x%X)\n",
                        fence, D3D_FENCE_WAIT_MS, *sem);
            break;
        }
        SwitchToThread();
    }
    QueryPerformanceCounter(&t1);
    fence_account(ret, waited, t0.QuadPart, t1.QuadPart);
    g_esp += 12; /* ret 8 */
}

/*
 * D3D's PFB flush before a kick, replacing the generated body. It sets a bit in
 * 0x100410 and spins until the hardware clears it, which takes the hardware moments.
 * Here the executor clears it, and it cannot while it runs a batch's vertex programs,
 * so the game thread sat out most of each frame. Writes to guest memory are already
 * visible to the executor, so there is nothing to flush.
 */
void sub_00386FA0(void)
{
    g_esp += 4; /* ret */
}

/*
 * D3D runs a recorded pushbuffer by jumping into it, and the recording ends in a
 * jump whose target has to be this call's return address. So just before the jump
 * it pushes NO_OPERATION with a non-zero value, which traps to its interrupt handler
 * (sub_0038D600) while the GPU waits. Value is argument << 5 | code. Codes 12, 13 and
 * 14 end in sub_003863F0, which writes the tail's jump and applies the recording's
 * fixups. Unhandled, every tail kept the return from its first use, so each later
 * use went back and replayed old commands: parts of old frames in new ones, and
 * flips in the middle of frames. The executor calls this at the same point in the
 * stream, so what it patches is what the walk reads next.
 *
 * Other codes are not needed: 1 is flip pacing, 5 wakes the event only the original
 * fence wait slept on, and 6 to 9 have not been seen.
 */
#define D3D_FIXUPS_PENDING 0x00391430u   /* counter the handler decrements */
#define D3D_FIXUP_TABLE    0x00391644u   /* code 13's records, 20 bytes each */

static uint32_t guest_read(uint32_t va)
{
    return *(const volatile uint32_t *)((const char *)g_xbox_mem_offset + va);
}

static void guest_write(uint32_t va, uint32_t v)
{
    *(volatile uint32_t *)((char *)g_xbox_mem_offset + va) = v;
}

static void d3d_apply_fixups(uint32_t rec)
{
    do {
        uint32_t head = guest_read(rec), next;

        guest_write(guest_read(rec + 8), (head & 0x0FFFFFFFu) + 1);  /* jump to the return */
        if (head & 0x20000000u) {
            uint32_t base = guest_read(rec + 0x10), p = guest_read(rec + 0xC), size;

            while ((size = guest_read(p)) != 0xFFFFFFFFu) {
                memcpy((char *)g_xbox_mem_offset + guest_read(p + 4) + base,
                       (const char *)g_xbox_mem_offset + p + 8, size);
                p += 8 + size;
            }
        }
        next = guest_read(rec + 4);
        guest_write(rec + 4, 0xFFFFFFFFu);
        if (head & 0x80000000u)
            guest_write(rec, head & 0xBFFFFFFFu);
        rec = next;
    } while (rec + 1 > 1);                  /* neither 0 nor -1 */
}

static void d3d_software_method(uint32_t param)
{
    uint32_t arg = param >> 5;

    switch (param & 0x1F) {
    case 12:
        guest_write(D3D_FIXUPS_PENDING, guest_read(D3D_FIXUPS_PENDING) - 1);
        d3d_apply_fixups(arg | 0x80000000u);
        break;
    case 13:
        guest_write(D3D_FIXUPS_PENDING, guest_read(D3D_FIXUPS_PENDING) - 1);
        d3d_apply_fixups(arg * 20 + D3D_FIXUP_TABLE);
        break;
    case 14:
        d3d_apply_fixups(arg | 0x80000000u);
        break;
    default: {
        static unsigned seen[32];
        if (!seen[param & 0x1F]++)
            fprintf(stderr, "[D3D] software method code %u first seen (value 0x%08X)\n",
                    param & 0x1F, param);
        break;
    }
    }
}

void recomp_manual_gpu_init(void)
{
    extern void nv2a_pb_set_software_method(void (*handler)(uint32_t));
    nv2a_pb_set_software_method(d3d_software_method);
}

/*
 * The game's wrapper for D3D's GetVisibilityTestResult(index, *result), replacing
 * the generated body. The result is a 16-byte report the GPU writes: the device
 * (at 0x00393888) holds a table of report blocks at +0x7D4, one per 256 tests,
 * and the status word at +12 stays 0xFFFFFFFF until the count at +8 is in. The
 * D3D11 back end writes reports once its occlusion queries finish, a frame or so
 * later (gpu_d3d11.c), and the software rasteriser at once. While one is pending
 * this answers "not ready", as the original does: the game asks without waiting,
 * keeps a pending test for later and treats it as visible meanwhile. The original
 * also kicks the push buffer when the test has not been sent; the walker reads
 * the buffer as it is written, so that is left out.
 *
 * Before the back end counted pixels this answered a full 640x480 visible every
 * time, so the game's occlusion culling of characters never culled.
 *
 * RECOMP_VIS_PIXELS=<n> answers n at once instead (640x480 is 307200), and logs
 * each caller once as [VIS]. 0 culls everything the game tests: characters
 * disappear, including the player's own.
 */
#define D3D_DEVICE_PTR       0x00393888u
#define D3D_REPORT_BLOCKS    0x7D4u
#define D3D_REPORT_PENDING   0xFFFFFFFFu

void sub_002043A0(void)
{
    const char *mem = (const char *)g_xbox_mem_offset;
    uint32_t index = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t result = *(const uint32_t *)(mem + g_esp + 8);
    uint32_t dev = *(const uint32_t *)(mem + D3D_DEVICE_PTR);
    uint32_t block = *(const uint32_t *)(mem + dev + D3D_REPORT_BLOCKS + (index >> 8) * 4);
    uint32_t report = block + (index & 0xFF) * 16;
    static int pixels = -2;
    static uint32_t seen[16];

    if (pixels == -2) {
        const char *e = getenv("RECOMP_VIS_PIXELS");
        pixels = e ? atoi(e) : -1;
    }
    if (pixels != -1) {
        uint32_t ret = *(const uint32_t *)(mem + g_esp);
        for (int i = 0; i < 16; i++) {
            if (seen[i] == ret)
                break;
            if (!seen[i]) {
                seen[i] = ret;
                fprintf(stderr, "[VIS] caller 0x%08X index %u report 0x%08X status %08X count %u\n",
                        ret, index, report, *(const uint32_t *)(mem + report + 12),
                        *(const uint32_t *)(mem + report + 8));
                break;
            }
        }
        if (result)
            *(uint32_t *)(mem + result) = (uint32_t)pixels;
        g_eax = 1;
    } else if (*(const volatile uint32_t *)(mem + report + 12) == D3D_REPORT_PENDING) {
        g_eax = 0;
    } else {
        if (result)
            *(uint32_t *)(mem + result) = *(const volatile uint32_t *)(mem + report + 8);
        g_eax = 1;
    }
    g_esp += 12; /* ret 8 */
}

/*
 * The title's Sleep(ms), wrapping the generated body. Boot (sub_00019BB0) holds
 * its first screens for a fixed time: it sleeps out the rest of a minimum after
 * its set-up, then sleeps 4900 ms more, about 9 s of every boot with nothing to
 * do. With skip_boot_waits=1 under [game] in %LOCALAPPDATA%\xboxrecomp\ucc2_video.ini
 * (the settings panel's "Skip boot waits"), or RECOMP_SKIP_BOOT_WAITS=1, those two
 * sleeps are 0 ms. Every other caller sleeps as before.
 */
#define BOOT_WAIT_RET_MIN   0x00019CEDu  /* after the call that pads to a minimum */
#define BOOT_WAIT_RET_FIXED 0x00019D1Cu  /* after Sleep(0x1324) */

extern void sub_00266885_gen(void);

static int skip_boot_waits(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SKIP_BOOT_WAITS"), *base = getenv("LOCALAPPDATA");
        char ini[MAX_PATH];
        on = 0;
        if (base) {
            _snprintf(ini, sizeof ini, "%s\\xboxrecomp\\ucc2_video.ini", base);
            ini[sizeof ini - 1] = 0;
            on = GetPrivateProfileIntA("game", "skip_boot_waits", 0, ini) != 0;
        }
        if (e && *e)
            on = *e != '0';
        if (on)
            fprintf(stderr, "[BOOT] skipping the boot screens' fixed waits\n");
    }
    return on;
}

void sub_00266885(void)
{
    char *mem = (char *)g_xbox_mem_offset;
    uint32_t ret = *(const uint32_t *)(mem + g_esp);

    if ((ret == BOOT_WAIT_RET_MIN || ret == BOOT_WAIT_RET_FIXED) && skip_boot_waits())
        *(uint32_t *)(mem + g_esp + 4) = 0;
    sub_00266885_gen();
}

/*
 * A fixed spawn for benchmarks: the script natives FRand (sub_0003E890) and
 * Rand (sub_0003D930), wrapping the generated bodies. Where the player spawns
 * is chosen in script, by RatePlayerStart rating each PlayerStart with a random
 * term. Every frame's emitters and AI draw from the same CRT rand(), and the
 * number of frames before the match starts varies, so a fixed srand() would not
 * fix it.
 *
 * With RECOMP_BENCH_SPAWN=<seed> (non-zero), a call made for RatePlayerStart
 * (it draws through RandRange, a script function calling FRand) still draws
 * from rand() as before, but its result is replaced by the next value of a
 * private sequence started from the seed. The ratings, and so the spawn, are
 * then the same in every run of a map. The calling script function is told by
 * its UFunction, cached by pointer, so a call costs a read and a short lookup;
 * only a RandRange call looks one frame further up, which is slow (below). The
 * functions seen and the rating draws are logged as [BENCH], and tools/
 * drive_match.py --direct takes the first rating draw as the spawn. Off, the
 * natives run unchanged.
 */
#define FFRAME_NODE      0x04u        /* FFrame: the UFunction being run */
#define UOBJECT_NAME     0x24u        /* UObject: FName index */
#define GNAMES_DATA_VA   0x00440560u  /* TArray<FNameEntry*> data pointer */
#define GNAMES_NUM_VA    0x00440564u  /* its count */
#define FNAMEENTRY_TEXT  0x0Cu        /* FNameEntry: ANSI text */

extern void sub_0003E890_gen(void);
extern void sub_0003D930_gen(void);

static uint32_t bench_spawn_state;

static int bench_spawn(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_BENCH_SPAWN");
        bench_spawn_state = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
        on = bench_spawn_state != 0;
        if (on)
            fprintf(stderr, "[BENCH] fixed spawn, seed %u\n", bench_spawn_state);
    }
    return on;
}

/* Readable guest memory: the window goes up to 0x7FFE0000 (RECOMP_EXT_VMA), with
 * reserved pages in it that fault, and the crash handler is vectored, so test
 * the page first. Only bench runs come here. */
static int bench_readable(uint32_t va, uint32_t len)
{
    MEMORY_BASIC_INFORMATION mi;
    const char *p = (const char *)g_xbox_mem_offset + va;

    if (va < 0x10000u || va >= 0x7FFE0000u - len)
        return 0;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & 0xEEu))
        return 0;
    return p + len <= (const char *)mi.BaseAddress + mi.RegionSize || bench_readable(va + len - 1, 1);
}
#define GUEST_PTR_OK(p)  bench_readable((p), 0x40u)

static uint32_t bench_rd(uint32_t va)
{
    return bench_readable(va, 4) ? *(const uint32_t *)((const char *)g_xbox_mem_offset + va) : 0;
}

/* An object's name, or "?" if anything is off. */
static const char *bench_obj_name(uint32_t obj)
{
    uint32_t names, index, entry;

    if (!GUEST_PTR_OK(obj))
        return "?";
    names = bench_rd(GNAMES_DATA_VA);
    index = bench_rd(obj + UOBJECT_NAME);
    if (!GUEST_PTR_OK(names) || index >= bench_rd(GNAMES_NUM_VA) || !GUEST_PTR_OK(entry = bench_rd(names + 4u * index)))
        return "?";
    return (const char *)g_xbox_mem_offset + entry + FNAMEENTRY_TEXT;
}

/*
 * The FFrame that called the script function running in this one. A script
 * call goes through the virtual UObject::CallFunction(FFrame &Stack, ...),
 * which builds the callee's FFrame as a local, so the caller's frame is the
 * first word above it that points at a frame (same vtable) further up the
 * stack. *at gets the distance, which should be the same every time.
 */
#define BENCH_CALLER_SCAN 0x200u

static uint32_t bench_caller(uint32_t frame, uint32_t *at)
{
    uint32_t vtable = bench_rd(frame), off, q;

    for (off = 8; off < BENCH_CALLER_SCAN; off += 4) {
        q = bench_rd(frame + off);
        if (q > frame + off && q - frame < 0x4000u && bench_rd(q) == vtable) {
            *at = off;
            return q;
        }
    }
    return 0;
}

/*
 * What the script function running in an FFrame is to the bench. The FFrame is
 * the native's argument, so its UFunction can be read directly; its name is
 * looked up (with the slow checked reads) once per function and logged.
 */
enum { BENCH_OTHER, BENCH_RANDRANGE, BENCH_RATE };

static int bench_kind(uint32_t frame)
{
    static struct { uint32_t node; int kind; } cache[128];
    static int n;
    uint32_t node;
    const char *name;
    int i, kind;

    if (frame < 0x10000u || frame >= 0x7FFE0000u - 8u)
        return BENCH_OTHER;
    node = *(const uint32_t *)((const char *)g_xbox_mem_offset + frame + FFRAME_NODE);
    for (i = 0; i < n; i++)
        if (cache[i].node == node)
            return cache[i].kind;
    name = GUEST_PTR_OK(frame) ? bench_obj_name(node) : "?";
    kind = strcmp(name, "RandRange") == 0 ? BENCH_RANDRANGE
         : strcmp(name, "RatePlayerStart") == 0 ? BENCH_RATE
         : BENCH_OTHER;
    if (n < (int)(sizeof cache / sizeof cache[0])) {
        cache[n].node = node;
        cache[n].kind = kind;
        n++;
        fprintf(stderr, "[BENCH] script FRand/Rand caller %.32s\n", name);
    }
    return kind;
}

/*
 * The walk from RandRange to its caller costs a few VirtualQuery calls, about
 * 2 ms, so it stops once the spawn is chosen. The spawn is rated in one burst,
 * six draws on CTF-Aqueduct when the player presses fire, so the first
 * RandRange call that is not for RatePlayerStart and comes BENCH_OFF_MS or more
 * after a rating draw turns it off for the rest of the run. With bots, their
 * first spawns are rated in the same burst as the player's (StartMatch restarts
 * every controller in one loop); respawns after a death are random again.
 *
 * The calls are counted per 10 s (the first 30 windows), and the walks timed,
 * so the cost can be read from a run.
 */
#define BENCH_OFF_MS 2000u

static int bench_phase;              /* 0 before the first rating draw, 1 rating, 2 off */
static ULONGLONG bench_last_hit;
static uint64_t bench_walks, bench_walk_ticks;

static void bench_count(void)
{
    static ULONGLONG start;
    static uint64_t calls;
    static int windows;
    ULONGLONG now;

    if (windows >= 30)
        return;
    now = GetTickCount64();
    if (!start)
        start = now;
    calls++;
    if (now - start >= 10000) {
        windows++;
        fprintf(stderr, "[BENCH] %llu FRand/Rand calls in %.1f s, rating walk %s\n", (unsigned long long)calls,
                (now - start) / 1000.0, bench_phase == 2 ? "off" : "on");
        start = now;
        calls = 0;
    }
}

/* Whether a draw is for RatePlayerStart, directly or through RandRange. */
static int bench_rating(uint32_t stack, int kind)
{
    LARGE_INTEGER t0, t1, f;
    uint32_t caller, at = 0;
    int hit;

    if (kind == BENCH_RATE)
        hit = 1;
    else if (kind != BENCH_RANDRANGE || bench_phase == 2)
        return 0;
    else {
        QueryPerformanceCounter(&t0);
        caller = bench_caller(stack, &at);
        hit = caller && bench_kind(caller) == BENCH_RATE;
        QueryPerformanceCounter(&t1);
        bench_walks++;
        bench_walk_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
        if (bench_walks == 1)
            fprintf(stderr, "[BENCH] RandRange called from %.32s (+0x%X)\n",
                    caller ? bench_obj_name(bench_rd(caller + FFRAME_NODE)) : "?", at);
    }
    if (hit) {
        bench_phase = 1;
        bench_last_hit = GetTickCount64();
    } else if (bench_phase == 1 && GetTickCount64() - bench_last_hit >= BENCH_OFF_MS) {
        bench_phase = 2;
        QueryPerformanceFrequency(&f);
        fprintf(stderr, "[BENCH] spawn chosen, rating walk off (%llu walks, %.2f us each)\n",
                (unsigned long long)bench_walks, 1e6 * (double)bench_walk_ticks / bench_walks / f.QuadPart);
    }
    return hit;
}

static uint32_t bench_next(void)
{
    bench_spawn_state = bench_spawn_state * 1103515245u + 12345u;
    return (bench_spawn_state >> 16) & 0x7FFFu;
}

void sub_0003E890(void)            /* UObject::execFRand(FFrame &Stack, float *Result) */
{
    char *mem = (char *)g_xbox_mem_offset;
    uint32_t stack, result;
    static int logged;

    if (!bench_spawn()) {
        sub_0003E890_gen();
        return;
    }
    stack = *(const uint32_t *)(mem + g_esp + 4);
    result = *(const uint32_t *)(mem + g_esp + 8);
    sub_0003E890_gen();
    bench_count();
    if (bench_rating(stack, bench_kind(stack))) {
        float f = (float)bench_next() / 32767.0f;
        memcpy(mem + result, &f, 4);
        if (logged < 24) {
            logged++;
            fprintf(stderr, "[BENCH] RatePlayerStart FRand -> %.4f\n", f);
        }
    }
}

void sub_0003D930(void)            /* UObject::execRand(FFrame &Stack, int *Result) */
{
    char *mem = (char *)g_xbox_mem_offset;
    uint32_t esp0 = g_esp, stack, result;
    int32_t max;
    static int logged;

    if (!bench_spawn()) {
        sub_0003D930_gen();
        return;
    }
    stack = *(const uint32_t *)(mem + esp0 + 4);
    result = *(const uint32_t *)(mem + esp0 + 8);
    sub_0003D930_gen();
    bench_count();
    /* The body keeps Max in its local at entry esp - 4, which nothing below it
     * reuses before the return. */
    max = *(const int32_t *)(mem + esp0 - 4);
    if (max > 0 && bench_rating(stack, bench_kind(stack))) {
        int32_t v = (int32_t)(bench_next() % (uint32_t)max);
        *(int32_t *)(mem + result) = v;
        if (logged < 24) {
            logged++;
            fprintf(stderr, "[BENCH] RatePlayerStart Rand(%d) -> %d\n", max, v);
        }
    }
}

/*
 * A per-tick probe of game state, for checking that the game's logic runs the
 * same at any frame rate (#20): UGameEngine::Tick(float DeltaSeconds), engine
 * in ecx, wrapping the generated body. Off unless RECOMP_TICK_PROBE=<file>.
 *
 * After each tick it writes a CSV line: the host time, the DeltaSeconds the
 * main loop passed, the level's TimeSeconds and TimeDilation, the local
 * player's pawn (Location, Velocity, Physics) and any further properties named
 * in RECOMP_TICK_PROBE_FIELDS, comma separated as <obj>.<Property>, where obj
 * is L (LevelInfo), C (the PlayerController), P (its Pawn), W (the pawn's
 * Weapon), G (Level.Game), R (the game's GameReplicationInfo) or X (the
 * newest Projectile among the last TP_PROJ_SCAN entries of the level's actor
 * list, where spawned actors are added; X.Name gives its name, so a new shot
 * shows as a new name).
 *
 * Properties are found by name through the classes' reflection data, whose
 * layout is worked out at the first tick from the LevelInfo, whose
 * TimeDilation and TimeSeconds are known to sit at +0x2AC and +0x2B0 (ULevel::
 * Tick, sub_000E30D0). The layout found, and each class's property list, go to
 * <file>.props.
 */
#define TP_ENGINE_LEVEL    0x13Cu       /* UGameEngine: GLevel (+0x4C is the client) */
#define TP_LEVEL_ACTORS    0x30u        /* ULevel: Actors data; [0] is the LevelInfo, count at +4 */
#define TP_PROJ_SCAN       48
#define TP_SECONDS_PER_CYCLE 0x0040C590u /* float, calibrated at boot (sub_00068010) */
#define TP_TIMEDILATION    0x2ACu       /* LevelInfo */
#define TP_TIMESECONDS     0x2B0u

extern void sub_000C91B0_gen(void);
extern __declspec(thread) uint32_t g_ecx;

static FILE *tp_out, *tp_props;
static int tp_state = -1;               /* -1 unread, 0 off, 1 on, 2 layout failed */
static uint32_t tp_class_off, tp_super_off, tp_next_off, tp_children_off, tp_propoff_off;
static LARGE_INTEGER tp_freq, tp_t0;

/* Guest reads, checked against a small cache of 64 KB regions seen committed. */
static int tp_ok(uint32_t va)
{
    static uint32_t good[64];
    static int ngood, rr;
    uint32_t r = va >> 16;
    int i;

    if (va < 0x10000u || va >= 0x7FFD0000u)
        return 0;
    for (i = 0; i < ngood; i++)
        if (good[i] == r)
            return 1;
    if (!bench_readable(r << 16, 0x10000u))
        return 0;
    if (ngood < 64)
        good[ngood++] = r;
    else
        good[rr++ & 63] = r;
    return 1;
}

static uint32_t tp_rd(uint32_t va)
{
    return tp_ok(va) && tp_ok(va + 3) ? *(const uint32_t *)((const char *)g_xbox_mem_offset + va) : 0;
}

static const char *tp_name(uint32_t obj)
{
    uint32_t names = tp_rd(GNAMES_DATA_VA), index, entry;

    if (!obj || !tp_ok(obj) || !names)
        return "";
    index = tp_rd(obj + UOBJECT_NAME);
    if (index >= tp_rd(GNAMES_NUM_VA) || !(entry = tp_rd(names + 4u * index)))
        return "";
    return (const char *)g_xbox_mem_offset + entry + FNAMEENTRY_TEXT;
}

static const char *tp_class_name(uint32_t obj)
{
    return obj ? tp_name(tp_rd(obj + tp_class_off)) : "";
}

static int tp_ends(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && strcmp(s + a - b, tail) == 0;
}

/* A UField: what a class's Children list holds. */
static int tp_is_field(uint32_t obj)
{
    const char *k = tp_class_name(obj);
    return tp_ends(k, "Property") || !strcmp(k, "Function") || !strcmp(k, "State") ||
           !strcmp(k, "Const") || !strcmp(k, "Enum") || !strcmp(k, "Struct");
}

static uint32_t tp_field(uint32_t cls, const char *name)
{
    int depth, n;
    uint32_t f;

    for (depth = 0; cls && depth < 32; depth++, cls = tp_rd(cls + tp_super_off))
        for (f = tp_rd(cls + tp_children_off), n = 0; f && n < 4096; f = tp_rd(f + tp_next_off), n++)
            if (!strcmp(tp_name(f), name) && tp_ends(tp_class_name(f), "Property"))
                return f;
    return 0;
}

/* Works out the reflection layout from the LevelInfo. */
static int tp_layout(uint32_t li)
{
    uint32_t off, cls, f, p;

    for (off = 0x1C; off <= 0x40 && !tp_class_off; off += 4)
        if (!strcmp(tp_name(tp_rd(li + off)), "LevelInfo"))
            tp_class_off = off;
    if (!tp_class_off)
        return 0;
    cls = tp_rd(li + tp_class_off);
    for (off = 0x28; off <= 0x80 && !tp_super_off; off += 4)
        if (!strcmp(tp_name(tp_rd(cls + off)), "ZoneInfo"))
            tp_super_off = off;
    if (!tp_super_off)
        return 0;
    tp_next_off = tp_super_off + 4;
    for (off = tp_super_off + 8; off <= 0x100 && !tp_children_off; off += 4) {
        f = tp_rd(cls + off);
        if (f && tp_is_field(f) && (!tp_rd(f + tp_next_off) || tp_is_field(tp_rd(f + tp_next_off))))
            tp_children_off = off;
    }
    if (!tp_children_off)
        return 0;
    p = tp_field(cls, "TimeDilation");
    for (off = 0x2C; p && off <= 0x80 && !tp_propoff_off; off += 4)
        if (tp_rd(p + off) == TP_TIMEDILATION)
            tp_propoff_off = off;
    if (!tp_propoff_off || tp_rd(tp_field(cls, "TimeSeconds") + tp_propoff_off) != TP_TIMESECONDS)
        return 0;
    fprintf(tp_props, "layout: class +0x%X, super +0x%X, next +0x%X, children +0x%X, property offset +0x%X\n",
            tp_class_off, tp_super_off, tp_next_off, tp_children_off, tp_propoff_off);
    return 1;
}

/* Lists a class's properties once, with its supers'. */
static void tp_list(uint32_t cls)
{
    static uint32_t done[32];
    static int ndone;
    int i, depth, n;
    uint32_t c, f;

    if (!cls || ndone >= 32)
        return;
    for (i = 0; i < ndone; i++)
        if (done[i] == cls)
            return;
    done[ndone++] = cls;
    fprintf(tp_props, "class %s\n", tp_name(cls));
    for (depth = 0, c = cls; c && depth < 32; depth++, c = tp_rd(c + tp_super_off)) {
        fprintf(tp_props, "  from %s\n", tp_name(c));
        for (f = tp_rd(c + tp_children_off), n = 0; f && n < 4096; f = tp_rd(f + tp_next_off), n++)
            if (tp_ends(tp_class_name(f), "Property"))
                fprintf(tp_props, "    +0x%03X %-16s %s\n", tp_rd(f + tp_propoff_off), tp_class_name(f), tp_name(f));
    }
    fflush(tp_props);
}

/* A property's offset in an object, cached per class and name; -1 if none. */
static int tp_off(uint32_t obj, const char *name, const char **kind)
{
    static struct { uint32_t cls; const char *name; int off; const char *kind; } cache[128];
    static int n;
    uint32_t cls, f;
    int i;

    if (!obj || !(cls = tp_rd(obj + tp_class_off)))
        return -1;
    for (i = 0; i < n; i++)
        if (cache[i].cls == cls && cache[i].name == name) {
            if (kind)
                *kind = cache[i].kind;
            return cache[i].off;
        }
    f = tp_field(cls, name);
    if (n < 128) {
        cache[n].cls = cls;
        cache[n].name = name;
        cache[n].off = f ? (int)tp_rd(f + tp_propoff_off) : -1;
        cache[n].kind = f ? _strdup(tp_class_name(f)) : "";
        n++;
    }
    if (kind)
        *kind = f ? cache[n - 1].kind : "";
    return f ? (int)tp_rd(f + tp_propoff_off) : -1;
}

static uint32_t tp_obj(uint32_t obj, const char *name)
{
    int off = tp_off(obj, name, NULL);
    return off < 0 ? 0 : tp_rd(obj + (uint32_t)off);
}

static int tp_is_a(uint32_t obj, const char *cls_name)
{
    uint32_t c = obj ? tp_rd(obj + tp_class_off) : 0;
    int depth;

    for (depth = 0; c && depth < 32; depth++, c = tp_rd(c + tp_super_off))
        if (!strcmp(tp_name(c), cls_name))
            return 1;
    return 0;
}

/* The fields asked for in RECOMP_TICK_PROBE_FIELDS. */
static struct { char obj; char name[48]; } tp_fields[24];
static int tp_nfields;

static void tp_value(uint32_t obj, const char *name)
{
    const char *kind = "";
    int off;
    const char *p;

    if (!strcmp(name, "Name")) {
        fprintf(tp_out, ",%s", obj ? tp_name(obj) : "");
        return;
    }
    off = tp_off(obj, name, &kind);

    if (off < 0 || !tp_ok(obj + (uint32_t)off + 11)) {
        fputs(",", tp_out);
        return;
    }
    p = (const char *)g_xbox_mem_offset + obj + (uint32_t)off;
    if (!strcmp(kind, "FloatProperty"))
        fprintf(tp_out, ",%.6g", *(const float *)p);
    else if (!strcmp(kind, "IntProperty"))
        fprintf(tp_out, ",%d", *(const int32_t *)p);
    else if (!strcmp(kind, "ByteProperty"))
        fprintf(tp_out, ",%u", *(const uint8_t *)p);
    else if (!strcmp(kind, "StructProperty") && strstr(name, "Rotation"))     /* a Rotator: ints */
        fprintf(tp_out, ",%d;%d;%d", ((const int32_t *)p)[0], ((const int32_t *)p)[1], ((const int32_t *)p)[2]);
    else if (!strcmp(kind, "StructProperty"))
        fprintf(tp_out, ",%.3f;%.3f;%.3f", ((const float *)p)[0], ((const float *)p)[1], ((const float *)p)[2]);
    else if (!strcmp(kind, "ObjectProperty") || !strcmp(kind, "ClassProperty"))
        fprintf(tp_out, ",%s", tp_name(*(const uint32_t *)p));
    else
        fprintf(tp_out, ",0x%X", *(const uint32_t *)p);
}

static uint32_t tp_projectile(uint32_t level)
{
    uint32_t data = tp_rd(level + TP_LEVEL_ACTORS), count = tp_rd(level + TP_LEVEL_ACTORS + 4), i, a;

    for (i = count; data && i > 0 && count - i < TP_PROJ_SCAN; i--)
        if ((a = tp_rd(data + 4u * (i - 1))) && tp_is_a(a, "Projectile"))
            return a;
    return 0;
}

static void tp_open(void)
{
    const char *path = getenv("RECOMP_TICK_PROBE"), *fields = getenv("RECOMP_TICK_PROBE_FIELDS");
    char buf[MAX_PATH + 8];
    int i;

    tp_state = 0;
    if (!path || !*path)
        return;
    _snprintf(buf, sizeof buf, "%s.props", path);
    buf[sizeof buf - 1] = 0;
    tp_out = fopen(path, "w");
    tp_props = fopen(buf, "w");
    if (!tp_out || !tp_props)
        return;
    setvbuf(tp_out, NULL, _IOFBF, 1 << 16);
    QueryPerformanceFrequency(&tp_freq);
    QueryPerformanceCounter(&tp_t0);
    fputs("t,dt,level_time,dilation,pawn,x,y,z,vx,vy,vz,phys", tp_out);
    while (fields && *fields && tp_nfields < 24) {
        const char *end = strchr(fields, ','), *dot = strchr(fields, '.');
        size_t len = end ? (size_t)(end - fields) : strlen(fields);
        if (dot && dot < fields + len && dot == fields + 1 && len - 2 < sizeof tp_fields[0].name) {
            tp_fields[tp_nfields].obj = fields[0];
            memcpy(tp_fields[tp_nfields].name, dot + 1, len - 2);
            tp_fields[tp_nfields].name[len - 2] = 0;
            fprintf(tp_out, ",%c.%s", fields[0], tp_fields[tp_nfields].name);
            tp_nfields++;
        }
        fields += len + (end ? 1 : 0);
    }
    fputc('\n', tp_out);
    for (i = 0; i < tp_nfields; i++)
        tp_fields[i].name[sizeof tp_fields[0].name - 1] = 0;
    fprintf(stderr, "[PROBE] per-tick state to %s, %d extra fields\n", path, tp_nfields);
    {
        float spc;
        memcpy(&spc, (const char *)g_xbox_mem_offset + TP_SECONDS_PER_CYCLE, 4);
        fprintf(stderr, "[PROBE] seconds per cycle %.6e, %.5f x the emulated clock's\n", spc, spc * 733333333.0);
    }
    tp_state = 1;
}

static void tp_tick(uint32_t engine, float dt, double t)
{
    static double last_flush;
    uint32_t level = tp_rd(engine + TP_ENGINE_LEVEL), li, c, pc = 0, pawn = 0, obj;
    const char *mem = (const char *)g_xbox_mem_offset;
    int i, n, off;

    if (!level || !(li = tp_rd(tp_rd(level + TP_LEVEL_ACTORS))) || !tp_ok(li + TP_TIMESECONDS + 3))
        return;
    if (!tp_propoff_off && !tp_layout(li)) {
        uint32_t objs[3], off, w;
        objs[0] = li;
        objs[1] = tp_class_off ? tp_rd(li + tp_class_off) : 0;
        objs[2] = objs[1] && tp_children_off ? tp_rd(objs[1] + tp_children_off) : 0;
        for (i = 0; i < 3; i++) {
            fprintf(tp_props, "object %d at 0x%08X (%s)\n", i, objs[i], tp_name(objs[i]));
            for (off = 0; objs[i] && off < 0x100; off += 4) {
                w = tp_rd(objs[i] + off);
                fprintf(tp_props, "  +0x%02X 0x%08X %s\n", off, w, w >= 0x10000u ? tp_name(w) : "");
            }
        }
        fprintf(tp_props, "found: class +0x%X super +0x%X children +0x%X propoff +0x%X\n",
                tp_class_off, tp_super_off, tp_children_off, tp_propoff_off);
        fclose(tp_props);
        fprintf(stderr, "[PROBE] reflection layout not found; probe off\n");
        tp_state = 2;
        return;
    }
    for (c = tp_obj(li, "ControllerList"), n = 0; c && n < 64; c = tp_obj(c, "nextController"), n++)
        if (tp_is_a(c, "PlayerController")) {
            pc = c;
            break;
        }
    pawn = tp_obj(pc, "Pawn");
    fprintf(tp_out, "%.6f,%.6f,%.6f,%.4f", t, dt, *(const float *)(mem + li + TP_TIMESECONDS),
            *(const float *)(mem + li + TP_TIMEDILATION));
    off = tp_off(pawn, "Location", NULL);
    if (pawn && off >= 0 && tp_ok(pawn + (uint32_t)off)) {
        const float *loc = (const float *)(mem + pawn + (uint32_t)off);
        int voff = tp_off(pawn, "Velocity", NULL), poff = tp_off(pawn, "Physics", NULL);
        const float *vel = voff >= 0 ? (const float *)(mem + pawn + (uint32_t)voff) : NULL;
        fprintf(tp_out, ",%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u", tp_class_name(pawn), loc[0], loc[1], loc[2],
                vel ? vel[0] : 0.0f, vel ? vel[1] : 0.0f, vel ? vel[2] : 0.0f,
                poff >= 0 ? *(const uint8_t *)(mem + pawn + (uint32_t)poff) : 0u);
        tp_list(tp_rd(pawn + tp_class_off));
        tp_list(tp_rd(pc + tp_class_off));
    } else {
        fputs(",,,,,,,,", tp_out);
    }
    for (i = 0; i < tp_nfields; i++) {
        switch (tp_fields[i].obj) {
        case 'L': obj = li; break;
        case 'C': obj = pc; break;
        case 'P': obj = pawn; break;
        case 'W': obj = tp_obj(pawn, "Weapon"); break;
        case 'G': obj = tp_obj(li, "Game"); break;
        case 'R': obj = tp_obj(tp_obj(li, "Game"), "GameReplicationInfo"); break;
        case 'X': obj = tp_projectile(level); break;
        default: obj = 0;
        }
        if (obj && tp_fields[i].obj != 'L' && tp_fields[i].obj != 'C' && tp_fields[i].obj != 'P')
            tp_list(tp_rd(obj + tp_class_off));
        tp_value(obj, tp_fields[i].name);
    }
    fputc('\n', tp_out);
    if (t - last_flush > 1.0) {
        fflush(tp_out);
        last_flush = t;
    }
}

void sub_000C91B0(void)            /* UGameEngine::Tick(float DeltaSeconds) */
{
    uint32_t engine = g_ecx;
    float dt;
    LARGE_INTEGER now;

    if (tp_state < 0)
        tp_open();
    if (tp_state != 1) {
        sub_000C91B0_gen();
        return;
    }
    memcpy(&dt, (const char *)g_xbox_mem_offset + g_esp + 4, 4);
    sub_000C91B0_gen();
    QueryPerformanceCounter(&now);
    tp_tick(engine, dt, (double)(now.QuadPart - tp_t0.QuadPart) / (double)tp_freq.QuadPart);
}

/*
 * XACT's "is any packet of this voice still in flight", wrapping the generated
 * body. A voice playing from a streaming wave bank (A_Weapon.xwb) has two packet
 * slots, each filled by an overlapped disc read: state 0xAAAA while it is read
 * or waits to go to DirectSound, read status 0x103 while the read runs. When a
 * voice runs out of data (sub_003698E6, the call returning to 0x00369C08) XACT
 * asks this, and if nothing is in flight it ends the stream without handing it
 * the slots it has.
 *
 * A slot whose read has finished but which has not been handed over counts as
 * done here. On the Xbox that cannot happen at this call: a sound short enough
 * to fit one packet reaches it just after issuing its only read, which is still
 * running. The runtime completes reads before they return, so the read was
 * already done, the packet was dropped and nothing played: the countdown tick
 * before a match (A_Gameplay.Countdown) and short weapon sounds such as
 * A_Weapon.SN_Raise. At this call such a slot now counts as in flight, and is
 * handed over as soon as XACT next services the voice, as on the Xbox. The
 * other callers, which free a voice's buffers once nothing is in flight, keep
 * the original answer.
 */
#define XACT_EOS_RET         0x00369C08u  /* after the call in sub_003698E6 */
#define XACT_PKT_PENDING     0x8000000Au  /* slot status: not yet played */
#define XACT_PKT_READING     0x0000AAAAu  /* slot state: read issued */
#define XACT_PKT_END         0x0000DEADu  /* slot state: past the end of the data */
#define XACT_READ_PENDING    0x00000103u  /* STATUS_PENDING */

extern void sub_00362D65_gen(void);
extern __declspec(thread) uint32_t g_ecx;

void sub_00362D65(void)
{
    char *mem = (char *)g_xbox_mem_offset;
    uint32_t *slot = (uint32_t *)(mem + g_ecx + 0x10);
    int i;

    if (*(const uint32_t *)(mem + g_esp) != XACT_EOS_RET) {
        sub_00362D65_gen();
        return;
    }
    g_eax = 0;
    for (i = 0; i < 2; i++, slot += 8) {          /* 0x20-byte slots */
        if (slot[2] == XACT_PKT_END)
            continue;
        if (slot[2] == XACT_PKT_READING && slot[3] == XACT_READ_PENDING)
            slot[0] = XACT_PKT_PENDING;
        if (slot[0] == XACT_PKT_PENDING) {
            g_eax = 1;
            break;
        }
    }
    g_esp += 4; /* ret */
}

/* ── XMV colour conversion ────────────────────────────────── */

/*
 * The movie player's YUV 4:2:0 to 32-bit BGRA converter for one 16x16
 * macroblock, written in MMX and SSE. Lifted, it took half the guest core
 * during Story Mode cutscenes: every MMX and XMM register is a thread-local
 * global, every op goes lane by lane through memory, and frames that decoded
 * overran 33 ms. This is the same instruction sequence in SSE2 intrinsics on
 * locals, so it gives the same bits: the MMX registers live in the low halves
 * of __m128i, and the packs pair their operands first because the 128-bit
 * forms put the second operand's lanes in the upper half.
 *
 * The caller loads the constants into mm4-mm7 and xmm4-xmm7 once, so they are
 * read from the guest registers here, and mm0-mm3 and xmm0-xmm3 are written
 * back with what the original leaves in them.
 *
 * stdcall(y, u, v, y_stride, uv_stride, dst, dst_stride), ret 28.
 */
#include <emmintrin.h>

extern __declspec(thread) uint32_t g_ecx, g_edx, g_ebp, g_seh_ebp;
extern __declspec(thread) uint64_t g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
extern __declspec(thread) float g_xmm0[4], g_xmm1[4], g_xmm2[4], g_xmm3[4];
extern __declspec(thread) float g_xmm4[4], g_xmm5[4], g_xmm6[4], g_xmm7[4];

#define MMX_LOAD(r)     _mm_loadl_epi64((const __m128i *)&(r))
#define MMX_SAVE(r, v)  _mm_storel_epi64((__m128i *)&(r), (v))

/* punpckhwd on 64-bit registers: words 2 and 3 of each, interleaved. */
static __forceinline __m128i mmx_punpckhwd(__m128i a, __m128i b)
{
    return _mm_srli_si128(_mm_unpacklo_epi16(a, b), 8);
}

/* cvtpi2ps: two dwords into lanes 0 and 1, lanes 2 and 3 kept. */
static __forceinline __m128 sse_cvtpi2ps(__m128 dst, __m128i src)
{
    return _mm_shuffle_ps(_mm_cvtepi32_ps(src), dst, _MM_SHUFFLE(3, 2, 1, 0));
}

/* cvtps2pi, psrad 15, packssdw zero, packuswb zero, punpcklbw zero,
 * punpcklwd zero: two channel values as the low bytes of two dwords. */
static __forceinline __m128i xmv_channel(__m128 v, __m128i z)
{
    __m128i r = _mm_srai_epi32(_mm_cvtps_epi32(v), 15);
    r = _mm_packs_epi32(_mm_unpacklo_epi64(r, z), z);
    r = _mm_packus_epi16(_mm_unpacklo_epi64(r, z), z);
    r = _mm_unpacklo_epi8(r, z);
    return _mm_unpacklo_epi16(r, z);
}

typedef struct XmvRegs {
    __m128i mm0, mm1, mm2, mm4, mm5, mm7;
    __m128 xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;
} XmvRegs;

/* The routine at 0x002C6C86: four luma bytes in mm0 to four pixels in mm0:mm1. */
static __forceinline void xmv_four_pixels(XmvRegs *r)
{
    __m128i m0, m1;

    m0 = _mm_add_epi16(_mm_unpacklo_epi8(r->mm0, r->mm4), r->mm7);
    m1 = _mm_shufflelo_epi16(m0, 250);
    m0 = _mm_shufflelo_epi16(m0, 80);
    m0 = _mm_madd_epi16(m0, r->mm5);
    m1 = _mm_madd_epi16(m1, r->mm5);
    r->xmm0 = sse_cvtpi2ps(r->xmm0, m0);
    r->xmm3 = sse_cvtpi2ps(r->xmm3, m1);
    r->xmm0 = _mm_shuffle_ps(r->xmm0, r->xmm3, 0x44);

    r->xmm3 = _mm_add_ps(_mm_mul_ps(r->xmm1, r->xmm4), r->xmm0);
    m0 = _mm_slli_epi32(r->mm7, 20);
    r->mm2 = xmv_channel(r->xmm3, r->mm4);
    m0 = _mm_or_si128(m0, r->mm2);
    m1 = _mm_slli_epi32(r->mm7, 20);
    r->xmm3 = _mm_shuffle_ps(r->xmm3, r->xmm3, 0x2E);
    r->mm2 = xmv_channel(r->xmm3, r->mm4);
    m1 = _mm_or_si128(m1, r->mm2);

    r->xmm3 = _mm_add_ps(_mm_mul_ps(r->xmm2, r->xmm7), r->xmm0);
    r->mm2 = _mm_slli_epi64(xmv_channel(r->xmm3, r->mm4), 16);
    m0 = _mm_or_si128(m0, r->mm2);
    r->xmm3 = _mm_shuffle_ps(r->xmm3, r->xmm3, 0x2E);
    r->mm2 = _mm_slli_epi64(xmv_channel(r->xmm3, r->mm4), 16);
    m1 = _mm_or_si128(m1, r->mm2);

    r->xmm3 = _mm_mul_ps(r->xmm2, r->xmm5);
    r->xmm0 = _mm_add_ps(r->xmm0, r->xmm3);
    r->xmm3 = _mm_mul_ps(r->xmm1, r->xmm6);
    r->xmm0 = _mm_add_ps(r->xmm0, r->xmm3);
    r->mm2 = _mm_slli_epi64(xmv_channel(r->xmm0, r->mm4), 8);
    m0 = _mm_or_si128(m0, r->mm2);
    r->xmm0 = _mm_shuffle_ps(r->xmm0, r->xmm0, 0x2E);
    r->mm2 = _mm_slli_epi64(xmv_channel(r->xmm0, r->mm4), 8);
    m1 = _mm_or_si128(m1, r->mm2);

    r->mm0 = m0;
    r->mm1 = m1;
}

/* Two pixel rows from the four luma bytes at y and y + y_stride. */
static __forceinline void xmv_two_rows(XmvRegs *r, char *mem, uint32_t y, uint32_t y_stride,
                                       uint32_t dst, uint32_t dst_stride)
{
    r->mm0 = _mm_cvtsi32_si128(*(const int *)(mem + y));
    xmv_four_pixels(r);
    _mm_storel_epi64((__m128i *)(mem + dst), r->mm0);
    _mm_storel_epi64((__m128i *)(mem + dst + 8), r->mm1);
    r->mm0 = _mm_cvtsi32_si128(*(const int *)(mem + y + y_stride));
    xmv_four_pixels(r);
    _mm_storel_epi64((__m128i *)(mem + dst + dst_stride), r->mm0);
    _mm_storel_epi64((__m128i *)(mem + dst + dst_stride + 8), r->mm1);
}

void sub_002C6B80(void)
{
    char *mem = (char *)g_xbox_mem_offset;
    uint32_t sp = g_esp;
    const uint32_t *arg = (const uint32_t *)(mem + sp + 4);
    uint32_t y = arg[0], u = arg[1], v = arg[2], y_stride = arg[3], uv_stride = arg[4];
    uint32_t dst = arg[5], dst_stride = arg[6];
    __m128i mm3, mm6 = MMX_LOAD(g_mm6);
    XmvRegs r;
    uint32_t n;

    r.mm4 = MMX_LOAD(g_mm4);
    r.mm5 = MMX_LOAD(g_mm5);
    r.mm7 = MMX_LOAD(g_mm7);
    r.mm1 = MMX_LOAD(g_mm1);
    r.mm2 = MMX_LOAD(g_mm2);
    mm3 = MMX_LOAD(g_mm3);
    r.xmm0 = _mm_loadu_ps(g_xmm0);
    r.xmm1 = _mm_loadu_ps(g_xmm1);
    r.xmm2 = _mm_loadu_ps(g_xmm2);
    r.xmm3 = _mm_loadu_ps(g_xmm3);
    r.xmm4 = _mm_loadu_ps(g_xmm4);
    r.xmm5 = _mm_loadu_ps(g_xmm5);
    r.xmm6 = _mm_loadu_ps(g_xmm6);
    r.xmm7 = _mm_loadu_ps(g_xmm7);

    for (n = 16; ; ) {
        __m128i m0;

        m0 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*(const int *)(mem + u)), r.mm4);
        mm3 = m0;
        m0 = _mm_add_epi32(_mm_unpacklo_epi16(m0, r.mm4), mm6);
        r.xmm1 = sse_cvtpi2ps(r.xmm1, m0);
        r.xmm1 = _mm_shuffle_ps(r.xmm1, r.xmm1, 0x50);
        m0 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*(const int *)(mem + v)), r.mm4);
        r.mm1 = m0;
        m0 = _mm_add_epi32(_mm_unpacklo_epi16(m0, r.mm4), mm6);
        r.xmm2 = sse_cvtpi2ps(r.xmm2, m0);
        r.xmm2 = _mm_shuffle_ps(r.xmm2, r.xmm2, 0x50);
        mm3 = mmx_punpckhwd(mm3, r.mm1);
        xmv_two_rows(&r, mem, y, y_stride, dst, dst_stride);

        m0 = _mm_add_epi32(_mm_srli_epi32(_mm_slli_epi32(mm3, 16), 16), mm6);
        r.xmm1 = sse_cvtpi2ps(r.xmm1, m0);
        r.xmm1 = _mm_shuffle_ps(r.xmm1, r.xmm1, 0x50);
        mm3 = _mm_add_epi32(_mm_srli_epi32(mm3, 16), mm6);
        r.xmm2 = sse_cvtpi2ps(r.xmm2, mm3);
        r.xmm2 = _mm_shuffle_ps(r.xmm2, r.xmm2, 0x50);
        xmv_two_rows(&r, mem, y + 4, y_stride, dst + 0x10, dst_stride);

        if (--n == 0)
            break;
        if (n & 1) {
            dst += 0x20;
            y += 8;
            u += 4;
            v += 4;
        } else {
            dst += dst_stride * 2 - 32;
            y += y_stride * 2 - 8;
            u += uv_stride - 4;
            v += uv_stride - 4;
        }
    }

    MMX_SAVE(g_mm0, r.mm0);
    MMX_SAVE(g_mm1, r.mm1);
    MMX_SAVE(g_mm2, r.mm2);
    MMX_SAVE(g_mm3, mm3);
    _mm_storeu_ps(g_xmm0, r.xmm0);
    _mm_storeu_ps(g_xmm1, r.xmm1);
    _mm_storeu_ps(g_xmm2, r.xmm2);
    _mm_storeu_ps(g_xmm3, r.xmm3);
    g_eax = 0;
    g_ecx = u;
    g_edx = v;
    g_ebp = sp - 4;           /* the original publishes its frame and leaves it */
    g_seh_ebp = sp - 4;
    g_esp = sp + 32;          /* ret 28 */
}

/* ── CRT x87 special cases─────────────────────────────────── */

/*
 * Two targets of the table at 0x0040AAB8, which the CRT's x87 maths helpers
 * index by the class of their operands. The disassembler started neighbouring
 * functions two bytes further in, past the first `fstp st(0)`, so these
 * addresses were never translated. A skipped call leaves two extra values on the
 * guest's FPU stack.
 */
extern __declspec(thread) double g_fp_stack[8];
extern __declspec(thread) int g_fp_top;
extern __declspec(thread) uint32_t g_seh_ebp;
extern void sub_002B7B7E(void);

/* fstp st(0), then sub_002B7B7E: fstp st(0); fldz; ret */
static void crt_x87_pop2_zero(void)
{
    g_fp_top = (g_fp_top + 1) & 7;
    sub_002B7B7E();
}

/* fstp st(0); fstp st(0); fld tbyte [0x406938], the x87 indefinite NaN; mark the
 * error class in the caller's frame if none is set; ret */
static void crt_x87_pop2_indefinite(void)
{
    static const uint64_t indefinite = 0xFFF8000000000000ull;
    const char *mem = (const char *)g_xbox_mem_offset;
    int8_t *err = (int8_t *)(mem + g_seh_ebp - 0x90);

    g_fp_top = (g_fp_top + 1) & 7;
    memcpy(&g_fp_stack[g_fp_top], &indefinite, sizeof indefinite);
    if (*err <= 0)
        *err = 1;
    g_esp += 4;
}

/* ── C++ exceptions ────────────────────────────────────────── */

/*
 * The runtime does not dispatch exceptions: RtlRaiseException logs and returns,
 * so code carried on after every `throw`. Unreal's loader throws in normal use,
 * for instance "Can't resolve package name" for an optional package, and running
 * on past that ended in a garbage file name and the "dirty disc" screen.
 *
 * This replaces _CxxThrowException. It walks the guest's fs:[0] chain and reads
 * the MSVC tables, as __CxxFrameHandler would: find a catch for the current state,
 * run destructors in the frames being abandoned, then run the catch funclet and
 * the continuation it returns. Lifted C cannot be resumed in the middle, so each
 * function with a catch has a wrapper below, and a native longjmp lands there.
 * tools/add_eh_entries.py makes the continuations into entry points.
 *
 * Guest frames in the chain that are not C++ (__except) are passed over.
 */
#define EH_MAGIC 0x19930520u
#define EH_MAX_SITES 64

#define M8(a)  (*(uint8_t *)((char *)g_xbox_mem_offset + (uint32_t)(a)))
#define M32(a) (*(uint32_t *)((char *)g_xbox_mem_offset + (uint32_t)(a)))

extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern __declspec(thread) uint32_t g_fs_base;
extern __declspec(thread) uint32_t g_ebp;

typedef struct {
    jmp_buf env;
    uint32_t entry_esp;
    int fp_top;
} EhSite;

static __declspec(thread) EhSite *eh_sites[EH_MAX_SITES];
static __declspec(thread) int eh_depth;
static __declspec(thread) uint32_t eh_obj, eh_info;      /* for a rethrow */
static __declspec(thread) uint32_t eh_ebp, eh_funclet;   /* the landing */

/* Call a funclet on the frame at ebp. It ends in `ret` and leaves esp as it was. */
static int eh_call_funclet(uint32_t va, uint32_t ebp)
{
    recomp_func_t fn = recomp_lookup(va);

    if (!fn) {
        fprintf(stderr, "[EH] no translated funclet at 0x%08X\n", va);
        return 0;
    }
    g_ebp = ebp;
    g_seh_ebp = ebp;
    g_esp -= 4;
    M32(g_esp) = 0xEEEE0000u;
    fn();
    return 1;
}

static uint32_t eh_func_info(uint32_t reg)
{
    uint32_t handler = M32(reg + 4);

    if (handler < 0x11000u || handler >= 0x400000u)
        return 0;
    if (M8(handler) != 0xB8 || M8(handler + 5) != 0xE9)   /* mov eax, fi; jmp */
        return 0;
    return M32(M32(handler + 1)) == EH_MAGIC ? M32(handler + 1) : 0;
}

/* Run the unwind map from `from` down to, but not including, `to`. */
static void eh_unwind(uint32_t fi, uint32_t reg, int from, int to)
{
    uint32_t map = M32(fi + 8);
    int s = from;

    while (s >= 0 && s != to) {
        int next = (int)M32(map + s * 8);
        uint32_t action = M32(map + s * 8 + 4);

        M32(reg + 8) = (uint32_t)next;
        if (action)
            eh_call_funclet(action, reg + 0xC);
        s = next;
    }
}

/* Does this catch accept the thrown type? pType 0 is catch (...). */
static int eh_catch_matches(uint32_t catch_type, uint32_t info)
{
    uint32_t array, n, i;

    if (!catch_type)
        return 1;
    array = M32(info + 0xC);
    n = M32(array);
    for (i = 0; i < n; i++) {
        uint32_t type = M32(M32(array + 4 + i * 4) + 4);

        if (type == catch_type
                || !strcmp((char *)&M8(type + 8), (char *)&M8(catch_type + 8)))
            return 1;
    }
    return 0;
}

/* Find the handler for the current state of a frame; 0 if it has none. */
static uint32_t eh_find_catch(uint32_t fi, uint32_t reg, uint32_t info, uint32_t *try_block)
{
    int state = (int)M32(reg + 8);
    uint32_t n = M32(fi + 0xC), tries = M32(fi + 0x10), t, c;

    for (t = 0; t < n; t++) {
        uint32_t block = tries + t * 20;

        if (state < (int)M32(block) || state > (int)M32(block + 4))
            continue;
        for (c = 0; c < M32(block + 0xC); c++) {
            uint32_t handler = M32(block + 0x10) + c * 16;

            if (eh_catch_matches(M32(handler + 4), info)) {
                *try_block = block;
                return handler;
            }
        }
    }
    return 0;
}

extern void sub_002BA881_gen(void);

void sub_002BA881(void)
{
    static long logged;
    uint32_t obj = M32(g_esp + 4), info = M32(g_esp + 8);
    uint32_t reg, target = 0, handler = 0, block = 0;
    int site = -1;

    if (!info) {                                  /* a rethrow */
        obj = eh_obj;
        info = eh_info;
    }
    eh_obj = obj;
    eh_info = info;

    for (reg = M32(g_fs_base); reg >= 0x10000u && reg < 0x4000000u; reg = M32(reg)) {
        uint32_t fi = eh_func_info(reg);

        if (fi && (handler = eh_find_catch(fi, reg, info, &block)) != 0) {
            target = reg;
            break;
        }
    }
    if (target) {
        for (site = eh_depth - 1; site >= 0; site--) {
            uint32_t entry = eh_sites[site]->entry_esp;

            if (target < entry && target >= entry - 0x20)
                break;
        }
    }
    if (InterlockedIncrement(&logged) <= 32) {
        uint32_t text = obj ? M32(obj) : 0;

        fprintf(stderr, "[EH] throw \"%.120s\": %s frame 0x%08X, funclet 0x%08X\n",
                text >= 0x10000u && text < 0x4000000u ? (char *)&M8(text) : "?",
                site >= 0 ? "caught at" : "NO CATCH SITE,", target,
                handler ? M32(handler + 0xC) : 0);
    }
    if (site < 0) {
        sub_002BA881_gen();                       /* the old fall through */
        return;
    }

    for (reg = M32(g_fs_base); reg != target; reg = M32(reg)) {
        uint32_t fi = eh_func_info(reg);

        if (fi)
            eh_unwind(fi, reg, (int)M32(reg + 8), -1);
    }
    eh_unwind(eh_func_info(target), target, (int)M32(target + 8), (int)M32(block));
    M32(target + 8) = M32(block + 4) + 1;         /* the catch's own state */
    M32(g_fs_base) = target;
    if (M32(handler + 4) && M32(handler + 8))     /* catch by pointer: copy it */
        M32(target + 0xC + (int)M32(handler + 8)) = M32(obj);
    eh_ebp = target + 0xC;
    eh_funclet = M32(handler + 0xC);
    longjmp(eh_sites[site]->env, 1);
}

static void eh_try_site(void (*body)(void))
{
    EhSite site;
    int index = eh_depth;

    if (index >= EH_MAX_SITES) {
        body();
        return;
    }
    site.entry_esp = g_esp;
    site.fp_top = g_fp_top;
    eh_sites[index] = &site;
    eh_depth = index + 1;
    if (!setjmp(site.env)) {
        body();
        eh_depth = index;
        return;
    }
    {
        uint32_t ebp = eh_ebp;
        recomp_func_t resume;

        eh_depth = index + 1;
        g_fp_top = site.fp_top;
        g_esp = M32(ebp - 0x10);                  /* the frame's saved esp */
        if (!eh_call_funclet(eh_funclet, ebp))
            abort();
        eh_depth = index;
        resume = recomp_lookup(g_eax);
        if (!resume) {
            fprintf(stderr, "[EH] no continuation at 0x%08X\n", g_eax);
            abort();
        }
        g_ebp = ebp;
        g_seh_ebp = ebp;
        resume();
    }
}

extern void sub_00049450_gen(void);
void sub_00049450(void) { eh_try_site(sub_00049450_gen); }
extern void sub_000496B0_gen(void);
void sub_000496B0(void) { eh_try_site(sub_000496B0_gen); }
extern void sub_000568F0_gen(void);
void sub_000568F0(void) { eh_try_site(sub_000568F0_gen); }
extern void sub_00058780_gen(void);
void sub_00058780(void) { eh_try_site(sub_00058780_gen); }
extern void sub_00059080_gen(void);
void sub_00059080(void) { eh_try_site(sub_00059080_gen); }
extern void sub_00059470_gen(void);
void sub_00059470(void) { eh_try_site(sub_00059470_gen); }
extern void sub_00059590_gen(void);
void sub_00059590(void) { eh_try_site(sub_00059590_gen); }
extern void sub_00059680_gen(void);
void sub_00059680(void) { eh_try_site(sub_00059680_gen); }
extern void sub_00059D10_gen(void);
void sub_00059D10(void) { eh_try_site(sub_00059D10_gen); }
extern void sub_000C52E0_gen(void);
void sub_000C52E0(void) { eh_try_site(sub_000C52E0_gen); }
extern void sub_0020BBB0_gen(void);
void sub_0020BBB0(void) { eh_try_site(sub_0020BBB0_gen); }

/* ── Native widescreen (#9) ────────────────────────────────── */

/*
 * The settings panel's "16:9 (wider view)" shows the guest's 640x480 picture
 * stretched to 16:9, as "16:9 (stretched)" does, and narrows what the game
 * draws across so the stretch undoes it: the horizontal field of view widens
 * and the vertical one stays (Hor+). gpu_d3d11.c owns the setting.
 */
extern int gpu_wide_view(void);

#define WIDE_SQUEEZE 0.75f   /* 4:3 over 16:9 */

/*
 * The scene's perspective matrix, (out in eax, half FOV across, half FOV up, 1,
 * aspect, near, far), wrapping the generated body. Its only caller is the scene
 * set-up, sub_00133600, which then inverts it, so the game's own frustum, and
 * so its culling, follow the wider view. M[0][0] = 1/tan(half FOV across).
 */
extern void sub_001379C0_gen(void);

void sub_001379C0(void)
{
    const float *arg = (const float *)((char *)g_xbox_mem_offset + g_esp + 4);
    uint32_t out = g_eax;
    static float seen[16];
    static int nseen;
    int i;
    for (i = 0; i < nseen && seen[i] != arg[3]; i++)
        ;
    if (i == nseen && nseen < 16) {
        seen[nseen++] = arg[3];
        fprintf(stderr, "[WIDE] projection: half FOV %.2f x %.2f deg, aspect %.4f, near %g\n",
                arg[0] * 57.29578f, arg[1] * 57.29578f, arg[3], arg[4]);
    }
    sub_001379C0_gen();
    /* Only a 4:3 view: the aspect passed is its width over height times the
     * game's pixel shape, 0x467DE4 (0.909). Square targets (a 16.5-degree view
     * at 0.909) are left as they are. */
    if (gpu_wide_view()) {
        float k = *(const float *)((char *)g_xbox_mem_offset + 0x467DE4u);
        if (k > 0.0f && fabsf(arg[3] / k - 4.0f / 3.0f) < 0.01f)
            *(float *)((char *)g_xbox_mem_offset + out) *= WIDE_SQUEEZE;
    }
}

/*
 * The canvas's per-frame update, (canvas in ecx), wrapping the generated body:
 * it calls the script's Reset, copies the viewport's size into ClipX/ClipY
 * (canvas+0x40/0x44, floats) and SizeX/SizeY (+0x68/0x6C), and builds its
 * canvas utility's (+0xB4) matrices from ClipX: canvas-to-screen at +0x60,
 * screen-to-canvas at +0x20. In the wider view a 640x480 canvas is drawn
 * 480 * 16/9 wide: its matrices map 853 canvas pixels onto the whole picture,
 * so what the script draws keeps its shape. The script is still told 640
 * (ClipX, SizeX): most of the HUD and menus are placed in fixed pixels, and the
 * pop-ups that are placed from ClipX (Begin Match?) then agree with them.
 * sub_00142960 then moves what was drawn.
 */
extern void sub_00099350_gen(void);

#define WIDE_CANVAS_W (480.0f * 16.0f / 9.0f)

void sub_00099350(void)
{
    char *mem = (char *)g_xbox_mem_offset;
    uint32_t canvas = g_ecx, util;
    const int32_t *size;

    sub_00099350_gen();
    size = (const int32_t *)(mem + canvas + 0x68);
    util = *(const uint32_t *)(mem + canvas + 0xB4);
    if (!gpu_wide_view() || !util || size[0] != 640 || size[1] != 480)
        return;
    {
        float *c2s = (float *)(mem + util + 0x60), *s2c = (float *)(mem + util + 0x20);
        c2s[0] = 2.0f / WIDE_CANVAS_W;     /* x_ndc = x * c2s[0] + c2s[12] */
        s2c[0] = 1.0f / c2s[0];
        s2c[12] = -c2s[12] / c2s[0];
    }
}

/*
 * The canvas's flush, (canvas utility on the stack), wrapping the generated
 * body. Its batches are 64-byte entries at util+0xC (count at +0x10): +0 the
 * primitive type, +8 how many, +0xC/+0x10 the vertices and their count. A
 * vertex is x, y, z, colour, u, v as floats, in canvas pixels, four to a quad.
 * On a widened canvas (sub_00099350), vertices on the old right edge (x 639-640)
 * move to the new one: the script draws its full-width bars, fades and tints to
 * 640; in every mode, bars on the edges reach the bottom row. Menus
 * (WIDE_MENU_FLUSH_RET, and the HUD's flush while one is up, for its pop-ups)
 * move to the middle whole, at their shape; their background and bars fill the
 * width. The match HUD (WIDE_HUD_FLUSH_RET,
 * and WIDE_AIM_FLUSH_RET for the first-person crosshair) is spread out instead:
 * quads that touch are one widget (a string, the health bar, the scoreboard),
 * and each widget stays on the left, moves to the middle or to the right edge
 * by which third its middle is in, so the crosshair is centred and the radar
 * sits on the right. Between the top and bottom strips a widget stays with the
 * middle (the combo wheel by the crosshair) unless it lies wholly in an outer
 * third. One wider than 400 pixels (the scoreboard) is centred, and a line of
 * text ending at the right margin (x 582-600) goes to the right edge.
 * RECOMP_WIDE_ZONES=0 leaves the HUD laid out from the left.
 */
extern void sub_00142960_gen(void);

#define WIDE_HUD_FLUSH_RET   0x00135302u /* the flush that draws the match HUD */
#define WIDE_AIM_FLUSH_RET   0x0009A173u /* the first-person crosshair */
#define WIDE_MENU_FLUSH_RET  0x00135340u /* menus, and the title screen */
#define WIDE_MAX_QUADS 4096
#define WIDE_TOUCH 2.0f                  /* pixels apart that still count as one widget */

typedef struct { float *v; float x0, x1, y0, y1; int up; } WideQuad;

static int wide_root(WideQuad *q, int i)
{
    while (q[i].up != i)
        i = q[i].up = q[q[i].up].up;
    return i;
}

/* Move each widget of the n quads to its third of the 16:9 canvas. */
static void wide_spread(WideQuad *q, int n, int apply, uint32_t log_ret)
{
    static float bx0[WIDE_MAX_QUADS], bx1[WIDE_MAX_QUADS], by0[WIDE_MAX_QUADS], by1[WIDE_MAX_QUADS];
    static int cnt[WIDE_MAX_QUADS];
    const float extra = WIDE_CANVAS_W - 640.0f;
    int i, j, k;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (q[i].x0 <= q[j].x1 + WIDE_TOUCH && q[j].x0 <= q[i].x1 + WIDE_TOUCH
                && q[i].y0 <= q[j].y1 + WIDE_TOUCH && q[j].y0 <= q[i].y1 + WIDE_TOUCH) {
                int a = wide_root(q, i), b = wide_root(q, j);
                if (a != b)
                    q[a].up = b;
            }
    for (i = 0; i < n; i++) {
        bx0[i] = by0[i] = 1e9f;
        bx1[i] = by1[i] = -1e9f;
        cnt[i] = 0;
    }
    for (i = 0; i < n; i++) {
        int r = wide_root(q, i);
        if (q[i].x0 < bx0[r]) bx0[r] = q[i].x0;
        if (q[i].x1 > bx1[r]) bx1[r] = q[i].x1;
        if (q[i].y0 < by0[r]) by0[r] = q[i].y0;
        if (q[i].y1 > by1[r]) by1[r] = q[i].y1;
        cnt[r]++;
    }
    for (i = 0; i < n; i++) {
        int r = wide_root(q, i);
        float cx = (bx0[r] + bx1[r]) * 0.5f, dx;
        if (bx1[r] - bx0[r] > 400.0f)
            dx = extra * 0.5f;
        else if (by1[r] - by0[r] <= 40.0f && bx1[r] >= 582.0f && bx1[r] <= 600.0f)
            dx = extra;                 /* a line of text set against the right margin (kill messages) */
        else if (by0[r] >= 100.0f && by1[r] <= 420.0f)
            /* Between the top and bottom strips: around the crosshair, so it
             * moves with the middle, unless it lies wholly in an outer third. */
            dx = bx1[r] <= 640.0f / 3.0f ? 0.0f : bx0[r] >= 1280.0f / 3.0f ? extra : extra * 0.5f;
        else
            dx = cx < 640.0f / 3.0f ? 0.0f : cx > 1280.0f / 3.0f ? extra : extra * 0.5f;
        if (log_ret && r == i)
            fprintf(stderr, "[WIDELOG] %06X widget x %.0f-%.0f y %.0f-%.0f quads %d move %.0f\n",
                    log_ret, bx0[r], bx1[r], by0[r], by1[r], cnt[r], apply ? dx : 0.0f);
        if (apply)
            for (k = 0; k < 4; k++)
                q[i].v[k * 6] += dx;
    }
}

void sub_00142960(void)
{
    static WideQuad quads[WIDE_MAX_QUADS];
    static int zones = -1, logging = -1;
    static uint32_t log_rets[16];
    static ULONGLONG log_at[16];
    char *mem = (char *)g_xbox_mem_offset;
    uint32_t util = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t ret = *(const uint32_t *)(mem + g_esp);
    const float *c2s = (const float *)(mem + util + 0x60);

    if (zones < 0) {
        const char *e = getenv("RECOMP_WIDE_ZONES");
        zones = !(e && *e == '0');
        e = getenv("RECOMP_WIDE_LOG");
        logging = e && *e ? *e - '0' : 0;   /* 1: once a second a site; 2: every flush but the menus' */
    }
    if (gpu_wide_view() && c2s[0] > 0.0f && fabsf(2.0f / c2s[0] - WIDE_CANVAS_W) < 1.0f) {
        int32_t n = *(const int32_t *)(mem + util + 0x10), e;
        uint32_t arr = *(const uint32_t *)(mem + util + 0xC);
        int nq = 0, r;
        static ULONGLONG menu_ms;
        ULONGLONG now_ms = GetTickCount64();
        int menu_up, spread, centre;
        if (ret == WIDE_MENU_FLUSH_RET)
            for (e = 0; e < n; e++)
                if (*(const int32_t *)(mem + arr + e * 64 + 0x10) > 0)
                    menu_ms = now_ms;
        /* While a menu is drawn, the HUD's flush carries its pop-ups (Begin
         * Match?), which belong with the menu. */
        menu_up = now_ms - menu_ms < 150;
        spread = zones && !menu_up && (ret == WIDE_HUD_FLUSH_RET || ret == WIDE_AIM_FLUSH_RET);
        centre = zones && (ret == WIDE_MENU_FLUSH_RET || (menu_up && ret == WIDE_HUD_FLUSH_RET));
        const float half = (WIDE_CANVAS_W - 640.0f) * 0.5f;
        uint32_t log_ret = 0;
        if (logging) {                      /* each flush site at most once a second */
            ULONGLONG now = GetTickCount64();
            for (r = 0; r < 16 && log_rets[r] && log_rets[r] != ret; r++)
                ;
            if (r < 16 && (now - log_at[r] >= 1000 || (logging == 2 && ret != WIDE_MENU_FLUSH_RET))) {
                log_rets[r] = ret;
                log_at[r] = now;
                log_ret = ret;
            }
        }
        for (e = 0; e < n; e++) {
            const uint32_t *en = (const uint32_t *)(mem + arr + e * 64);
            float *v = en[3] ? (float *)(mem + en[3]) : NULL;
            int32_t nv = (int32_t)en[4], i, k;
            if (!v || nq == WIDE_MAX_QUADS)
                continue;
            for (i = 0; i < nv; i++)
                if (v[i * 6] >= 638.5f && v[i * 6] <= 640.5f) {   /* the old right edge */
                    v[i * 6] = WIDE_CANVAS_W;
                    if (log_ret)
                        fprintf(stderr, "[WIDELOG] %06X full width: entry %d type %u vertex %d y %.0f u %.3f v %.3f "
                                "colour %08X material %08X\n", log_ret, (int)e, en[0] & 0xFF, (int)i, v[i * 6 + 1],
                                v[i * 6 + 4], v[i * 6 + 5], *(const uint32_t *)&v[i * 6 + 3], en[1]);
                }
            if (centre) {                   /* a menu: the whole 4:3 layout to the middle */
                int m = nv % 4 == 0 ? 4 : nv;   /* quads, or the batch as one piece */
                for (i = 0; i < nv; i += m) {
                    int full = 0;
                    for (k = 0; k < m; k++)
                        full |= v[(i + k) * 6] > 640.5f || v[(i + k) * 6] < 0.5f;   /* on an edge: stays */
                    if (!full)
                        for (k = 0; k < m; k++)
                            v[(i + k) * 6] += half;
                }
            }
            if (!(spread || log_ret) || nv % 4 != 0)
                continue;
            for (i = 0; i < nv; i += 4) {
                WideQuad *q = &quads[nq];
                float *p = v + i * 6;
                q->v = p;
                q->x0 = q->x1 = p[0];
                q->y0 = q->y1 = p[1];
                for (k = 1; k < 4; k++) {
                    if (p[k * 6] < q->x0) q->x0 = p[k * 6];
                    if (p[k * 6] > q->x1) q->x1 = p[k * 6];
                    if (p[k * 6 + 1] < q->y0) q->y0 = p[k * 6 + 1];
                    if (p[k * 6 + 1] > q->y1) q->y1 = p[k * 6 + 1];
                }
                if (q->x1 > 640.5f)          /* full width: done above */
                    continue;
                q->up = nq;
                if (++nq == WIDE_MAX_QUADS)
                    break;
            }
        }
        if (nq)
            wide_spread(quads, nq, spread, log_ret);
    }
    {   /* In every mode, bars on the screen's edges down to the bottom row: the
         * game stops them at y 479, a row a TV's overscan hid. */
        int32_t n = *(const int32_t *)(mem + util + 0x10), e, i, k;
        uint32_t arr = *(const uint32_t *)(mem + util + 0xC);
        for (e = 0; e < n; e++) {
            const uint32_t *en = (const uint32_t *)(mem + arr + e * 64);
            float *v = en[3] ? (float *)(mem + en[3]) : NULL;
            int32_t nv = (int32_t)en[4];
            if (!v || nv % 4 != 0)
                continue;
            for (i = 0; i < nv; i += 4) {
                int edge = 0;
                for (k = 0; k < 4; k++)
                    edge |= v[(i + k) * 6] <= 1.5f || v[(i + k) * 6] >= 638.5f;
                if (edge)
                    for (k = 0; k < 4; k++)
                        if (v[(i + k) * 6 + 1] >= 476.0f && v[(i + k) * 6 + 1] < 480.0f)
                            v[(i + k) * 6 + 1] = 480.0f;
            }
        }
    }
    sub_00142960_gen();
}

/*
 * Full-screen movies (boot logos, the attract loop, story cutscenes) live in
 * \Movies; those shown inside menus in \Textures\Movies. Both decode through
 * sub_00030570, from the same callers, so the game's CreateFile (sub_00262187,
 * the name first) says which kind was opened last, and the decoder says one
 * is playing. The boot logos decode from their own caller, after the loading
 * movie has already been opened. gpu_d3d11.c shows a full-screen one 4:3.
 */
extern void gpu_movie_opened(int full_screen);
extern void gpu_movie_decoded(int boot);

#define BOOT_MOVIE_DECODE_RET 0x0002EE97u   /* the boot code's player (the logos) */
extern void sub_00262187_gen(void);
extern void sub_00030570_gen(void);

void sub_00262187(void)
{
    const char *mem = (const char *)g_xbox_mem_offset;
    uint32_t name = *(const uint32_t *)(mem + g_esp + 4);
    if (name) {
        char path[160];
        size_t n;
        for (n = 0; n + 1 < sizeof path && mem[name + n]; n++)
            path[n] = (char)tolower((unsigned char)mem[name + n]);
        path[n] = 0;
        if (n > 4 && !strcmp(path + n - 4, ".xmv"))
            gpu_movie_opened(strstr(path, "\\movies\\") && !strstr(path, "\\textures\\"));
    }
    sub_00262187_gen();
}

void sub_00030570(void)
{
    const char *mem = (const char *)g_xbox_mem_offset;
    gpu_movie_decoded(*(const uint32_t *)(mem + g_esp) == BOOT_MOVIE_DECODE_RET);
    sub_00030570_gen();
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x002B7B7Cu) return crt_x87_pop2_zero;
    if (xbox_va == 0x002B7C1Eu) return crt_x87_pop2_indefinite;
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
