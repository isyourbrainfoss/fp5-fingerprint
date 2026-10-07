"""Status parser: PRESS / TZ / template / match honesty."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app"))

from fp5_fp_status import (  # noqa: E402
    build_status,
    drop_old_ktime,
    hero_plain,
    match_hero_while_matching,
    match_should_leave_wait,
    next_enroll_mode,
    scan_text,
    score,
    window_last_auth_kick,
    window_this_run,
)

HEALTH = """
[  12.1] START focal32 ret=0 APP_START success id=0x5
[  12.4] sn num: 0b0a6944171209070800fb
[  13.0] HEALTH final er=0
[  13.1] listener id=10 registered OK
"""

FID_ONLY = HEALTH + """
[  20.0] new fid (= 1841345350)
[  20.1] AUTH er=0 rem_seen=0
[  20.2] enrollment is unexpected broken off
"""

TEMPLATE = HEALTH + """
[  30.0] REAL DOWN
[  30.1] frame raw[0]: 112x88
[  30.2] enrolled fid 42 rem = 19
[  30.3] new fid (= 42)
[  30.4] rem_seen=1
"""

MATCH = TEMPLATE + """
[  40.0] AUTH er=0 rem_seen=1
"""

IMAGE_FAKE = HEALTH + """
[  21.0] IMAGE PIXELS avg=200
[  21.1] new fid (= 7)
"""


class Scan(unittest.TestCase):
    def test_health_is_tz_not_template(self) -> None:
        ev = scan_text(HEALTH)
        sc = score(ev)
        self.assertTrue(sc["tz_up"])
        self.assertFalse(sc["template"])
        self.assertFalse(sc["match"])

    def test_fid_without_frame_is_not_template(self) -> None:
        ev = scan_text(FID_ONLY)
        sc = score(ev)
        self.assertTrue(sc["fid_only"])
        self.assertFalse(sc["template"])
        self.assertFalse(sc["match"])
        self.assertTrue(ev["broken_off"])

    def test_template_needs_frame_and_rem(self) -> None:
        ev = scan_text(TEMPLATE)
        sc = score(ev)
        self.assertTrue(sc["template"])
        self.assertFalse(sc["match"])
        self.assertEqual(ev["new_fid"], "42")

    def test_match_rejects_broken_off(self) -> None:
        ev = scan_text(FID_ONLY)
        sc = score(ev)
        self.assertTrue(ev["auth_er0"])
        self.assertFalse(sc["match"])

    def test_match_ok(self) -> None:
        ev = scan_text(MATCH)
        sc = score(ev)
        self.assertTrue(sc["match"])

    def test_empty_sensor_avgv_is_not_template(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 9)
[  30.1] frame raw[0]: avgv = 864, minv = 745, maxv = 917.
[  30.2] enroll sample loop done rem_seen=1
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["empty_sensor"])
        self.assertEqual(ev["frame_avgv"], 864)
        self.assertFalse(sc["template"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "NO FINGER")
        self.assertIn("power button", p.lower())

    def test_quality_pass_low_avgv_is_finger_seen(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 1627438910)
[  30.1] frame raw[0]: avgv = 55, minv = 0, maxv = 345.
[  30.2] ION HIT dev-REPORT-ev5 ret=0 req0=0x80001017
[  30.3] quality = 68 (thr = 40), area = 99 (thr = 70)
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertFalse(ev["empty_sensor"])
        self.assertTrue(ev["qual_pass"])
        self.assertFalse(sc["template"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "SENSOR READY")
        self.assertNotEqual(h, "FINGER SEEN")
        self.assertTrue(sc["tz_up"])
        self.assertIn("no reboot", p.lower())
        self.assertNotIn("Linux reboot", p)

    def test_overlay_rem_19_is_enrolling_not_saved(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 1167449860)
[  30.1] REAL DOWN
[  30.2] frame raw[0]: avgv = 133, minv = 0, maxv = 444.
[  30.3] ION HIT dev-REPORT-ev5 ret=0 req0=0x80001017
[  30.4] quality = 80 (thr = 40), area = 99 (thr = 70)
[  30.5] overlay rem=19 fid=0x4595df04 sample=0
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertEqual(ev["overlay_rem"], 19)
        self.assertFalse(sc["template"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "ENROLLING")
        self.assertIn("19 remaining", p)
        self.assertNotEqual(h, "SAVED")
        self.assertNotEqual(h, "SENSOR READY")

    def test_enroll_phase_press_ok_is_accepted(self) -> None:
        text = HEALTH + """
