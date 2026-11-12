"""Check that the game's logic runs the same at any frame rate (#20).

    python -I tools/tick_rates.py --rates 60,144,240,0 [--runs 1] [--name tr1] [--fields "P.Health,..."]
    python -I tools/tick_rates.py --analyse build/runs/tr1

Each run boots straight into CTF-Aqueduct at the corridor spawn with no bots
(tools/drive_match.py --direct), with RECOMP_FPS_LIMIT=<rate> (0: no limit) and
the per-tick probe on (RECOMP_TICK_PROBE, src/recomp_manual.c), which writes the
frame delta, the level's time and the player's pawn every tick. Once the match
is up it plays a fixed pad sequence through RECOMP_PAD_LIVE (SEQUENCE below):
standing jumps, a turn, running, dodges, fire. Then it reads the probe's file
and measures, per run:

    tick        ticks a second, mean delta, and the delta against the host's
                time between ticks
    clock       the level's TimeSeconds and the GRI's ElapsedTime against the
                host clock over the run, the host seconds per step of ElapsedTime
                (the match clock), and the seconds-per-cycle the game
                calibrated at boot, as a multiple of the emulated clock's
    jump        each jump from standing: apex height and time in the air
    run         top ground speed with the stick held
    air moves   peak horizontal and vertical speed of each move that leaves the
                ground while the stick is held (dodges)
    turn        the view's yaw rate with the right stick held
    fire        shots a second with fire held, and the ammo's recharge
    projectile  speed over the first 0.15 s of each shot's flight

--menu runs a menu test instead: through the title to Instant Action's game
type list, then the d-pad held down for MENU_HOLD ms, three times, counting the
game type portraits the list opens (one per step) while it is held.

and prints a table of each measure by rate. Runs go to build/runs/<name>/r<rate>_<n>/.
Don't run it while the game is running (the owner may be playing).
"""
import argparse
import csv
import json
import math
import os
import re
import shutil
import statistics
import sys
import time

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import drive_match as dm             # noqa: E402

# Physics modes (Actor.EPhysics, as in UE2).
PHYS_WALKING, PHYS_FALLING = 1, 2

# The pad sequence, after the match is up: (seconds to wait first, line for the pad file, label).
# Lines are pressed one after another with 200 ms released between (RECOMP_PAD_LIVE).
SEQUENCE = [
    (3.0, "a:100", "jump"),
    (3.0, "a:100", "jump"),
    (3.0, "a:100", "jump"),
    (3.0, "rright:1000", "turn"),
    (3.0, "lup:3000", "run"),
    (4.0, "ldown:3000", "run"),
    (4.0, "lleft+a:120", "dodge"),
    (3.0, "lright+a:120", "dodge"),
    (3.0, "lup+a:120", "dodge"),
    (3.0, "rt:5000", "fire"),
    (7.0, "", "end"),
]

DEFAULT_FIELDS = ("C.Rotation,W.AmmoPct,W.LastFireTime,W.FlashCount,R.ElapsedTime,P.Health,"
                  "X.Name,X.Location,X.Velocity")


def run_one(args, rate, n):
    label = "r%d_%d" % (rate, n)
    run = os.path.join(args.out, label)
    os.makedirs(run)
    probe = os.path.join(run, "probe.csv")
    env = ("RECOMP_FPS_LIMIT=%d;RECOMP_TICK_PROBE=%s;RECOMP_TICK_PROBE_FIELDS=%s"
           % (rate, probe, args.fields.replace(";", ",")))
    if args.env:
        env += ";" + args.env
    dargs = argparse.Namespace(secs=60, timeout=240, map="CTF-Aqueduct", env=env, exe=None,
                               no_window=False, dumps=False, boot_waits=False, keep_frames=False,
                               verbose=False, min_fps=None, direct=True, bots=0, seed=1)
    drv = dm.Driver(dargs, run)
    marks = []
    status = "ok"
    try:
        drv.drive()
        for wait, line, what in SEQUENCE:
            drv.wait(wait)
            if line:
                with open(drv.pad_path, "a") as f:
                    f.write(line + "\n")
                drv.note("press %s (%s)" % (line, what))
            marks.append((drv.now(), what))
        drv.frames.save("end")
    except dm.Failed as e:
        status = "failed: %s in %s" % (e, drv.state)
    finally:
        if drv.proc and drv.proc.poll() is None:
            drv.proc.kill()
            drv.proc.wait(10)
    with open(os.path.join(run, "marks.json"), "w") as f:
        json.dump({"status": status, "rate": rate, "marks": marks}, f)
    print("%s: %s" % (label, status), flush=True)


