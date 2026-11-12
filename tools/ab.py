"""Compare match fps between two settings or two builds, over several interleaved runs.

    python -I tools/ab.py --direct --runs 3 --secs 60 --a "" --b "RECOMP_X=1" [--a-exe P --b-exe P]
                          [--name ab1] [--bots N] [--seed S]
    python -I tools/ab.py --runs 4 --secs 60 ... [--view corridor|any] [--rerolls 15] [--quick]

A and B are overrides in drive_match's --env form ("K=V;K=V"); --a-exe/--b-exe run
another build (copy build/game/ucc2_recomp.exe aside before rebuilding; it finds
assets/ from the repository root, so any path works). Runs go A B A B ..., so
drift hits both sides alike, each driven into CTF-Aqueduct by tools/drive_match.py.
Every run is 4:3 (RECOMP_WIDESCREEN=0, RECOMP_WIDE_VIEW=0, before A's or B's own
overrides), whatever the settings panel says, as the reference frames are.

Where the player spawns sets an idle match's fps: the red base has at least seven
spawn points, from 65 to 158 fps, and the view from each is the same every time.
The corridor view, looking down the long corridor, is the heavy view of issue #1.
Spawns are told apart by the scene above the player against build/route/corridor.png
(local, from the game; same view 0-1, the same spawn after a jump about 5.3, which
drive_match avoids by not pressing A once the player is in).

--direct (use this) boots straight into the match (drive_match --direct): no menus,
no bots unless --bots, no time limit, and RECOMP_BENCH_SPAWN=<--seed> fixing the
spawn. Seed 1, the default, is the corridor view every time: 30 of 30 on 2026-11-12
(12 boots with no bots, 12 with 7, and an A/A of 3 a side at 100.5-102.2 fps, B/A
+0.8%). Before the driver stopped pressing A into the spawn, 31 of 56 no-bot runs
and 13 of 14 with bots were. A run with --secs 60 takes 83-85 s (the match is up 9 s
after launch); the A/A below ran before the boot logos were skipped, at 102 s. A run that spawns elsewhere fails rather than rerolling. With no bots nothing
crosses the view, so every window is kept, but the workload is lighter than a played
match: --bots N puts bots back for real load (and they end the measurement when they
reach the spawn, about a minute in).

Noise floor, --direct (A/A, 3 runs a side, 60 s, 2026-11-11, v0.17.0 toolkit): 85.0-
87.5 fps, 1.0% standard deviation a run; B/A -0.9%, 95% CI -2.1% to +1.5%. So 3 a side
detects about 3% or more.

Without --direct, runs go through the menus and the spawn is random: --view corridor
(the default) forfeits a match spawning elsewhere and enters again (Driver.rematch,
about 34 s) up to --rerolls times, and since toolkit v0.17.0 the corridor is about one
spawn in ten, so a run takes several minutes. --view any measures wherever the player
spawned, and says which; its fps spreads over 65-158. Noise floor through the menus
(A/A, 12 runs in the corridor view, older build, with bots): 63-71 fps, 3.5% standard
deviation a run, so 3 a side detects about 10%, 6 a side about 5%.

The measurement starts once the view is reached and has settled for 2 s, and lasts
about --secs + 10 s. It counts only whole [FRAMES] windows (10 s each) in which the
view stood still: a frame is looked at every second and compared with the run's own
view at the start, and a window is "busy", and dropped, if any of its frames differs
(a bot running through, shots, the player hit). Two misses running (the player
killed, which leaves the scoreboard up) end it, and a run with under two still
windows is excluded. Per run: fps over the still windows,
p50 and p99 as the median of their values, walker cpu ms a frame, and [HITCH] lines.

A run whose route fails, never finds the view, crashes, or loses the view early is
counted and listed, and left out. Each run keeps log.txt, route.txt and view.png
(a frame from the measured view) in build/runs/<name>/<a1|b1|...>/; the table and
results.json go in build/runs/<name>/. The difference is the ratio of median fps,
with a 95% bootstrap interval and an exact Mann-Whitney p. Exits 0 when every run
gave a measurement, 1 if any was left out, 2 if a side has none.
Run it with -I: without it tools/dis.py shadows the standard library's dis.
"""
import argparse
import itertools
import json
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import time

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import drive_match as dm             # noqa: E402
from check_run import RE_FRAMES, _frames, analyse   # noqa: E402

