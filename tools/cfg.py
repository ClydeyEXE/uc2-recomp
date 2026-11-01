"""Per-function control-flow recovery shared by the lifter and analysis scripts."""
import json
import os
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from discover import DATA_SECTIONS, Image  # noqa: E402
from xbeinfo import Xbe  # noqa: E402

STOP = {"ret", "retf", "iret", "iretd", "ud2", "hlt", "int3"}
MAX_TABLE = 4096


class Function:
    def __init__(self, va):
        self.va = va
        self.ins = {}          # va -> capstone instruction
        self.labels = set()    # va of internal branch targets
        self.tables = {}       # va of indirect jmp -> list of target VAs
        self.tails = set()     # external jmp targets
        self.calls = set()     # direct call targets
        self.bad = False       # decoding fell off the rails


class Program:
    def __init__(self, xbe_path, functions_json):
        self.xbe = Xbe(open(xbe_path, "rb").read())
        self.img = Image(self.xbe)
        self.code_secs = [s for s in self.xbe.sections
                          if s["name"] not in DATA_SECTIONS and s["flags"] & 4]
        self.funcs = json.load(open(functions_json))
        self.starts = {f["va"] for f in self.funcs}
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    def in_code(self, va):
        return any(s["va"] <= va < s["va"] + s["vsize"] for s in self.code_secs)

    def _table(self, base):
        out = []
        while len(out) < MAX_TABLE:
            b = self.img.read(base + 4 * len(out), 4)
            if len(b) < 4:
                break
            t = struct.unpack("<I", b)[0]
            if not self.in_code(t):
                break
            out.append(t)
        return out

    def decode(self, va):
        f = Function(va)
        work = [va]
        while work:
            pc = work.pop()
            while pc not in f.ins:
                if not self.in_code(pc):
                    f.bad = True
                    break
                ins = next(self.md.disasm(self.img.read(pc, 16), pc, 1), None)
                if ins is None:
                    f.bad = True
                    break
                f.ins[pc] = ins
                nxt = pc + ins.size
                if ins.mnemonic in STOP:
                    break
                g = set(ins.groups)
                if x86.X86_GRP_CALL in g:
                    op = ins.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        f.calls.add(op.imm & 0xFFFFFFFF)
                elif x86.X86_GRP_JUMP in g:
                    op = ins.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        if t in self.starts and t != va:
                            f.tails.add(t)
                        else:
                            f.labels.add(t)
                            work.append(t)
                    elif op.type == x86.X86_OP_MEM and op.mem.index and op.mem.scale == 4 \
                            and op.mem.base == 0:
                        ts = self._table(op.mem.disp & 0xFFFFFFFF)
                        f.tables[pc] = ts
                        for t in ts:
                            f.labels.add(t)
                            work.append(t)
                    if ins.mnemonic == "jmp":
                        break
                pc = nxt
        return f
