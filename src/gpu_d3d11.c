/**
 * Direct3D 11 back end for the pushbuffer executor (nv2a_backend.h).
 *
 * The executor still reads the pushbuffer, tracks state and runs vertex programs
 * on the CPU. What it used to rasterise on the CPU comes here instead: triangles
 * in surface pixels, with every vertex output and the combiner state, drawn into
 * GPU render targets keyed by guest address.
 *
 * The pixel shader is an interpreter of the register combiners, following
 * nv2a_combiner.c: one shader, with the combiner words in a per-draw constant
 * buffer. A frame here is about a hundred draws, so that costs nothing worth
 * generating shaders to avoid.
 *
 * Everything runs on the NV2A poll thread, so the immediate context needs no lock.
 *
 * RECOMP_GPU=sw keeps the software rasteriser. RECOMP_GPU_STATS=1 logs a line every
 * few seconds.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <xinput.h>
#include <intrin.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/nv2a_backend.h"
#include "d3d/d3d8_swizzle.h"
#include "version.h"            /* written by tools/version.cmake as the game builds */

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ---- Shaders ------------------------------------------------------------ */

static const char s_hlsl[] =
"cbuffer C : register(b0) {\n"
"  float4 rt;\n"             /* surface width, height, zmax, constant depth bias (0..1) */
"  uint4  col[8];\n"         /* colour icw, colour ocw, alpha icw, alpha ocw */
"  float4 f0[8];\n"
"  float4 f1[8];\n"
"  uint4  fin;\n"            /* final0, final1, control, stage_program */
"  float4 fc0, fc1;\n"
"  float4 fogc;\n"
"  uint4  misc;\n"           /* alpha test, alpha func, alpha ref, clip plane mode */
"  float4 ts[4];\n"          /* texture coordinate scale: 1/texels for a linear texture */
"  uint4  cube;\n"
"  uint4  flags;\n"          /* x: combiners programmed, y: stage 0 usable, z: dot mapping, w: stage inputs */
"};\n"
"Texture2D t0_ : register(t0); Texture2D t1_ : register(t1);\n"
"Texture2D t2_ : register(t2); Texture2D t3_ : register(t3);\n"
"TextureCube c0_ : register(t4); TextureCube c1_ : register(t5);\n"
"TextureCube c2_ : register(t6); TextureCube c3_ : register(t7);\n"
"SamplerState s0_ : register(s0); SamplerState s1_ : register(s1);\n"
"SamplerState s2_ : register(s2); SamplerState s3_ : register(s3);\n"
"struct VI { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float4 fog : FOG;\n"
"            float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
"struct VO { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n"
"            float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
"VO vs(VI i) {\n"
"  VO o; float w = i.pos.w;\n"
"  o.p = float4((i.pos.x / rt.x * 2 - 1) * w, (1 - i.pos.y / rt.y * 2) * w,\n"
"               (i.pos.z / rt.z + rt.w) * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog.x;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  return o;\n"
"}\n"
"float4 fetch(uint st, Texture2D tx, TextureCube cx, SamplerState s, float4 c) {\n"
"  uint mode = (fin.w >> (st * 5)) & 31;\n"
"  float q = c.w != 0 ? c.w : 1;\n"
"  float2 uv = c.xy / q * ts[st].xy, dx = ddx(uv), dy = ddy(uv);\n"
"  float3 dxd = ddx(c.xyz), dyd = ddy(c.xyz);\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode == 1 || mode == 2) {\n"
"    float4 v = tx.SampleGrad(s, uv, dx, dy);\n"
"    return ts[st].z != 0 ? float4(v.bbb, 1) : v;\n"   /* an 8-bit target: see d3d11_draw */
"  }\n"
"  if (mode == 3) {\n"
"    if (cube[st]) return cx.SampleGrad(s, c.xyz, dxd, dyd);\n"
"    return tx.SampleGrad(s, c.xy * ts[st].xy, ddx(c.xy * ts[st].xy), ddy(c.xy * ts[st].xy));\n"
"  }\n"
"  if (mode == 4) return saturate(c);\n"
"  if (mode == 5) {\n"
"    for (uint j = 0; j < 4; j++) {\n"
"      uint ge = (misc.w >> (st * 4 + j)) & 1;\n"
"      if (ge ? c[j] >= 0 : c[j] < 0) discard;\n"
"    }\n"
"    return float4(0, 0, 0, 0);\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
"static float4 R[16];\n"
"float mi(float x, uint b) {\n"
"  float p = max(x, 0); uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p;\n"
"  if (m == 1) return 1 - saturate(x);\n"
"  if (m == 2) return 2 * p - 1;\n"
"  if (m == 3) return -2 * p + 1;\n"
"  if (m == 4) return p - 0.5;\n"
"  if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x;\n"
"  return -x;\n"
"}\n"
"float3 mi3(float3 x, uint b) {\n"
"  float3 p = max(x, 0); uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p;\n"
"  if (m == 1) return 1 - saturate(x);\n"
"  if (m == 2) return 2 * p - 1;\n"
"  if (m == 3) return -2 * p + 1;\n"
"  if (m == 4) return p - 0.5;\n"
"  if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x;\n"
"  return -x;\n"
"}\n"
"float mo(float x, uint m) {\n"
"  if (m == 0x08) return x - 0.5;\n"
"  if (m == 0x10) return x * 2;\n"
"  if (m == 0x18) return (x - 0.5) * 2;\n"
"  if (m == 0x20) return x * 4;\n"
"  if (m == 0x30) return x * 0.5;\n"
"  return x;\n"
"}\n"
"float3 mo3(float3 x, uint m) {\n"
"  if (m == 0x08) return x - 0.5;\n"
"  if (m == 0x10) return x * 2;\n"
"  if (m == 0x18) return (x - 0.5) * 2;\n"
"  if (m == 0x20) return x * 4;\n"
"  if (m == 0x30) return x * 0.5;\n"
"  return x;\n"
"}\n"
"float3 rgb(uint b) { b &= 0xFF; float4 r = R[b & 15]; return mi3((b & 0x10) ? r.aaa : r.rgb, b); }\n"
"float alp(uint b) { b &= 0xFF; float4 r = R[b & 15]; return mi((b & 0x10) ? r.a : r.b, b); }\n"
"float4 combine(float4 v0, float4 v1, float fog, float4 t0, float4 t1, float4 t2, float4 t3) {\n"
"  uint n = min(fin.z & 0xFF, 8), fl = fin.z >> 8, s, i;\n"
"  for (i = 0; i < 16; i++) R[i] = float4(0, 0, 0, 0);\n"
"  R[3] = float4(fogc.rgb, fog); R[4] = v0; R[5] = v1;\n"
"  R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
"  R[12].a = (fin.w & 31) ? t0.a : 1;\n"
"  for (s = 0; s < n; s++) {\n"
"    uint icw = col[s].x, ocw = col[s].y, aicw = col[s].z, aocw = col[s].w;\n"
"    uint f = ocw >> 12, af = aocw >> 12, mux;\n"
"    float3 A, B, C, D, pab, pcd, sum, ab, cd, ms;\n"
"    float aA, aB, aC, aD, ap, aq, as_;\n"
"    R[1] = f0[(fl & 0x010) ? s : 0];\n"
"    R[2] = f1[(fl & 0x100) ? s : 0];\n"
"    mux = (fl & 1) ? (R[12].a >= 0.5 ? 1 : 0) : ((uint)(R[12].a * 255) & 1);\n"
"    A = rgb(icw >> 24); B = rgb(icw >> 16); C = rgb(icw >> 8); D = rgb(icw);\n"
"    aA = alp(aicw >> 24); aB = alp(aicw >> 16); aC = alp(aicw >> 8); aD = alp(aicw);\n"
"    pab = (f & 2) ? dot(A, B).xxx : A * B;\n"
"    pcd = (f & 1) ? dot(C, D).xxx : C * D;\n"
"    sum = (f & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    ab = clamp(mo3(pab, f & 0x38), -1, 1);\n"
"    cd = clamp(mo3(pcd, f & 0x38), -1, 1);\n"
"    ms = clamp(mo3(sum, f & 0x38), -1, 1);\n"
"    ap = aA * aB; aq = aC * aD;\n"
"    as_ = (af & 4) ? (mux ? aq : ap) : ap + aq;\n"
"    ap = clamp(mo(ap, af & 0x38), -1, 1);\n"
"    aq = clamp(mo(aq, af & 0x38), -1, 1);\n"
"    as_ = clamp(mo(as_, af & 0x38), -1, 1);\n"
"    { uint d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (f & 0x80) R[d].a = ab.b; } }\n"
"    { uint d = ocw & 15;        if (d) { R[d].rgb = cd; if (f & 0x40) R[d].a = cd.b; } }\n"
"    { uint d = (ocw >> 8) & 15; if (d) R[d].rgb = ms; }\n"
"    { uint d = (aocw >> 4) & 15; if (d) R[d].a = ap; }\n"
"    { uint d = aocw & 15;        if (d) R[d].a = aq; }\n"
"    { uint d = (aocw >> 8) & 15; if (d) R[d].a = as_; }\n"
"    R[0] = float4(0, 0, 0, 0);\n"
"  }\n"
"  if (fin.x || fin.y) {\n"
"    uint f0w = fin.x, f1w = fin.y, ff = f1w & 0xFF;\n"
"    float3 A, B, C, D, E, F;\n"
"    R[1] = fc0; R[2] = fc1;\n"
"    {\n"
"      float3 a = (ff & 0x40) ? 1 - R[5].rgb : R[5].rgb;\n"
"      float3 b = (ff & 0x20) ? 1 - R[12].rgb : R[12].rgb;\n"
"      R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0);\n"
"    }\n"
"    E = rgb(f1w >> 24); F = rgb(f1w >> 16);\n"
"    R[15] = float4(E * F, 0);\n"
"    A = rgb(f0w >> 24); B = rgb(f0w >> 16); C = rgb(f0w >> 8); D = rgb(f0w);\n"
"    return saturate(float4(D + C * (1 - A) + B * A, alp(f1w >> 8)));\n"
"  }\n"
"  return saturate(R[12]);\n"
"}\n"
/* DOT_PRODUCT in one stage, then DOT_ST in the next: each dots its coordinate with a
 * colour from an earlier stage, and the two results address the second stage's texture.
 * UC2's heat haze and its colour grading both read the scene this way. */
"float3 dotmap(float3 x, uint m) { return m >= 1 && m <= 3 ? x * 2 - 1 : x; }\n"
"float dot_stage(uint st, float4 c, float4 t0, float4 t1, float4 t2) {\n"
"  uint from = (flags.w >> (16 + (st - 1) * 4)) & 15;\n"
"  float3 v = from == 2 ? t2.rgb : from == 1 ? t1.rgb : t0.rgb;\n"
"  return dot(c.xyz, dotmap(v, (flags.z >> ((st - 1) * 4)) & 15));\n"
"}\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 t0 = fetch(0, t0_, c0_, s0_, i.t0), t1 = fetch(1, t1_, c1_, s1_, i.t1);\n"
"  float4 t2 = fetch(2, t2_, c2_, s2_, i.t2), t3 = fetch(3, t3_, c3_, s3_, i.t3);\n"
"  float4 c;\n"
"  if (((fin.w >> 5) & 31) == 0x11 && ((fin.w >> 10) & 31) == 9) {\n"
"    float2 uv = float2(dot_stage(1, i.t1, t0, t1, t2), dot_stage(2, i.t2, t0, t1, t2)) * ts[2].xy;\n"
"    t2 = t2_.SampleGrad(s2_, uv, ddx(uv), ddy(uv));\n"
"  }\n"
"  if (flags.x) {\n"
"    c = combine(i.d0, i.d1, i.fog, t0, t1, t2, t3);\n"
"  } else {\n"
"    c = i.d0;\n"
"    if (flags.y) c *= t0_.Sample(s0_, i.t0.xy / (i.t0.w != 0 ? i.t0.w : 1) * ts[0].xy);\n"
"  }\n"
"  if (misc.x) {\n"
"    int v = (int)(saturate(c.a) * 255 + 0.5), r = (int)(misc.z & 0xFF); bool ok = true;\n"
"    switch (misc.y) {\n"
"    case 0x200: ok = false; break;\n"
"    case 0x201: ok = v < r; break;\n"
"    case 0x202: ok = v == r; break;\n"
"    case 0x203: ok = v <= r; break;\n"
"    case 0x204: ok = v > r; break;\n"
"    case 0x205: ok = v != r; break;\n"
"    case 0x206: ok = v >= r; break;\n"
"    default: break;\n"
"    }\n"
"    if (!ok) discard;\n"
"  }\n"
"  return c;\n"
"}\n"
/* Partial clears: a quad over the viewport at the clear depth, with the scissor
 * doing the rest. */
"cbuffer K : register(b1) { float4 kcolor; float4 kdepth; };\n"
"float4 vs_clear(uint i : SV_VertexID) : SV_Position {\n"
"  float2 q = float2((i << 1) & 2, i & 2);\n"
"  return float4(q * float2(2, -2) + float2(-1, 1), kdepth.x, 1);\n"
"}\n"
"float4 ps_clear() : SV_Target { return kcolor; }\n"
/* A depth buffer as the console holds it in memory, and back: Z24S8 is depth in
 * the top 24 bits, so read as A8R8G8B8 it is A R G high to low and B the stencil.
 * kcolor.xy scales a destination pixel to a source one. See zeta_encode(). */
"Texture2D<float> zsrc : register(t8);\n"
"Texture2D<float4> csrc : register(t9);\n"
"float4 ps_zenc(float4 p : SV_Position) : SV_Target {\n"
"  uint d = (uint)(saturate(zsrc.Load(int3(p.xy * kcolor.xy, 0))) * 16777215.0 + 0.5);\n"
"  return float4((d >> 8) & 255, d & 255, 0, d >> 16) / 255.0;\n"
"}\n"
"float ps_zdec(float4 p : SV_Position) : SV_Depth {\n"
"  float4 c = round(csrc.Load(int3(p.xy * kcolor.xy, 0)) * 255.0);\n"
"  return (c.a * 65536.0 + c.r * 256.0 + c.g) / 16777215.0;\n"
"}\n";

typedef struct {
    float    rt[4];
    uint32_t col[8][4];
    float    f0[8][4], f1[8][4];
    uint32_t fin[4];
    float    fc0[4], fc1[4], fogc[4];
    uint32_t misc[4];
    float    ts[4][4];
    uint32_t cube[4];
    uint32_t flags[4];
} DrawConstants;

/* ---- Device state -------------------------------------------------------- */