[  30.0] overlay PRESS OK rem=17 sample=3
[  30.1] overlay rem=17 fid=0x1 sample=3
"""
        ev = scan_text(text)
        sc = score(ev)
        h, p = hero_plain(ev, sc, phase="enroll")
        self.assertEqual(ev["press_result"], "ok")
        self.assertEqual(h, "ACCEPTED")
        self.assertIn("17 remaining", p)

    def test_enroll_phase_press_empty_is_miss(self) -> None:
        text = HEALTH + """
[  30.0] overlay PRESS EMPTY avgv=863 sample=4
[  30.1] frame raw[0]: avgv = 863, minv = 745, maxv = 917.
"""
        ev = scan_text(text)
        sc = score(ev)
        h, p = hero_plain(ev, sc, phase="enroll")
        self.assertEqual(ev["press_result"], "empty")
        self.assertEqual(h, "MISS")
        self.assertIn("no finger", p.lower())
        self.assertIn("still", p.lower())

    def test_enroll_empty_then_later_banner_is_hold_not_miss(self) -> None:
        text = HEALTH + """
[  30.0] overlay rem=17 fid=0x1 sample=3
[  30.1] overlay PRESS EMPTY avgv=863 sample=4
[  30.2] *** PRESS then LIFT finger sample 5/25 (HOLD on power-button sensor) ***
"""
        ev = scan_text(text)
        sc = score(ev)
        h, p = hero_plain(ev, sc, phase="enroll")
        self.assertIsNone(ev["press_result"])
        self.assertEqual(h, "HOLD FINGER")
        self.assertNotEqual(h, "MISS")

    def test_overlay_rem_19_empty_avgv_is_not_enrolling(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 1167449860)
[  30.1] REAL DOWN
[  30.2] frame raw[0]: avgv = 863, minv = 745, maxv = 917.
[  30.3] quality = 5 (thr = 40), area = 44 (thr = 70)
[  30.4] overlay rem=19 fid=0x411dda44 sample=19
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertEqual(ev["overlay_rem"], 19)
        self.assertTrue(ev["empty_sensor"])
        self.assertFalse(ev["qual_pass"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertNotEqual(h, "ENROLLING")
        self.assertEqual(h, "NO FINGER")
        self.assertIn("19 remaining", p)
        self.assertIn("close", p.lower())
        self.assertNotIn("starts at 19", p)
        self.assertNotIn("Register", p)

    def test_overlay_rem_0_without_fid_in_window_is_saved(self) -> None:
        text = HEALTH + """
[  30.1] REAL DOWN
[  30.2] frame raw[0]: avgv = 260, minv = 0, maxv = 500.
[  30.3] overlay rem=0 fid=0x638eb0e4 sample=19
[  30.4] overlay PRESS OK rem=0 sample=19
[  30.5] overlay SAVE after REPORT er=0 enroll=0 qerr=0 rem=0
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertEqual(ev["overlay_rem"], 0)
        self.assertTrue(ev["rem_zero"])
        self.assertIsNone(ev["new_fid"])
        self.assertTrue(sc["template"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "SAVED")

    def test_overlay_rem_0_is_saved(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 1167449860)
[  30.1] REAL DOWN
[  30.2] frame raw[0]: avgv = 133, minv = 0, maxv = 444.
[  30.3] overlay rem=0 fid=0x4595df04 sample=19
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertEqual(ev["overlay_rem"], 0)
        self.assertTrue(ev["rem_zero"])
        self.assertTrue(sc["template"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "SAVED")

    def test_too_dark_avgv_is_not_template(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 1897820660)
[  30.1] frame raw[0]: avgv = 49, minv = 0, maxv = 343.
[  30.2] enroll sample loop done rem_seen=1
[  30.3] AUTH er=0 rem_seen=1
[  30.4] enrollment is unexpected broken off
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["empty_sensor"])
        self.assertEqual(ev["frame_avgv"], 49)
        self.assertFalse(sc["template"])
        h, _ = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "NO FINGER")

    def test_image_pixels_is_not_frame(self) -> None:
        ev = scan_text(IMAGE_FAKE)
        sc = score(ev)
        self.assertTrue(ev["image_pixels"])
        self.assertEqual(ev["frame_raw"], 0)
        self.assertFalse(sc["template"])

    def test_no_real_down_is_not_press(self) -> None:
        ev = scan_text("sample 0 no REAL DOWN — FORCE idle CAP")
        self.assertFalse(ev["real_down"])
        self.assertFalse(score(ev)["press_ok"])

    def test_overlay_auth_er0_is_match(self) -> None:
        text = HEALTH + """
[  30.1] REAL DOWN
[  30.2] frame raw[0]: avgv = 260, minv = 0, maxv = 500.
[  30.3] overlay AUTH er=0 err=0x0 sample=0
[  30.4] overlay AUTH HIT err=0x0 sample=0
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["auth_er0"])
        self.assertTrue(sc["match"])
        self.assertFalse(ev["broken_off"])

    def test_overlay_auth_ta_err_is_not_match(self) -> None:
        text = HEALTH + """
