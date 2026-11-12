"""Read one game log and say whether the run worked, with numbers.

    python -I tools/check_run.py <log> [--no-expect-match] [--min-fps N] [--strict-watchdog] [--json out.json]

Looks for the maps loaded, the [FRAMES] reports in the menu and in the match,
hitches, the first [CRASH], a [WATCHDOG] dump, untranslated instructions reached,
failed opens and scripted pad steps. Exits 0 on pass, 1 on fail, 2 if the log
cannot be read.

The log has no clock. Positions are line numbers, and times are seconds since
the first present, summed from the [FRAMES] windows (each covers about 10 s)
and the [HITCH] lines, interpolated by line in between. RECOMP_WATCHDOG_SECS
counts from process start, a few seconds earlier.

A watchdog dump is how a scripted run ends (run_match.ps1 sets it to -Secs), so
it passes by default. It counts as a hang, and fails, when the frames stopped:
the [FRAMES] windows account for much less of the watchdog's time than boot
can explain, or the last window ran under 2 fps.

    from check_run import analyse; r = analyse("run.txt", min_match_fps=50)
"""
import argparse
import bisect
import json
import re
import statistics
import sys

MENU = "menu"
# Boot to first present, plus up to one unreported 10 s window.
HANG_SLACK_SECS = 40.0
STALL_FPS = 2.0
UNIMPL_CAP = 50   # recomp_unimpl prints the first 50 hits, then goes quiet

RE_FRAMES = re.compile(
    r"\[FRAMES\] (\d+) in ([\d.]+) s \(([\d.]+) fps\): ms p50 ([\d.]+) p90 ([\d.]+) p99 ([\d.]+) max ([\d.]+), "
    r">25 ms (\d+), >50 ms (\d+) \| per frame: d3d11 (-?[\d.]+), present (-?[\d.]+), "
    r"other walker cpu (-?[\d.]+), walker idle (-?[\d.]+)(?: \(limit (-?[\d.]+)\))?"
    r"(?: \| lows: 1% ([\d.]+) ms, 0\.1% ([\d.]+) ms)?")
RE_HITCH = re.compile(r"^\[HITCH\] (\d+) ms frame at ([\d.]+) s")
RE_MAP = re.compile(r"\[FILE\] [A-Za-z]:\\XboxMaps\\([^\\\s]+)\.lin\b", re.I)
RE_FAIL = re.compile(r"\[FILE\] (.+?) \(disp [^)]*\) -> (0x[0-9A-Fa-f]+) FAILED \(win32 err=(\d+)(?: (\w+))?\)")
RE_UNIMPL = re.compile(r"\[UNIMPL\] untranslated instruction (REACHED)?:? ?`([^`]*)` at (0x[0-9A-Fa-f]+)")
RE_WATCHDOG = re.compile(r"\[WATCHDOG\] no exit after (\d+)s")
RE_PAD_STEP = re.compile(r"PAD: t=(\d+) ms step (\d+)")
RE_PAD_SCRIPT = re.compile(r"PAD: script of (\d+) step")
RE_FILE_SUMMARY = re.compile(r"\[FILE\] summary: (.*)")


def _frames(m, line):
    g = m.groups()
    return {"line": line, "frames": int(g[0]), "secs": float(g[1]), "fps": float(g[2]),
            "p50": float(g[3]), "p90": float(g[4]), "p99": float(g[5]), "max": float(g[6]),
            "over25": int(g[7]), "over50": int(g[8]), "d3d11": float(g[9]), "present": float(g[10]),
            "walker_cpu": float(g[11]), "walker_idle": float(g[12]),
            # The walker thread's whole CPU time a frame: "other walker cpu" alone can read
            # negative, as d3d11 and present are wall times that include waits.
            "walker_total": float(g[9]) + float(g[10]) + float(g[11]),
            # Mean ms of the slowest 1% and 0.1% of the window's frames; None in older logs.
            "low1": float(g[14]) if g[14] else None, "low01": float(g[15]) if g[15] else None}