static ID3D11Device        *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader  *s_vs, *s_vs_clear;
static ID3D11PixelShader   *s_ps, *s_ps_clear, *s_ps_zenc, *s_ps_zdec;
static ID3D11InputLayout   *s_layout;
static ID3D11Buffer        *s_vb, *s_cb, *s_kb;
static ID3D11ShaderResourceView *s_white, *s_white_cube;
static uint32_t             s_vb_used;
static uint32_t             s_flip;
static int                  s_failed;

#define VB_BYTES (96u * 1024u * 1024u)

static struct {
    uint32_t draws, tris, clears, uploads, hashes, feedback, presents, zeta_copies;
    uint64_t upload_bytes;
    DWORD    last;
} s_stats;
static struct { uint32_t reports, zero, max_pending; } s_vis_stats;   /* visibility tests */
static ID3D11Query *s_vis_open;     /* counting a visibility test now (defined below) */
static UINT64 s_passed;             /* -DrawLog: what the last draw passed */

/* ---- Frame times ---------------------------------------------------------- */
/* One [FRAMES] line every 10 s: the spread of present-to-present times, and how
 * the walker thread (the one that calls present) spent them: D3D11 draw and clear
 * calls, d3d11_present (the blit and Present, or the readback), and its other CPU time (method decode,
 * vertex programs). What is left is the walker waiting for the game. A frame over
 * 100 ms gets a [HITCH] line of its own, naming where the walker was: a thread
 * samples its instruction pointer every few ms once a frame passes 50 ms. */
#define FT_MAX 8192

static struct {
    double   qms;                    /* QPC ticks per ms */
    double   tsc_ms;                 /* cycle counts per ms, measured against QPC */
    uint64_t tsc0;
    LONGLONG q0, last, report;
    ULONG64  cycles;                 /* the walker's cycle count at the last present */
    double   d3d, present, limit;   /* this frame, ms; limit is the frame limit's wait, in present */
    double   sum, d3d_sum, pr_sum, cpu_sum, lim_sum;
    uint32_t n, over25, over50;
    float    ms[FT_MAX];
} s_ft;

static LONGLONG ft_qpc(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

/* The hitch sampler: where the walker and the title's main thread are while a
 * frame runs long. Averages cannot show what one 150 ms frame in a minute was
 * doing; this can. The main thread's stack is unwound a few frames, as its leaf
 * is often a helper (a copy, a kernel call) and the caller says what for. */
#define HS_MAX 160
#define GS_DEPTH 10

extern void *xbox_guest_main_thread;           /* kernel_thread.c */

static HANDLE        s_walker;
static DWORD64       s_hs_rip[HS_MAX];
static volatile LONG s_hs_n;
static DWORD64       s_gs_pc[HS_MAX][GS_DEPTH];
static int           s_gs_depth[HS_MAX];
static volatile LONG s_gs_n;

/* The suspended thread's return addresses, innermost first. */
static int unwind(CONTEXT *c, DWORD64 *pc, int max)
{
    int n = 0;
    __try {
        while (n < max && c->Rip) {
            DWORD64 base = 0, frame = 0;
            PVOID data = NULL;
            PRUNTIME_FUNCTION fe;
            pc[n++] = c->Rip;
            fe = RtlLookupFunctionEntry(c->Rip, &base, NULL);
            if (fe) {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, fe, c, &data, &frame, NULL);
            } else {
                c->Rip = *(DWORD64 *)c->Rsp;        /* a leaf: the return address is on top */
                c->Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

static DWORD WINAPI hitch_sampler(LPVOID unused)
{
    (void)unused;
    for (;;) {
        CONTEXT c;
        HANDLE game = (HANDLE)xbox_guest_main_thread;
        Sleep(3);
        if ((double)(ft_qpc() - s_ft.last) / s_ft.qms < 50.0)
            continue;
        if (s_hs_n < HS_MAX && SuspendThread(s_walker) != (DWORD)-1) {
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(s_walker, &c) && s_hs_n < HS_MAX)
                s_hs_rip[s_hs_n++] = c.Rip;
            ResumeThread(s_walker);
        }
        if (game && s_gs_n < HS_MAX && SuspendThread(game) != (DWORD)-1) {
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(game, &c) && s_gs_n < HS_MAX) {
                LONG i = s_gs_n;
                s_gs_depth[i] = unwind(&c, s_gs_pc[i], GS_DEPTH);
                s_gs_n = i + 1;
            }
            ResumeThread(game);
        }
    }
    return 0;
}

static void sym_name(DWORD64 pc, char *name, size_t size)
{
    char buf[sizeof(SYMBOL_INFO) + 64];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 63;
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, sym))
        _snprintf(name, size, "%s", sym->Name);
    else
        _snprintf(name, size, "0x%llX", (unsigned long long)pc);
    name[size - 1] = 0;
}

/* Count names, then print the most common: a tally kept across calls until
 * tally_print. A name counted again for the same sample (same tag) is skipped,
 * so a stack's counts are inclusive. */
#define TALLY_MAX (HS_MAX * GS_DEPTH)
static struct { char name[48]; int count, tag; } s_tally[TALLY_MAX];
static int s_tally_n;

static void tally(DWORD64 pc, int tag)
{
    char name[48];
    int k;
    sym_name(pc, name, sizeof name);
    for (k = 0; k < s_tally_n && strcmp(s_tally[k].name, name); k++)
        ;
    if (k == s_tally_n) {
        if (s_tally_n == TALLY_MAX)
            return;
        memcpy(s_tally[k].name, name, sizeof name);
        s_tally[k].count = 0;
        s_tally[k].tag = -1;
        s_tally_n++;
    }
    if (s_tally[k].tag != tag) {
        s_tally[k].count++;
        s_tally[k].tag = tag;
    }
}

static void tally_print(const char *what, int n, int max)
{
    int shown;
    fprintf(stderr, "[HITCH]   %s (%d samples from 50 ms):", what, n);
    for (shown = 0; shown < max && shown < s_tally_n; shown++) {
        int best = 0, k;
        for (k = 1; k < s_tally_n; k++)
            if (s_tally[k].count > s_tally[best].count)
                best = k;
        if (s_tally[best].count <= 0)
            break;
        fprintf(stderr, " %s %d", s_tally[best].name, s_tally[best].count);
        s_tally[best].count = -1;
    }
    fprintf(stderr, "\n");
    s_tally_n = 0;
}

/* The sampled functions of the frame just ended, most often first, appended
 * to its [HITCH] line: the walker's, the main thread's innermost, and every
 * function on the main thread's sampled stacks. dbghelp is initialised in main.c. */
static void hitch_where(void)
{
    int n = s_hs_n, g = s_gs_n, i, d;

    for (i = 0; i < n; i++)
        tally(s_hs_rip[i], i);
    tally_print("walker", n, 8);
    if (!g)
        return;
    for (i = 0; i < g; i++)
        if (s_gs_depth[i])
            tally(s_gs_pc[i][0], i);
    tally_print("game thread", g, 8);
    for (i = 0; i < g; i++)
        for (d = 0; d < s_gs_depth[i]; d++)
            tally(s_gs_pc[i][d], i);
    tally_print("game thread, inclusive", g, 14);
}

static int ft_cmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* At each present, before the readback: close the frame that just ended. */
static void ft_frame(void)
{
    LONGLONG now = ft_qpc();
    ULONG64 cyc = 0;
    double dt, cpu = 0.0;

    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    if (!s_ft.qms) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_ft.qms = (double)f.QuadPart / 1000.0;
        s_ft.q0 = s_ft.last = s_ft.report = now;
        s_ft.tsc0 = __rdtsc();
        s_ft.cycles = cyc;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_walker,
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                            FALSE, 0))
            CloseHandle(CreateThread(NULL, 0, hitch_sampler, NULL, 0, NULL));
        return;
    }
    if (!s_ft.tsc_ms && now - s_ft.q0 > (LONGLONG)(s_ft.qms * 1000.0))
        s_ft.tsc_ms = (double)(__rdtsc() - s_ft.tsc0) / ((double)(now - s_ft.q0) / s_ft.qms);
    dt = (double)(now - s_ft.last) / s_ft.qms;
    if (s_ft.tsc_ms)
        cpu = (double)(cyc - s_ft.cycles) / s_ft.tsc_ms;
    if (dt > 100.0) {
        fprintf(stderr, "[HITCH] %.0f ms frame at %.1f s: walker cpu %.0f ms (d3d11 %.0f, present %.0f)\n",
                dt, (double)(now - s_ft.q0) / s_ft.qms / 1000.0, cpu, s_ft.d3d, s_ft.present);
        hitch_where();
    }
    if (s_ft.n < FT_MAX)
        s_ft.ms[s_ft.n++] = (float)dt;
    s_ft.over25 += dt > 25.0;
    s_ft.over50 += dt > 50.0;
    s_ft.sum += dt;
    s_ft.d3d_sum += s_ft.d3d;
    s_ft.pr_sum += s_ft.present - s_ft.limit;
    s_ft.lim_sum += s_ft.limit;
    s_ft.cpu_sum += cpu;
    s_ft.d3d = s_ft.present = s_ft.limit = 0.0;
    s_ft.last = now;
    s_ft.cycles = cyc;
    InterlockedExchange(&s_hs_n, 0);
    InterlockedExchange(&s_gs_n, 0);

    if (now - s_ft.report > (LONGLONG)(s_ft.qms * 10000.0) && s_ft.n) {
        uint32_t n = s_ft.n;
        double busy = s_ft.cpu_sum - s_ft.d3d_sum - s_ft.pr_sum;
        qsort(s_ft.ms, n, sizeof s_ft.ms[0], ft_cmp);
        fprintf(stderr, "[FRAMES] %u in %.1f s (%.1f fps): ms p50 %.1f p90 %.1f p99 %.1f max %.1f, >25 ms %u, >50 ms %u"
                        " | per frame: d3d11 %.2f, present %.2f, other walker cpu %.2f, walker idle %.2f (limit %.2f)\n",
                n, s_ft.sum / 1000.0, n * 1000.0 / s_ft.sum, s_ft.ms[n / 2], s_ft.ms[n * 9 / 10],
                s_ft.ms[n * 99 / 100], s_ft.ms[n - 1], s_ft.over25, s_ft.over50,
                s_ft.d3d_sum / n, s_ft.pr_sum / n, busy / n, (s_ft.sum - s_ft.cpu_sum) / n, s_ft.lim_sum / n);
        s_ft.n = s_ft.over25 = s_ft.over50 = 0;
        s_ft.sum = s_ft.d3d_sum = s_ft.pr_sum = s_ft.cpu_sum = s_ft.lim_sum = 0.0;
        s_ft.report = now;
    }
}

/* ---- Render targets ------------------------------------------------------ */

#define MAX_TARGETS 48

/* w and h are the surface's size in guest pixels, which the vertex positions,
 * clip rectangles and texture coordinates are in. The texture is pw x ph, sx and
 * sy times that (the internal resolution, s_sx and s_sy, which differ when the
 * picture is stretched to 16:9), and only the viewport, scissor and depth bias
 * need to know. res is the s_res it was made for. */
typedef struct {
    uint32_t va, w, h, bpp, last_flip, pw, ph, res;
    float sx, sy;
    ID3D11Texture2D *tex, *copy;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv, *copy_srv;
    uint32_t seq;                   /* s_seq at its last write */
    int from_zeta;                  /* made by zeta_encode, not drawn by the title */
} Target;

/* w and h are host pixels; gw and gh guest ones, and zmax that of the surface
 * that last used it. */
typedef struct {
    uint32_t va, w, h, last_flip;
    uint32_t seq, gw, gh;
    float zmax;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;
} Zeta;

/* The order of writes to targets and depth buffers: which is newer at an address. */
static uint32_t s_seq;

static Target s_targets[MAX_TARGETS];
static Zeta   s_zetas[MAX_TARGETS];
static ID3D11Texture2D *s_staging;
static uint32_t s_staging_w, s_staging_h;
/* Internal resolution, as a scale on each axis from the guest's 640x480 (see the
 * settings panel), and a count that changes with it, so targets remake themselves. */
static float s_sx = 1.0f, s_sy = 1.0f;
static uint32_t s_res;
/* The last target and depth buffer bound, so a draw into the same pair does not
 * rebind, and a texture stage knows when it would sample what it is drawing to. */
static Target  *s_cur_target;
static Zeta    *s_cur_zeta;

static void target_release(Target *t)
{
    if (t->copy_srv) ID3D11ShaderResourceView_Release(t->copy_srv);
    if (t->copy) ID3D11Texture2D_Release(t->copy);
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    memset(t, 0, sizeof *t);
}

/* The surface's size in memory, which the clip rectangle may only part of. */
static void surface_size(const Nv2aSurface *s, uint32_t *w, uint32_t *h)
{
    if (((s->format >> 8) & 0xF) == 2) {          /* swizzled: log2 sizes */
        *w = 1u << ((s->format >> 16) & 0xF);
        *h = 1u << ((s->format >> 24) & 0xF);
        return;
    }
    *w = s->bytes_per_pixel ? s->pitch / s->bytes_per_pixel : s->width;
    if (*w < s->clip_x + s->width)
        *w = s->clip_x + s->width;
    *h = s->clip_y + s->height;
}

static Target *target_for(const Nv2aSurface *s)
{
    uint32_t w, h;
    int i, oldest = 0;
    Target *t = NULL;
    D3D11_TEXTURE2D_DESC td;
    int kept = 0;
    uint32_t pw, ph;

    surface_size(s, &w, &h);
    pw = (uint32_t)((float)w * s_sx + 0.5f);
    ph = (uint32_t)((float)h * s_sy + 0.5f);
    if (!w || !h || pw > 8192 || ph > 8192)
        return NULL;
    for (i = 0; i < MAX_TARGETS; i++) {
        if (s_targets[i].va == s->color_va) {
            t = &s_targets[i];
            if (t->w == w && t->h >= h && t->res == s_res)
                return t;
            /* Grown: a title narrows the clip for some draws, so the first one
             * seen may have been small. What was drawn is kept. A new scale
             * starts afresh. */
            kept = t->h < h && t->w == w && t->res == s_res;
            break;
        }
        if (s_targets[i].last_flip < s_targets[oldest].last_flip || !s_targets[i].va)
            if (s_targets[oldest].va)
                oldest = i;
    }
    ZeroMemory(&td, sizeof td);
    td.Width = pw; td.Height = ph; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    {
        ID3D11Texture2D *tex = NULL;
        ID3D11RenderTargetView *rtv = NULL;
        ID3D11ShaderResourceView *srv = NULL;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &tex))
            || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)tex, NULL, &rtv))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &srv))) {
            fprintf(stderr, "[D3D11] cannot create a %ux%u target\n", pw, ph);
            return NULL;
        }
        if (kept && t) {
            D3D11_BOX box = {0, 0, 0, t->pw, t->ph, 1};
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)tex, 0, 0, 0, 0,
                                                      (ID3D11Resource *)t->tex, 0, &box);
        }
        if (!t)
            t = &s_targets[oldest];
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        s_cur_target = NULL;            /* unbound: the next draw binds again */
        target_release(t);
        t->va = s->color_va; t->w = w; t->h = h; t->bpp = s->bytes_per_pixel;
        t->pw = pw; t->ph = ph; t->res = s_res; t->sx = (float)pw / (float)w; t->sy = (float)ph / (float)h;
        t->tex = tex; t->rtv = rtv; t->srv = srv;
    }
    return t;
}