RE_HITCH = re.compile(r"^\[HITCH\] (\d+) ms frame")
CHECK_EVERY = 1.0     # s between frames looked at while measuring; a grab costs the game nothing measurable
CLEAN = 3.0           # scene distance under which nothing moves in the view (still view: 0-1)
MIN_WINDOWS = 2
BASE_ENV = "RECOMP_WIDESCREEN=0;RECOMP_WIDE_VIEW=0"


def game_running():
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout
    return "ucc2_recomp" in out.lower()


def parse(log, start, end):
    """[FRAMES] windows, each with the byte offset of its line, and [HITCH] offsets, in [start, end)."""
    with open(log, "rb") as f:
        f.seek(start)
        data = f.read(max(0, end - start))
    wins, hitches = [], []
    off = start
    for raw in data.split(b"\n"):
        s = raw.decode("latin-1").strip()
        m = RE_FRAMES.search(s)
        if m:
            w = _frames(m, 0)
            w["off"] = off
            wins.append(w)
        elif RE_HITCH.match(s):
            hitches.append(off)
        off += len(raw) + 1
    return wins, hitches


def clean_windows(wins, checks, start):
    """Windows wholly after start in which every frame looked at showed the still view.

    A window's line ends it, and it began at the previous window's line."""
    out = []
    for prev, w in zip(wins, wins[1:]):
        inside = [d for pos, d in checks if prev["off"] < pos <= w["off"] + 4096]
        w["clean"] = prev["off"] >= start and bool(inside) and max(inside) < CLEAN
        out.append(w)
    return out