def _aggregate(wins):
    if not wins:
        return {"lines": 0, "frames": 0, "secs": 0.0, "mean_fps": None, "worst_fps": None,
                "settled_fps": None, "median_p50_ms": None, "max_p99_ms": None, "max_frame_ms": None,
                "walker_cpu_ms": None, "d3d11_ms": None, "present_ms": None, "first_line": None,
                "max_low1_ms": None, "max_low01_ms": None}
    frames = sum(w["frames"] for w in wins)
    secs = sum(w["secs"] for w in wins)

    def per_frame(k):
        return round(sum(w[k] * w["frames"] for w in wins) / frames, 2) if frames else None
    # The first window after a map load holds the load itself.
    rest = wins[1:]
    rf, rs = sum(w["frames"] for w in rest), sum(w["secs"] for w in rest)
    return {"lines": len(wins), "frames": frames, "secs": round(secs, 1),
            "mean_fps": round(frames / secs, 1) if secs else None,
            "worst_fps": min(w["fps"] for w in wins),
            "settled_fps": round(rf / rs, 1) if rs else None,
            "median_p50_ms": statistics.median(w["p50"] for w in wins),
            "max_p99_ms": max(w["p99"] for w in wins),
            "max_frame_ms": max(w["max"] for w in wins),
            "walker_cpu_ms": per_frame("walker_cpu"), "d3d11_ms": per_frame("d3d11"),
            "present_ms": per_frame("present"), "first_line": wins[0]["line"],
            "max_low1_ms": max((w["low1"] for w in wins if w["low1"] is not None), default=None),
            "max_low01_ms": max((w["low01"] for w in wins if w["low01"] is not None), default=None)}