static Zeta *zeta_for(uint32_t va, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    int i, oldest = 0;
    Zeta *z = NULL;

    for (i = 0; i < MAX_TARGETS; i++) {
        if (s_zetas[i].va == va && s_zetas[i].w == w && s_zetas[i].h == h)
            return &s_zetas[i];
        if (s_zetas[i].va == va) {
            static int logged;
            if (logged++ < 16)
                fprintf(stderr, "[D3D11] depth %08X wanted at %ux%u, held at %ux%u (flip %u)\n",
                        va, w, h, s_zetas[i].w, s_zetas[i].h, s_flip);
        }
        if (s_zetas[i].last_flip < s_zetas[oldest].last_flip || !s_zetas[i].va)
            if (s_zetas[oldest].va)
                oldest = i;
    }
    z = &s_zetas[oldest];
    if (z->dsv) {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        s_cur_target = NULL;
        s_cur_zeta = NULL;
        ID3D11DepthStencilView_Release(z->dsv);
    }
    if (z->srv) ID3D11ShaderResourceView_Release(z->srv);
    if (z->tex) ID3D11Texture2D_Release(z->tex);
    memset(z, 0, sizeof *z);
    ZeroMemory(&td, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;  /* also read as a texture */
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        ZeroMemory(&dd, sizeof dd);
        dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ZeroMemory(&sd, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &z->tex))
            || FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)z->tex, &dd, &z->dsv))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)z->tex, &sd, &z->srv))) {
            fprintf(stderr, "[D3D11] cannot create a %ux%u depth buffer\n", w, h);
            return NULL;
        }
    }
    z->va = va; z->w = w; z->h = h;
    /* A new buffer reads as cleared to the far plane, not as garbage. */
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return z;
}

/* ---- Textures ------------------------------------------------------------ */

#define TEX_SLOTS 8192u
#define TEX_LARGE 0x10000u         /* above this, hash once a frame and sparsely */

typedef struct {
    uint32_t va, color, w, h, pitch, levels, palette, cube;
    uint32_t bytes;
    uint64_t hash;
    uint32_t checked_flip, last_flip;
    ID3D11ShaderResourceView *srv;
} TexEntry;

static TexEntry *s_tex;
static uint64_t s_tex_bytes;

static uint64_t hash_span(const uint8_t *p, size_t n, uint64_t h)
{
    while (n >= 8) {
        h = (h ^ *(const uint64_t *)p) * 0x9E3779B185EBCA87ull;
        h ^= h >> 29;
        p += 8;
        n -= 8;
    }
    while (n--)
        h = (h ^ *p++) * 0x100000001B3ull;
    return h;
}

static uint64_t hash_texture(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull ^ n;
    size_t at;

    if (n <= TEX_LARGE)
        return hash_span(p, n, h);
    /* A streamed texture or a video frame rewrites the lot, so a sample of every
     * page is enough to notice. */
    for (at = 0; at + 256 <= n; at += 4096)
        h = hash_span(p + at, 256, h);
    return h;
}

static uint32_t level_dim(uint32_t d, uint32_t l)
{
    d >>= l;
    return d ? d : 1;
}

/* Bytes of one mip level, and whether it is passed through as block data. */
static uint32_t level_bytes(const Nv2aTexture *t, uint32_t l)
{
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t w = level_dim(t->width, l), h = level_dim(t->height, l);

    if (block)
        return ((w + 3) / 4) * ((h + 3) / 4) * block;
    switch (t->color) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return w * h;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D: case 0x24: case 0x25:
        return w * h * 2;
    default:
        return w * h * 4;
    }
}

static int tex_is_linear(const Nv2aTexture *t)
{
    return !d3d8_format_is_swizzled(t->color) && !d3d8_format_dxt_block_bytes(t->color);
}

static void tex_create(TexEntry *e, const Nv2aTexture *t, const uint8_t *mem)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[6 * 16];
    void *freeme[6 * 16];
    uint32_t nfree = 0, block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = e->levels, faces = t->cube ? 6 : 1, f, l, at = 0;
    int native_bc = block && !(t->width & 3) && !(t->height & 3);
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D11Texture2D *tex = NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    int ok = 1;

    if (native_bc)
        fmt = t->color == 0x0C ? DXGI_FORMAT_BC1_UNORM
            : t->color == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;

    for (f = 0; f < faces && ok; f++) {
        for (l = 0; l < levels && ok; l++) {
            uint32_t lw = level_dim(t->width, l), lh = level_dim(t->height, l);
            D3D11_SUBRESOURCE_DATA *d = &init[f * levels + l];

            if (native_bc) {
                d->pSysMem = mem + t->offset + at;
                d->SysMemPitch = ((lw + 3) / 4) * block;
            } else {
                Nv2aTexture lv = *t;
                uint32_t *px = (uint32_t *)malloc((size_t)lw * lh * 4);
                if (!px) { ok = 0; break; }
                freeme[nfree++] = px;
                lv.offset = t->offset + at;
                lv.width = lw;
                lv.height = lh;
                if (!nv2a_backend_decode_texture(&lv, px))
                    ok = 0;
                d->pSysMem = px;
                d->SysMemPitch = lw * 4;
            }
            d->SysMemSlicePitch = 0;
            at += level_bytes(t, l);
        }
        at = (at + 127u) & ~127u;              /* faces are 128-byte aligned */
    }
    if (ok) {
        ZeroMemory(&td, sizeof td);
        td.Width = t->width; td.Height = t->height;
        td.MipLevels = levels; td.ArraySize = faces;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ok = SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, init, &tex));
    }
    if (ok) {
        ZeroMemory(&sv, sizeof sv);
        sv.Format = fmt;
        if (t->cube) {
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sv.TextureCube.MipLevels = levels;
        } else {
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = levels;
        }
        ok = SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, &sv, &e->srv));
    }
    if (tex)
        ID3D11Texture2D_Release(tex);
    for (l = 0; l < nfree; l++)
        free(freeme[l]);
    if (!ok) {
        static uint8_t said[256];
        if (!said[t->color & 0xFF]++)
            fprintf(stderr, "[D3D11] cannot decode texture format 0x%02X (%ux%u at 0x%08X): reads white\n",
                    t->color, t->width, t->height, t->offset);
        e->srv = NULL;
        return;
    }
    s_stats.uploads++;
    s_stats.upload_bytes += e->bytes;
}

static uint32_t tex_total_bytes(const Nv2aTexture *t, uint32_t levels)
{
    uint32_t total = 0, l;
    for (l = 0; l < levels; l++)
        total += level_bytes(t, l);
    if (t->cube)
        total = ((total + 127u) & ~127u) * 6;
    return total;
}

/* A srv for the stage's texture, or NULL for the white stand-in. */
static ID3D11ShaderResourceView *tex_lookup(const Nv2aTexture *t, const uint8_t *mem)
{
    uint32_t key, i, levels = t->levels ? t->levels : 1, maxlv = 1, bytes;
    uint32_t pitch = tex_is_linear(t) ? t->pitch : 0;
    uint32_t tries, free_slot = TEX_SLOTS;
    TexEntry *e = NULL;
    uint64_t h;

    if (!s_tex && !(s_tex = (TexEntry *)calloc(TEX_SLOTS, sizeof *s_tex)))
        return NULL;
    while (level_dim(t->width, maxlv - 1) > 1 || level_dim(t->height, maxlv - 1) > 1)
        maxlv++;
    if (levels > maxlv) levels = maxlv;
    if (tex_is_linear(t)) levels = 1;
    if (levels > 16) levels = 16;

    key = (t->offset >> 4) * 2654435761u;
    key ^= (t->color * 31u) ^ (t->width << 8) ^ t->height;
    for (tries = 0; tries < 64; tries++) {
        i = (key + tries) & (TEX_SLOTS - 1);
        if (!s_tex[i].va) {
            if (free_slot == TEX_SLOTS)
                free_slot = i;
            break;
        }
        if (s_tex[i].va == t->offset && s_tex[i].color == t->color && s_tex[i].w == t->width
            && s_tex[i].h == t->height && s_tex[i].pitch == pitch && s_tex[i].levels == levels
            && s_tex[i].palette == t->palette && s_tex[i].cube == (uint32_t)t->cube) {
            e = &s_tex[i];
            break;
        }
    }
    if (!e && free_slot == TEX_SLOTS) {
        /* A full run of the table: drop its least recently used entry. */
        uint32_t best = key & (TEX_SLOTS - 1);
        for (tries = 0; tries < 64; tries++) {
            i = (key + tries) & (TEX_SLOTS - 1);
            if (s_tex[i].last_flip < s_tex[best].last_flip)
                best = i;
        }
        free_slot = best;
    }
    if (!e) {
        e = &s_tex[free_slot];
        if (e->srv) {
            ID3D11ShaderResourceView_Release(e->srv);
            s_tex_bytes -= e->bytes;
        }
        memset(e, 0, sizeof *e);
        e->va = t->offset; e->color = t->color; e->w = t->width; e->h = t->height;
        e->pitch = pitch; e->levels = levels; e->palette = t->palette; e->cube = t->cube;
        e->bytes = tex_total_bytes(t, levels);
        e->checked_flip = 0xFFFFFFFFu;
    }
    e->last_flip = s_flip;
    bytes = pitch ? pitch * t->height : e->bytes;
    if (e->checked_flip == s_flip && (e->srv || bytes > TEX_LARGE) && bytes > TEX_LARGE)
        return e->srv;
    h = hash_texture(mem + t->offset, bytes);
    if (t->color == 0x0B && t->palette)
        h = hash_span(mem + t->palette, 1024, h);
    s_stats.hashes++;
    e->checked_flip = s_flip;
    if (e->srv && e->hash == h)
        return e->srv;
    if (e->srv) {
        ID3D11ShaderResourceView_Release(e->srv);
        e->srv = NULL;
        s_tex_bytes -= e->bytes;
    }
    e->hash = h;
    tex_create(e, t, mem);
    if (e->srv)
        s_tex_bytes += e->bytes;
    return e->srv;
}

/* ---- State caches -------------------------------------------------------- */

#define CACHE_MAX 256

typedef struct { uint32_t key[6]; ID3D11SamplerState *s; } SamplerEntry;
typedef struct { uint32_t key[6]; ID3D11BlendState *s; } BlendEntry;
typedef struct { uint32_t key[10]; ID3D11DepthStencilState *s; } DepthEntry;
typedef struct { float slope; ID3D11RasterizerState *s; } RasterEntry;

static SamplerEntry s_samplers[CACHE_MAX]; static int s_nsamplers;
static int          s_sampler_aniso;    /* the forced anisotropy the samplers were made for */
static int cfg_aniso(void);
static BlendEntry   s_blends[CACHE_MAX];   static int s_nblends;
static DepthEntry   s_depths[CACHE_MAX];   static int s_ndepths;
static RasterEntry  s_rasters[32];         static int s_nrasters;

static D3D11_TEXTURE_ADDRESS_MODE address_mode(uint32_t m, int border)
{
    switch (m) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return border ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static ID3D11SamplerState *sampler_for(const Nv2aTexture *t)
{
    uint32_t key[6];
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0x3F;
    int i, aniso = cfg_aniso();

    /* A change in the settings panel remakes them all, here on the drawing thread. */
    if (aniso != s_sampler_aniso) {
        for (i = 0; i < s_nsamplers; i++)
            ID3D11SamplerState_Release(s_samplers[i].s);
        s_nsamplers = 0;
        s_sampler_aniso = aniso;
    }
    key[0] = t->addr_u; key[1] = t->addr_v; key[2] = t->filter;
    key[3] = t->border; key[4] = (uint32_t)t->border_color; key[5] = 0;
    for (i = 0; i < s_nsamplers; i++)
        if (!memcmp(s_samplers[i].key, key, sizeof key))
            return s_samplers[i].s;
    if (s_nsamplers == CACHE_MAX)
        return s_samplers[0].s;
    {
        D3D11_SAMPLER_DESC d;
        /* MIN: 1 box lod0, 2 tent lod0, 3 box nearest-lod, 4 tent nearest-lod,
         * 5 box tent-lod, 6 tent tent-lod, 7 convolution. MAG: 1 box, 2 tent. */
        int min_linear = min == 2 || min == 4 || min == 6 || min == 7;
        int mag_linear = mag != 1;
        int mip_linear = min == 5 || min == 6 || min == 7;
        int lod0 = min <= 2;
        /* The low bits are a signed 4.8 fixed point level-of-detail bias. */
        int bias = (int)(t->filter & 0x1FFF);
        if (bias & 0x1000) bias -= 0x2000;

        ZeroMemory(&d, sizeof d);
        d.Filter = D3D11_ENCODE_BASIC_FILTER(min_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                             mag_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                             mip_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                             D3D11_FILTER_REDUCTION_TYPE_STANDARD);
        /* Forced anisotropy only where the game filters smoothly across mip levels:
         * point-sampled and single-level textures (the HUD, fonts) stay as they are. */
        if (aniso > 1 && min_linear && mag_linear && !lod0) {
            d.Filter = D3D11_FILTER_ANISOTROPIC;
            d.MaxAnisotropy = (UINT)aniso;
        }
        d.AddressU = address_mode(t->addr_u, t->border_color);
        d.AddressV = address_mode(t->addr_v, t->border_color);
        d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d.MipLODBias = (float)bias / 256.0f;
        d.BorderColor[0] = (float)((t->border >> 16) & 0xFF) / 255.0f;
        d.BorderColor[1] = (float)((t->border >> 8) & 0xFF) / 255.0f;
        d.BorderColor[2] = (float)(t->border & 0xFF) / 255.0f;
        d.BorderColor[3] = (float)(t->border >> 24) / 255.0f;
        d.MinLOD = 0;
        d.MaxLOD = lod0 ? 0.0f : D3D11_FLOAT32_MAX;
        memcpy(s_samplers[s_nsamplers].key, key, sizeof key);
        if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s_samplers[s_nsamplers].s)))
            return s_nsamplers ? s_samplers[0].s : NULL;
        return s_samplers[s_nsamplers++].s;
    }
}

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static uint8_t write_mask(uint32_t m)
{
    uint8_t o = 0;
    if (m & 0x00010000u) o |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (m & 0x00000100u) o |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (m & 0x00000001u) o |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (m & 0x01000000u) o |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return o;
}