MENU_HOLD = 3000


def menu_one(args, rate, n):
    """Key repeat in a menu: game type steps while down is held."""
    label = "m%d_%d" % (rate, n)
    run = os.path.join(args.out, label)
    os.makedirs(run)
    dargs = argparse.Namespace(secs=60, timeout=240, map="CTF-Aqueduct", env="RECOMP_FPS_LIMIT=%d" % rate,
                               exe=None, no_window=False, dumps=False, boot_waits=False, keep_frames=False,
                               verbose=False, min_fps=None, direct=False, bots=0, seed=0)
    drv = dm.Driver(dargs, run)
    holds, status = [], "ok"
    try:
        drv.launch()
        drv.enter("boot")
        if not drv.wait_for(r"UI_Start_Movie\w*\.xmv$", 0, 180):
            raise dm.Failed("no title movie")
        drv.enter("title")
        for _ in range(6):
            since = drv.press("start")
            if drv.wait_for(r"UI_LoadingMovie\.xmv$", since, 4):
                break
        drv.enter("main menu")
        target = dm.MAIN_ITEMS.index("instant")
        for _ in range(8):
            im, ok = drv.wait_frame(drv.is_main, 8)
            if not ok:
                raise dm.Failed("main menu not seen")
            lit = dm.lit_item(im)
            if lit == target:
                break
            drv.press("down" if lit < target else "up")
            drv.wait(0.6)
        since = drv.press("a")
        if not drv.wait_for(r"Portrait_GameType_\w+\.xmv$", since, 5):
            raise dm.Failed("no game type list")
        drv.enter("hold")
        for _ in range(3):
            drv.wait(2.0)
            since = drv.press("down", MENU_HOLD)
            t0 = drv.now()
            drv.wait(MENU_HOLD / 1000 + 1.5)
            steps = [t for t, p in drv.events[since:] if re.search(r"Portrait_GameType_\w+\.xmv$", p, re.I)]
            holds.append([round(t - t0, 3) for t in steps])
            drv.note("held down: %d steps" % len(steps))
    except dm.Failed as e:
        status = "failed: %s in %s" % (e, drv.state)
    finally:
        if drv.proc and drv.proc.poll() is None:
            drv.proc.kill()
            drv.proc.wait(10)
    with open(os.path.join(run, "menu.json"), "w") as f:
        json.dump({"status": status, "rate": rate, "holds": holds}, f)
    print("%s: %s, steps per hold %s" % (label, status, [len(h) for h in holds]), flush=True)


# ---- analysis ------------------------------------------------------------------

def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("pawn"):
                r["_t"] = num(r["t"])
                r["_dt"] = num(r["dt"])
                r["_lt"] = num(r["level_time"])
                r["_pos"] = (num(r["x"]), num(r["y"]), num(r["z"]))
                r["_vel"] = (num(r["vx"]), num(r["vy"]), num(r["vz"]))
                r["_phys"] = int(num(r["phys"]) or 0)
                rows.append(r)
    return rows


def slope(xs, ys):
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else float("nan")


def hspeed(v):
    return math.hypot(v[0], v[1])


def airborne(rows):
    """Spans of ticks in PHYS_FALLING, entered from PHYS_WALKING: (first, last) indices."""
    spans, start = [], None
    for i in range(1, len(rows)):
        if rows[i]["_phys"] == PHYS_FALLING and rows[i - 1]["_phys"] == PHYS_WALKING:
            start = i
        elif start is not None and rows[i]["_phys"] != PHYS_FALLING:
            spans.append((start, i))
            start = None
    return spans


