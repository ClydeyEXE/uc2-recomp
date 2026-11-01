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
    int kicked = 0;

    if (fence == *seq) {
        g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0;
        g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0x00386B5Fu;
        sub_00386840();
    }
    while (*seq - fence < *seq - *sem) {
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
    g_esp += 12; /* ret 8 */
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
 * the generated body. It reads a report the GPU writes, and the executor does not
 * write them, so a caller looping until the test completes never gets out. That
 * froze map loading. The executor counts no pixels, so report the test as
 * finished with a full 640x480 frame visible: occlusion culling then only draws
 * more than it needs to.
 */
#define D3D_VISIBILITY_ALL_PIXELS (640u * 480u)

void sub_002043A0(void)
{
    const char *mem = (const char *)g_xbox_mem_offset;
    uint32_t result = *(const uint32_t *)(mem + g_esp + 8);

    if (result)
        *(uint32_t *)(mem + result) = D3D_VISIBILITY_ALL_PIXELS;
    g_eax = 1;
    g_esp += 12; /* ret 8 */
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