static ID3D11BlendState *blend_for(const Nv2aRenderState *r)
{
    uint32_t key[6];
    int i;

    key[0] = r->blend_enable != 0;
    key[1] = key[0] ? r->blend_src : 0;
    key[2] = key[0] ? r->blend_dst : 0;
    key[3] = key[0] ? r->blend_eq : 0;
    key[4] = r->color_mask;
    key[5] = 0;
    for (i = 0; i < s_nblends; i++)
        if (!memcmp(s_blends[i].key, key, sizeof key))
            return s_blends[i].s;
    if (s_nblends == CACHE_MAX)
        return s_blends[0].s;
    {
        D3D11_BLEND_DESC d;
        ZeroMemory(&d, sizeof d);
        d.RenderTarget[0].BlendEnable = key[0];
        d.RenderTarget[0].SrcBlend = blend_factor(key[1], 0);
        d.RenderTarget[0].DestBlend = blend_factor(key[2], 0);
        d.RenderTarget[0].BlendOp = blend_op(key[3]);
        d.RenderTarget[0].SrcBlendAlpha = blend_factor(key[1], 1);
        d.RenderTarget[0].DestBlendAlpha = blend_factor(key[2], 1);
        d.RenderTarget[0].BlendOpAlpha = blend_op(key[3]);
        d.RenderTarget[0].RenderTargetWriteMask = write_mask(key[4]);
        memcpy(s_blends[s_nblends].key, key, sizeof key);
        if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &s_blends[s_nblends].s)))
            return NULL;
        return s_blends[s_nblends++].s;
    }
}

static D3D11_STENCIL_OP stencil_op(uint32_t o)
{
    switch (o) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static D3D11_COMPARISON_FUNC compare_func(uint32_t f)
{
    return (f >= 0x200 && f <= 0x207) ? (D3D11_COMPARISON_FUNC)(f - 0x200 + 1)
                                      : D3D11_COMPARISON_ALWAYS;
}

static ID3D11DepthStencilState *depth_for(const Nv2aRenderState *r)
{
    uint32_t key[10];
    int i;

    key[0] = r->depth_test_enable != 0;
    key[1] = key[0] ? r->depth_func : 0x207;
    key[2] = r->depth_write != 0;
    key[3] = r->stencil_enable != 0;
    key[4] = key[3] ? r->stencil_func : 0;
    key[5] = key[3] ? r->stencil_func_mask & 0xFF : 0;
    key[6] = key[3] ? r->stencil_write_mask & 0xFF : 0;
    key[7] = key[3] ? r->stencil_fail : 0;
    key[8] = key[3] ? r->stencil_zfail : 0;
    key[9] = key[3] ? r->stencil_zpass : 0;
    for (i = 0; i < s_ndepths; i++)
        if (!memcmp(s_depths[i].key, key, sizeof key))
            return s_depths[i].s;
    if (s_ndepths == CACHE_MAX)
        return s_depths[0].s;
    {
        D3D11_DEPTH_STENCIL_DESC d;
        ZeroMemory(&d, sizeof d);
        /* Depth writes without a test still need the depth unit on. */
        d.DepthEnable = key[0] || key[2];
        d.DepthWriteMask = key[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = compare_func(key[1]);
        d.StencilEnable = key[3];
        d.StencilReadMask = (UINT8)key[5];
        d.StencilWriteMask = (UINT8)key[6];
        d.FrontFace.StencilFunc = compare_func(key[4]);
        d.FrontFace.StencilFailOp = stencil_op(key[7]);
        d.FrontFace.StencilDepthFailOp = stencil_op(key[8]);
        d.FrontFace.StencilPassOp = stencil_op(key[9]);
        d.BackFace = d.FrontFace;
        memcpy(s_depths[s_ndepths].key, key, sizeof key);
        if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &s_depths[s_ndepths].s)))
            return NULL;
        return s_depths[s_ndepths++].s;
    }
}

static ID3D11RasterizerState *raster_for(float slope)
{
    int i;
    for (i = 0; i < s_nrasters; i++)
        if (s_rasters[i].slope == slope)
            return s_rasters[i].s;
    if (s_nrasters == 32)
        return s_rasters[0].s;
    {
        D3D11_RASTERIZER_DESC d;
        ZeroMemory(&d, sizeof d);
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;     /* culled by the executor */
        d.DepthClipEnable = FALSE;        /* window z is compared as it is */
        d.ScissorEnable = TRUE;
        d.SlopeScaledDepthBias = slope;
        s_rasters[s_nrasters].slope = slope;
        if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &d, &s_rasters[s_nrasters].s)))
            return NULL;
        return s_rasters[s_nrasters++].s;
    }
}

/* ---- Set-up -------------------------------------------------------------- */

static ID3DBlob *compile_from(const char *src, size_t len, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, len, "gpu_d3d11", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (err) {
        if (FAILED(hr))
            fprintf(stderr, "[D3D11] shader %s:\n%s\n", entry,
                    (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    return SUCCEEDED(hr) ? code : NULL;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    return compile_from(s_hlsl, sizeof s_hlsl - 1, entry, target);
}

static ID3D11Buffer *make_buffer(UINT bytes, UINT bind)
{
    D3D11_BUFFER_DESC d;
    ID3D11Buffer *b = NULL;
    ZeroMemory(&d, sizeof d);
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = bind;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &d, NULL, &b)))
        return NULL;
    return b;
}

static ID3D11ShaderResourceView *make_white(int cube)
{
    static const uint32_t white[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[6];
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    int i;

    ZeroMemory(&td, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = cube ? 6 : 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    for (i = 0; i < 6; i++) {
        init[i].pSysMem = &white[i];
        init[i].SysMemPitch = 4;
        init[i].SysMemSlicePitch = 0;
    }
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, init, &tex))) {
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &srv);
        ID3D11Texture2D_Release(tex);
    }
    return srv;
}

static void settings_load(void);

static int device_up(void)
{
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3DBlob *vb, *pb, *vcb, *pcb;
    HRESULT hr;
    DWORD t0 = GetTickCount();
    int ok;

    if (s_failed)
        return 0;
    if (s_dev)
        return 1;
    settings_load();
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           getenv("RECOMP_GPU_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0,
                           NULL, 0, D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] no device (0x%08lX)\n", (unsigned long)hr);
        s_failed = 1;
        return 0;
    }
    {   /* The default adapter: on a machine with two GPUs, say which. */
        IDXGIDevice *dd = NULL;
        IDXGIAdapter *ad = NULL;
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dd))
            && SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad))
            && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
            fprintf(stderr, "[D3D11] adapter: %ls (%u MB)\n", desc.Description,
                    (unsigned)(desc.DedicatedVideoMemory >> 20));
        if (ad) IDXGIAdapter_Release(ad);
        if (dd) IDXGIDevice_Release(dd);
    }
    vb = compile("vs", "vs_5_0");
    pb = compile("ps", "ps_5_0");
    vcb = compile("vs_clear", "vs_5_0");
    pcb = compile("ps_clear", "ps_5_0");
    {
        ID3DBlob *e = compile("ps_zenc", "ps_5_0"), *d = compile("ps_zdec", "ps_5_0");
        if (e) {
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(e),
                                           ID3D10Blob_GetBufferSize(e), NULL, &s_ps_zenc);
            ID3D10Blob_Release(e);
        }
        if (d) {
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(d),
                                           ID3D10Blob_GetBufferSize(d), NULL, &s_ps_zdec);
            ID3D10Blob_Release(d);
        }
    }
    ok = vb && pb && vcb && pcb
        && SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vb),
                                                     ID3D10Blob_GetBufferSize(vb), NULL, &s_vs))
        && SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(pb),
                                                    ID3D10Blob_GetBufferSize(pb), NULL, &s_ps))
        && SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vcb),
                                                     ID3D10Blob_GetBufferSize(vcb), NULL, &s_vs_clear))
        && SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(pcb),
                                                    ID3D10Blob_GetBufferSize(pcb), NULL, &s_ps_clear))
        && SUCCEEDED(ID3D11Device_CreateInputLayout(s_dev, el, 8, ID3D10Blob_GetBufferPointer(vb),
                                                    ID3D10Blob_GetBufferSize(vb), &s_layout));
    if (ok) {
        s_vb = make_buffer(VB_BYTES, D3D11_BIND_VERTEX_BUFFER);
        s_cb = make_buffer(sizeof(DrawConstants), D3D11_BIND_CONSTANT_BUFFER);
        s_kb = make_buffer(32, D3D11_BIND_CONSTANT_BUFFER);
        s_white = make_white(0);
        s_white_cube = make_white(1);
        ok = s_vb && s_cb && s_kb && s_white && s_white_cube;
    }
    if (!ok) {
        fprintf(stderr, "[D3D11] set-up failed\n");
        s_failed = 1;
        return 0;
    }
    {
        UINT stride = sizeof(Nv2aVshOutput), off = 0;
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    }
    fprintf(stderr, "[D3D11] renderer up, shaders compiled in %lu ms\n",
            (unsigned long)(GetTickCount() - t0));
    return 1;
}

/* ---- Draws --------------------------------------------------------------- */

static float zmax_of(uint32_t format)
{
    return ((format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
}

static void unpack(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static int     s_drawlog, s_skip;

static void bind_targets(Target *t, Zeta *z)
{
    D3D11_VIEWPORT vp;

    if (t == s_cur_target && z == s_cur_zeta)
        return;
    {
        /* Nothing bound as a texture may also be a target. */
        ID3D11ShaderResourceView *none[8] = {0};
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, none);
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &t->rtv, z ? z->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)t->pw; vp.Height = (float)t->ph;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    s_cur_target = t;
    s_cur_zeta = z;
}

/* Is this guest address a surface drawn into recently? A render target is sampled
 * from where it lives on the GPU; its guest memory is not up to date. */
static Target *target_at(uint32_t va)
{
    int i;
    for (i = 0; i < MAX_TARGETS; i++)
        if (s_targets[i].va == va && s_flip - s_targets[i].last_flip < 240)
            return &s_targets[i];
    return NULL;
}

static ID3D11ShaderResourceView *target_view(Target *t)
{
    if (t != s_cur_target)
        return t->srv;
    /* Sampling what is being drawn to: a copy of it as it stands. */
    if (!t->copy) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(t->tex, &td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &t->copy))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t->copy, NULL, &t->copy_srv)))
            return s_white;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)t->copy, (ID3D11Resource *)t->tex);
    s_stats.feedback++;
    return t->copy_srv;
}

/* ---- Depth buffers as memory ------------------------------------------------ */
/* On the console a depth buffer is memory like any other, and UC2's distortion
 * effects (the barrier on Sovereign, cloaked players, explosions) lean on that:
 * a full-screen pass reads the scene's Z24S8 depth buffer as an A8R8G8B8 texture
 * and draws it into a half-size colour surface, then the effect's sprites draw
 * into the distortion map with that surface named as their depth buffer. So
 * walls hide them. Here depth lives in a D3D11 depth buffer and a colour surface
 * in a render target, so the read found no texture and the sprites tested
 * against an empty depth buffer: they showed through walls. Two passes carry the
 * bytes across: zeta_encode when a draw samples a depth buffer's address, and
 * zeta_decode when a depth buffer's address was last written as colour. Only
 * 24-bit depth; the stencil byte reads as 0. */

/* RECOMP_ZETA_COPY=0 turns both passes off, for comparing. */
static int zeta_copy_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_ZETA_COPY");
        on = !(e && *e == '0');
    }
    return on;
}

static void zeta_pass_constants(float sx, float sy)
{
    D3D11_MAPPED_SUBRESOURCE m;
    float k[8] = {sx, sy, 0, 0, 0, 0, 0, 0};    /* kdepth.x 0: the near plane */

    ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_kb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, k, sizeof k);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_kb, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 1, 1, &s_kb);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_kb);
}

static void zeta_pass_state(uint32_t w, uint32_t h, int depth)
{
    Nv2aRenderState cr;
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    D3D11_RECT sc = {0, 0, (LONG)w, (LONG)h};
    float none[4] = {0, 0, 0, 0};

    memset(&cr, 0, sizeof cr);
    cr.color_mask = depth ? 0 : 0x01010101u;
    cr.depth_write = depth;
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_for(&cr), none, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth ? depth_for(&cr) : NULL, 0);
    ID3D11DeviceContext_RSSetState(s_ctx, raster_for(0.0f));
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_clear, NULL, 0);
}

/* A draw samples `va`: if that is a depth buffer's address, with no colour
 * surface the title drew there, put the depth into a render target at `va` as
 * the console's memory holds it, for the texture stage to find. Afterwards the
 * caller's targets are unbound (s_cur_target is NULL), to be bound again. */
static void zeta_encode(uint32_t va)
{
    Zeta *z = NULL;
    Target *t;
    Nv2aSurface surf;
    ID3D11ShaderResourceView *none[2] = {0, 0};
    int i;

    if (!s_ps_zenc || !zeta_copy_on())
        return;
    for (i = 0; i < MAX_TARGETS; i++) {
        Zeta *c = &s_zetas[i];
        if (c->va == va && c->zmax > 70000.0f && s_flip - c->last_flip < 240
            && (!z || c->w * c->h > z->w * z->h))
            z = c;
    }
    if (!z)
        return;
    t = target_at(va);
    if (t && (!t->from_zeta || t->seq > z->seq))
        return;                                 /* the title's own, or up to date */
    memset(&surf, 0, sizeof surf);
    surf.color_va = va;
    surf.width = z->gw; surf.height = z->gh;
    surf.bytes_per_pixel = 4; surf.pitch = z->gw * 4;
    surf.aa_sx = surf.aa_sy = 1;
    surf.format = 0x128;                        /* linear A8R8G8B8, Z24S8 */
    t = target_for(&surf);
    if (!t)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &t->rtv, NULL);
    s_cur_target = NULL;
    s_cur_zeta = NULL;
    zeta_pass_constants((float)z->w / (float)t->pw, (float)z->h / (float)t->ph);
    zeta_pass_state(t->pw, t->ph, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_zenc, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 8, 1, &z->srv);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 8, 2, none);
    t->from_zeta = 1;
    t->last_flip = s_flip;
    t->seq = ++s_seq;
    s_stats.zeta_copies++;
}

/* The depth buffer `z` is about to be used: if the title last wrote its memory
 * as a colour surface, take the depth from there. Unbinds like zeta_encode. */
