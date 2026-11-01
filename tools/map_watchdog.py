"""Summarize a watchdog dump. argv: <err log> <functions.json>"""
import bisect
import json
import re
import sys

lines = open(sys.argv[1], errors="replace").read().splitlines()
funcs = json.load(open(sys.argv[2]))
rows = sorted((int(f["start"], 16), int(f["end"], 16), f["name"], f["section"]) for f in funcs)
starts = [r[0] for r in rows]


def where(va):
    i = bisect.bisect_right(starts, va) - 1
    if i >= 0 and rows[i][0] <= va < rows[i][1]:
        return f"{rows[i][2]}+0x{va - rows[i][0]:X} [{rows[i][3]}]"
    return None


idx = next((i for i, l in enumerate(lines) if re.search(r"watchdog", l, re.I)), None)
print("first watchdog line:", idx)
if idx is None:
    sys.exit()
body = lines[idx:]
print("--- dump, stack words removed")
shown = 0
for l in body:
    if re.match(r"\s+GS [0-9A-F]{8} ", l):
        continue
    print(l[:170])
    shown += 1
    if shown > 70:
        print("...")
        break
print("--- stack words that point into code, innermost first")
n = 0
for l in body:
    m = re.match(r"\s+GS ([0-9A-F]{8}) ([0-9A-F]{8})", l)
    if not m:
        continue
    w = where(int(m.group(2), 16))
    if w:
        print(f"  sp={m.group(1)} val={m.group(2)} {w}")
        n += 1
        if n >= 30:
            break
print("--- addresses named in the non-stack dump")
seen = set()
for l in body:
    if re.match(r"\s+GS ", l):
        continue
    for h in re.findall(r"0x([0-9A-Fa-f]{8})", l):
        v = int(h, 16)
        w = where(v)
        if w and v not in seen:
            seen.add(v)
            print(f"  0x{v:08X} {w}")