def run_one(args, label, env, exe):
    run = os.path.join(args.out, label)
    os.makedirs(run)
    timeout = int(150 + 45 * args.rerolls + args.secs + 40)
    # The references are 4:3, so every run is, whatever the settings panel says;
    # the side's own overrides come after and win.
    env = BASE_ENV + (";" + env if env else "")
    dargs = argparse.Namespace(secs=args.secs, timeout=timeout, map="CTF-Aqueduct", env=env, exe=exe,
                               no_window=False, dumps=False, boot_waits=False, keep_frames=False,
                               verbose=False, min_fps=None, direct=args.direct, bots=args.bots,
                               seed=args.seed)
    drv = dm.Driver(dargs, run)
    rec = {"run": label, "env": env, "exe": exe, "status": "ok", "reason": "", "spawn": None, "rerolls": 0}
    t0 = time.time()
    try:
        drv.drive()
        drv.wait(1.0)
        im = drv.frame()
        ref = drv.refs.get("corridor")
        if args.view == "corridor":
            while not drv.in_view(im):
                if args.direct:
                    drv.frames.save("spawn")
                    raise dm.Failed("spawned outside the corridor (scene %.1f; seed %d)"
                                    % (dm.dist(im, ref, dm.SCENE), args.seed))
                if rec["rerolls"] >= args.rerolls:
                    raise dm.Failed("no corridor spawn in %d rerolls" % args.rerolls)
                rec["rerolls"] += 1
                drv.note("reroll %d" % rec["rerolls"])
                drv.rematch()
                drv.wait(1.0)
                im = drv.frame()
            rec["spawn"] = "corridor"
        else:
            rec["spawn"] = ("corridor" if drv.in_view(im) else
                            "other(%.0f)" % dm.dist(im, ref, dm.SCENE) if ref else "?")
        # Stillness is judged against this run's own view once the spawn has
        # settled, not the reference: small differences between builds then
        # do not make every window busy.
        drv.wait(2.0)
        im2 = drv.frame()
        ref = im2 if im2 is not None and drv.is_match(im2) else im
        rec["spawn_scene"] = round(dm.dist(ref, drv.refs.get("corridor"), dm.SCENE), 1)
        drv.frames.save("spawn")

        def scene(f):
            return dm.dist(f, ref, dm.SCENE) if f is not None and drv.is_match(f) else 999.0

        # Look at a frame every second: a bot in view or the player killed shows in
        # the scene; two misses running end the measurement.
        drv.enter("measure")
        drv.poll()
        start = drv.pos
        t_start = drv.now()
        t_end = t_start + args.secs + 10
        checks, bad, saved = [], 0, False
        while drv.now() < t_end:
            drv.wait(CHECK_EVERY)
            d = scene(drv.frame())
            checks.append((drv.pos, d))
            if d < dm.SCENE_MAX:
                bad = 0
                if not saved and drv.now() > t_start + args.secs / 2:
                    drv.frames.save("view")
                    saved = True
            else:
                bad += 1
                if bad >= 2:
                    rec["reason"] = "view lost %.0f s in" % (drv.now() - t_start)
                    drv.note(rec["reason"])
                    drv.frames.save("view_lost")
                    break
        if not saved:
            drv.frames.save("view")
        drv.poll()
        allw, hitch_offs = parse(drv.log_path, 0, drv.pos)
        wins = clean_windows(allw, checks, start)
        wins = [w for w in wins if checks and w["off"] <= checks[-1][0] + 4096]
        rec["busy"] = sum(not w["clean"] and w["off"] > start for w in wins)
        wins = [w for w in wins if w["clean"]]
        rec["checks"] = [round(d, 1) for _, d in checks]
        if len(wins) < MIN_WINDOWS:
            raise dm.Failed("%s; %d still windows" % (rec["reason"] or "view busy", len(wins)))
        frames = sum(w["frames"] for w in wins)
        secs = sum(w["secs"] for w in wins)
        hitches = sum(1 for h in hitch_offs if any(p["off"] < h <= w["off"] for p, w in zip(allw, allw[1:])
                                                   if w in wins))
        rec.update(fps=frames / secs, p50=statistics.median(w["p50"] for w in wins),
                   p99=statistics.median(w["p99"] for w in wins),
                   walker=sum(w["walker_cpu"] * w["frames"] for w in wins) / frames,
                   hitches=hitches, windows=len(wins), window_fps=[w["fps"] for w in wins])
    except dm.Failed as e:
        rec["status"], rec["reason"] = "excluded", "%s: %s" % (drv.state, e)
        if drv.frames:
            drv.frames.save("last")
    except (OSError, ValueError) as e:
        rec["status"], rec["reason"] = "excluded", "%s: error %r" % (drv.state, e)
    finally:
        drv.note("end")
        if drv.proc and drv.proc.poll() is None:
            drv.proc.kill()
            drv.proc.wait(10)
        drv.route.close()
        dump = drv.frames.dump_prefix if drv.frames else None
        if dump:
            for f in os.listdir(os.path.dirname(dump)):
                p = os.path.join(os.path.dirname(dump), f)
                if p.startswith(dump) and f.endswith(".bmp"):
                    os.remove(p)
    if rec["status"] == "ok" and os.path.exists(drv.log_path):
        r = analyse(drv.log_path, expect_match=True, allow_watchdog=True)
        if r["crash"]:
            rec["status"], rec["reason"] = "excluded", "crash: " + r["crash"]
    rec["secs_total"] = round(time.time() - t0)
    return rec


# ---- statistics ----------------------------------------------------------------