static void zeta_decode(Zeta *z)
{
    Target *c = NULL;
    ID3D11ShaderResourceView *none[2] = {0, 0};
    int i;

    if (!s_ps_zdec || z->zmax < 70000.0f || !zeta_copy_on())
        return;
    for (i = 0; i < MAX_TARGETS; i++)
        if (s_targets[i].va == z->va && !s_targets[i].from_zeta && s_targets[i].seq > z->seq)
            c = &s_targets[i];
    if (!c)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, z->dsv);
    s_cur_target = NULL;
    s_cur_zeta = NULL;
    zeta_pass_constants((float)c->pw / (float)z->w, (float)c->ph / (float)z->h);
    zeta_pass_state(z->w, z->h, 1);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_zdec, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 9, 1, &c->srv);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 8, 2, none);
    z->seq = ++s_seq;
    s_stats.zeta_copies++;
}

/* The depth buffer for a draw or clear into `t`, brought up to date. */
static Zeta *zeta_get(const Nv2aSurface *s, uint32_t va, Target *t)
{
    Zeta *z = zeta_for(va, t->pw, t->ph);

    if (!z)
        return NULL;
    z->last_flip = s_flip;
    z->gw = t->w;
    z->gh = t->h;
    z->zmax = zmax_of(s->format);
    zeta_decode(z);
    return z;
}

static void d3d11_draw_inner(const Nv2aSurface *s, const Nv2aBatch *b)
{
    static DrawConstants c;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const Nv2aRenderState *r = b->state;
    const Nv2aShading *sh = b->shading;
    D3D11_MAPPED_SUBRESOURCE m;
    Target *t;
    Zeta *z = NULL;
    uint32_t bytes = b->count * (uint32_t)sizeof(Nv2aVshOutput), first;
    ID3D11ShaderResourceView *views[8];
    ID3D11SamplerState *samps[4];
    D3D11_RECT sc;
    int st, i;

    if (!sh || !b->outputs || !device_up() || bytes > VB_BYTES)
        return;
    t = target_for(s);
    if (!t)
        return;
    t->last_flip = s_flip;
    /* Depth or stencil in use at all: a title that never tests them may still
     * name a zeta surface from an earlier pass. */
    if (r->zeta_va && (r->depth_test_enable || r->depth_write || r->stencil_enable))
        z = zeta_get(s, r->zeta_va, t);
    bind_targets(t, z);

    /* Vertices. */
    if (s_vb_used + bytes > VB_BYTES) {
        ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        s_vb_used = 0;
    } else {
        ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m);
    }
    memcpy((uint8_t *)m.pData + s_vb_used, b->outputs, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    first = s_vb_used / (uint32_t)sizeof(Nv2aVshOutput);
    s_vb_used += bytes;

    /* Textures. */
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *tx = &sh->stage[st];
        uint32_t mode = (sh->rc.stage_program >> (st * 5)) & 0x1F;
        int used = sh->rc_seen ? mode != 0 : st == 0;
        float sx = 1.0f, sy = 1.0f;
        ID3D11ShaderResourceView *v = NULL;

        views[st] = s_white;
        views[st + 4] = s_white_cube;
        samps[st] = NULL;
        c.cube[st] = 0;
        if (used && sh->stage_valid[st]) {
            Target *rt;
            zeta_encode(tx->offset);
            rt = target_at(tx->offset);
            int linear = tex_is_linear(tx);

            samps[st] = sampler_for(tx);
            if (rt) {
                v = target_view(rt);
                if (linear) {
                    sx = 1.0f / (float)rt->w;
                    sy = 1.0f / (float)rt->h;
                }
            } else {
                v = tex_lookup(tx, mem);
                if (linear) {
                    sx = 1.0f / (float)tx->width;
                    sy = 1.0f / (float)tx->height;
                }
            }
            if (v) {
                if (tx->cube && !rt) {
                    views[st + 4] = v;
                    c.cube[st] = 1;
                } else {
                    views[st] = v;
                }
            }
        }
        c.ts[st][0] = sx;
        c.ts[st][1] = sy;
        /* An 8-bit surface keeps its one channel in blue here, as B8 is written from
         * blue on the console, and is read back as luminance. */
        c.ts[st][2] = (used && sh->stage_valid[st] && tx->color == 0x13 && target_at(tx->offset)) ? 1.0f : 0.0f;
        if (!samps[st])
            samps[st] = s_nsamplers ? s_samplers[0].s : NULL;
    }
    bind_targets(t, z);                 /* again, if zeta_encode drew */
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, views);
    {
        /* The first sampler may not exist yet, when no stage has been used. */
        static const Nv2aTexture dummy = {0};
        ID3D11SamplerState *fallback = sampler_for(&dummy);
        for (st = 0; st < 4; st++)
            if (!samps[st])
                samps[st] = fallback;
    }
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, samps);

    /* Constants. */
    c.rt[0] = (float)t->w;
    c.rt[1] = (float)t->h;
    c.rt[2] = zmax_of(s->format);
    c.rt[3] = r->poly_offset_fill ? r->poly_offset_bias / c.rt[2] : 0.0f;
    for (i = 0; i < 8; i++) {
        c.col[i][0] = sh->rc.color_icw[i];
        c.col[i][1] = sh->rc.color_ocw[i];
        c.col[i][2] = sh->rc.alpha_icw[i];
        c.col[i][3] = sh->rc.alpha_ocw[i];
        unpack(sh->rc.factor0[i], c.f0[i]);
        unpack(sh->rc.factor1[i], c.f1[i]);
    }
    c.fin[0] = sh->rc.final0;
    c.fin[1] = sh->rc.final1;
    c.fin[2] = sh->rc.control;
    c.fin[3] = sh->rc.stage_program;
    unpack(sh->rc.final_c0, c.fc0);
    unpack(sh->rc.final_c1, c.fc1);
    /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
    unpack(sh->fog_color, c.fogc);
    { float x = c.fogc[0]; c.fogc[0] = c.fogc[2]; c.fogc[2] = x; }
    c.misc[0] = r->alpha_test_enable;
    c.misc[1] = r->alpha_func;
    c.misc[2] = r->alpha_ref;
    c.misc[3] = sh->clip_plane_mode;
    c.flags[0] = sh->rc_seen != 0;
    c.flags[1] = sh->stage_valid[0] != 0;
    c.flags[2] = sh->dot_rgb_mapping;
    c.flags[3] = sh->other_stage_input;
    ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, &c, sizeof c);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);

    /* Fixed-function state. */
    {
        float bf[4];
        unpack(r->blend_color, bf);
        /* CONSTANT_ALPHA has no D3D factor of its own, so all four carry alpha. */
        if (r->blend_src == 0x8003 || r->blend_src == 0x8004
            || r->blend_dst == 0x8003 || r->blend_dst == 0x8004)
            bf[0] = bf[1] = bf[2] = bf[3];
        ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_for(r), bf, 0xFFFFFFFFu);
    }
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, z ? depth_for(r) : NULL,
                                               r->stencil_ref & 0xFF);
    /* The slope term is per pixel, and a pixel here is 1/sy of a guest one (or
     * 1/sx across, when stretched: the larger is the safer bias). */
    ID3D11DeviceContext_RSSetState(s_ctx, raster_for(r->poly_offset_fill
                                                     ? r->poly_offset_scale * (t->sx > t->sy ? t->sx : t->sy) : 0.0f));
    sc.left = (LONG)((float)s->clip_x * t->sx + 0.5f);
    sc.top = (LONG)((float)s->clip_y * t->sy + 0.5f);
    sc.right = (LONG)((float)(s->clip_x + s->width) * t->sx + 0.5f);
    sc.bottom = (LONG)((float)(s->clip_y + s->height) * t->sy + 0.5f);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);

    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    {
        /* -DrawLog's F5: leave out one kind of blended draw, to see what a stray
         * shape on screen is made of. */
        int skip = (s_skip == 1 && r->blend_enable && r->blend_src == 0 && r->blend_dst == 0x301)
                || (s_skip == 2 && r->blend_enable && r->blend_src == 0x306)
                || (s_skip == 3 && r->blend_enable && r->blend_src == 0x302 && r->blend_dst == 0x303)
                || (s_skip == 4 && r->blend_enable && r->blend_src == 1 && r->blend_dst != 0);
        /* In a logged frame, count what passed the depth and stencil tests: a
         * pass that should be hidden behind walls and is not shows here. */
        static ID3D11Query *passed_q;
        if (s_drawlog && !passed_q) {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
            ID3D11Device_CreateQuery(s_dev, &qd, &passed_q);
        }
        if (s_drawlog && passed_q && !s_vis_open)
            ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)passed_q);
        if (!skip)
            ID3D11DeviceContext_Draw(s_ctx, b->count, first);
        if (s_drawlog && passed_q && !s_vis_open) {
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)passed_q);
            while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)passed_q, &s_passed, sizeof s_passed, 0) == S_FALSE)
                ;
        }
    }
    t->seq = ++s_seq;
    if (z && r->depth_write)
        z->seq = s_seq;
    if (s_drawlog) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        const Nv2aVshOutput *o = b->outputs;
        uint32_t k;
        for (k = 0; k < b->count; k++) {
            if (o[k].pos[0] < x0) x0 = o[k].pos[0];
            if (o[k].pos[0] > x1) x1 = o[k].pos[0];
            if (o[k].pos[1] < y0) y0 = o[k].pos[1];
            if (o[k].pos[1] > y1) y1 = o[k].pos[1];
        }
        fprintf(stderr, "[DRAW] rt %08X zeta %08X %ux%u fmt %08X n %u box %.0f,%.0f..%.0f,%.0f prog %08X blend %u %X %X eq %X col %08X mask %08X "
                        "passed %llu at %u/%X/%u z %u/%X/%u st %u rc %u ctl %X f0 %08X f1 %08X d0 %.2f,%.2f,%.2f,%.2f t0 %.3f,%.3f,%.3f,%.3f\n",
                s->color_va, r->zeta_va, t->w, t->h, s->format, b->count, x0, y0, x1, y1, sh->rc.stage_program,
                r->blend_enable, r->blend_src, r->blend_dst, r->blend_eq, r->blend_color, r->color_mask,
                (unsigned long long)s_passed, r->alpha_test_enable, r->alpha_func, r->alpha_ref,
                r->depth_test_enable, r->depth_func, r->depth_write, r->stencil_enable,
                sh->rc_seen, sh->rc.control, sh->rc.final0, sh->rc.final1,
                o[0].d0[0], o[0].d0[1], o[0].d0[2], o[0].d0[3],
                o[0].tex[0][0], o[0].tex[0][1], o[0].tex[0][2], o[0].tex[0][3]);
        if (((sh->rc.stage_program >> 5) & 0x1F) == 0x11)
            fprintf(stderr, "[DRAW]   dot map %08X input %08X t1 %.3f,%.3f,%.3f t2 %.3f,%.3f,%.3f\n", sh->dot_rgb_mapping,
                    sh->other_stage_input, o[0].tex[1][0], o[0].tex[1][1], o[0].tex[1][2],
                    o[0].tex[2][0], o[0].tex[2][1], o[0].tex[2][2]);
        for (st = 0; st < 4; st++) {
            const Nv2aTexture *tx = &sh->stage[st];
            if (sh->stage_valid[st] && ((sh->rc.stage_program >> (st * 5)) & 0x1F))
                fprintf(stderr, "[DRAW]   st%d %08X %ux%u color %02X addr %u/%u border %d/%08X %s\n", st, tx->offset, tx->width,
                        tx->height, tx->color, tx->addr_u, tx->addr_v, tx->border_color, tx->border,
                        target_at(tx->offset) ? "TARGET" : "");
        }
        if (r->blend_src == 0x306 || (r->blend_src == 0 && r->blend_dst == 0x301)) {
            for (st = 0; st < 2; st++) {
                float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f, q0 = 1e9f, q1 = -1e9f;
                for (k = 0; k < b->count; k++) {
                    float q = o[k].tex[st][3] != 0 ? o[k].tex[st][3] : 1;
                    float u = o[k].tex[st][0] / q, v = o[k].tex[st][1] / q;
                    if (u < u0) u0 = u;
                    if (u > u1) u1 = u;
                    if (v < v0) v0 = v;
                    if (v > v1) v1 = v;
                    if (o[k].tex[st][3] < q0) q0 = o[k].tex[st][3];
                    if (o[k].tex[st][3] > q1) q1 = o[k].tex[st][3];
                }
                fprintf(stderr, "[DRAW]   uv%d %.3f,%.3f..%.3f,%.3f q %.3f..%.3f first %.3f,%.3f,%.3f,%.3f\n", st, u0, v0, u1, v1, q0, q1,
                        o[0].tex[st][0], o[0].tex[st][1], o[0].tex[st][2], o[0].tex[st][3]);
                if (sh->stage_valid[st] && !target_at(sh->stage[st].offset)) {
                    const Nv2aTexture *tx = &sh->stage[st];
                    uint32_t n = tx->width * tx->height, *px = malloc(n * 4);
                    if (px && nv2a_backend_decode_texture(tx, px)) {
                        uint64_t sum[4] = {0, 0, 0, 0};
                        uint32_t j;
                        char name[260];
                        FILE *f;
                        for (j = 0; j < n; j++) {
                            sum[0] += px[j] >> 24; sum[1] += (px[j] >> 16) & 0xFF;
                            sum[2] += (px[j] >> 8) & 0xFF; sum[3] += px[j] & 0xFF;
                        }
                        fprintf(stderr, "[DRAW]   tex%d %08X mean a %u r %u g %u b %u corner %08X centre %08X\n", st, tx->offset,
                                (uint32_t)(sum[0] / n), (uint32_t)(sum[1] / n), (uint32_t)(sum[2] / n), (uint32_t)(sum[3] / n),
                                px[0], px[tx->height / 2 * tx->width + tx->width / 2]);
                        snprintf(name, sizeof name, "%s.%08X.ppm", getenv("RECOMP_GPU_DRAWLOG"), tx->offset);
                        if ((f = fopen(name, "wb")) != NULL) {
                            fprintf(f, "P6\n%u %u\n255\n", tx->width, tx->height);
                            for (j = 0; j < n; j++) {
                                uint8_t rgb[3] = {(uint8_t)(px[j] >> 16), (uint8_t)(px[j] >> 8), (uint8_t)px[j]};
                                fwrite(rgb, 1, 3, f);
                            }
                            fclose(f);
                        }
                    }
                    free(px);
                }
            }
        }
        for (i = 0; i < (int)(sh->rc.control & 0xFF) && i < 8; i++)
            fprintf(stderr, "[DRAW]   rc%d %08X %08X %08X %08X k %08X %08X\n", i, sh->rc.color_icw[i], sh->rc.color_ocw[i],
                    sh->rc.alpha_icw[i], sh->rc.alpha_ocw[i], sh->rc.factor0[i], sh->rc.factor1[i]);
    }
    s_stats.draws++;
    s_stats.tris += b->count / 3;
}

