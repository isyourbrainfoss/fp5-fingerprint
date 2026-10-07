#!/usr/bin/env python3
"""On lock: notify and AUTH-kick. On overlay AUTH HIT: loginctl unlock-session.

Phosh 0.56 only starts PAM after PIN submit, so finger-only unlock is this
watcher. PIN still works. Never unlocks on AUTH FAIL.
"""
from __future__ import annotations

import os
import subprocess
import time

KICK = "/sys/module/qsee_fingerpr/parameters/auth_kick"
SESSION = "/usr/local/bin/fp5-fp-session"
MARK = "/var/lib/fp5-fp/enrolled"


def _session_id() -> str:
    sid = os.environ.get("XDG_SESSION_ID", "")
    if sid:
        return sid
    try:
        out = subprocess.check_output(
            ["loginctl", "list-sessions", "--no-legend"], text=True, timeout=5
        )
        user = os.environ.get("USER", "user")
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == user:
                return parts[0]
    except Exception:
        pass
    return ""


def _locked(sid: str) -> bool:
    if not sid:
        return False
    try:
        out = subprocess.check_output(
            ["loginctl", "show-session", sid, "-p", "LockedHint"],
            text=True,
            timeout=5,
        )
        return "yes" in out.lower()
    except Exception:
        return False


def _auth_hit() -> bool:
    try:
        dmesg = subprocess.check_output(
            ["dmesg"], text=True, timeout=5, stderr=subprocess.DEVNULL
        )
    except Exception:
        return False
    chunk = dmesg.split("overlay AUTH")[-1] if "overlay AUTH" in dmesg else ""
    return "HIT err=0x0" in chunk and "FAIL" not in chunk.split("HIT")[0]


def _notify(msg: str) -> None:
    try:
        subprocess.run(
            ["notify-send", "-t", "8000", "Fingerprint", msg],
            check=False,
            timeout=3,
        )
    except Exception:
        pass


def _kick() -> None:
    if os.path.isfile(KICK) and os.access(KICK, os.W_OK):
        try:
            open(KICK, "w").write("1\n")
            return
        except OSError:
            pass
    subprocess.run([SESSION, "match"], timeout=60, check=False)


def main() -> None:
    if not os.path.isfile(MARK):
        time.sleep(30)
    sid = _session_id()
    was = False
    while True:
        if not sid:
            sid = _session_id()
        locked = _locked(sid)
        if locked and not was and os.path.isfile(MARK):
            _notify("Touch the power-button sensor to unlock. PIN still works.")
            _kick()
            for _ in range(50):
                time.sleep(0.4)
                if _auth_hit():
                    subprocess.run(
                        ["loginctl", "unlock-session", sid or ""],
                        check=False,
                    )
                    break
                if not _locked(sid):
                    break
        was = locked
        time.sleep(1.2)


if __name__ == "__main__":
    main()