def analyse(log_path, expect_match=True, allow_watchdog=True, min_match_fps=None):
    """Return a dict describing the run in log_path. Raises OSError if it cannot be read."""
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()

    maps, frames, hitches, anchors = [], [], [], []
    crash = crash_line = crash_va = None
    crash_count = 0
    watchdog = watchdog_secs = watchdog_line = None
    exit_kind, exit_line = None, None
    unimpl, unimpl_lines = {}, 0
    failed = {}
    pad_steps, pad_script, pad_live = 0, None, 0
    file_summary = None
    first_present = None
    cur_map = None
    cum = 0.0

    for i, raw in enumerate(lines, 1):
        s = raw.strip()
        if not s:
            continue
        if s.startswith("[FRAMES]"):
            m = RE_FRAMES.search(s)
            if m:
                w = _frames(m, i)
                cum += w["secs"]
                w["t_end"] = round(cum, 1)
                w["map"] = cur_map
                frames.append(w)
                anchors.append((i, cum))
            continue
        if s.startswith("[HITCH]"):
            m = RE_HITCH.match(s)
            if m:
                hitches.append({"line": i, "ms": int(m.group(1)), "t": float(m.group(2))})
                anchors.append((i, float(m.group(2))))
            continue
        if s.startswith("[FILE]"):
            m = RE_MAP.search(s)
            if m:
                name = m.group(1)
                if cur_map is None or name.lower() != cur_map.lower():
                    maps.append({"name": name, "line": i})
                    cur_map = name
            m = RE_FAIL.search(s)
            if m:
                key = m.group(1)
                err = m.group(4) or "win32 err=" + m.group(3)
                e = failed.setdefault(key, {"error": err, "status": m.group(2), "count": 0})
                e["count"] += 1
            m = RE_FILE_SUMMARY.search(s)
            if m:
                file_summary = m.group(1)
            continue
        if s.startswith("[UNIMPL]"):
            m = RE_UNIMPL.search(s)
            unimpl_lines += 1
            if m and m.group(1):
                e = unimpl.setdefault(m.group(3).upper().replace("0X", "0x"),
                                      {"insn": m.group(2), "hits": 0, "first_line": i})
                e["hits"] += 1
            continue
        if s.startswith("[CRASH]"):
            crash_count += 1
            if crash is None:
                crash, crash_line = s, i
                for nxt in lines[i:i + 8]:
                    mv = re.search(r"Xbox VA of fault: (0x[0-9A-Fa-f]+)", nxt)
                    if mv:
                        crash_va = mv.group(1)
                        break
            continue
        if s.startswith("[WATCHDOG]"):
            m = RE_WATCHDOG.search(s)
            if m and watchdog is None:
                watchdog, watchdog_secs, watchdog_line = s, int(m.group(1)), i
            continue
        if s.startswith("PAD:"):
            if RE_PAD_STEP.search(s):
                pad_steps += 1
            elif s.startswith("PAD: live \""):
                pad_live += 1
            else:
                m = RE_PAD_SCRIPT.search(s)
                if m:
                    pad_script = int(m.group(1))
            continue
        if s.startswith("[FBWIN]"):
            if "presents to the window now" in s and first_present is None:
                first_present = i
            elif "window closed" in s and exit_kind is None:
                exit_kind, exit_line = "window closed", i
            elif "guest returned" in s and exit_kind is None:
                exit_kind, exit_line = "guest returned", i

    # Approximate seconds since the first present for any line.
    if first_present is not None:
        anchors.append((first_present, 0.0))
    anchors.sort()
    a_lines = [a[0] for a in anchors]

    def t_at(line):
        if not anchors or line < anchors[0][0]:
            return None
        k = bisect.bisect_right(a_lines, line) - 1
        l0, t0 = anchors[k]
        if k + 1 >= len(anchors):
            return round(t0, 1)
        l1, t1 = anchors[k + 1]
        return round(t0 + (t1 - t0) * (line - l0) / max(1, l1 - l0), 1)

    for m in maps:
        m["t_secs"] = t_at(m["line"])

    match = next((m for m in maps if m["name"].lower() != MENU), None)
    match_map = match["name"] if match else None
    menu_wins = [w for w in frames if w["map"] is None or w["map"].lower() == MENU]
    match_wins = [w for w in frames if w["map"] is not None and w["map"].lower() != MENU]
    reached_match = bool(match_wins)

    # Timer or hang: a timer expiry leaves [FRAMES] windows up to the dump that
    # sum to nearly the watchdog's time.
    present_secs = round(cum, 1)
    watchdog_kind = None
    if watchdog is not None:
        last = frames[-1] if frames else None
        before = [w for w in frames if w["line"] < watchdog_line]
        gap = watchdog_secs - (before[-1]["t_end"] if before else 0.0)
        if not before or gap > HANG_SLACK_SECS:
            watchdog_kind = "hang"
        elif last and last["fps"] < STALL_FPS:
            watchdog_kind = "stall"
        else:
            watchdog_kind = "timer"
        exit_kind, exit_line = exit_kind or "watchdog", exit_line or watchdog_line
    if crash is not None:
        exit_kind = exit_kind if exit_kind and exit_line and exit_line < crash_line else "crash"
    if exit_kind is None:
        exit_kind = "log ends"

    hitch_info = {"count": len(hitches), "worst_ms": max((h["ms"] for h in hitches), default=None),
                  "worst_at_secs": max(hitches, key=lambda h: h["ms"])["t"] if hitches else None,
                  "in_match": sum(1 for h in hitches if match_wins and h["line"] > match_wins[0]["line"]
                                  - 1 and h["line"] >= (match["line"] if match else 0))}

    fm, fx = _aggregate(menu_wins), _aggregate(match_wins)

    reasons, notes = [], []
    if crash is not None:
        reasons.append("crash at line %d: %s" % (crash_line, crash))
    if watchdog is not None:
        if watchdog_kind != "timer":
            reasons.append("watchdog after %ds looks like a %s (frames cover %.0f s)"
                           % (watchdog_secs, watchdog_kind, present_secs))
        elif not allow_watchdog:
            reasons.append("watchdog fired after %ds" % watchdog_secs)
    if expect_match and not reached_match:
        if match_map:
            reasons.append("loaded %s but no [FRAMES] line followed it" % match_map)
        else:
            reasons.append("no match map loaded")
    if min_match_fps is not None:
        fps = fx["settled_fps"] if fx["settled_fps"] is not None else fx["mean_fps"]
        if fps is None:
            if expect_match or reached_match:
                reasons.append("no match frames to check against %.1f fps" % min_match_fps)
        elif fps < min_match_fps:
            reasons.append("match ran %.1f fps, under %.1f" % (fps, min_match_fps))
    if not frames:
        notes.append("no [FRAMES] lines: times are unknown, positions are line numbers")
    if exit_kind == "log ends":
        notes.append("log ends with no exit marker: killed, still running, or cut short")
    if unimpl_lines >= UNIMPL_CAP:
        notes.append("[UNIMPL] hit its %d-line cap, so later addresses would not show" % UNIMPL_CAP)

    return {
        "log": log_path,
        "lines": len(lines),
        "time_basis": "seconds since first present, from [FRAMES]/[HITCH]; line numbers are exact",
        "maps": maps,
        "reached_match": reached_match,
        "match_map": match_map,
        "crash": crash,
        "crash_line": crash_line,
        "crash_va": crash_va,
        "crash_count": crash_count,
        "watchdog": watchdog is not None,
        "watchdog_secs": watchdog_secs,
        "watchdog_kind": watchdog_kind,
        "closed_cleanly": exit_kind in ("window closed", "guest returned") and crash is None,
        "exit": exit_kind,
        "present_secs": present_secs,
        "unimpl_reached": sorted(unimpl),
        "unimpl": unimpl,
        "frames_menu": fm,
        "frames_match": fx,
        "hitches": hitch_info,
        "failed_opens": failed,
        "failed_open_total": sum(e["count"] for e in failed.values()),
        "file_summary": file_summary,
        "pad_steps": pad_steps,
        "pad_script_steps": pad_script,
        "pad_live": pad_live,
        "verdict": "fail" if reasons else "pass",
        "reasons": reasons,
        "notes": notes,
    }