static void d3d11_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    LONGLONG t = ft_qpc();
    d3d11_draw_inner(s, b);
    if (s_ft.qms)
        s_ft.d3d += (double)(ft_qpc() - t) / s_ft.qms;
}

static void d3d11_clear_inner(const Nv2aSurface *s, const Nv2aRenderState *r, uint32_t flags,
                              uint32_t argb, uint32_t zstencil)
{
    Target *t;
    Zeta *z = NULL;
    uint32_t x0 = s->clip_x, y0 = s->clip_y, x1 = s->clip_x + s->width, y1 = s->clip_y + s->height;
    int colour = (flags & 0xF0) != 0, depth = flags & 1, stencil = (flags >> 1) & 1;
    float zmax = zmax_of(s->format);
    float zval = (zmax < 70000.0f ? (float)(zstencil & 0xFFFF) : (float)(zstencil >> 8)) / zmax;
    uint8_t sval = (uint8_t)(zstencil & 0xFF);
    float col[4];

    if (s_drawlog)
        fprintf(stderr, "[DRAW] clear %08X fmt %08X clip %u,%u %ux%u flags %02X argb %08X rectset %d %08X %08X\n",
                s->color_va, s->format, s->clip_x, s->clip_y, s->width, s->height, flags, argb,
                r->clear_rect_set, r->clear_rect_h, r->clear_rect_v);
    if (!device_up())
        return;
    t = target_for(s);
    if (!t)
        return;
    t->last_flip = s_flip;
    if (r->zeta_va && (depth || stencil))
        z = zeta_get(s, r->zeta_va, t);
    if (colour)
        t->seq = ++s_seq;
    if (z && depth)
        z->seq = ++s_seq;
    if (r->clear_rect_set) {
        x0 = r->clear_rect_h & 0xFFFF;
        x1 = (r->clear_rect_h >> 16) + 1;
        y0 = r->clear_rect_v & 0xFFFF;
        y1 = (r->clear_rect_v >> 16) + 1;
    }
    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    unpack(argb, col);
    s_stats.clears++;

    if (x0 == 0 && y0 == 0 && x1 == t->w && y1 == t->h && (!colour || (flags & 0xF0) == 0xF0)) {
        if (colour)
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, t->rtv, col);
        if (z)
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv,
                (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0), zval, sval);
        return;
    }
    {
        /* Part of the surface, or some channels only: a quad under the scissor. */
        Nv2aRenderState cr;
        D3D11_RECT sc;
        D3D11_MAPPED_SUBRESOURCE m;
        float k[8];
        float none[4] = {0, 0, 0, 0};

        memset(&cr, 0, sizeof cr);
        /* CLEAR_SURFACE's colour bits: 0x10 R, 0x20 G, 0x40 B, 0x80 A. */
        cr.color_mask = ((flags & 0x10) ? 0x00010000u : 0) | ((flags & 0x20) ? 0x00000100u : 0)
                      | ((flags & 0x40) ? 0x00000001u : 0) | ((flags & 0x80) ? 0x01000000u : 0);
        cr.depth_test_enable = 0;
        cr.depth_write = depth;
        cr.stencil_enable = stencil;
        cr.stencil_func = 0x207;
        cr.stencil_func_mask = 0xFF;
        cr.stencil_write_mask = 0xFF;
        cr.stencil_fail = cr.stencil_zfail = cr.stencil_zpass = 0x1E01;

        bind_targets(t, z);
        memcpy(k, col, sizeof col);
        k[4] = zval; k[5] = k[6] = k[7] = 0;
        ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_kb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, k, sizeof k);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_kb, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 1, 1, &s_kb);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_kb);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_for(&cr), none, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, z ? depth_for(&cr) : NULL, sval);
        ID3D11DeviceContext_RSSetState(s_ctx, raster_for(0.0f));
        sc.left = (LONG)((float)x0 * t->sx + 0.5f); sc.top = (LONG)((float)y0 * t->sy + 0.5f);
        sc.right = (LONG)((float)x1 * t->sx + 0.5f); sc.bottom = (LONG)((float)y1 * t->sy + 0.5f);
        ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_clear, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_clear, NULL, 0);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    }
}

static void d3d11_clear(const Nv2aSurface *s, const Nv2aRenderState *r, uint32_t flags,
                        uint32_t argb, uint32_t zstencil)
{
    LONGLONG t = ft_qpc();
    d3d11_clear_inner(s, r, flags, argb, zstencil);
    if (s_ft.qms)
        s_ft.d3d += (double)(ft_qpc() - t) / s_ft.qms;
}

/* ---- Presenting to the window ------------------------------------------------ */
/* The finished frame goes from its render target straight to a swap chain on the
 * game window, which fb_present.c hands over (xbox_FramebufferWindowClaim). It
 * used to be copied back to guest memory, which waited for the GPU every frame
 * (5-8 ms of a 22 ms frame in a match), then copied twice more and uploaded again
 * by the window's own presenter. Guest memory now gets the frame only for frame
 * dumps (RECOMP_FB_DUMP). RECOMP_D3D_READBACK=1 keeps the old way to the screen.
 * Letterboxed to 4:3, or 16:9 when the panel stretches it, at any window size. The filter, vsync, frame limit and
 * internal resolution are the settings panel's (F1), below. */
extern void *xbox_FramebufferWindowClaim(void);

static const char s_blit_hlsl[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint i : SV_VertexID) { V o; float2 q = float2((i << 1) & 2, i & 2);\n"
    "  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";

static HWND                    s_hwnd;
static IDXGISwapChain1        *s_chain;
static ID3D11RenderTargetView *s_back;
static ID3D11VertexShader     *s_vs_blit;
static ID3D11PixelShader      *s_ps_blit;
static ID3D11SamplerState     *s_blit_samp;
static UINT                    s_bw, s_bh;
static int                     s_failed_present;

/* ---- Settings ------------------------------------------------------------------ */
/* What the owner can change while playing, in the panel F1 opens (or Back+Start on
 * the pad). They are kept in %LOCALAPPDATA%\xboxrecomp\ucc2_video.ini, and these
 * override the file for one run: RECOMP_RES_SCALE=1..4 or RECOMP_RES_HEIGHT=<lines>,
 * RECOMP_WIDESCREEN=0/1, RECOMP_FPS_LIMIT=<fps> (0 off), RECOMP_PRESENT_VSYNC=0/1,
 * RECOMP_PRESENT_FILTER=point/linear, RECOMP_ANISOTROPY=<1, 2, 4, 8 or 16>. */
static const int s_limits[] = {0, 30, 60, 120, 144, 240};
#define NLIMITS ((int)(sizeof s_limits / sizeof s_limits[0]))
/* Forced anisotropic filtering; 1 leaves the game's own filtering. */
static const int s_anisos[] = {1, 2, 4, 8, 16};
#define NANISOS ((int)(sizeof s_anisos / sizeof s_anisos[0]))
/* Internal resolutions, by height: the guest's 480, its multiples (960, 1440, 1920)
 * and the heights of 16:9 displays (720p, 1080p, 1440p, 4K). */
static const int s_heights[] = {480, 720, 960, 1080, 1440, 1920, 2160};
#define NHEIGHTS ((int)(sizeof s_heights / sizeof s_heights[0]))

static struct {
    int height;                 /* internal resolution, in lines (one of s_heights) */
    int wide;                   /* stretched to 16:9: the game's camera is still 4:3 */
    int limit;                  /* frame limit in fps, 0 for none */
    int vsync;
    int point;                  /* sharp pixels when scaling to the window */
    int counter;                /* frame rate in the corner */
    int boot;                   /* skip the boot screens' fixed waits, from the next start
                                   ([game] skip_boot_waits, read by recomp_manual.c) */
    int aniso;                  /* forced anisotropy (one of s_anisos), 1 for the game's own */
} s_cfg = {480, 0, 0, 0, 0, 0, 0, 1};

static int cfg_aniso(void)
{
    return s_cfg.aniso;
}

/* The width of a picture height lines high, in the chosen shape, kept even. */
static int res_width(int height)
{
    return s_cfg.wide ? (height * 16 / 9 + 1) & ~1 : height * 4 / 3;
}

/* The scale on each axis from 640x480, after a change of resolution or shape. */
static void res_apply(void)
{
    int i = 0;
    while (i < NHEIGHTS - 1 && s_heights[i] < s_cfg.height)
        i++;
    s_cfg.height = s_heights[i];
    s_sy = (float)s_cfg.height / 480.0f;
    s_sx = (float)res_width(s_cfg.height) / 640.0f;
    s_res++;
}

static char s_ini[MAX_PATH];

static int env_int(const char *name, int *out)
{
    const char *e = getenv(name);
    if (!e || !*e)
        return 0;
    *out = atoi(e);
    return 1;
}

static void settings_load(void)
{
    const char *base = getenv("LOCALAPPDATA"), *f;
    if (base) {
        char dir[MAX_PATH];
        _snprintf(dir, sizeof dir, "%s\\xboxrecomp", base);
        dir[sizeof dir - 1] = 0;
        CreateDirectoryA(dir, NULL);
        _snprintf(s_ini, sizeof s_ini, "%s\\ucc2_video.ini", dir);
        s_ini[sizeof s_ini - 1] = 0;
        /* Older files have only a whole scale. */
        s_cfg.height = (int)GetPrivateProfileIntA("video", "height",
                                                  480 * (int)GetPrivateProfileIntA("video", "scale", 1, s_ini), s_ini);
        s_cfg.wide = (int)GetPrivateProfileIntA("video", "widescreen", 0, s_ini);
        s_cfg.limit = (int)GetPrivateProfileIntA("video", "fps_limit", 0, s_ini);
        s_cfg.vsync = (int)GetPrivateProfileIntA("video", "vsync", 0, s_ini);
        s_cfg.point = (int)GetPrivateProfileIntA("video", "sharp", 0, s_ini);
        s_cfg.counter = (int)GetPrivateProfileIntA("video", "fps_counter", 0, s_ini);
        s_cfg.boot = (int)GetPrivateProfileIntA("game", "skip_boot_waits", 0, s_ini);
        s_cfg.aniso = (int)GetPrivateProfileIntA("video", "anisotropy", 1, s_ini);
    }
    {
        int n;
        if (env_int("RECOMP_RES_SCALE", &n))
            s_cfg.height = 480 * n;
    }
    env_int("RECOMP_RES_HEIGHT", &s_cfg.height);
    env_int("RECOMP_WIDESCREEN", &s_cfg.wide);
    env_int("RECOMP_FPS_LIMIT", &s_cfg.limit);
    env_int("RECOMP_PRESENT_VSYNC", &s_cfg.vsync);
    env_int("RECOMP_ANISOTROPY", &s_cfg.aniso);
    if ((f = getenv("RECOMP_PRESENT_FILTER")) != NULL && *f)
        s_cfg.point = *f == 'p' || *f == 'P';
    s_cfg.wide = s_cfg.wide != 0;
    if (s_cfg.limit < 0) s_cfg.limit = 0;
    {
        /* The nearest offered level at or below what was asked. */
        int i = 0;
        while (i < NANISOS - 1 && s_anisos[i + 1] <= s_cfg.aniso)
            i++;
        s_cfg.aniso = s_anisos[i];
    }
    res_apply();
    fprintf(stderr, "[D3D11] settings: %dx%d internal resolution (%s), frame limit %d, vsync %s, %s filter, "
            "anisotropy %dx (%s)\n",
            res_width(s_cfg.height), s_cfg.height, s_cfg.wide ? "16:9" : "4:3", s_cfg.limit,
            s_cfg.vsync ? "on" : "off", s_cfg.point ? "point" : "linear", s_cfg.aniso,
            s_ini[0] ? s_ini : "no settings file");
}

static void settings_save(void)
{
    char v[16];
    if (!s_ini[0])
        return;
    _snprintf(v, sizeof v, "%d", s_cfg.height);  WritePrivateProfileStringA("video", "height", v, s_ini);
    _snprintf(v, sizeof v, "%d", s_cfg.wide);    WritePrivateProfileStringA("video", "widescreen", v, s_ini);
    /* The nearest whole scale, for a build from before heights. */
    _snprintf(v, sizeof v, "%d", (s_cfg.height + 240) / 480); WritePrivateProfileStringA("video", "scale", v, s_ini);
    _snprintf(v, sizeof v, "%d", s_cfg.limit);   WritePrivateProfileStringA("video", "fps_limit", v, s_ini);
    _snprintf(v, sizeof v, "%d", s_cfg.vsync);   WritePrivateProfileStringA("video", "vsync", v, s_ini);
    _snprintf(v, sizeof v, "%d", s_cfg.point);   WritePrivateProfileStringA("video", "sharp", v, s_ini);
    _snprintf(v, sizeof v, "%d", s_cfg.counter); WritePrivateProfileStringA("video", "fps_counter", v, s_ini);
    _snprintf(v, sizeof v, "%d", s_cfg.boot);    WritePrivateProfileStringA("game", "skip_boot_waits", v, s_ini);
    _snprintf(v, sizeof v, "%d", s_cfg.aniso);   WritePrivateProfileStringA("video", "anisotropy", v, s_ini);
}

static int make_blit_sampler(void)
{
    D3D11_SAMPLER_DESC smp;
    if (s_blit_samp)
        ID3D11SamplerState_Release(s_blit_samp);
    s_blit_samp = NULL;
    ZeroMemory(&smp, sizeof smp);
    smp.Filter = s_cfg.point ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(ID3D11Device_CreateSamplerState(s_dev, &smp, &s_blit_samp));
}

static void client_size(UINT *w, UINT *h)
{
    RECT r;
    GetClientRect(s_hwnd, &r);
    *w = r.right > r.left ? (UINT)(r.right - r.left) : 1;
    *h = r.bottom > r.top ? (UINT)(r.bottom - r.top) : 1;
}

static int make_back(void)
{
    ID3D11Texture2D *b = NULL;
    HRESULT hr = IDXGISwapChain1_GetBuffer(s_chain, 0, &IID_ID3D11Texture2D, (void **)&b);
    if (FAILED(hr))
        return 0;
    hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)b, NULL, &s_back);
    ID3D11Texture2D_Release(b);
    return SUCCEEDED(hr);
}

/* A flip-model swap chain on the window, and what the blit needs. 0 on failure,
 * after which the window shows nothing: RECOMP_D3D_READBACK=1 is the way back. */
