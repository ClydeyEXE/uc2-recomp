"""Disassemble the XBE, or list its sections.

    python -I tools/dis.py sections
    python -I tools/dis.py 386B30 170       (hex address and byte count)

Run from the repo root. Reads assets/default.xbe; do not commit what it prints.
"""
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

d = open("assets/default.xbe", "rb").read()
base = struct.unpack_from("<I", d, 0x104)[0]
nsec, sechdr = struct.unpack_from("<II", d, 0x11C)
secs = []
for i in range(nsec):
    fl, va, vs, ra, rs, nameaddr = struct.unpack_from("<IIIIII", d, sechdr - base + i * 56)
    name = d[nameaddr - base:nameaddr - base + 12].split(b"\0")[0].decode(errors="replace")
    secs.append((name, va, vs, ra, rs))

if sys.argv[1] == "sections":
    for name, va, vs, ra, rs in secs:
        print("%-10s %08X..%08X" % (name, va, va + vs))
    sys.exit(0)

start, count = int(sys.argv[1], 16), int(sys.argv[2], 16)
for name, va, vs, ra, rs in secs:
    if va <= start < va + rs:
        at = ra + start - va
        for ins in Cs(CS_ARCH_X86, CS_MODE_32).disasm(d[at:at + count], start):
            print("%08X  %s %s" % (ins.address, ins.mnemonic, ins.op_str))
        break
else:
    sys.exit("address is in no section")
