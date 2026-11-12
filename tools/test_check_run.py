"""Tests for check_run.py on made-up log text.

    python -I tools/test_check_run.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_run import analyse  # noqa: E402


def frames(n, fps, cpu=5.0):
    ms = 1000.0 / fps
    return ("[FRAMES] %d in 10.0 s (%.1f fps): ms p50 %.1f p90 %.1f p99 %.1f max %.1f, >25 ms 0, >50 ms 0"
            " | per frame: d3d11 1.00, present 0.50, other walker cpu %.2f, walker idle 0.10 (limit 0.00)"
            % (n, fps, ms, ms, ms, ms + 5, cpu))


def maplog(name):
    return "  [FILE] Z:\\XboxMaps\\%s.lin (disp 1 opts 0x60 from 0x00000000) -> 0x00000000" % name


HEAD = [
    "[BOOT] emulated APU up",
    "  [FILE] Z:\\Nope.ini (disp 1) -> 0xC0000034 FAILED (win32 err=2 ERROR_FILE_NOT_FOUND)",
    "  [FILE] Z:\\Nope.ini (disp 1) -> 0xC0000034 FAILED (win32 err=2 ERROR_FILE_NOT_FOUND)",
    "[UNIMPL] untranslated instruction REACHED: `cli` at 0x00001000 (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)",
    "[UNIMPL] untranslated instruction REACHED: `cli` at 0x00001000 (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)",
    maplog("Menu"),
    "  [FBWIN] the renderer presents to the window now",
    "  PAD: script of 2 step(s)",
    frames(600, 60.0),
    "  PAD: t=1000 ms step 0 (digital 0x10 analog 0x00)",
    maplog("Menu"),
    "  PAD: t=2000 ms step 1 (digital 0x00 analog 0x01)",
    frames(600, 60.0),
    maplog("DM-Test"),
    "[HITCH] 300 ms frame at 22.0 s: walker cpu 290 ms (d3d11 1, present 1)",
    "[HITCH]   walker (10 samples from 50 ms): x 10",
    frames(300, 30.0),
    frames(400, 40.0, cpu=10.0),
    frames(400, 40.0, cpu=10.0),
]


class CheckRun(unittest.TestCase):
    def run_log(self, lines, **kw):
        fd, path = tempfile.mkstemp(suffix=".txt")
        with os.fdopen(fd, "w") as f:
            f.write("\n".join(lines) + "\n")
        try:
            return analyse(path, **kw)
        finally:
            os.remove(path)

    def test_match_timer(self):
        r = self.run_log(HEAD + ["[WATCHDOG] no exit after 55s; guest esp=0x00000000"])
        self.assertEqual([m["name"] for m in r["maps"]], ["Menu", "DM-Test"])
        self.assertTrue(r["reached_match"])
        self.assertEqual(r["match_map"], "DM-Test")
        self.assertEqual(r["watchdog_kind"], "timer")
        self.assertEqual(r["verdict"], "pass", r["reasons"])
        self.assertEqual(r["frames_menu"]["lines"], 2)
        self.assertEqual(r["frames_match"]["frames"], 1100)
        self.assertEqual(r["frames_match"]["worst_fps"], 30.0)
        self.assertEqual(r["frames_match"]["settled_fps"], 40.0)
        self.assertEqual(r["hitches"]["count"], 1)
        self.assertEqual(r["hitches"]["worst_ms"], 300)
        self.assertEqual(r["pad_steps"], 2)
        self.assertEqual(r["unimpl_reached"], ["0x00001000"])
        self.assertEqual(r["failed_opens"]["Z:\\Nope.ini"]["count"], 2)
        self.assertEqual(r["maps"][1]["t_secs"], 21.0)

    def test_lows(self):
        # Older logs have no lows; newer ones end the [FRAMES] line with them.
        r = self.run_log(HEAD + [frames(400, 40.0) + " | lows: 1% 31.25 ms, 0.1% 48.00 ms",
                                 "[WATCHDOG] no exit after 55s; guest esp=0x00000000"])
        self.assertEqual(r["verdict"], "pass", r["reasons"])
        self.assertEqual(r["frames_match"]["max_low1_ms"], 31.25)
        self.assertEqual(r["frames_match"]["max_low01_ms"], 48.0)
        self.assertEqual(r["frames_menu"]["max_low1_ms"], None)

    def test_fps_threshold(self):
        r = self.run_log(HEAD, min_match_fps=50)
        self.assertEqual(r["verdict"], "fail")
        self.assertEqual(self.run_log(HEAD, min_match_fps=35)["verdict"], "pass")

    def test_hang(self):
        r = self.run_log(HEAD + ["[WATCHDOG] no exit after 300s; guest esp=0x00000000"])
        self.assertEqual(r["watchdog_kind"], "hang")
        self.assertEqual(r["verdict"], "fail")

    def test_strict_watchdog(self):
        r = self.run_log(HEAD + ["[WATCHDOG] no exit after 55s; guest esp=0x00000000"], allow_watchdog=False)
        self.assertEqual(r["verdict"], "fail")

    def test_crash_first(self):
        r = self.run_log(HEAD + ["[CRASH] Access violation at RIP=0x1, fault addr=0x2 (read)",
                                 "  Xbox VA of fault: 0x00000002",
                                 "[CRASH] Access violation at RIP=0x3, fault addr=0x4 (read)"])
        self.assertIn("RIP=0x1", r["crash"])
        self.assertEqual(r["crash_va"], "0x00000002")
        self.assertEqual(r["crash_count"], 2)
        self.assertFalse(r["closed_cleanly"])
        self.assertEqual(r["verdict"], "fail")

    def test_menu_only_closed(self):
        lines = HEAD[:13] + ["  [FBWIN] window closed: exiting"]
        r = self.run_log(lines)
        self.assertFalse(r["reached_match"])
        self.assertTrue(r["closed_cleanly"])
        self.assertEqual(r["verdict"], "fail")
        self.assertEqual(self.run_log(lines, expect_match=False)["verdict"], "pass")

    def test_map_without_frames(self):
        r = self.run_log(HEAD[:13] + [maplog("DM-Test")])
        self.assertEqual(r["match_map"], "DM-Test")
        self.assertFalse(r["reached_match"])


if __name__ == "__main__":
    unittest.main()