static int blit_init(HWND hwnd)
{
    IDXGIDevice1 *dd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *fac = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd;
    ID3DBlob *vb, *pb;
    HRESULT hr;
    int ok;

    s_hwnd = hwnd;
    client_size(&s_bw, &s_bh);
    hr = ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice1, (void **)&dd);
    if (SUCCEEDED(hr)) {
        /* One frame queued, not three: the GPU's work is small, and a deeper
         * queue is only input latency. */
        IDXGIDevice1_SetMaximumFrameLatency(dd, 1);
        hr = IDXGIDevice1_GetAdapter(dd, &ad);
    }
    if (SUCCEEDED(hr))
        hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
    if (SUCCEEDED(hr)) {
        ZeroMemory(&sd, sizeof sd);
        sd.Width = s_bw;
        sd.Height = s_bh;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)s_dev, hwnd, &sd, NULL, NULL, &s_chain);
        /* Alt+Enter is the window's borderless fullscreen, not DXGI's exclusive mode. */
        if (SUCCEEDED(hr))
            IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    if (fac) IDXGIFactory2_Release(fac);
    if (ad) IDXGIAdapter_Release(ad);
    if (dd) IDXGIDevice1_Release(dd);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] no swap chain on the window (0x%08lX): set RECOMP_D3D_READBACK=1\n",
                (unsigned long)hr);
        return 0;
    }
    vb = compile_from(s_blit_hlsl, sizeof s_blit_hlsl - 1, "vs", "vs_5_0");
    pb = compile_from(s_blit_hlsl, sizeof s_blit_hlsl - 1, "ps", "ps_5_0");
    ok = vb && pb && make_back()
        && SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vb),
                                                     ID3D10Blob_GetBufferSize(vb), NULL, &s_vs_blit))
        && SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(pb),
                                                    ID3D10Blob_GetBufferSize(pb), NULL, &s_ps_blit))
        && make_blit_sampler();
    if (vb) ID3D10Blob_Release(vb);
    if (pb) ID3D10Blob_Release(pb);
    if (!ok) {
        fprintf(stderr, "[D3D11] present set-up failed: set RECOMP_D3D_READBACK=1\n");
        IDXGISwapChain1_Release(s_chain);
        s_chain = NULL;
        return 0;
    }
    fprintf(stderr, "[D3D11] presenting to the window (%ux%u)\n", s_bw, s_bh);
    return 1;
}

/* ---- Settings panel and frame rate counter ------------------------------------- */
/* Text is drawn with GDI into a bitmap, which is uploaded only when the text
 * changes and drawn over the frame, at one texel a pixel, half see-through.
 * While the panel is open the game sees its pads at rest (xbox_input_held). */
extern volatile long xbox_input_held;          /* src/input/xinput_device.c */

enum { ROW_SCALE, ROW_ASPECT, ROW_LIMIT,ROW_VSYNC, ROW_FILTER, ROW_ANISO, ROW_COUNTER, ROW_BOOT, ROWS };

static struct {
    int open, row;
    int pad_ok;                 /* pad 0 answered last time */
    DWORD pad_retry;            /* when to ask a missing pad again */
    WORD pad_last;
    uint8_t key_last[256];
    /* frame rate, over half-second windows */
    LONGLONG fps_t0;
    uint32_t fps_n;
    double fps;
    uint32_t game_pw, game_ph;  /* the shown surface, in its own pixels */
    /* the text bitmap */
    HDC dc;
    HBITMAP bmp;
    void *bits;
    HFONT font;
    int font_px, bw, bh;        /* font size, bitmap size */
    char text[1024];            /* what the texture holds now */
    UINT tw, th;                /* texture and text size */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11BlendState *blend;
} s_ui;

static int key_pressed(int vk, int fg)
{
    int down = fg && (GetAsyncKeyState(vk) & 0x8000) != 0;
    int edge = down && !s_ui.key_last[vk];
    s_ui.key_last[vk] = (uint8_t)down;
    return edge;
}

static void ui_change(int row, int dir)
{
    int i;
    switch (row) {
    case ROW_SCALE:
        for (i = 0; i < NHEIGHTS && s_heights[i] != s_cfg.height; i++)
            ;
        i = i == NHEIGHTS ? 0 : (i + dir + NHEIGHTS) % NHEIGHTS;
        s_cfg.height = s_heights[i];
        res_apply();                            /* targets remake themselves as they are drawn */
        break;
    case ROW_ASPECT:
        s_cfg.wide = !s_cfg.wide;
        res_apply();
        break;
    case ROW_LIMIT:
        for (i = 0; i < NLIMITS && s_limits[i] != s_cfg.limit; i++)
            ;
        i = i == NLIMITS ? 0 : (i + dir + NLIMITS) % NLIMITS;
        s_cfg.limit = s_limits[i];
        break;
    case ROW_VSYNC:
        s_cfg.vsync = !s_cfg.vsync;
        break;
    case ROW_FILTER:
        s_cfg.point = !s_cfg.point;
        make_blit_sampler();
        break;
    case ROW_ANISO:
        for (i = 0; i < NANISOS && s_anisos[i] != s_cfg.aniso; i++)
            ;
        i = i == NANISOS ? 0 : (i + dir + NANISOS) % NANISOS;
        s_cfg.aniso = s_anisos[i];              /* the samplers remake themselves as they are used */
        break;
    case ROW_COUNTER:
        s_cfg.counter = !s_cfg.counter;
        break;
    case ROW_BOOT:
        s_cfg.boot = !s_cfg.boot;
        break;
    }
    settings_save();
    fprintf(stderr, "[D3D11] settings: %dx%d internal resolution (%s), frame limit %d, vsync %s, %s filter, "
            "anisotropy %dx, counter %s\n",
            res_width(s_cfg.height), s_cfg.height, s_cfg.wide ? "16:9" : "4:3", s_cfg.limit,
            s_cfg.vsync ? "on" : "off", s_cfg.point ? "point" : "linear", s_cfg.aniso, s_cfg.counter ? "on" : "off");
}

/* Keys while the window has the focus, and pad 0: F1 or Back+Start opens and
 * closes; up and down choose, left and right (or A, Enter) change; B or Esc closes. */
static void ui_input(void)
{
    int fg = s_hwnd && GetForegroundWindow() == s_hwnd;
    WORD pad = 0, press;
    int toggle, up, down, left, right, close;

    if (s_ui.pad_ok || GetTickCount() - s_ui.pad_retry < 0x80000000u) {
        /* XInputGetState is slow for a pad that is not there: ask every 2 s. */
        XINPUT_STATE xs;
        s_ui.pad_ok = XInputGetState(0, &xs) == ERROR_SUCCESS;
        if (s_ui.pad_ok)
            pad = xs.Gamepad.wButtons;
        else
            s_ui.pad_retry = GetTickCount() + 2000;
    }
    press = (WORD)(pad & ~s_ui.pad_last);
    s_ui.pad_last = pad;

    toggle = key_pressed(VK_F1, fg)
          || ((pad & XINPUT_GAMEPAD_BACK) && (pad & XINPUT_GAMEPAD_START)
              && (press & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)));
    up = key_pressed(VK_UP, fg) | !!(press & XINPUT_GAMEPAD_DPAD_UP);
    down = key_pressed(VK_DOWN, fg) | !!(press & XINPUT_GAMEPAD_DPAD_DOWN);
    left = key_pressed(VK_LEFT, fg) | !!(press & XINPUT_GAMEPAD_DPAD_LEFT);
    right = key_pressed(VK_RIGHT, fg) | key_pressed(VK_RETURN, fg) | !!(press & XINPUT_GAMEPAD_DPAD_RIGHT)
          | !!(press & XINPUT_GAMEPAD_A);
    close = key_pressed(VK_ESCAPE, fg) | !!(press & XINPUT_GAMEPAD_B);

    if (toggle || (s_ui.open && close)) {
        s_ui.open = toggle ? !s_ui.open : 0;
        InterlockedExchange(&xbox_input_held, s_ui.open);
        return;
    }
    if (!s_ui.open)
        return;
    if (up) s_ui.row = (s_ui.row + ROWS - 1) % ROWS;
    if (down) s_ui.row = (s_ui.row + 1) % ROWS;
    if (left) ui_change(s_ui.row, -1);
    if (right) ui_change(s_ui.row, 1);
}

/* Render the lines (separated by \n) into the bitmap and the texture, if they
 * differ from what is there. Line 0 of the panel is a title and its last line a
 * dim footnote; the row the cursor is on is the line hl (or none, -1). */
static int ui_text(const char *text, int hl)
{
    char key[sizeof s_ui.text];
    int px = (int)s_bh / 32, lines = 1, w = 0, i, y;
    const char *p;
    D3D11_TEXTURE2D_DESC td;

    if (px < 14) px = 14;
    if (px > 40) px = 40;
    _snprintf(key, sizeof key, "%d|%d|%s", px, hl, text);
    key[sizeof key - 1] = 0;
    if (s_ui.srv && !strcmp(key, s_ui.text))
        return 1;
    if (!s_ui.dc && !(s_ui.dc = CreateCompatibleDC(NULL)))
        return 0;
    if (px != s_ui.font_px) {
        if (s_ui.font) DeleteObject(s_ui.font);
        s_ui.font = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        s_ui.font_px = px;
    }
    SelectObject(s_ui.dc, s_ui.font);
    for (p = text; *p; p++)
        lines += *p == '\n';
    for (p = text; ; ) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        SIZE sz;
        if (GetTextExtentPoint32A(s_ui.dc, p, n, &sz) && sz.cx > w)
            w = sz.cx;
        if (!e) break;
        p = e + 1;
    }
    {
        int lh = px * 5 / 4, pad = px / 2;
        UINT tw = (UINT)(w + pad * 2), th = (UINT)(lines * lh + pad * 2);
        if (!s_ui.bmp || (int)tw > s_ui.bw || (int)th > s_ui.bh) {
            BITMAPINFO bi;
            ZeroMemory(&bi, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = (LONG)tw;
            bi.bmiHeader.biHeight = -(LONG)th;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            if (s_ui.bmp) DeleteObject(s_ui.bmp);
            s_ui.bmp = CreateDIBSection(s_ui.dc, &bi, DIB_RGB_COLORS, &s_ui.bits, NULL, 0);
            if (!s_ui.bmp)
                return 0;
            SelectObject(s_ui.dc, s_ui.bmp);
            s_ui.bw = (int)tw;
            s_ui.bh = (int)th;
        }
        {
            RECT all = {0, 0, s_ui.bw, s_ui.bh};
            HBRUSH bg = CreateSolidBrush(RGB(12, 14, 22));
            FillRect(s_ui.dc, &all, bg);
            DeleteObject(bg);
        }
        SetBkMode(s_ui.dc, TRANSPARENT);
        for (p = text, i = 0, y = pad; ; i++, y += lh) {
            const char *e = strchr(p, '\n');
            int n = e ? (int)(e - p) : (int)strlen(p);
            if (i == hl) {
                RECT r = {pad / 2, y - px / 8, (LONG)tw - pad / 2, y + lh - px / 8};
                HBRUSH b = CreateSolidBrush(RGB(48, 56, 92));
                FillRect(s_ui.dc, &r, b);
                DeleteObject(b);
            }
            SetTextColor(s_ui.dc, i == hl ? RGB(255, 214, 90) : i == 0 && hl >= 0 ? RGB(150, 190, 255)
                                  : i == lines - 1 && hl >= 0 ? RGB(120, 124, 140) : RGB(230, 230, 230));
            TextOutA(s_ui.dc, pad, y, p, n);
            if (!e) break;
            p = e + 1;
        }
        GdiFlush();
        if (!s_ui.tex || s_ui.tw != tw || s_ui.th != th) {
            if (s_ui.srv) ID3D11ShaderResourceView_Release(s_ui.srv);
            if (s_ui.tex) ID3D11Texture2D_Release(s_ui.tex);
            s_ui.srv = NULL;
            s_ui.tex = NULL;
            ZeroMemory(&td, sizeof td);
            td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ui.tex))
                || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_ui.tex, NULL, &s_ui.srv)))
                return 0;
            s_ui.tw = tw;
            s_ui.th = th;
        }
        {
            D3D11_BOX box = {0, 0, 0, tw, th, 1};
            ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_ui.tex, 0, &box, s_ui.bits,
                                                  (UINT)s_ui.bw * 4, 0);
        }
    }
    memcpy(s_ui.text, key, sizeof key);
    return 1;
}

/* Over the frame in the back buffer, at the top left of the picture (x0, y0).
 * Uses the blit shaders and sampler that blit_present has bound. */
static void ui_draw(float x0, float y0)
{
    char text[sizeof s_ui.text - 64];
    int hl = -1;
    D3D11_VIEWPORT vp;
    float k[4] = {0.88f, 0.88f, 0.88f, 0.88f};

    if (s_ui.open) {
        static const char *const sharp[] = {"smooth", "sharp"}, *const onoff[] = {"off", "on"};
        char limit[16], aniso[16];
        if (s_cfg.limit)
            _snprintf(limit, sizeof limit, "%d fps", s_cfg.limit);
        else
            _snprintf(limit, sizeof limit, "off");
        limit[sizeof limit - 1] = 0;
        if (s_cfg.aniso > 1)
            _snprintf(aniso, sizeof aniso, "%dx", s_cfg.aniso);
        else
            _snprintf(aniso, sizeof aniso, "off");
        aniso[sizeof aniso - 1] = 0;
        _snprintf(text, sizeof text,
                  "Settings                      %4.0f fps\n"
                  "Resolution    %dx%d%s\n"
                  "Aspect        %s\n"
                  "Frame limit   %s\n"
                  "Vsync         %s\n"
                  "Scaling       %s\n"
                  "Anisotropic   %s\n"
                  "FPS counter   %s\n"
                  "Boot waits    %s\n"
                  " \n"
                  "Up/Down choose, Left/Right change\n"
                  "F1, Esc or B closes\n"
                  " \n"
                  "uc2-recomp %s",
                  s_ui.fps, s_ui.game_pw, s_ui.game_ph,
                  s_cfg.height == 720 ? "  (720p)" : s_cfg.height == 1080 ? "  (1080p)"
                  : s_cfg.height == 1440 ? "  (1440p)" : s_cfg.height == 2160 ? "  (4K)" : "",
                  s_cfg.wide ? "16:9 (stretched)" : "4:3",
                  limit, onoff[s_cfg.vsync], sharp[s_cfg.point], aniso, onoff[s_cfg.counter],
                  s_cfg.boot ? "skip (from next start)" : "keep", UCC2_VERSION);
        hl = 1 + s_ui.row;
    } else if (s_cfg.counter) {
        _snprintf(text, sizeof text, "%.0f fps", s_ui.fps);
    } else {
        return;
    }
    text[sizeof text - 1] = 0;
    if (!ui_text(text, hl))
        return;
    if (!s_ui.blend) {
        D3D11_BLEND_DESC bd;
        ZeroMemory(&bd, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bd, &s_ui.blend)))
            return;
    }
    vp.TopLeftX = x0 + (float)(s_ui.font_px / 2);
    vp.TopLeftY = y0 + (float)(s_ui.font_px / 2);
    vp.Width = (float)s_ui.tw;
    vp.Height = (float)s_ui.th;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_ui.blend, k, 0xFFFFFFFFu);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s_ui.srv);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
}

