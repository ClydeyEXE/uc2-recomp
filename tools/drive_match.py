"""Drive the game into an Instant Action match by watching it, not by the clock.

    python -I tools/drive_match.py [--secs 120] [--timeout 600] [--map CTF-Aqueduct] [--name run1]
                                   [--env "K=V;K=V"] [--no-window] [--dumps] [--keep-frames]
                                   [--boot-waits]

Starts build/game/ucc2_recomp.exe with run_match.ps1's settings, but with no
RECOMP_FLIP_PACING and with RECOMP_SKIP_BOOT_WAITS=1 unless --boot-waits. Follows
its log and its frames, and presses the next button through RECOMP_PAD_LIVE only
once the screen it is meant for is up:

    boot        until the title movie (UI_Start_Movie*.xmv) opens
    title       Start, until UI_LoadingMovie.xmv opens (pressed again if not)
    main menu   frame: the header matches build/route/main_menu.png and the bright
                bar says which item is lit; down/up to Instant Action, then A
    game type   Portrait_GameType_<x>.xmv names the lit type; down until it is the
                map's (CTF for CTF-...), then A until Maplists.ini or an arena opens
    rules       A, until Portrait_Arena_<x>.xmv
    arena       down until Portrait_Arena_<map name>, then A
    character   A, until Portrait_Armory_<x>.xmv
    armory      A for each category, then A on "Begin Match?", until <map>.lin opens
    scoreboard  A ("press fire") every 2.5 s until the HUD shows
    match       frame: the HUD's coloured pixels match route/match.png, twice running

Each press waits until the game has opened no file for 1.2 s, as a screen that
is still sliding in drops input. A press that does not bring its event in a few
seconds is repeated. Story Mode, or any screen that is not the expected one, is
backed out of with B until the main menu shows, and the route starts again from
there (three times at most).

Frames come from the game window (PrintWindow; the game's frame rate is as
without it, and the window may be covered but not minimised).
--no-window or --dumps uses RECOMP_FB_DUMP instead, and its readback halves the
frame rate (menus 80 fps against 145, a match 37 against 105), so match numbers
from such a run mean little. Reference screens live in build/route/ (local,
never committed): main_menu, match and title are used, scoreboard only noted,
and corridor (the spawn looking down CTF-Aqueduct's long corridor) by tools/ab.py.
Refresh one by copying a frame from a run folder (--keep-frames) over it.

Everything goes to build/runs/<name>/: log.txt, pad.txt, route.txt (each state
and press with its time) and a few frames, or every frame with --keep-frames.
Ends with "ROUTE reached match at <t> s" or "ROUTE failed: <state> (<why>) after
<t> s", seconds from launch, then tools/check_run.py's verdict on the log.
Exits 0 only when both pass.
Run it with -I: without it tools/dis.py shadows the standard library's dis.
"""
import argparse
import configparser
import ctypes
import ctypes.wintypes as W
import glob
import os
import re
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "game", "ucc2_recomp.exe")
ROUTE = os.path.join(ROOT, "build", "route")

SETTINGS = {
    "RECOMP_EXT_VMA": "1",
    "RECOMP_AC97_READY": "1",
    "RECOMP_PB_EXEC": "1",
    "RECOMP_VBLANK": "paced",
    "RECOMP_PB_WALK": "dma",
    "RECOMP_GUEST_CPUS": "2",
    "RECOMP_HEAP_RECLAIM": "1",
    "RECOMP_DISPATCH_GATE": "1",
    "RECOMP_USB": "1",
    "RECOMP_PB_REPORT_MS": "15000",
    "RECOMP_SKIP_BOOT_WAITS": "1",
}