[  30.1] REAL DOWN
[  30.2] overlay AUTH er=0 err=0xffffff38 sample=0
[  30.3] overlay AUTH FAIL err=0xffffff38 tmpl_missing=1 id_ok=0 (not a match)
[  30.4] templates with gid(23 != 0) hasn't been loaded.
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["auth_fail"])
        self.assertFalse(sc["match"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "NO MATCH")
        self.assertNotEqual(h, "MATCH")

    def test_enroll_phase_press_weak_is_try_again(self) -> None:
        text = HEALTH + """
[  30.0] overlay rem=19 fid=0x1 sample=0
[  30.1] overlay PRESS WEAK slots=0x1 inte=0 sample=1
"""
        ev = scan_text(text)
        sc = score(ev)
        h, p = hero_plain(ev, sc, phase="enroll")
        self.assertEqual(ev["press_result"], "weak")
        self.assertEqual(h, "TRY AGAIN")
        self.assertIn("same finger", p.lower())
        self.assertIn("other fingers", p.lower())

    def test_auth_kick_flag(self) -> None:
        ev = scan_text(HEALTH + "\noverlay AUTH kick — wait itype 0x2\n")
        self.assertTrue(ev["auth_kick"])

    def test_match_fail_line_is_auth_fail_not_wait(self) -> None:
        text = HEALTH + """
[  30.1] overlay rem=0
[  30.2] overlay AUTH kick — wait lift then PRESS
[  30.3] overlay AUTH kick empty avgv=966 — keep waiting
MATCH_FAIL
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["auth_fail"])
        self.assertTrue(ev["auth_kick"])
        self.assertFalse(sc["match"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "NO MATCH")
        self.assertNotEqual(h, "WAIT")
        self.assertIn("press and lift", p.lower())
        self.assertNotIn("one second", p.lower())

    def test_auth_kick_no_itype_is_auth_fail(self) -> None:
        text = HEALTH + "\noverlay AUTH kick no itype 0x2\n"
        ev = scan_text(text)
        self.assertTrue(ev["auth_fail"])
        h, _ = hero_plain(ev, score(ev), phase="match")
        self.assertEqual(h, "NO MATCH")

    def test_build_status_reads_match_ui(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            session = Path(td)
            (session / "enroll-dmesg.txt").write_text(
                HEALTH + "\n[  30.0] overlay rem=0\n[  30.1] frame raw[0]: avgv = 260\n"
            )
            (session / "match-ui.txt").write_text("MATCH_FAIL\n")
            st = build_status(session, phase="ready")
            self.assertTrue(st["events"]["auth_fail"])
            self.assertEqual(st["hero"], "NO MATCH")

    def test_leftover_match_fail_this_run_hit_is_match(self) -> None:
        text = HEALTH + """
[  30.0] overlay rem=0
[  30.1] overlay AUTH kick — wait lift then PRESS
MATCH_FAIL
[  40.0] overlay AUTH kick — wait lift then PRESS
[  40.1] overlay AUTH HIT err=0x0 sample=0
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["auth_er0"])
        self.assertFalse(ev["auth_fail"])
        self.assertTrue(sc["match"])
        h, _ = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "MATCH")
        win = window_last_auth_kick(text)
        self.assertIn("AUTH HIT err=0x0", win)
        self.assertNotIn("MATCH_FAIL", win)

    def test_build_status_leftover_match_fail_then_hit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            session = Path(td)
            (session / "enroll-dmesg.txt").write_text(
                HEALTH + "\n[  30.0] overlay rem=0\n[  30.1] frame raw[0]: avgv = 260\n"
            )
            (session / "match-ui.txt").write_text("MATCH_FAIL\n")
            (session / "match-dmesg.txt").write_text(
                "[  40.0] overlay AUTH kick — wait lift then PRESS\n"
                "[  40.1] overlay AUTH HIT err=0x0 sample=0\n"
            )
            st = build_status(session, phase="ready")
            self.assertTrue(st["score"]["match"])
            self.assertFalse(st["events"]["auth_fail"])
            self.assertEqual(st["hero"], "MATCH")

    def test_leftover_no_match_does_not_leave_wait_before_phase_match(self) -> None:
        # Same-second leftover MATCH_FAIL / hero NO MATCH at Match tap.
        leftover = HEALTH + "\n[  30.0] overlay rem=0\nMATCH_FAIL\n"
        ev = scan_text(leftover)
        sc = score(ev)
        h, _ = hero_plain(ev, sc, phase="ready")
        self.assertTrue(ev["auth_fail"])
        self.assertEqual(h, "NO MATCH")
        self.assertFalse(
            match_should_leave_wait(
                phase="ready",
                saw_match_phase=False,
                match_t=100.0,
                now=100.2,
                score_match=bool(sc.get("match")),
            )
        )
        self.assertEqual(match_hero_while_matching(h), "WAIT")
        self.assertEqual(match_hero_while_matching("NO MATCH"), "WAIT")
        self.assertEqual(match_hero_while_matching("SAVED"), "WAIT")

    def test_leave_wait_after_this_run_match_then_ready(self) -> None:
        self.assertFalse(
            match_should_leave_wait(
                phase="ready",
                saw_match_phase=True,
                match_t=100.0,
                now=100.5,
                score_match=False,
                auth_fail=True,
            )
        )
        self.assertTrue(
            match_should_leave_wait(
                phase="ready",
                saw_match_phase=False,
                match_t=100.0,
                now=102.0,
                score_match=False,
                auth_fail=True,
            )
        )

    def test_leave_wait_on_hit_or_cap(self) -> None:
        self.assertTrue(
            match_should_leave_wait(
                phase="match",
                saw_match_phase=True,
                match_t=100.0,
                now=101.0,
                score_match=True,
            )
        )
        self.assertTrue(
            match_should_leave_wait(
                phase="match",
                saw_match_phase=False,
                match_t=100.0,
                now=121.0,
                score_match=False,
            )
        )

    def test_lab_uses_shipped_match_leave(self) -> None:
        lab = (Path(__file__).resolve().parents[1] / "app" / "fp5-fp-lab.py").read_text()
        self.assertIn("match_should_leave_wait(", lab)
        self.assertIn("match_hero_while_matching(", lab)

    def test_enrolling_ellipsis_is_not_ta_rem(self) -> None:
        ev = scan_text(HEALTH + "\nnew fid (= 1)\nframe raw[0]: avgv = 300\nenrolling...\nrem = 19\n")
        self.assertFalse(ev["rem_seen"])
        self.assertFalse(ev["ta_enrolling"])
        self.assertFalse(score(ev)["template"])

    def test_enrolled_fid_is_ta_rem(self) -> None:
        ev = scan_text(HEALTH + "\nnew fid (= 1)\nframe raw[0]: avgv = 300\nenrolled fid 1 rem = 19\n")
        self.assertTrue(ev["ta_enrolling"])
        self.assertTrue(score(ev)["template"])

    def test_rem_zero_is_last_overlay_rem_not_rem_seen(self) -> None:
        leftover = HEALTH + """
[  20.1] AUTH er=0 rem_seen=0
[  20.2] enroll sample loop done rem_seen=0 ta_enrolling=0
"""
        ev = scan_text(leftover)
        self.assertIsNone(ev["overlay_rem"])
        self.assertFalse(ev["rem_zero"])
        saved = HEALTH + """
[  30.3] overlay rem=0 fid=0x1 sample=19
"""
        ev0 = scan_text(saved)
        self.assertEqual(ev0["overlay_rem"], 0)
        self.assertTrue(ev0["rem_zero"])

    def test_overlay_rem_9_empty_last_frame_is_not_restart_19(self) -> None:
        text = HEALTH + """
[  30.0] overlay rem=9 fid=0x5639c78a sample=11
[  30.1] overlay PRESS OK rem=9 sample=11
[  30.2] *** PRESS then LIFT finger sample 12/25 (HOLD on power-button sensor) ***
[  30.3] frame raw[0]: avgv = 863, minv = 745, maxv = 917.
[  30.4] enroll sample loop done rem_seen=0 ta_enrolling=0
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertEqual(ev["overlay_rem"], 9)
        self.assertFalse(ev["rem_zero"])
        self.assertTrue(ev["empty_sensor"])
        self.assertEqual(ev["frame_avgv"], 863)
        self.assertFalse(sc["template"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "NO FINGER")
        self.assertIn("9 remaining", p)
        self.assertIn("close", p.lower())
        self.assertNotIn("starts at 19", p)
        self.assertNotIn("Register", p)

    def test_extra_unloaded_enrolled_text_empty_is_not_template(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 42)
[  30.1] frame raw[0]: avgv = 863, minv = 745, maxv = 917.
[  30.2] enrolled fid 42 rem = 19
[  30.3] rem_seen=1
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["ta_enrolling"])
        self.assertTrue(ev["empty_sensor"])
        self.assertNotEqual(ev["overlay_rem"], 0)
        self.assertFalse(ev["rem_zero"])
        self.assertFalse(sc["template"])
        h, _ = hero_plain(ev, sc, phase="ready")
        self.assertNotEqual(h, "SAVED")

    def test_bare_quality_pass_is_finger_seen(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 9)
[  30.1] frame raw[0]: avgv = 197, minv = 0, maxv = 513.
[  30.2] ION HIT dev-REPORT-ev5 ret=0 req0=0x80001017
[  30.3] quality = 85, area = 99, cond = 88, contrast = 8
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertFalse(ev["empty_sensor"])
        self.assertTrue(ev["qual_pass"])
        self.assertEqual(ev["frame_quality"], 85)
        self.assertFalse(sc["template"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "SENSOR READY")
        self.assertNotEqual(h, "FINGER SEEN")
        self.assertTrue(sc["tz_up"])
        self.assertIn("no reboot", p.lower())
        self.assertNotIn("Linux reboot", p)

    def test_last_quality_line_wins(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 9)
[  30.1] quality(= 5),threshold = 40, error, ret = -3
[  30.2] frame raw[0]: avgv = 73
[  30.3] ION HIT dev-REPORT-ev5 ret=0 req0=0x80001017
[  30.4] quality = 72 (thr = 40), area = 99 (thr = 70)
"""
        ev = scan_text(text)
        self.assertFalse(ev["qual_err"])
        self.assertTrue(ev["qual_pass"])
        self.assertEqual(ev["frame_quality"], 72)

    def test_window_keeps_fid_before_enroll_start_line(self) -> None:
        text = """qsee_fp: init
[  12.1] START focal32 ret=0 APP_START success id=0x4
[  30.0] new fid (= 42) generated.
[  30.1] qsee_fp: ENROLL start er=0 err=0x0 (expect one new fid)
[  30.2] frame raw[0]: avgv = 864, minv = 745, maxv = 917.
"""
        w = window_this_run(text)
        ev = scan_text(w)
        self.assertEqual(ev["new_fid"], "42")
        self.assertTrue(ev["empty_sensor"])

    def test_window_drops_leftover_fid_and_quality(self) -> None:
        leftover = HEALTH + """
[  30.0] new fid (= 9)
[  30.1] quality(= 5),threshold = 40, error, ret = -3
[  30.2] enrolled fid 9 rem = 19
[  30.3] qsee_fp: init
[  40.0] START focal32 ret=0 APP_START success id=0x4
[  40.1] new fid (= 42)
[  40.2] frame raw[0]: avgv = 864, minv = 745, maxv = 917.
"""
        w = window_this_run(leftover)
        ev = scan_text(w)
        self.assertFalse(ev["qual_err"])
        self.assertFalse(ev["ta_enrolling"])
        self.assertEqual(ev["new_fid"], "42")
        self.assertTrue(ev["empty_sensor"])
        self.assertFalse(score(ev)["template"])

    def test_window_drops_leftover_press_before_ktime(self) -> None:
        leftover = """[  100.0] overlay REAL DOWN — skip factory enroll-flag itype=0x2
[  100.1] PRESS then LIFT finger sample 0/1
[  100.2] frame raw[0]: avgv = 200, minv = 0, maxv = 400.
[  100.3] quality = 90, area = 99, cond = 88, contrast = 8
[  700.0] qsee_fp: init
[  700.1] START focal32 ret=0 APP_START success id=0x4
[  700.2] HEALTH final er=0
"""
        w = window_this_run(leftover, kmin=700)
        ev = scan_text(w)
        self.assertFalse(ev["real_down"])
        self.assertFalse(ev["press_banner"])
        self.assertEqual(ev["frame_raw"], 0)
        self.assertFalse(ev["qual_pass"])
        h, _ = hero_plain(ev, score(ev), phase="enroll")
        self.assertEqual(h, "WAIT")
        self.assertNotEqual(h, "FINGER SEEN")
        self.assertNotEqual(h, "HOLD FINGER")
        dropped = drop_old_ktime(leftover, 700)
        self.assertNotIn("REAL DOWN", dropped)
        self.assertIn("HEALTH final er=0", dropped)

    def test_quality_reject_is_not_template(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 9)
