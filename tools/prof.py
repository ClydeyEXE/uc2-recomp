"""Sampling profiler for a running ucc2_recomp.exe, using only Win32 calls.

Suspends each thread in turn, reads its instruction pointer, and after the time
is up names the addresses through the build's PDB. Reports each busy thread's
share of samples and its top functions, then the same over all threads.

    python -I tools/prof.py --secs 30 [--wait-secs 200] [--hz 500] [--lines 12] [--out report.txt]
                            [--callers NAME]

--callers NAME lists, for the samples whose function name contains NAME, the code
addresses found on the stack above them (a scan of the first 4 KB for values inside
a loaded module, so stale return addresses show too): which caller each comes from.

A thread sampled inside a wait still counts, under the kernel's wait function,
so "ntdll!NtWaitForSingleObject" at the top of a thread means it is idle.

Most of the game's generated code is built without /Zi, so the PDB names only
its public functions, and dbghelp names an address in an unnamed function after
the public before it: older reports blamed "sscanf" for recomp_fp_round24 (the
x87 rounding helper). So an address in the exe is first placed in its function
by the exe's .pdata (start and end of every function with unwind data; a chunk
the compiler split off a function counts as that function), and a function
whose start has no symbol of its own is reported as "symbol+0xOFF?", the
nearest public plus the offset, for host disassembly (dumpbin /disasm /range)
to identify. The project's own sources and recomp_0000.c, whose copies of the
runtime header's static helpers are the ones linked, are built with /Zi
(CMakeLists.txt), so those have names. A leaf function with no unwind data
keeps the nearest public's name.
"""
import argparse
import bisect
import collections
import ctypes
import ctypes.wintypes as w
import os
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)
k32.OpenProcess.restype = w.HANDLE
k32.OpenThread.restype = w.HANDLE
k32.CreateToolhelp32Snapshot.restype = w.HANDLE
k32.SuspendThread.argtypes = [w.HANDLE]
k32.ResumeThread.argtypes = [w.HANDLE]
k32.GetThreadContext.argtypes = [w.HANDLE, ctypes.c_void_p]
k32.GetThreadTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
k32.CloseHandle.argtypes = [w.HANDLE]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
dbg.SymInitialize.argtypes = [w.HANDLE, ctypes.c_char_p, w.BOOL]
dbg.SymFromAddr.argtypes = [w.HANDLE, ctypes.c_ulonglong,
                            ctypes.POINTER(ctypes.c_ulonglong), ctypes.c_void_p]
dbg.SymLoadModuleEx.argtypes = [w.HANDLE, w.HANDLE, ctypes.c_char_p, ctypes.c_char_p,
                                ctypes.c_ulonglong, w.DWORD, ctypes.c_void_p, w.DWORD]
dbg.SymLoadModuleEx.restype = ctypes.c_ulonglong