def analyse_run(run):
    rows = load(os.path.join(run, "probe.csv"))
    if len(rows) < 100:
        return None
    m = {}
    t = [r["_t"] for r in rows]
    gaps = [b - a for a, b in zip(t, t[1:])]
    dts = [r["_dt"] for r in rows[1:]]
    m["ticks/s"] = len(rows) / (t[-1] - t[0])
    m["mean dt ms"] = 1000 * statistics.fmean(dts)
    m["dt-gap ms"] = 1000 * statistics.fmean(d - g for d, g in zip(dts, gaps))
    m["max dt ms"] = 1000 * max(dts)
    m["level/host"] = slope(t, [r["_lt"] for r in rows])
    try:
        with open(os.path.join(run, "log.txt"), "rb") as f:
            cal = re.search(rb"\[PROBE\] seconds per cycle \S+, ([\d.]+) x", f.read())
        if cal:
            m["cycle calib"] = float(cal.group(1))
    except OSError:
        pass
    # The match clock: host seconds between steps of the GRI's ElapsedTime (whole seconds).
    steps, prev = [], None
    for r in rows:
        e = num(r.get("R.ElapsedTime"))
        if e is not None and prev is not None and e != prev:
            steps.append(r["_t"])
        prev = e
    if len(steps) > 10:
        m["elapsed step s"] = (steps[-1] - steps[0]) / (len(steps) - 1)

    jumps, moves = [], []
    for a, b in airborne(rows):
        z0 = rows[a - 1]["_pos"][2]
        top = max(rows[i]["_pos"][2] for i in range(a, b)) - z0
        air = rows[b]["_t"] - rows[a]["_t"]
        hs = max(hspeed(rows[i]["_vel"]) for i in range(a, b))
        vz = max(rows[i]["_vel"][2] for i in range(a, b))
        rec = {"t": rows[a]["_t"], "apex": top, "air": air, "h": hs, "vz": vz,
               "dist": math.dist(rows[a - 1]["_pos"][:2], rows[b]["_pos"][:2])}
        (jumps if hspeed(rows[a - 1]["_vel"]) < 5 and hs < 50 else moves).append(rec)
    if jumps:
        m["jump apex"] = statistics.fmean(j["apex"] for j in jumps)
        m["jump air s"] = statistics.fmean(j["air"] for j in jumps)
        m["jump vz"] = statistics.fmean(j["vz"] for j in jumps)
        m["jumps"] = len(jumps)
    if moves:
        m["dodge h"] = statistics.fmean(j["h"] for j in moves)
        m["dodge vz"] = statistics.fmean(j["vz"] for j in moves)
        m["dodge apex"] = statistics.fmean(j["apex"] for j in moves)
        m["dodge air s"] = statistics.fmean(j["air"] for j in moves)
        m["dodge dist"] = statistics.fmean(j["dist"] for j in moves)
        m["dodges"] = len(moves)
    walk = [hspeed(r["_vel"]) for r in rows if r["_phys"] == PHYS_WALKING]
    if walk:
        m["run speed"] = max(walk)
        top = [v for v in walk if v > 0.95 * max(walk)]
        m["run speed p50"] = statistics.median(top)

    # The view's yaw, unwrapped, against the host clock: the fastest 0.5 s.
    yaw = []
    for r in rows:
        v = r.get("C.Rotation")
        if v:
            yaw.append((r["_t"], int(v.split(";")[1])))
    if len(yaw) > 10:
        un, prev, acc = [], yaw[0][1], 0
        for tt, y in yaw:
            d = ((y - prev + 32768) % 65536) - 32768
            acc += d
            prev = y
            un.append((tt, acc))
        best, j = 0.0, 0
        for i in range(len(un)):
            while un[i][0] - un[j][0] > 0.5:
                j += 1
            if un[i][0] - un[j][0] > 0.4:
                best = max(best, abs(un[i][1] - un[j][1]) / (un[i][0] - un[j][0]))
        m["yaw/s"] = best

    # Fire: each new LastFireTime is a shot; the rate over the run's shots.
    shots = []
    for r in rows:
        v = num(r.get("W.LastFireTime"))
        if v and (not shots or v != shots[-1][1]):
            shots.append((r["_t"], v))
    if len(shots) > 3:
        gaps = [b[1] - a[1] for a, b in zip(shots, shots[1:]) if b[1] - a[1] < 2]
        if gaps:
            m["shots"] = len(shots)
            m["shot gap s"] = statistics.median(gaps)
            m["shots/s"] = 1 / statistics.fmean(gaps)
    # Projectiles: speed over each one's flight, from its positions on the level clock.
    flights = {}
    for r in rows:
        nm, loc = r.get("X.Name"), r.get("X.Location")
        if nm and loc:
            flights.setdefault(nm, []).append((r["_lt"], tuple(float(v) for v in loc.split(";"))))
    speeds = []
    for pts in flights.values():
        if len(pts) >= 5 and pts[-1][0] - pts[0][0] > 0.05:
            # The first stretch only: it may hit something later.
            end = [p for p in pts if p[0] - pts[0][0] <= 0.15]
            if len(end) >= 3:
                speeds.append(math.dist(end[0][1], end[-1][1]) / (end[-1][0] - end[0][0]))
    if speeds:
        m["proj speed"] = statistics.median(speeds)
        m["projectiles"] = len(speeds)
    am = [(r["_t"], num(r.get("W.AmmoPct"))) for r in rows if num(r.get("W.AmmoPct")) is not None]
    if len(am) > 10:
        lo = min(range(len(am)), key=lambda i: am[i][1])
        m["ammo min"] = am[lo][1]
        # Recharge: the slope from the lowest point over the next 2 s.
        nxt = [a for a in am[lo:] if a[0] - am[lo][0] <= 2.0]
        if len(nxt) > 10 and nxt[-1][0] > nxt[0][0]:
            m["recharge/s"] = (nxt[-1][1] - nxt[0][1]) / (nxt[-1][0] - nxt[0][0])
    return m


