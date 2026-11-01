"""Add the game's catch continuations to the lifter's function list.

A catch funclet returns the address where its function resumes, and that address
needs an entry point to be reached from src/recomp_manual.c's exception dispatch.
argv: <functions.json> <output json>
"""
import json
import sys

CONTINUATIONS = [
    0x00049768, 0x0005692D, 0x00058CF3, 0x00059316, 0x00059405, 0x0005957D, 0x00059663,
    0x0005985B, 0x0005B778, 0x000C79AA, 0x0020BBFB,
]

funcs = json.load(open(sys.argv[1]))
owners = sorted((int(f["start"], 16), int(f["end"], 16), f) for f in funcs)
for va in CONTINUATIONS:
    owner = next(f for start, end, f in owners if start <= va < end)
    end = int(owner["end"], 16)
    funcs.append({
        "start": f"0x{va:08X}", "end": owner["end"], "size": end - va,
        "name": f"sub_{va:08X}", "section": owner["section"], "confidence": 0.9,
        "detection_method": "eh_continuation", "num_instructions": 0,
        "has_prologue": False, "calls_to": [], "called_by": [],
    })
json.dump(funcs, open(sys.argv[2], "w"))
print(f"added {len(CONTINUATIONS)} entries, {len(funcs)} functions")
