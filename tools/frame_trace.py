"""Summarise a log made with RECOMP_FRAME_TRACE_ALL.

    python -I tools/frame_trace.py <log>            one line per frame
    python -I tools/frame_trace.py <log> --walk     surfaces, clears, flips and walks, in order

A healthy run has nearly the same counts from frame to frame while the view is still,
one clear pair per frame, and no surface at address 0.
"""
import collections
import re
import sys

BATCH = re.compile(r"\[FTRACE\] (?:PRG|FIX) prim (\d+) n (\d+) surf (\w+) (\d+x\d+).*\| tris (\d+) px \d+"
                   r" \| fog (\d+)")


def summary(path):
    frames, cur = [], None
    for line in open(path, errors="replace"):
        if "[FTRACE] frame begins" in line:
            cur = {"flip": line.split("flip ")[1].split(")")[0], "b": [], "clears": 0}
            frames.append(cur)
        elif cur is not None:
            m = BATCH.search(line)
            if m:
                cur["b"].append(m.groups())
            elif "[FTRACE] clear " in line:
                cur["clears"] += 1
            elif "end of frame" in line:
                cur = None
    print("frames", len(frames))
    for f in frames:
        surfaces = collections.Counter(x[2] for x in f["b"])
        main = surfaces.most_common(1)[0] if surfaces else ("-", 0)
        print("flip %s: %d batches, %d indices, %d tris, %d clears, %d quad batches, main %s x%d" % (
            f["flip"], len(f["b"]), sum(int(x[1]) for x in f["b"]), sum(int(x[4]) for x in f["b"]),
            f["clears"], sum(1 for x in f["b"] if x[0] == "8"), main[0], main[1]))


def walk(path):
    last, count = None, 0

    def flush():
        nonlocal last, count
        if last:
            print("   %3d batches into %s" % (count, last))
        last, count = None, 0

    for line in open(path, errors="replace"):
        if "[FTRACE]" not in line:
            continue
        m = BATCH.search(line)
        if m:
            key = m.group(3) + " " + m.group(4)
            if key != last:
                flush()
                last = key
            count += 1
        elif re.search(r"\[FTRACE\] (frame begins|clear |FLIP|P_READ|P_WRITE|walk)", line):
            flush()
            print(line.strip()[9:][:120])
    flush()


if __name__ == "__main__":
    (walk if "--walk" in sys.argv else summary)(sys.argv[1])