CONTEXT_CONTROL_AMD64 = 0x100001
RIP_OFFSET = 0xF8
RSP_OFFSET = 0x98
STACK_SCAN = 4096


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ThreadID", w.DWORD),
                ("th32OwnerProcessID", w.DWORD), ("tpBasePri", w.LONG),
                ("tpDeltaPri", w.LONG), ("dwFlags", w.DWORD)]


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", w.DWORD),
                ("cntThreads", w.DWORD), ("th32ParentProcessID", w.DWORD),
                ("pcPriClassBase", w.LONG), ("dwFlags", w.DWORD),
                ("szExeFile", ctypes.c_char * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("th32ModuleID", w.DWORD), ("th32ProcessID", w.DWORD),
                ("GlblcntUsage", w.DWORD), ("ProccntUsage", w.DWORD),
                ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)), ("modBaseSize", w.DWORD),
                ("hModule", w.HMODULE), ("szModule", ctypes.c_char * 256),
                ("szExePath", ctypes.c_char * 260)]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(2, 0)
    e = PROCESSENTRY32()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32First(w.HANDLE(snap), ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.decode(errors="replace").lower() == name:
            pid = e.th32ProcessID
        ok = k32.Process32Next(w.HANDLE(snap), ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def thread_ids(pid):
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(w.HANDLE(snap), ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(w.HANDLE(snap), ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def ft(f):
    return ((f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7


def thread_cpu(h):
    a, b, c, d = w.FILETIME(), w.FILETIME(), w.FILETIME(), w.FILETIME()
    if not k32.GetThreadTimes(h, ctypes.byref(a), ctypes.byref(b), ctypes.byref(c), ctypes.byref(d)):
        return 0.0
    return ft(c) + ft(d)


def pdata_ranges(path):
    """Sorted (start RVA, end RVA, owning function's start RVA) of every entry in the PE's
    exception table."""
    with open(path, "rb") as f:
        data = f.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:          # PE32+ only
        return []
    exc_rva, exc_size = struct.unpack_from("<II", data, opt + 112 + 3 * 8)
    sect = opt + struct.unpack_from("<H", data, pe + 20)[0]
    off = None
    for i in range(nsec):
        va, raw_size, raw_ptr = struct.unpack_from("<III", data, sect + 40 * i + 12)
        if va <= exc_rva < va + raw_size:
            off = raw_ptr + exc_rva - va
    if off is None:
        return []

    def file_off(rva):
        for i in range(nsec):
            va, raw_size, raw_ptr = struct.unpack_from("<III", data, sect + 40 * i + 12)
            if va <= rva < va + raw_size:
                return raw_ptr + rva - va
        return None

    out = []
    for i in range(exc_size // 12):
        b, e, u = struct.unpack_from("<III", data, off + 12 * i)
        # A chunk the compiler split off a function (UNW_FLAG_CHAININFO) carries
        # its parent's entry after its unwind codes: count it as the parent.
        top, seen = b, 0
        while seen < 8:
            uo = file_off(u & ~1)
            if uo is None or not (data[uo] >> 3) & 4:
                break
            codes = data[uo + 2]
            top, _, u = struct.unpack_from("<III", data, uo + 4 + 2 * (codes + (codes & 1)))
            seen += 1
        out.append((b, e, top))
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, default=30)
    ap.add_argument("--wait-secs", type=float, default=0)
    ap.add_argument("--hz", type=float, default=500)
    ap.add_argument("--exe", default="ucc2_recomp.exe")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--lines", type=int, default=12, help="functions listed per thread")
    ap.add_argument("--out")
    ap.add_argument("--callers", help="list stack callers of the functions whose name contains this")
    args = ap.parse_args()

    deadline = time.time() + 900
    pid = None
    while pid is None and time.time() < deadline:
        pid = find_pid(args.exe.lower())
        if pid is None:
            time.sleep(0.5)
    if pid is None:
        sys.exit("process not found")
    time.sleep(args.wait_secs)

    proc = k32.OpenProcess(0x1F0FFF, False, pid)
    if not proc:
        sys.exit("cannot open process")

    # A 16-byte aligned CONTEXT.
    raw = ctypes.create_string_buffer(1232 + 16)
    addr = ctypes.addressof(raw)
    ctx = addr + ((16 - addr % 16) % 16)

    handles = {}
    samples = collections.defaultdict(collections.Counter)
    stacks = collections.defaultdict(list)            # rip: [tuple of code addresses on the stack]
    mods = []
    if args.callers:
        snap = k32.CreateToolhelp32Snapshot(0x18, pid)
        me = MODULEENTRY32()
        me.dwSize = ctypes.sizeof(me)
        ok = k32.Module32First(w.HANDLE(snap), ctypes.byref(me))
        while ok:
            b = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
            mods.append((b, b + me.modBaseSize))
            ok = k32.Module32Next(w.HANDLE(snap), ctypes.byref(me))
        k32.CloseHandle(snap)
    stk = ctypes.create_string_buffer(STACK_SCAN)
    got = ctypes.c_size_t()
    start_cpu = {}
    t_end = time.time() + args.secs
    period = 1.0 / args.hz
    n = 0
    tids, tids_at = [], 0.0
    while time.time() < t_end:
        if time.time() - tids_at > 1.0:
            tids, tids_at = thread_ids(pid), time.time()
        for tid in tids:
            if tid not in handles:
                h = k32.OpenThread(0x005A, False, tid)   # suspend, context, query
                if not h:
                    continue
                handles[tid] = h
                start_cpu[tid] = thread_cpu(h)
            h = handles[tid]
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            struct.pack_into("<I", (ctypes.c_char * 64).from_address(ctx), 0x30, CONTEXT_CONTROL_AMD64)
            if k32.GetThreadContext(h, ctx):
                rip = struct.unpack_from("<Q", (ctypes.c_char * 256).from_address(ctx), RIP_OFFSET)[0]
                samples[tid][rip] += 1
                if mods:
                    rsp = struct.unpack_from("<Q", (ctypes.c_char * 256).from_address(ctx), RSP_OFFSET)[0]
                    if k32.ReadProcessMemory(proc, ctypes.c_void_p(rsp), stk, STACK_SCAN, ctypes.byref(got)):
                        vals = struct.unpack_from("<%dQ" % (got.value // 8), stk.raw)
                        stacks[rip].append(tuple(v for v in vals if any(a <= v < b for a, b in mods))[:24])
            k32.ResumeThread(h)
        n += 1
        time.sleep(period)
    cpu = {tid: thread_cpu(h) - start_cpu[tid] for tid, h in handles.items()}
    secs = args.secs

    # Names, grouped by function.
    dbg.SymSetOptions(0x00000002 | 0x00000004)        # undname, deferred loads
    dbg.SymInitialize(proc, os.path.abspath("build/game").encode(), False)
    # Loaded by hand: invading this process with SymInitialize finds no modules.
    snap = k32.CreateToolhelp32Snapshot(0x18, pid)    # modules, 32 and 64 bit
    m = MODULEENTRY32()
    m.dwSize = ctypes.sizeof(m)
    exe_base = exe_size = 0
    ok = k32.Module32First(w.HANDLE(snap), ctypes.byref(m))
    while ok:
        if m.szModule.decode(errors="replace").lower() == args.exe.lower():
            exe_base = ctypes.cast(m.modBaseAddr, ctypes.c_void_p).value
            exe_size = m.modBaseSize
            exe_path = m.szExePath.decode(errors="replace")
        dbg.SymLoadModuleEx(proc, None, m.szExePath, None,
                            ctypes.c_ulonglong(ctypes.cast(m.modBaseAddr, ctypes.c_void_p).value),
                            m.modBaseSize, None, 0)
        ok = k32.Module32Next(w.HANDLE(snap), ctypes.byref(m))
    k32.CloseHandle(snap)
    buf = ctypes.create_string_buffer(88 + 256)
    cache = {}
    funcs = pdata_ranges(exe_path) if exe_base else []
    starts = [b for b, _, _ in funcs]

    def sym(addr):
        struct.pack_into("<II", buf, 0, 88, 0)
        struct.pack_into("<I", buf, 80, 255)          # MaxNameLen
        disp = ctypes.c_ulonglong()
        if not dbg.SymFromAddr(proc, addr, ctypes.byref(disp), buf):
            return None, 0
        namelen = struct.unpack_from("<I", buf, 76)[0]
        return buf.raw[84:84 + namelen].decode(errors="replace"), disp.value

    def name(rip):
        if rip in cache:
            return cache[rip]
        addr = rip
        if exe_base <= rip < exe_base + exe_size and starts:
            i = bisect.bisect_right(starts, rip - exe_base) - 1
            if i >= 0 and rip - exe_base < funcs[i][1]:
                addr = exe_base + funcs[i][2]          # the function's own start
        s, disp = sym(addr)
        if s is None:
            s = "?%X" % (rip >> 12)
        elif addr != rip and disp:
            s = "%s+0x%X?" % (s, disp)                 # a function with no symbol of its own
        cache[rip] = s
        return s

    lines = ["%d sweeps over %.0f s, pid %d" % (n, secs, pid)]
    total = collections.Counter()
    order = sorted(samples, key=lambda t: -cpu.get(t, 0))
    total_cpu = sum(cpu.values())
    lines.append("total cpu %.2f cores" % (total_cpu / secs))
    for tid in order:
        c = cpu.get(tid, 0)
        by_fn = collections.Counter()
        for rip, cnt in samples[tid].items():
            by_fn[name(rip)] += cnt
        all_n = sum(by_fn.values())
        # Weight functions across threads by the CPU the thread really used.
        for fn, cnt in by_fn.items():
            total[fn] += cnt / all_n * c
        if c / secs < 0.02:
            continue
        lines.append("")
        lines.append("thread %d: %.0f%% of a core" % (tid, 100 * c / secs))
        for fn, cnt in by_fn.most_common(args.lines):
            lines.append("  %5.1f%%  %s" % (100.0 * cnt / all_n, fn))
    lines.append("")
    lines.append("all threads, by CPU time")
    for fn, t in total.most_common(args.top):
        lines.append("  %5.1f%%  %s" % (100.0 * t / max(total_cpu, 1e-9), fn))
    if args.callers:
        callers = collections.Counter()
        hits = 0
        for rip, sts in stacks.items():
            if args.callers.lower() not in name(rip).lower():
                continue
            for st in sts:
                hits += 1
                for fn in dict.fromkeys(name(v) for v in st):   # each caller once a sample
                    callers[fn] += 1
        lines.append("")
        lines.append("on the stack in %d samples in a function matching %r" % (hits, args.callers))
        for fn, c in callers.most_common(40):
            lines.append("  %5.1f%%  %s" % (100.0 * c / max(hits, 1), fn))
    text = "\n".join(lines)
    print(text)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")


if __name__ == "__main__":
    main()