def _f(v, fmt="%.1f"):
    return "-" if v is None else fmt % v


def summary(r):
    out = []
    maps = " > ".join("%s@%s" % (m["name"], ("%ss" % m["t_secs"]) if m["t_secs"] is not None
                                 else "L%d" % m["line"]) for m in r["maps"]) or "none"
    out.append("%s  %s  (%d lines, frames cover %.0f s, exit: %s%s)" % (
        r["verdict"].upper(), r["log"], r["lines"], r["present_secs"], r["exit"],
        " after %ds, %s" % (r["watchdog_secs"], r["watchdog_kind"]) if r["watchdog"] else ""))
    out.append("maps: %s; match %s" % (maps, "reached (%s)" % r["match_map"] if r["reached_match"]
                                       else "not reached"))
    for label, a in (("menu ", r["frames_menu"]), ("match", r["frames_match"])):
        if a["lines"]:
            out.append("%s: %d windows, %d frames, %s fps mean, %s settled, worst %s; p50 %s ms, p99 max %s, "
                       "frame max %s, worst 1%%/0.1%% low %s/%s ms; walker cpu %s ms/frame" % (
                           label, a["lines"], a["frames"], _f(a["mean_fps"]), _f(a["settled_fps"]),
                           _f(a["worst_fps"]), _f(a["median_p50_ms"]), _f(a["max_p99_ms"]),
                           _f(a["max_frame_ms"]), _f(a["max_low1_ms"], "%.2f"), _f(a["max_low01_ms"], "%.2f"),
                           _f(a["walker_cpu_ms"], "%.2f")))
        else:
            out.append("%s: no [FRAMES] lines" % label)
    h = r["hitches"]
    top = sorted(r["failed_opens"].items(), key=lambda kv: -kv[1]["count"])[:3]
    out.append("hitches %d (worst %s ms); unimpl %s; pad steps %d%s; failed opens %d (%s)" % (
        h["count"], _f(h["worst_ms"], "%d"), ", ".join(r["unimpl_reached"]) or "none", r["pad_steps"],
        "/%d" % r["pad_script_steps"] if r["pad_script_steps"] else "", r["failed_open_total"],
        "; ".join("%s x%d %s" % (k, v["count"], v["error"]) for k, v in top)))
    if r["crash"]:
        out.append("crash (first of %d) at line %d: %s" % (r["crash_count"], r["crash_line"], r["crash"]))
    for s in r["reasons"]:
        out.append("FAIL: " + s)
    for s in r["notes"]:
        out.append("note: " + s)
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description="Say whether a game run worked.")
    ap.add_argument("log")
    ap.add_argument("--no-expect-match", action="store_true", help="a menu-only run can pass")
    ap.add_argument("--min-fps", type=float, default=None, help="fail if the match runs slower")
    ap.add_argument("--strict-watchdog", action="store_true", help="fail on any watchdog dump")
    ap.add_argument("--json", help="also write the full result here")
    args = ap.parse_args()
    try:
        r = analyse(args.log, expect_match=not args.no_expect_match,
                    allow_watchdog=not args.strict_watchdog, min_match_fps=args.min_fps)
    except OSError as e:
        print("cannot read %s: %s" % (args.log, e), file=sys.stderr)
        return 2
    print(summary(r))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(r, f, indent=1)
    return 0 if r["verdict"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
