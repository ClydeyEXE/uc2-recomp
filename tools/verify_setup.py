"""Checks default.xbe is intact. Usage: python -I tools/verify_setup.py <extracted dir>

Verifies each section against the SHA-1 digest stored in its header.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbeinfo import Xbe, _u32  # noqa: E402


def check_sections(path):
    data = open(path, "rb").read()
    x = Xbe(data)
    sec_addr = _u32(data, 0x120) - x.base
    bad = 0
    for s in x.sections:
        stored = data[sec_addr + s["index"] * 56 + 36:sec_addr + s["index"] * 56 + 56]
        raw = data[s["raw"]:s["raw"] + s["rsize"]]
        h = hashlib.sha1(struct.pack("<I", s["rsize"]) + raw).digest()
        ok = h == stored
        bad += not ok
        print(f"  {s['name']:<10} {'ok' if ok else 'MISMATCH'}")
    print(f"sections: {len(x.sections) - bad}/{len(x.sections)} match; file size {len(data)}; "
          f"sha1 {hashlib.sha1(data).hexdigest()}")
    return bad


def main(argv):
    root = argv[1]
    bad = check_sections(os.path.join(root, "default.xbe"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