# Main menu items, top to bottom, and the 640x480 row each one's bar sits on.
MAIN_ITEMS = ["story", "instant", "network", "profiles", "options"]
MAIN_ROWS = [146, 186, 224, 263, 300]
# Crops of a 640x480 frame, with the size each is shrunk to before comparing.
HEADER = ((200, 20, 640, 80), (88, 12))          # "UNREAL CHAMPIONSHIP 2" vs "STORY MODE"
WHOLE = ((0, 0, 640, 480), (64, 48))
# The HUD: health bar, radar and weapon slots. Its brightly coloured pixels in
# route/match.png are compared one by one, as the scene shows through around them.
HUD_BOXES = [(0, 0, 250, 45), (500, 0, 640, 100), (30, 290, 150, 420)]
HUD_MIN = 0.6         # share of those pixels that match: 0.77-1.0 in a match, 0.4 or less elsewhere
HEADER_MAX, SCORE_MAX = 9.0, 10.0
# The scene above the player and clear of the HUD, for telling spawn views apart:
# the same spawn measures 0-1 against its reference, any other 28 or more.
SCENE = ((160, 50, 620, 280), (46, 23))
SCENE_MAX = 8.0
# Seconds to leave a screen after the last file the game opened, before pressing.
SETTLE, SETTLE_MAX = 1.2, 3.0
# Game type portraits, by map prefix. Only CTF has been seen; the rest are guesses.
GAMETYPES = {"CTF": ["CTF"], "DM": ["DM", "DeathMatch"], "TDM": ["TDM", "TeamDeathMatch"]}

RE_FILE = re.compile(r"\[FILE\] (\S+) \(disp [^)]*\) -> 0x00000000")
RE_PORTRAIT = re.compile(r"Portrait_(GameType|Arena|Char|Armory)_(\w+)\.xmv$", re.I)
RE_MAP = re.compile(r"XboxMaps\\([\w-]+)\.lin$", re.I)


class Failed(Exception):
    pass


class Fatal(Failed):
    """A failure that going back to the main menu would not mend."""


# ---- frames ------------------------------------------------------------------

u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32
u32.SetProcessDPIAware()


class BIH(ctypes.Structure):
    _fields_ = [("biSize", W.DWORD), ("biWidth", W.LONG), ("biHeight", W.LONG), ("biPlanes", W.WORD),
                ("biBitCount", W.WORD), ("biCompression", W.DWORD), ("biSizeImage", W.DWORD),
                ("biXPelsPerMeter", W.LONG), ("biYPelsPerMeter", W.LONG), ("biClrUsed", W.DWORD),
                ("biClrImportant", W.DWORD)]


def find_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def each(h, _):
        p = W.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            r = W.RECT()
            u32.GetClientRect(h, ctypes.byref(r))
            if r.right >= 320:
                found.append(h)
        return True
    u32.EnumWindows(each, 0)
    return found[0] if found else None


def grab_window(h, aspect=4 / 3):
    """The window's client area, as D3D presented it, even when covered: the picture's
    largest centred rectangle of that aspect, which the present letterboxes into it."""
    r = W.RECT()
    if not u32.GetClientRect(h, ctypes.byref(r)) or r.right <= 0 or r.bottom <= 0:
        return None
    w, hh = r.right, r.bottom
    wdc = u32.GetDC(h)
    mdc = g32.CreateCompatibleDC(wdc)
    bmp = g32.CreateCompatibleBitmap(wdc, w, hh)
    old = g32.SelectObject(mdc, bmp)
    ok = u32.PrintWindow(h, mdc, 1 | 2)          # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    bi = BIH(biSize=ctypes.sizeof(BIH), biWidth=w, biHeight=-hh, biPlanes=1, biBitCount=32)
    buf = ctypes.create_string_buffer(w * hh * 4)
    got = g32.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(bi), 0)
    g32.SelectObject(mdc, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mdc)
    u32.ReleaseDC(h, wdc)
    if not ok or got != hh:
        return None
    a = np.frombuffer(buf, np.uint8).reshape(hh, w, 4)[:, :, 2::-1]
    pw, ph = (w, int(round(w / aspect))) if w / hh < aspect else (int(round(hh * aspect)), hh)
    x0, y0 = (w - pw) // 2, (hh - ph) // 2
    return Image.fromarray(a[y0:y0 + ph, x0:x0 + pw].copy())


def video_setting(env, var, key, default):
    """A setting from its RECOMP_ variable, else the settings panel's file."""
    v = env.get(var, "")
    if not v:
        ini = configparser.ConfigParser()
        try:
            ini.read(os.path.join(env.get("LOCALAPPDATA", ""), "xboxrecomp", "ucc2_video.ini"))
            v = ini.get("video", key, fallback=str(default))
        except configparser.Error:
            v = str(default)
    try:
        return int(v) != 0
    except ValueError:
        return bool(default)