[  30.1] frame raw[0]: avgv = 330, minv = 80, maxv = 800.
[  30.2] enrolling...
[  30.3] quality(= 5),threshold = 40, error, ret = -3
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["qual_err"])
        self.assertFalse(sc["template"])

    def test_frame_raw_without_enrolling_is_not_template(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 9)
[  30.1] frame raw[0]: avgv = 330, minv = 80, maxv = 800.
[  30.2] enroll sample loop done rem_seen=1
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["rem_seen"])
        self.assertFalse(ev["ta_enrolling"])
        self.assertFalse(sc["template"])

    def test_three_frame_inte_without_enrolled_fid_is_not_template(self) -> None:
        text = HEALTH + """
[  30.0] frame raw[0]: avgv = 335, minv = 120, maxv = 848.
[  30.0] frame raw[1]: avgv = 330, minv = 114, maxv = 842.
[  30.0] frame raw[2]: avgv = 325, minv = 111, maxv = 837.
[  30.0] current inte times: 128
[  30.1] got finger touched event.
[  30.2] enrolling...
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertGreaterEqual(ev["frame_raw"], 1)
        self.assertFalse(ev["ta_enrolling"])
        self.assertFalse(sc["template"])

    def test_heroes(self) -> None:
        h, p = hero_plain(scan_text(FID_ONLY), score(scan_text(FID_ONLY)))
        self.assertEqual(h, "NO FRAME")
        self.assertTrue(score(scan_text(FID_ONLY))["tz_up"])
        self.assertIn("no reboot", p.lower())
        self.assertNotIn("will not save", p)
        h, _ = hero_plain(scan_text(TEMPLATE), score(scan_text(TEMPLATE)))
        self.assertEqual(h, "SAVED")
        h, _ = hero_plain(scan_text(MATCH), score(scan_text(MATCH)))
        self.assertEqual(h, "MATCH")
        # Empty fid is minted at ENROLL start — UI must stay on WAIT/HOLD.
        h, p = hero_plain(scan_text(FID_ONLY), score(scan_text(FID_ONLY)), phase="enroll")
        self.assertIn(h, ("WAIT", "PRESS", "HOLD FINGER"))
        self.assertNotEqual(h, "NO IMAGE")

    def test_partial_before_press_is_failed_not_no_image(self) -> None:
        text = HEALTH + "\nFP_PREPARE_OK\nENROLL start killed TA\nENROLL_PARTIAL pix=0 rem=0\n"
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["enroll_partial"])
        self.assertFalse(ev["press_banner"])
        self.assertFalse(ev["real_down"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "REBOOT")
        self.assertIn("before the flash", p)
        self.assertNotEqual(h, "NO IMAGE")

    def test_busy_after_health_is_not_hold_power(self) -> None:
        text = HEALTH + "\nFP_PREPARE_OK\nqsee_fingerpr busy\n"
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["wedge"])
        self.assertTrue(ev["prepare_ok"])
        h, _ = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "SENSOR READY")
        self.assertNotEqual(h, "HOLD POWER")

    def test_enroll_wedge_is_hold_power_even_with_leftover_health(self) -> None:
        text = HEALTH + "\nFP_PREPARE_OK\nENROLL_FAIL WEDGED_REBOOT (qsee_fingerpr already loaded — do not rmmod)\n"
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(ev["wedge"])
        self.assertTrue(ev["enroll_fail"])
        self.assertTrue(sc["tz_up"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "REBOOT")
        self.assertNotEqual(h, "HOLD POWER")
        self.assertIn("Linux reboot", p)
        self.assertIn("frozen", p)

    def test_frame_without_rem_is_finger_seen_not_no_image(self) -> None:
        text = HEALTH + """
