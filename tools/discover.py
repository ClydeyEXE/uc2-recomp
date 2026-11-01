"""Kernel import naming + function discovery for default.xbe.

Usage: python -I tools/discover.py <default.xbe> <outdir>
Writes <outdir>/kernel_imports.json and <outdir>/functions.json.

Discovery is recursive descent from the entry point, call targets and code pointers found in
data, with jump-table following. Library sections (D3D, DSOUND, ...) are tagged separately so
they can be replaced by PC implementations instead of lifted.
"""
import json
import os
import re
import struct
import sys
from collections import deque

import capstone
from capstone import x86

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbeinfo import Xbe  # noqa: E402

# DOLBY has no x86 references and its bytes do not look like x86. XON_RD is only ever reached
# through data pointers, never a direct call. Both are treated as data blobs.
DATA_SECTIONS = {".rdata", ".data", ".data1", "$$XTIMAGE", ".XTLID", "DOLBY", "XON_RD"}
DEF_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "xboxkrnl.exe.def")


def load_kernel_names():
    names = {}
    for line in open(DEF_PATH):
        # stdcall: Name@N, fastcall: @Name@N, data export: Name ... DATA
        m = re.match(r"\s*(@?)(\w+?)(?:@(\d+))?\s+@\s*(\d+)(.*)", line)
        if m:
            kind = "data" if "DATA" in m.group(5) else "fastcall" if m.group(1) else "stdcall"
            names[int(m.group(4))] = (m.group(2), int(m.group(3)) if m.group(3) else None, kind)
    return names


class Image:
    """Flat memory view of the XBE sections keyed by VA."""

    def __init__(self, xbe):
        self.xbe = xbe
        self.secs = xbe.sections
        self.mem = {}
        for s in self.secs:
            blob = bytearray(xbe.data[s["raw"]:s["raw"] + s["rsize"]])
            blob += b"\0" * (s["vsize"] - len(blob))
            self.mem[s["index"]] = bytes(blob)

    def sec_at(self, va):
        for s in self.secs:
            if s["va"] <= va < s["va"] + s["vsize"]:
                return s
        return None

    def read(self, va, n):
        s = self.sec_at(va)
        if not s:
            return b""
        o = va - s["va"]
        return self.mem[s["index"]][o:o + n]

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else None


def main(argv):
    xbe = Xbe(open(argv[1], "rb").read())
    out = argv[2]
    os.makedirs(out, exist_ok=True)
    img = Image(xbe)
    knames = load_kernel_names()

    # --- kernel imports: slot VA -> name
    slots = {}
    imports = []
    for i, ordinal in enumerate(xbe.kernel_imports):
        n, argbytes, kind = knames.get(ordinal, (f"ordinal_{ordinal}", None, "unknown"))
        slot = xbe.thunk + 4 * i
        slots[slot] = n
        imports.append({"ordinal": ordinal, "name": n, "slot": slot, "arg_bytes": argbytes, "kind": kind})
    json.dump(imports, open(os.path.join(out, "kernel_imports.json"), "w"), indent=1)

    code_secs = [s for s in xbe.sections if s["name"] not in DATA_SECTIONS and s["flags"] & 4]
    in_code = lambda va: any(s["va"] <= va < s["va"] + s["vsize"] for s in code_secs)  # noqa: E731

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    funcs = {}        # start -> source
    seen = set()      # decoded instruction starts
    covered = set()   # decoded bytes
    tables = 0
    bad = 0
    work = deque()    # (addr, is_function_start)

    def add_func(va, src):
        if in_code(va) and va not in funcs:
            funcs[va] = src
            work.append(va)

    add_func(xbe.entry, "entry")

    def run():
        nonlocal tables, bad
        while work:
            pc = work.popleft()
            while in_code(pc) and pc not in seen:
                code = img.read(pc, 16)
                ins = next(md.disasm(code, pc, 1), None)
                if ins is None:
                    bad += 1
                    break
                seen.add(pc)
                covered.update(range(pc, pc + ins.size))
                g = set(ins.groups)
                nxt = pc + ins.size
                if ins.mnemonic in ("ret", "retf", "iret", "ud2", "hlt", "int3"):
                    break
                if x86.X86_GRP_JUMP in g or x86.X86_GRP_CALL in g:
                    op = ins.operands[0] if ins.operands else None
                    is_call = x86.X86_GRP_CALL in g
                    if op is not None and op.type == x86.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        if is_call:
                            add_func(t, "call")
                        else:
                            work.append(t)
                    elif op is not None and op.type == x86.X86_OP_MEM and not is_call:
                        # jmp [idx*4 + table] : follow the jump table
                        m = op.mem
                        if m.index and m.scale == 4 and m.base == 0 and img.sec_at(m.disp & 0xFFFFFFFF):
                            base = m.disp & 0xFFFFFFFF
                            k = 0
                            while k < 4096:
                                t = img.u32(base + 4 * k)
                                if t is None or not in_code(t):
                                    break
                                work.append(t)
                                k += 1
                            if k:
                                tables += 1
                    if ins.mnemonic == "jmp":
                        break
                pc = nxt

    run()
    from_flow = len(funcs)

    # Code pointers stored in data (vtables, callbacks) -> function candidates
    ptr_starts = set()
    for s in xbe.sections:
        if s["name"] in DATA_SECTIONS:  # not flags&1: XDK code sections are writable too
            blob = img.mem[s["index"]]
            for o in range(0, len(blob) - 3, 4):
                v = struct.unpack_from("<I", blob, o)[0]
                if in_code(v) and v not in funcs:
                    first = img.read(v, 3)
                    # plausible prologues: push ebp/ebx/esi/edi, sub esp, mov, push imm, xor, ...
                    if first[:1] in (b"\x55", b"\x53", b"\x56", b"\x57", b"\x83", b"\x8b", b"\x81",
                                     b"\x6a", b"\x68", b"\xb8", b"\x33", b"\xa1", b"\x8a", b"\x64", b"\x51"):
                        ptr_starts.add(v)
    for v in sorted(ptr_starts):
        add_func(v, "pointer")
    run()

    # --- stats and output
    result = []
    for va in sorted(funcs):
        s = img.sec_at(va)
        result.append({"va": va, "src": funcs[va], "section": s["name"]})
    json.dump(result, open(os.path.join(out, "functions.json"), "w"))

    print(f"kernel imports named: {sum(1 for i in imports if not i['name'].startswith('ordinal_'))}/{len(imports)}")
    print(f"functions: {len(funcs)} (flow {from_flow}, +pointer-seeded {len(funcs) - from_flow}), jump tables: {tables}, decode failures: {bad}")
    print(f"{'section':<10} {'size':>9} {'covered':>9} {'pct':>6} {'funcs':>6}")
    for s in code_secs:
        cov = sum(1 for a in range(s["va"], s["va"] + s["vsize"]) if a in covered)
        nf = sum(1 for r in result if r["section"] == s["name"])
        print(f"{s['name']:<10} {s['vsize']:>9} {cov:>9} {100 * cov / s['vsize']:>5.1f}% {nf:>6}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