/* The frame limit: hold each present until 1/limit s after the last one. A
 * high-resolution waitable timer sleeps most of the way and the last half
 * millisecond is spun. Holding the walker holds the game, which waits on its
 * flips. A late frame starts a new schedule rather than letting the next ones
 * hurry to catch up. Returns the ms spent waiting. */
static double limit_wait(void)
{
    static HANDLE timer;
    static int tried;
    static LONGLONG next;
    LONGLONG now = ft_qpc(), start = now, period;

    if (!s_cfg.limit || !s_ft.qms) {
        next = 0;
        return 0.0;
    }
    period = (LONGLONG)(s_ft.qms * 1000.0 / s_cfg.limit);
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
    }
    if (next && now < next) {
        LONGLONG spin = (LONGLONG)(s_ft.qms * 0.5);
        if (next - now > spin) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((double)(next - now - spin) / s_ft.qms * 10000.0);
            if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, 100);
            else
                Sleep((DWORD)((double)(next - now - spin) / s_ft.qms));
        }
        while ((now = ft_qpc()) < next)
            YieldProcessor();
    }
    next = next && now - next < period ? next + period : now + period;
    return (double)(now - start) / s_ft.qms;
}

/* Draw the frame into the back buffer, letterboxed, and present it. The state it
 * changes is put back or left for the next draw to set: d3d11_draw sets blend,
 * depth, raster, scissor, shaders and samplers every time, and bind_targets binds
 * the target again once s_cur_target is cleared. */
static void blit_present(Target *t)
{
    static const float black[4] = {0, 0, 0, 1};
    ID3D11ShaderResourceView *none = NULL;
    D3D11_VIEWPORT vp;
    UINT cw, ch;
    float k;

    client_size(&cw, &ch);
    if (cw != s_bw || ch != s_bh) {
        /* Resized, or fullscreen toggled: a back buffer the client area's size. */
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(s_back);
        s_back = NULL;
        s_cur_target = NULL;
        if (FAILED(IDXGISwapChain1_ResizeBuffers(s_chain, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0)) || !make_back())
            return;
        s_bw = cw;
        s_bh = ch;
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back, black);
    /* The largest 4:3 (or 16:9) rectangle that fits, centred. */
    {
        float aw = s_cfg.wide ? 16.0f : 4.0f, ah = s_cfg.wide ? 9.0f : 3.0f;
        k = (float)s_bw * ah < (float)s_bh * aw ? (float)s_bw / aw : (float)s_bh / ah;
        vp.Width = aw * k;
        vp.Height = ah * k;
    }
    vp.TopLeftX = ((float)s_bw - vp.Width) * 0.5f;
    vp.TopLeftY = ((float)s_bh - vp.Height) * 0.5f;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_blit, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_blit, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &t->srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_samp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    s_ui.game_pw = t->pw;
    s_ui.game_ph = t->ph;
    ui_draw((float)(int)vp.TopLeftX, (float)(int)vp.TopLeftY);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    s_cur_target = NULL;
    s_cur_zeta = NULL;
    s_ft.limit += limit_wait();
    IDXGISwapChain1_Present(s_chain, (UINT)(s_cfg.vsync != 0), 0);
}

/* The frame is finished: copy it to guest memory, where the window and the frame
 * dumps read it from. */
static void vis_poll(int all);

static void d3d11_present(uint32_t va, uint32_t pitch)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    D3D11_MAPPED_SUBRESOURCE m;
    Target *t = NULL;
    LONGLONG t0;
    uint32_t y;
    int i;

    ft_frame();
    t0 = ft_qpc();
    s_flip++;
    s_stats.presents++;
    /* RECOMP_GPU_DRAWLOG=<flag file>: log every draw and clear of the frame that
     * follows, once per second while the file exists, and every frame while F7 is
     * held. F5 steps through the kinds of draw to leave out (see d3d11_draw). */
    {
        static const char *flag;
        static DWORD last;
        if (!flag) { flag = getenv("RECOMP_GPU_DRAWLOG"); if (!flag) flag = ""; }
        s_drawlog = 0;
        if (*flag) {
            int held = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            if (GetAsyncKeyState(VK_F5) & 1) {
                s_skip = (s_skip + 1) % 5;
                fprintf(stderr, "[DRAW] leaving out kind %d\n", s_skip);
            }
            if (held || (GetTickCount() - last > 1000
                         && GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES)) {
                last = GetTickCount();
                s_drawlog = 1;
                fprintf(stderr, "[DRAW] frame after flip %u, shown %08X%s\n", s_flip, va, held ? " (F7)" : "");
            }
        }
    }
    {
        LONGLONG now = ft_qpc();
        s_ui.fps_n++;
        if (!s_ui.fps_t0) {
            s_ui.fps_t0 = now;
        } else if (s_ft.qms && now - s_ui.fps_t0 > (LONGLONG)(s_ft.qms * 500.0)) {
            s_ui.fps = s_ui.fps_n * 1000.0 / ((double)(now - s_ui.fps_t0) / s_ft.qms);
            s_ui.fps_t0 = now;
            s_ui.fps_n = 0;
        }
    }
    ui_input();
    if (getenv("RECOMP_GPU_STATS") && GetTickCount() - s_stats.last > 5000) {
        s_stats.last = GetTickCount();
        fprintf(stderr, "[D3D11] %u presents, %u draws, %u tris, %u clears, %u uploads (%llu KB, %llu KB held), "
                        "%u hashes, %u feedback copies, %u depth copies\n",
                s_stats.presents, s_stats.draws, s_stats.tris, s_stats.clears, s_stats.uploads,
                (unsigned long long)(s_stats.upload_bytes / 1024),
                (unsigned long long)(s_tex_bytes / 1024), s_stats.hashes, s_stats.feedback, s_stats.zeta_copies);
        fprintf(stderr, "[D3D11] visibility: %u tests, %u with no pixels, at most %u pending\n",
                s_vis_stats.reports, s_vis_stats.zero, s_vis_stats.max_pending);
        memset(&s_vis_stats, 0, sizeof s_vis_stats);
        memset(&s_stats, 0, sizeof s_stats);
        s_stats.last = GetTickCount();
    }
    if (!s_dev)
        return;
    vis_poll(0);
    for (i = 0; i < MAX_TARGETS; i++)
        if (s_targets[i].va == va)
            t = &s_targets[i];
    if (!t)
        return;
    {
        static int direct = -1, dumps;
        if (direct < 0) {
            const char *e = getenv("RECOMP_D3D_READBACK");
            direct = !(e && *e && *e != '0');
            dumps = getenv("RECOMP_FB_DUMP") != NULL;
        }
        if (direct && !s_failed_present) {
            void *hwnd = s_chain ? (void *)s_hwnd : xbox_FramebufferWindowClaim();
            if (hwnd && !s_chain && !blit_init((HWND)hwnd))
                s_failed_present = 1;
            if (s_chain) {
                blit_present(t);
                if (!dumps) {
                    s_ft.present += (double)(ft_qpc() - t0) / s_ft.qms;
                    return;
                }
            }
        }
    }
    if (!s_staging || s_staging_w != t->pw || s_staging_h != t->ph) {
        D3D11_TEXTURE2D_DESC td;
        if (s_staging)
            ID3D11Texture2D_Release(s_staging);
        s_staging = NULL;
        ID3D11Texture2D_GetDesc(t->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_staging)))
            return;
        s_staging_w = t->pw;
        s_staging_h = t->ph;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_staging, (ID3D11Resource *)t->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0, D3D11_MAP_READ, 0, &m)))
        return;
    /* Above 1x, guest memory gets the nearest pixel: dumps stay the guest's size. */
    for (y = 0; y < t->h; y++) {
        const uint8_t *src = (const uint8_t *)m.pData + (size_t)(y * t->ph / t->h) * m.RowPitch;
        uint8_t *dst = mem + va + (size_t)y * pitch;
        if (t->bpp == 4 && t->pw == t->w) {
            memcpy(dst, src, (size_t)t->w * 4);
        } else if (t->bpp == 4) {
            uint32_t x;
            for (x = 0; x < t->w; x++)
                ((uint32_t *)dst)[x] = ((const uint32_t *)src)[x * t->pw / t->w];
        } else if (t->bpp == 2) {
            uint32_t x;
            for (x = 0; x < t->w; x++) {
                uint32_t p = ((const uint32_t *)src)[x * t->pw / t->w];
                ((uint16_t *)dst)[x] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0)
                                                  | ((p >> 3) & 0x001F));
            }
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
    s_ft.present += (double)(ft_qpc() - t0) / s_ft.qms;
}

/* ---- Visibility tests ------------------------------------------------------ */
/* D3D's Begin/EndVisibilityTest: the title draws something with pixel counting
 * on, and GET_REPORT asks for the count. The title culls characters with these,
 * which a test that always passed never let it do. Each stretch of counting is a D3D11 occlusion query; a report waits for
 * its queries and is written once they finish, polled once a frame without
 * stalling. The title asks without waiting and treats a pending test as
 * visible, so a frame or two of latency is what it was built for. Counts are
 * scaled from the internal resolution back to the guest's pixels. */
#define VIS_QUERIES 4096                /* the pool: in flight, or free */
#define VIS_REPORTS 2048
#define VIS_PARTS   8                   /* counting stretches in one report */

typedef struct {
    uint32_t at;                        /* guest offset of the 16-byte report */
    float    scale;                     /* host pixels per guest pixel */
    int      n;
    ID3D11Query *q[VIS_PARTS];
} VisReport;

static ID3D11Query *s_vis_free[VIS_QUERIES];
static int          s_vis_nfree, s_vis_made;
static ID3D11Query *s_vis_cur[VIS_PARTS];   /* since CLEAR_REPORT_VALUE */
static int          s_vis_ncur;
static VisReport    s_vis[VIS_REPORTS];     /* a ring, oldest first */
static int          s_vis_head, s_vis_count;

static ID3D11Query *vis_get(void)
{
    D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
    ID3D11Query *q = NULL;

    if (s_vis_nfree)
        return s_vis_free[--s_vis_nfree];
    if (s_vis_made >= VIS_QUERIES || FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &q)))
        return NULL;
    s_vis_made++;
    return q;
}

static void vis_put(ID3D11Query *q)
{
    s_vis_free[s_vis_nfree++] = q;
}

static void vis_write(uint32_t at, uint32_t pixels)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t done = 0;

    memcpy(mem + at + 8, &pixels, 4);
    MemoryBarrier();                    /* the count before the status */
    memcpy(mem + at + 12, &done, 4);
}

/* The stretch being counted ends, and joins the report being built. */
static void vis_close(void)
{
    if (!s_vis_open)
        return;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_vis_open);
    if (s_vis_ncur < VIS_PARTS)
        s_vis_cur[s_vis_ncur++] = s_vis_open;
    else
        vis_put(s_vis_open);            /* cannot happen with D3D's pairs */
    s_vis_open = NULL;
}

static void vis_drop_cur(void)
{
    int i;
    for (i = 0; i < s_vis_ncur; i++)
        vis_put(s_vis_cur[i]);
    s_vis_ncur = 0;
}

/* Writes every report whose queries have finished, oldest first, and stops at
 * the first that has not: the GPU finishes them in order. With `all`, waits. */
static void vis_poll(int all)
{
    while (s_vis_count) {
        VisReport *r = &s_vis[s_vis_head];
        UINT64 sum = 0;
        int i;

        for (i = 0; i < r->n; i++) {
            UINT64 n = 0;
            HRESULT hr;
            while ((hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)r->q[i], &n, sizeof n,
                                                     all ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE && all)
                ;
            if (hr != S_OK)
                return;
            sum += n;
        }
        sum = (UINT64)((double)sum / r->scale + 0.5);
        if (!sum)
            s_vis_stats.zero++;
        vis_write(r->at, sum > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sum);
        for (i = 0; i < r->n; i++)
            vis_put(r->q[i]);
        s_vis_head = (s_vis_head + 1) % VIS_REPORTS;
        s_vis_count--;
    }
}

static void d3d11_zpass_clear(void)
{
    if (!s_dev)
        return;
    vis_close();
    vis_drop_cur();
}

static void d3d11_zpass_enable(int on)
{
    if (!s_dev)
        return;
    if (!on) {
        vis_close();
        return;
    }
    if (s_vis_open)
        return;
    if (!(s_vis_open = vis_get())) {    /* pool spent: make room */
        vis_poll(1);
        s_vis_open = vis_get();
    }
    if (s_vis_open)
        ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s_vis_open);
}

static void d3d11_zpass_report(uint32_t at)
{
    VisReport *r;
    int i, open = s_vis_open != NULL;

    s_vis_stats.reports++;
    if (!s_dev) {                       /* nothing drawn yet: call it visible */
        vis_write(at, 640u * 480u);
        return;
    }
    vis_close();
    if (!s_vis_ncur) {                  /* nothing was counted */
        vis_write(at, 0);
        s_vis_stats.zero++;
    } else {
        if (s_vis_count == VIS_REPORTS)
            vis_poll(1);
        r = &s_vis[(s_vis_head + s_vis_count++) % VIS_REPORTS];
        r->at = at;
        r->scale = s_cur_target ? s_cur_target->sx * s_cur_target->sy : s_sx * s_sy;
        if (r->scale <= 0.0f)
            r->scale = 1.0f;
        r->n = s_vis_ncur;
        for (i = 0; i < s_vis_ncur; i++)
            r->q[i] = s_vis_cur[i];
        s_vis_ncur = 0;
        if ((uint32_t)s_vis_count > s_vis_stats.max_pending)
            s_vis_stats.max_pending = (uint32_t)s_vis_count;
    }
    if (open)                           /* still counting: a fresh stretch */
        d3d11_zpass_enable(1);
}

void gpu_d3d11_register(void)
{
    static const Nv2aBackend backend = {
        d3d11_clear, d3d11_draw, NULL, d3d11_present, 1,
        d3d11_zpass_clear, d3d11_zpass_enable, d3d11_zpass_report
    };
    const char *e = getenv("RECOMP_GPU");

    if (e && !strcmp(e, "sw"))
        return;
    nv2a_backend_register(&backend);
    fprintf(stderr, "[D3D11] back end registered\n");
}