[  30.0] overlay REAL DOWN — skip factory enroll-flag itype=0x2
[  30.1] PRESS then LIFT finger sample 0/8
[  30.2] frame raw[0]: avgv = 295, minv = 64, maxv = 507.
[  30.3] new fid (= 1580892630)
[  30.4] overlay REPORT ev=5 killed TA — stop samples
ENROLL_PARTIAL new fid (= 1580892630) avgv=295 rem=0
"""
        ev = scan_text(text)
        sc = score(ev)
        self.assertTrue(sc["fid_only"])
        self.assertGreaterEqual(ev["frame_raw"], 1)
        self.assertFalse(ev["empty_sensor"])
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "SENSOR READY")
        self.assertNotEqual(h, "FINGER SEEN")
        self.assertNotEqual(h, "NO IMAGE")
        self.assertNotEqual(h, "HOLD POWER")
        self.assertTrue(sc["tz_up"])
        self.assertIn("did not store", p)
        self.assertIn("no reboot", p.lower())
        self.assertNotIn("Linux reboot", p)

    def test_finger_seen_tz_up_is_register_again_not_reboot_mode(self) -> None:
        text = HEALTH + """
[  30.0] overlay REAL DOWN — skip factory enroll-flag itype=0x2
[  30.1] PRESS then LIFT finger sample 0/1
[  30.2] frame raw[0]: avgv = 244, minv = 16, maxv = 521.
[  30.3] ION HIT dev-REPORT-ev5 ret=0 req0=0x80001017
[  30.4] quality = 85 (thr = 40), area = 99 (thr = 70)
[  30.5] new fid (= 2145270680)
ENROLL_PARTIAL new fid (= 2145270680) avgv=244 rem=0
"""
        ev = scan_text(text)
        sc = score(ev)
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "SENSOR READY")
        self.assertNotEqual(h, "FINGER SEEN")
        self.assertTrue(sc["tz_up"])
        self.assertTrue(sc["fid_only"])
        self.assertTrue(ev["enroll_partial"])
        self.assertEqual(next_enroll_mode("enroll", "ready", h, sc, ev), "ready")
        self.assertNotEqual(next_enroll_mode("enroll", "ready", h, sc, ev), "need_reboot")

    def test_wedge_and_dead_ta_still_need_reboot_mode(self) -> None:
        wedge = scan_text(
            HEALTH
            + "\nENROLL_FAIL WEDGED_REBOOT (qsee_fingerpr already loaded — do not rmmod)\n"
        )
        wsc = score(wedge)
        self.assertEqual(
            next_enroll_mode("enroll", "ready", "REBOOT", wsc, wedge),
            "need_reboot",
        )
        dead = scan_text("[  12.1] START focal32 ret=0\nHEALTH final er=-22\nENROLL_FAIL\n")
        dsc = score(dead)
        h, _ = hero_plain(dead, dsc, phase="ready")
        self.assertEqual(h, "REBOOT")
        self.assertEqual(next_enroll_mode("enroll", "ready", h, dsc, dead), "need_reboot")

    def test_fid_only_tz_up_no_frame_is_register_again(self) -> None:
        text = HEALTH + """
