"""fp5-fprintd D-Bus surface (static)."""
from __future__ import annotations

import ast
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "fprintd" / "fp5-fprintd.py"
WATCH = Path(__file__).resolve().parents[1] / "fprintd" / "fp5-lock-watcher.py"
UNIT = Path(__file__).resolve().parents[1] / "fprintd" / "fp5-fprintd.service"
PAM = Path(__file__).resolve().parents[1] / "pam" / "phosh"


class FprintdGlue(unittest.TestCase):
    def test_parses(self) -> None:
        ast.parse(SRC.read_text())
        ast.parse(WATCH.read_text())

    def test_dbus_api(self) -> None:
        text = SRC.read_text()
        self.assertIn("net.reactivated.Fprint", text)
        self.assertIn("GetDevices", text)
        self.assertIn("EnrollStart", text)
        self.assertIn("VerifyStart", text)
        self.assertIn("ListEnrolledFingers", text)
        self.assertIn("enroll-stage-passed", text)
        self.assertIn("verify-match", text)
        self.assertIn("HIT err=0x0", text)

    def test_unit_and_pam(self) -> None:
        self.assertIn("ExecStart=/usr/local/bin/fp5-fprintd", UNIT.read_text())
        self.assertIn("pam_fprintd.so", PAM.read_text())
        self.assertIn("base-auth", PAM.read_text())
        self.assertIn("loginctl", WATCH.read_text())
        self.assertIn("HIT err=0x0", WATCH.read_text())
