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

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

/* ── GPU semaphore ─────────────────────────────────────────── */

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#define D3D_DEVICE_PTR_VA     0x00393888u  /* D3D's global device pointer */
#define D3D_DEV_WRITE_SEQ     0x2Cu        /* device field: next fence to issue */
#define D3D_DEV_SEMAPHORE_PTR 0x30u        /* device field: semaphore address */
#define D3D_FENCE_WAIT_MS     250u         /* longest a fence wait may last */

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
 * wait for the real semaphore to reach it. Bounded, as a lost release must not
 * freeze the title. The original's callback at device+0x19FC is not called.
 */
extern void sub_00386840(void);
extern __declspec(thread) uint32_t g_esp;

void sub_00386B30(void)
{
    const char *mem = (const char *)g_xbox_mem_offset;
    uint32_t fence = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t dev = *(const uint32_t *)(mem + D3D_DEVICE_PTR_VA);
    const volatile uint32_t *seq =
        (const volatile uint32_t *)(mem + dev + D3D_DEV_WRITE_SEQ);
    const volatile uint32_t *sem = (const volatile uint32_t *)
        (mem + *(const uint32_t *)(mem + dev + D3D_DEV_SEMAPHORE_PTR));
    ULONGLONG start = 0;

    if (fence == *seq) {
        g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0;
        g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0x00386B5Fu;
        sub_00386840();
    }
    while (*seq - fence < *seq - *sem) {
        if (!start) {
            start = GetTickCount64();
        } else if (GetTickCount64() - start >= D3D_FENCE_WAIT_MS) {
            static unsigned timeouts;
            if (timeouts++ < 8)
                fprintf(stderr, "[D3D] fence 0x%X not reached in %u ms (semaphore 0x%X)\n",
                        fence, D3D_FENCE_WAIT_MS, *sem);
            break;
        }
        SwitchToThread();
    }
    g_esp += 12; /* ret 8 */
}

/* ── CRT x87 special cases ─────────────────────────────────── */

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