[  30.0] new fid (= 2064608140)
ENROLL_PARTIAL new fid (= 2064608140) avgv=none rem=0
"""
        ev = scan_text(text)
        sc = score(ev)
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "NO FRAME")
        self.assertTrue(sc["tz_up"])
        self.assertTrue(sc["fid_only"])
        self.assertEqual(ev["frame_raw"], 0)
        self.assertIn("no reboot", p.lower())
        self.assertEqual(next_enroll_mode("enroll", "ready", h, sc, ev), "ready")
        self.assertNotEqual(next_enroll_mode("enroll", "ready", h, sc, ev), "need_reboot")

    def test_press_timeout_tz_up_is_no_frame_not_hold_or_reboot(self) -> None:
        # m4hm: PRESS banner + 8s timeout, no REAL DOWN, HEALTH 0. GTK stayed
        # HOLD FINGER then jumped to REBOOT. Living TA is Register-again.
        text = HEALTH + """
[  30.0] PRESS then LIFT finger sample 0/1 (HOLD on power-button sensor)
[  30.1] HOLD FINGER NOW — 1 samples
[  30.2] sample 0 no IRQ/DOWN — skip CAP (finger-only)
ENROLL_PARTIAL avgv=none rem=0 no_real_down=0
"""
        ev = scan_text(text)
        sc = score(ev)
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertEqual(h, "NO FRAME")
        self.assertNotEqual(h, "HOLD FINGER")
        self.assertNotEqual(h, "REBOOT")
        self.assertTrue(sc["tz_up"])
        self.assertTrue(ev["press_banner"])
        self.assertTrue(sc["press_ok"])
        self.assertFalse(ev["real_down"])
        self.assertTrue(ev["enroll_partial"])
        self.assertIn("no reboot", p.lower())
        self.assertEqual(next_enroll_mode("enroll", "ready", h, sc, ev), "ready")
        self.assertEqual(
            next_enroll_mode("enroll", "ready", "REBOOT", sc, ev),
            "ready",
        )
        self.assertEqual(
            next_enroll_mode("enroll", "enroll", "HOLD FINGER", sc, ev),
            "enroll",
        )

    def test_enroll_dmesg_not_dropped_by_uptime_skew(self) -> None:
        # m4h6: /proc/uptime 24498 vs dmesg 24400 dropped this-run frame → NO IMAGE.
        text = """[24400.0] qsee_fp: init