def picture_shape(env):
    """(the shown picture's aspect, the share of its width the menus fill, in the
    middle). 16:9 widescreen keeps menus at their shape in the middle 3/4, and
    spreads the match HUD by thirds (see Frames.grab)."""
    if not video_setting(env, "RECOMP_WIDESCREEN", "widescreen", 0):
        return 4 / 3, 1.0
    return 16 / 9, 0.75 if video_setting(env, "RECOMP_WIDE_VIEW", "wide_view", 0) else 1.0


class Frames:
    """The latest frame, from the window or from RECOMP_FB_DUMP files. Frames are
    compared at 640x480, over the part the menus fill, and saved as shown (16:9 when widescreen)."""

    def __init__(self, run, pid, dump_prefix, keep, shape=(4 / 3, 1.0)):
        self.run, self.pid, self.dump_prefix, self.keep = run, pid, dump_prefix, keep
        self.aspect, self.hud_share = shape
        self.hwnd = None
        self.n = 0
        self.last = self.last_raw = self.last_hud = None
        self.last_t = 0.0

    def grab(self, t):
        im = raw = None                 # compared; as shown
        if self.dump_prefix:
            files = sorted(glob.glob(self.dump_prefix + "*.bmp"))
            # The newest may still be being written.
            for f in reversed(files[-2:]):
                try:
                    raw = Image.open(f).convert("RGB")
                    raw.load()
                    break
                except (OSError, ValueError):
                    continue
        else:
            if self.hwnd is None or not u32.IsWindow(self.hwnd):
                self.hwnd = find_window(self.pid)
            if self.hwnd:
                raw = grab_window(self.hwnd, self.aspect)
        if raw is not None:
            # Compared where the menus and HUD are, at the references' size.
            cut = int(round(raw.width * (1 - self.hud_share) / 2))
            im = raw.crop((cut, 0, raw.width - cut, raw.height)).resize((640, 480))
            hud = im
            if self.hud_share < 1.0:
                # The match HUD keeps its shape in thirds of the 16:9 picture: the
                # left third, the middle and the right third make the 4:3 layout.
                w, h = raw.size
                third = int(round(w * self.hud_share / 3))
                hud = Image.new("RGB", (3 * third, h))
                for i, x in enumerate((0, (w - third) // 2, w - third)):
                    hud.paste(raw.crop((x, 0, x + third, h)), (i * third, 0))
                hud = hud.resize((640, 480))
            self.last, self.last_raw, self.last_hud, self.last_t = im, raw, hud, t
            if self.keep:
                self.last_raw.save(os.path.join(self.run, "frames", "%05d_%06.1f.png" % (self.n, t)))
            self.n += 1
        return im

    def save(self, name):
        if self.last is not None:
            self.last_raw.save(os.path.join(self.run, name + ".png"))


def thumb(im, crop, mode="L"):
    box, size = crop
    return np.asarray(im.convert(mode).crop(box).resize(size)).astype(np.float32)


def dist(im, ref, crop, mode="L"):
    """Mean absolute difference of two shrunk crops, as tools/sheet.py measures frames."""
    if im is None or ref is None:
        return 999.0
    return float(np.abs(thumb(im, crop, mode) - thumb(ref, crop, mode)).mean())


def lit_item(im):
    """Which main menu item has the bright bar, or None if no single one does."""
    a = np.asarray(im.convert("L")).astype(np.float32)
    rows = [float(a[y - 6:y + 7, 60:280].mean()) for y in MAIN_ROWS]
    order = sorted(range(len(rows)), key=lambda i: -rows[i])
    if rows[order[0]] > 70 and rows[order[0]] > 1.8 * rows[order[1]]:
        return order[0]
    return None


def load_refs():
    refs = {}
    for name in ("main_menu", "scoreboard", "match", "title", "story_menu", "corridor"):
        p = os.path.join(ROUTE, name + ".png")
        refs[name] = Image.open(p).convert("RGB").resize((640, 480)) if os.path.exists(p) else None
    if refs["match"] is not None:
        hsv = np.asarray(refs["match"].convert("HSV")).astype(np.int16)
        box = np.zeros(hsv.shape[:2], bool)
        for x0, y0, x1, y1 in HUD_BOXES:
            box[y0:y1, x0:x1] = True
        refs["hud_mask"] = box & (hsv[..., 1] > 110) & (hsv[..., 2] > 70)
        refs["hud_rgb"] = np.asarray(refs["match"]).astype(np.int16)
    return refs


def hud_share(im, refs):
    """How much of the match HUD is on screen, 0..1."""
    if refs.get("hud_mask") is None or im is None:
        return 0.0
    a = np.asarray(im.convert("RGB")).astype(np.int16)
    return float((np.abs(a - refs["hud_rgb"]).max(axis=2)[refs["hud_mask"]] < 60).mean())


# ---- the run -----------------------------------------------------------------

class Driver:
    def __init__(self, args, run):
        self.args, self.run = args, run
        self.log_path = os.path.join(run, "log.txt")
        self.pad_path = os.path.join(run, "pad.txt")
        self.route = open(os.path.join(run, "route.txt"), "w")
        self.refs = load_refs()
        self.events = []          # (t, file path) for successful opens
        self.pos = 0
        self.partial = b""
        self.state = "launch"
        self.t0 = time.time()
        self.proc = None
        self.frames = None
        self.resets = 0
        self.last_press = 0
        self.reached = None
        want = args.map.split("-", 1)
        self.want_type = want[0].upper() if len(want) == 2 else "CTF"
        self.want_arena = want[-1]

    # -- plumbing

    def now(self):
        return time.time() - self.t0

    def note(self, text):
        line = "%7.1f  %-11s %s" % (self.now(), self.state, text)
        self.route.write(line + "\n")
        self.route.flush()
        if self.args.verbose:
            print(line, flush=True)

    def enter(self, state):
        self.state = state
        self.note("--")

    def launch(self):
        env = dict(os.environ)
        env.update(SETTINGS)
        env["RECOMP_WATCHDOG_SECS"] = str(self.args.timeout)
        if self.args.boot_waits:
            env["RECOMP_SKIP_BOOT_WAITS"] = "0"     # 0 also beats the settings panel's ini
        dump = None
        if self.args.no_window or self.args.dumps:
            dump = os.path.join(self.run, "dump_")
            env["RECOMP_FB_DUMP"] = dump
            env["RECOMP_PB_REPORT_MS"] = "1000"
        if not self.args.no_window:
            env["RECOMP_FB_WINDOW"] = "1"
        for pair in self.args.env.split(";"):
            if "=" in pair:
                k, v = pair.split("=", 1)
                env[k.strip()] = v
        if env.get("RECOMP_FB_DUMP") and not dump:
            dump = env["RECOMP_FB_DUMP"]
        env["RECOMP_LOG"] = self.log_path
        env["RECOMP_PAD_LIVE"] = self.pad_path
        open(self.pad_path, "w").close()
        exe = getattr(self.args, "exe", None) or EXE      # tools/ab.py can run another build
        self.proc = subprocess.Popen([exe], cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.t0 = time.time()
        self.frames = Frames(self.run, self.proc.pid, dump, self.args.keep_frames,
                             picture_shape(env))
        self.note("pid %d, frames from %s" % (self.proc.pid, "dumps" if dump else "the window"))

    def poll(self):
        """Read new log lines; fail if the game has gone or the time is up."""
        try:
            with open(self.log_path, "rb") as f:
                f.seek(self.pos)
                data = f.read()
            self.pos += len(data)
        except OSError:
            data = b""
        data = self.partial + data
        lines = data.split(b"\n")
        self.partial = lines.pop()
        t = self.now()
        for raw in lines:
            ln = raw.decode("latin-1").rstrip()
            m = RE_FILE.search(ln)
            if m:
                self.events.append((t, m.group(1)))
        if self.proc.poll() is not None:
            raise Fatal("game exited with code %d" % self.proc.returncode)
        if t > self.args.timeout:
            raise Fatal("timeout")

    def wait(self, secs):
        end = time.time() + secs
        while time.time() < end:
            self.poll()
            time.sleep(0.1)

    def press(self, button, hold=100):
        # A screen ignores input while it slides in: give it a moment after the
        # file that announced it (or whatever the game last opened).
        end = self.now() + SETTLE_MAX
        while self.events and self.now() - self.events[-1][0] < SETTLE and self.now() < end:
            self.wait(0.1)
        self.last_press = len(self.events)
        with open(self.pad_path, "a") as f:
            f.write("%s:%d\n" % (button, hold))
        self.note("press %s" % button)
        return self.last_press

    def seen(self, pattern, since):
        """The last event from index `since` on whose path matches, as a match object."""
        rx = re.compile(pattern, re.I)
        for t, path in reversed(self.events[since:]):
            m = rx.search(path)
            if m:
                return m
        return None

    def wait_for(self, pattern, since, secs):
        end = self.now() + secs
        while self.now() < end:
            self.poll()
            m = self.seen(pattern, since)
            if m:
                return m
            time.sleep(0.1)
        return None

    def frame(self):
        self.poll()
        return self.frames.grab(self.now())

    def wait_frame(self, test, secs, every=0.5):
        """Grab frames until test(frame) is true; the last frame either way."""
        end = self.now() + secs
        im = None
        while True:
            im = self.frame()
            if im is not None and test(im):
                return im, True
            if self.now() >= end:
                return im, False
            self.wait(every)

    def is_main(self, im):
        return dist(im, self.refs["main_menu"], HEADER) < HEADER_MAX and lit_item(im) is not None

    def is_title(self, im):
        return self.refs["title"] is not None and dist(im, self.refs["title"], WHOLE) < 15

    def is_scoreboard(self, im):
        return dist(im, self.refs["scoreboard"], WHOLE) < SCORE_MAX

    def is_match(self, im):
        hud = self.frames.last_hud if self.frames and im is self.frames.last else im
        return hud_share(hud, self.refs) >= HUD_MIN and not self.is_scoreboard(im)

    # -- the route

    def drive(self):
        self.launch()
        self.enter("boot")
        if not self.wait_for(r"UI_Start_Movie\w*\.xmv$", 0, 180):
            raise Failed("no title movie")
        self.enter("title")
        for _ in range(6):
            since = self.press("start")
            if self.wait_for(r"UI_LoadingMovie\.xmv$", since, 4):
                break
        else:
            raise Failed("Start not taken")
        self.menus_retried()
        self.match()

    def menus_retried(self, resets=3):
        """menus(), backing out to the main menu and starting again after a wrong turn."""
        tries = 0
        while True:
            try:
                self.menus()
                return
            except Failed as e:
                # Only a wrong turn in the menus is retried from the main menu.
                if isinstance(e, Fatal) or tries >= resets:
                    raise
                tries += 1
                self.resets += 1
                self.note("reset %d: %s" % (self.resets, e))
                self.back_to_main()

    def back_to_main(self):
        self.enter("backing")
        for _ in range(10):
            im, ok = self.wait_frame(self.is_main, 2.0)
            if ok:
                return
            if im is not None and self.is_title(im):
                since = self.press("start")
                self.wait_for(r"UI_LoadingMovie\.xmv$", since, 4)
            else:
                self.press("b")
        raise Failed("could not get back to the main menu")

    def menus(self):
        self.enter("main menu")
        target = MAIN_ITEMS.index("instant")
        for _ in range(8):
            im, ok = self.wait_frame(self.is_main, 8)
            if not ok:
                self.frames.save("not_main_menu")
                raise Failed("main menu not seen (header distance %.1f)"
                             % dist(im, self.refs["main_menu"], HEADER))
            lit = lit_item(im)
            if lit == target:
                break
            self.note("lit: %s" % MAIN_ITEMS[lit])
            self.press("down" if lit < target else "up")
            self.wait(0.6)
        else:
            raise Failed("could not light Instant Action")
        since = self.press("a")
        if not self.wait_for(r"Portrait_GameType_\w+\.xmv$", since, 5):
            self.frames.save("after_instant_action")
            raise Failed("A on Instant Action opened no game type")

        self.enter("game type")
        wanted = [w.lower() for w in GAMETYPES.get(self.want_type, [self.want_type])]
        seen = []
        for _ in range(10):
            m = self.seen(r"Portrait_GameType_(\w+)\.xmv$", 0)
            cur = m.group(1)
            if cur.lower() in wanted:
                break
            if cur in seen:
                raise Fatal("no game type for %s among %s" % (self.want_type, ", ".join(seen)))
            seen.append(cur)
            since = self.press("down")
            self.wait_for(r"Portrait_GameType_\w+\.xmv$", since, 3)
        self.note("type %s" % cur)
        self.step("a", r"(Maplists\.ini|Portrait_Arena_\w+\.xmv)$", "rules")
        if not self.seen(r"Portrait_Arena_\w+\.xmv$", self.last_press):
            self.step("a", r"Portrait_Arena_\w+\.xmv$", "arena")
        else:
            self.enter("arena")

        seen = []
        for _ in range(30):
            cur = self.seen(r"Portrait_Arena_(\w+)\.xmv$", 0).group(1)
            if cur.lower() == self.want_arena.lower():
                break
            if cur in seen:
                raise Fatal("no arena %s among %s" % (self.want_arena, ", ".join(seen)))
            seen.append(cur)
            since = self.press("down")
            self.wait_for(r"Portrait_Arena_\w+\.xmv$", since, 3)
        self.note("arena %s" % cur)
        self.step("a", r"Portrait_Char_\w+\.xmv$", "character")
        self.step("a", r"Portrait_Armory_\w+\.xmv$", "armory")

        # Each category takes an A, then "Begin Match?" takes one more.
        for _ in range(8):
            since = self.press("a")
            m = self.wait_for(r"(Portrait_Armory_\w+\.xmv|XboxMaps\\[\w-]+\.lin)$", since, 3)
            if m and m.group(0).lower().endswith(".lin"):
                break
        else:
            self.frames.save("armory_stuck")
            raise Failed("no map loaded from the armory")
        name = self.seen(r"XboxMaps\\([\w-]+)\.lin$", 0).group(1)
        if name.lower() != self.args.map.lower():
            raise Fatal("loaded %s, not %s" % (name, self.args.map))
        self.enter("loading")
        self.note("map %s" % name)

    def step(self, button, pattern, next_state, tries=3):
        for _ in range(tries):
            since = self.press(button)
            if self.wait_for(pattern, since, 4):
                self.enter(next_state)
                return
        self.frames.save("stuck_" + self.state.replace(" ", "_"))
        raise Failed("%s did not lead to %s" % (button, next_state))

    def match(self):
        # The load shows the scoreboard with "LOADING...", then "PRESS FIRE".
        # A is pressed every few seconds until the HUD shows: route/scoreboard.png
        # only matches with Aqueduct behind it, so it is noted, not waited for.
        t_map = self.now()
        hud = 0
        board = False
        last_a = 0.0
        self.enter("scoreboard")
        while True:
            im = self.frame()
            if im is not None and self.is_match(im):
                hud += 1
                if hud >= 2:
                    break
            else:
                hud = 0
            if im is not None and not board and self.is_scoreboard(im):
                board = True
                self.note("scoreboard seen")
            if hud == 0 and self.now() - last_a > 2.5:
                self.press("a")
                last_a = self.now()
            if self.now() - t_map > 120:
                self.frames.save("not_in_match")
                raise Failed("no match %d s after the map loaded (HUD share %.2f)"
                             % (self.now() - t_map, hud_share(im, self.refs)))
            self.wait(0.5)
        self.enter("match")
        self.reached = self.now()
        self.frames.save("match_first")

    def rematch(self):
        """Forfeit the match and drive into a new one, which spawns the player anew.

        Start opens the pause menu; Forfeit Game is its fourth item and asks to be
        confirmed, then the game type screen shows. About 33 s in all."""
        self.enter("forfeit")
        self.press("start", 150)
        self.wait(1.5)
        for _ in range(3):
            self.press("down", 150)
            self.wait(0.6)
        self.press("a", 150)
        self.wait(1.5)
        since = self.press("a", 150)
        if not self.wait_for(r"Portrait_GameType_\w+\.xmv$", since, 8):
            self.frames.save("forfeit_stuck")
            raise Failed("forfeit did not reach the game type screen")
        self.back_to_main()
        self.menus_retried()
        self.match()

    def in_view(self, im, name="corridor"):
        """Whether frame im shows the scene of reference route/<name>.png, HUD up."""
        return (im is not None and self.refs.get(name) is not None and self.is_match(im)
                and dist(im, self.refs[name], SCENE) < SCENE_MAX)


def stay(drv, secs):
    """Keep the match running; sample its frames, say whether the HUD stayed up."""
    end = drv.now() + secs
    hud = total = 0
    mid_saved = False
    while drv.now() < end:
        drv.wait(min(5.0, max(0.1, end - drv.now())))
        im = drv.frame()
        if im is None:
            continue
        total += 1
        hud += drv.is_match(im)
        if not mid_saved and drv.now() > end - secs / 2:
            drv.frames.save("match_mid")
            mid_saved = True
    drv.frames.save("match_last")
    return hud, total


def main():
    ap = argparse.ArgumentParser(description="Drive the game into a match by watching it.")
    ap.add_argument("--secs", type=float, default=120, help="time to stay in the match")
    ap.add_argument("--timeout", type=int, default=600, help="hard limit for the whole run, s")
    ap.add_argument("--map", default="CTF-Aqueduct")
    ap.add_argument("--name", default=None, help="run folder under build/runs/")
    ap.add_argument("--env", default="", help='overrides, "K=V;K=V"')
    ap.add_argument("--no-window", action="store_true", help="no game window; frames from dumps")
    ap.add_argument("--dumps", action="store_true", help="frames from RECOMP_FB_DUMP (slows the game)")
    ap.add_argument("--boot-waits", action="store_true",
                    help="keep the title's two boot sleeps (about 9 s; skipped by default)")
    ap.add_argument("--keep-frames", action="store_true", help="keep every frame looked at")
    ap.add_argument("--min-fps", type=float, default=None, help="passed to check_run as min_match_fps")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the route as it goes")
    args = ap.parse_args()

    if not os.path.exists(EXE):
        sys.exit("no %s: build first" % EXE)
    if not os.path.exists(os.path.join(ROUTE, "main_menu.png")):
        print("warning: no reference frames in %s; menu and match screens cannot be recognised" % ROUTE)
    name = args.name or time.strftime("drive-%Y%m%d-%H%M%S")
    run = os.path.join(ROOT, "build", "runs", name)
    if os.path.isdir(run):
        shutil.rmtree(run)
    os.makedirs(os.path.join(run, "frames") if args.keep_frames else run)

    drv = Driver(args, run)
    ok = False
    try:
        drv.drive()
        ok = True
        try:
            hud, total = stay(drv, args.secs)
            early = ""
        except Failed as e:
            hud, total, early = 0, 0, "; match ended early: %s at %.1f s" % (e, drv.now())
            ok = False
        print("ROUTE reached match at %.1f s" % drv.reached)
        print("match: HUD in %d of %d frames%s" % (hud, total, early))
    except Failed as e:
        drv.frames and drv.frames.save("last")
        print("ROUTE failed: %s (%s) after %.1f s" % (drv.state, e, drv.now()))
    except KeyboardInterrupt:
        print("ROUTE failed: %s (interrupted) after %.1f s" % (drv.state, drv.now()))
    finally:
        drv.note("end")
        if drv.proc and drv.proc.poll() is None:
            drv.proc.kill()
            drv.proc.wait(10)
        if drv.frames and not args.keep_frames and drv.frames.dump_prefix:
            for f in glob.glob(drv.frames.dump_prefix + "*.bmp"):
                os.remove(f)
    print("log: %s" % drv.log_path)

    # tools/check_run.py reads the log on its own and says whether the run worked.
    # Appended, not prepended: tools/dis.py would shadow the standard library's dis.
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    try:
        from check_run import analyse
    except ImportError:
        print("check: tools/check_run.py not found, log not checked")
        return 0 if ok else 1
    r = analyse(drv.log_path, expect_match=True, allow_watchdog=True, min_match_fps=args.min_fps)
    fx = r.get("frames_match") or {}
    fps = fx.get("settled_fps") if fx.get("settled_fps") is not None else fx.get("mean_fps")
    print("check: %s, match %s fps%s" % (r["verdict"], "-" if fps is None else "%.1f" % fps,
                                        "".join("; " + s for s in r.get("reasons", []))))
    return 0 if ok and r["verdict"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