def analyse(out):
    by_rate = {}
    for name in sorted(os.listdir(out)):
        run = os.path.join(out, name)
        if not os.path.isfile(os.path.join(run, "probe.csv")):
            continue
        rate = int(name[1:].split("_")[0])
        m = analyse_run(run)
        if m:
            by_rate.setdefault(rate, []).append(m)
    rates = sorted(by_rate, key=lambda r: r or 99999)
    keys = []
    for ms in by_rate.values():
        for m in ms:
            keys += [k for k in m if k not in keys]
    lines = ["%-14s" % "measure" + "".join("%14s" % ("limit %d" % r if r else "no limit") for r in rates)]
    for k in keys:
        cells = []
        for r in rates:
            vals = [m[k] for m in by_rate[r] if k in m]
            if not vals:
                cells.append("%14s" % "-")
            elif len(vals) == 1:
                cells.append("%14.4g" % vals[0])
            else:
                cells.append("%14s" % ("%.4g..%.4g" % (min(vals), max(vals))))
        lines.append("%-14s" % k + "".join(cells))
    text = "\n".join(lines)
    print(text)
    with open(os.path.join(out, "table.txt"), "w") as f:
        f.write(text + "\n")
    with open(os.path.join(out, "measures.json"), "w") as f:
        json.dump({str(r): by_rate[r] for r in rates}, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description="Compare the game's logic across frame rates.")
    ap.add_argument("--rates", default="60,144,240,0", help="RECOMP_FPS_LIMIT values; 0 is no limit")
    ap.add_argument("--runs", type=int, default=1, help="runs per rate")
    ap.add_argument("--name", default=None)
    ap.add_argument("--fields", default=DEFAULT_FIELDS, help="RECOMP_TICK_PROBE_FIELDS")
    ap.add_argument("--env", default="", help='extra settings, "K=V;K=V"')
    ap.add_argument("--analyse", default=None, help="only analyse this run folder")
    ap.add_argument("--menu", action="store_true", help="the menu key-repeat test instead")
    args = ap.parse_args()
    if args.analyse:
        analyse(args.analyse)
        return 0
    args.name = args.name or time.strftime("tr-%Y%m%d-%H%M%S")
    args.out = os.path.join(dm.ROOT, "build", "runs", args.name)
    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)
    rates = [int(r) for r in args.rates.split(",")]
    for n in range(1, args.runs + 1):
        for rate in rates:
            (menu_one if args.menu else run_one)(args, rate, n)
    if not args.menu:
        analyse(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