[24402.4] HEALTH final er=0 (TA alive if 0)
[24402.8] new fid (= 1872075220) generated.
[24406.2] overlay REAL DOWN — skip factory enroll-flag itype=0x2
[24406.3] frame raw[0]: avgv = 216, minv = 0, maxv = 535.
[24406.3] quality = 87 (thr = 40), area = 99 (thr = 70)
[24406.4] ION HIT dev-REPORT-ev5 ret=0 req0=0x80001017
"""
        with tempfile.TemporaryDirectory() as raw:
            d = Path(raw)
            (d / "enroll-dmesg.txt").write_text(text)
            (d / "ktime-start.txt").write_text("24498\n")
            (d / "ktime-enroll.txt").write_text("24501\n")
            (d / "enroll-run.txt").write_text(
                "ENROLL_PARTIAL new fid (= 1872075220) avgv=216 rem=0\n"
            )
            payload = build_status(d, phase="ready")
        self.assertGreater(payload["events"]["frame_raw"], 0)
        self.assertTrue(payload["events"]["qual_pass"])
        self.assertTrue(payload["score"]["tz_up"])
        self.assertEqual(payload["hero"], "SENSOR READY")
        self.assertNotEqual(payload["hero"], "NO IMAGE")
        self.assertIn("no reboot", payload["plain"].lower())

    def test_fid_only_dead_ta_is_no_image(self) -> None:
        text = "new fid (= 7)\n"
        ev = scan_text(text)
        sc = score(ev)
        h, p = hero_plain(ev, sc, phase="ready")
        self.assertFalse(sc["tz_up"])
        self.assertEqual(h, "NO IMAGE")
        self.assertIn("will not save", p)

    def test_hold_finger_now_during_enroll_is_still_wait(self) -> None:
        # HOLD FINGER NOW is printed before FDT is armed. Flash/hold is PRESS then LIFT.
        text = HEALTH + "\nqsee_fp: *** HOLD FINGER NOW — 1 samples, up to ~8s waits + CAP (press/lift each) force_cap=0 ***\n"
        ev = scan_text(text)
        self.assertTrue(ev["hold_now"])
        self.assertFalse(ev["press_banner"])
        h, p = hero_plain(ev, score(ev), phase="enroll")
        self.assertEqual(h, "WAIT")
        self.assertIn("Hands off", p)
        self.assertNotEqual(h, "HOLD FINGER")

    def test_press_then_lift_during_enroll_is_hold_finger(self) -> None:
        text = HEALTH + "\nqsee_fp: *** PRESS then LIFT finger sample 0/8 (HOLD on power-button sensor) ***\n"
        ev = scan_text(text)
        self.assertTrue(ev["press_banner"])
        h, p = hero_plain(ev, score(ev), phase="enroll")
        self.assertEqual(h, "HOLD FINGER")
        self.assertIn("Flash = ready", p)
        self.assertIn("own speed", p)
        self.assertNotIn("one second", p)


if __name__ == "__main__":
    unittest.main()