def ranks(v):
    order = sorted(range(len(v)), key=lambda i: v[i])
    r = [0.0] * len(v)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and v[order[j + 1]] == v[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2 + 1
        i = j + 1
    return r


def mann_whitney_p(a, b):
    """Exact two-sided p of the rank sum, over every split of the pooled runs."""
    rk = ranks(a + b)
    mean = len(a) * (len(a) + len(b) + 1) / 2
    obs = abs(sum(rk[:len(a)]) - mean)
    tot = hit = 0
    for idx in itertools.combinations(range(len(rk)), len(a)):
        tot += 1
        hit += abs(sum(rk[i] for i in idx) - mean) >= obs - 1e-9
    return hit / tot


def bootstrap_ratio(a, b, n=5000):
    """95% interval of median(b) / median(a) - 1, resampling each side's runs."""
    rng = random.Random(12345)
    rs = sorted(statistics.median(rng.choices(b, k=len(b))) / statistics.median(rng.choices(a, k=len(a))) - 1
                for _ in range(n))
    return rs[int(0.025 * n)], rs[int(0.975 * n) - 1]


def quartiles(v):
    if len(v) < 2:
        return v[0], v[0]
    q = statistics.quantiles(v, n=4, method="inclusive")
    return q[0], q[2]


def report(args, recs):
    out = []
    say = out.append
    say("%s: %d run%s a side, %g s in the %s view%s" % (
        args.name, args.runs, "s" * (args.runs > 1), args.secs, "corridor" if args.view == "corridor" else "spawn",
        ", direct boot (seed %d, %d bots)" % (args.seed, args.bots) if args.direct else ", through the menus"))
    say('A: env "%s"%s' % (args.a, ", exe " + args.a_exe if args.a_exe else ""))
    say('B: env "%s"%s' % (args.b, ", exe " + args.b_exe if args.b_exe else ""))
    say("")
    say("run  rerolls  spawn        fps   p50 ms  p99 ms  walker ms  hitches  busy  still windows (fps)  run s")
    for r in recs:
        if r["status"] == "ok":
            say("%-4s %7d  %-10s %6.1f  %6.1f  %6.1f  %9.2f  %7d  %4d  %d (%s)  %d%s" % (
                r["run"], r["rerolls"], r["spawn"][:10], r["fps"], r["p50"], r["p99"], r["walker"], r["hitches"],
                r["busy"], r["windows"], " ".join("%.0f" % f for f in r["window_fps"]), r["secs_total"],
                "  (" + r["reason"] + ")" if r["reason"] else ""))
        else:
            say("%-4s %7d  EXCLUDED   %s  (%d s)" % (r["run"], r["rerolls"], r["reason"], r["secs_total"]))
    side = {s: [r for r in recs if r["run"][0] == s.lower() and r["status"] == "ok"] for s in "AB"}
    bad = [r for r in recs if r["status"] != "ok"]
    say("")
    say("excluded: %d of %d runs%s" % (len(bad), len(recs),
                                       "".join("\n  %s: %s" % (r["run"], r["reason"]) for r in bad)))
    say("")
    say("%-10s %-30s %-30s" % ("", "A median [IQR] (min-max)", "B median [IQR] (min-max)"))
    for key, name, fmt in (("fps", "fps", "%.1f"), ("p50", "p50 ms", "%.1f"), ("p99", "p99 ms", "%.1f"),
                           ("walker", "walker ms", "%.2f"), ("hitches", "hitches", "%.0f")):
        cells = []
        for s in "AB":
            v = [r[key] for r in side[s]]
            if not v:
                cells.append("-")
                continue
            q1, q3 = quartiles(v)
            cells.append(("%s [%s-%s] (%s-%s)" % ((fmt,) * 5)) % (statistics.median(v), q1, q3, min(v), max(v)))
        say("%-10s %-30s %-30s" % (name, cells[0], cells[1]))
    say("")
    a, b = [r["fps"] for r in side["A"]], [r["fps"] for r in side["B"]]
    result = {}
    if not a or not b:
        say("no comparison: a side has no measured runs")
    else:
        d = statistics.median(b) / statistics.median(a) - 1
        lo, hi = bootstrap_ratio(a, b)
        p = mann_whitney_p(a, b)
        result = {"diff": d, "ci95": [lo, hi], "mann_whitney_p": p}
        say("B/A median fps: %+.1f%% (bootstrap 95%% CI %+.1f%% to %+.1f%%), Mann-Whitney p = %.3f, n = %d/%d"
            % (100 * d, 100 * lo, 100 * hi, p, len(a), len(b)))
        if min(len(a), len(b)) < 3:
            say("too few runs to judge: 3 a side at least")
        elif lo > 0 or hi < 0:
            word = "faster" if d > 0 else "slower"
            m, n = sorted((abs(lo), abs(hi)))
            say("B is %.0f%% %s (95%% CI %.0f-%.0f%%)" % (100 * abs(d), word, 100 * m, 100 * n))
        else:
            say("no difference detectable at this n (95%% CI %+.0f%% to %+.0f%%)" % (100 * lo, 100 * hi))
    return "\n".join(out), result


def main():
    ap = argparse.ArgumentParser(description="Compare match fps between two settings or builds.")
    ap.add_argument("--runs", type=int, default=4, help="runs a side")
    ap.add_argument("--secs", type=float, default=60, help="seconds measured in the view")
    ap.add_argument("--a", default="", help='side A overrides, "K=V;K=V"')
    ap.add_argument("--b", default="", help='side B overrides, "K=V;K=V"')
    ap.add_argument("--a-exe", default=None, help="side A's exe (default build/game/ucc2_recomp.exe)")
    ap.add_argument("--b-exe", default=None, help="side B's exe")
    ap.add_argument("--name", default=None, help="folder under build/runs/")
    ap.add_argument("--view", choices=["corridor", "any"], default="corridor")
    ap.add_argument("--rerolls", type=int, default=15, help="most forfeits a run spends looking for the view")
    ap.add_argument("--quick", action="store_true", help="smoke check: 1 run a side, 20 s, any view")
    ap.add_argument("--direct", action="store_true",
                    help="boot straight into the match at a fixed spawn (drive_match --direct), no menus or rerolls")
    ap.add_argument("--bots", type=int, default=0, help="--direct: bots in the match (real load)")
    ap.add_argument("--seed", type=int, default=dm.BENCH_SEED, help="--direct: spawn seed")
    args = ap.parse_args()
    if args.quick:
        args.runs, args.secs, args.view = 1, 20, "any"
    if args.view == "any" or args.direct:
        args.rerolls = 0

    for exe in (args.a_exe, args.b_exe, dm.EXE):
        if exe and not os.path.exists(exe):
            sys.exit("no %s" % exe)
    if args.view == "corridor" and not os.path.exists(os.path.join(dm.ROUTE, "corridor.png")):
        sys.exit("no %s: copy a frame of the corridor spawn there, or use --view any"
                 % os.path.join(dm.ROUTE, "corridor.png"))
    if game_running():
        sys.exit("the game is already running; close it first")
    args.name = args.name or time.strftime("ab-%Y%m%d-%H%M%S")
    args.out = os.path.join(dm.ROOT, "build", "runs", args.name)
    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)

    t0 = time.time()
    recs = []
    try:
        for i in range(1, args.runs + 1):
            for s, env, exe in (("a", args.a, args.a_exe), ("b", args.b, args.b_exe)):
                label = "%s%d" % (s, i)
                if "RECOMP_FB_DUMP=" in env:     # each run's dumps in its own folder
                    dump = "RECOMP_FB_DUMP=" + os.path.join(args.out, label, "dump_")
                    env = re.sub(r"RECOMP_FB_DUMP=[^;]*", lambda _: dump, env)
                print("%s ..." % label, end=" ", flush=True)
                r = run_one(args, label, env, exe)
                recs.append(r)
                with open(os.path.join(args.out, "results.json"), "w") as f:   # so far, if stopped
                    json.dump({"args": vars(args), "runs": recs}, f, indent=1)
                print(("%.1f fps, %s" % (r["fps"], r["spawn"]) if r["status"] == "ok" else r["reason"])
                      + " (%d s)" % r["secs_total"], flush=True)
    except KeyboardInterrupt:
        print("interrupted; reporting the runs done")
    text, result = report(args, recs)
    text += "\ntook %.0f min" % ((time.time() - t0) / 60)
    print()
    print(text)
    with open(os.path.join(args.out, "summary.txt"), "w") as f:
        f.write(text + "\n")
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump({"args": {k: v for k, v in vars(args).items()}, "runs": recs, "result": result}, f, indent=1)
    sides = {r["run"][0] for r in recs if r["status"] == "ok"}
    if sides != {"a", "b"}:
        return 2
    return 0 if all(r["status"] == "ok" for r in recs) and len(recs) == 2 * args.runs else 1


if __name__ == "__main__":
    sys.exit(main())
