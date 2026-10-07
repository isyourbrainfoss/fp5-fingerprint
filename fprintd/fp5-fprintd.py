#!/usr/bin/env python3
"""fprintd-compatible system bus for Fairphone 5 QSEE extra.

Owns net.reactivated.Fprint so GNOME/Phosh Settings → Users can list,
enroll, and delete a print. Verify is the same AUTH path as the lab app.
Does not claim Phosh unlock by itself (see fp5-lock-watcher).
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time

from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

DBusGMainLoop(set_as_default=True)

import dbus
import dbus.service

BUS_NAME = "net.reactivated.Fprint"
MGR_PATH = "/net/reactivated/Fprint/Manager"
DEV_PATH = "/net/reactivated/Fprint/Device/0"
IFACE_MGR = "net.reactivated.Fprint.Manager"
IFACE_DEV = "net.reactivated.Fprint.Device"
SESSION = "/usr/local/bin/fp5-fp-session"
STATE = "/var/lib/fp5-fp/enrolled.json"
MARK = "/var/lib/fp5-fp/enrolled"
FINGERS = (
    "left-thumb",
    "left-index-finger",
    "left-middle-finger",
    "left-ring-finger",
    "left-little-finger",
    "right-thumb",
    "right-index-finger",
    "right-middle-finger",
    "right-ring-finger",
    "right-little-finger",
)


def _load_state() -> dict:
    if os.path.isfile(STATE):
        try:
            return json.load(open(STATE))
        except Exception:
            pass
    if os.path.isfile(MARK):
        return {"user": ["right-index-finger"]}
    return {"user": []}


def _save_state(st: dict) -> None:
    os.makedirs("/var/lib/fp5-fp", exist_ok=True)
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(st, f)
    os.replace(tmp, STATE)
    if st.get("user"):
        open(MARK, "a").close()
    else:
        try:
            os.remove(MARK)
        except OSError:
            pass


def _dmesg_tail(n: int = 80) -> str:
    try:
        return subprocess.check_output(
            ["dmesg"], text=True, timeout=3, stderr=subprocess.DEVNULL
        )[-12000:]
    except Exception:
        return ""


class Device(dbus.service.Object):
    def __init__(self, bus: dbus.SystemBus) -> None:
        super().__init__(bus, DEV_PATH)
        self._claimed = ""
        self._busy = False
        self._stop = threading.Event()

    def _err(self, name: str, msg: str) -> None:
        raise dbus.exceptions.DBusException(name, msg)

    @dbus.service.method(IFACE_DEV, in_signature="s", out_signature="as")
    def ListEnrolledFingers(self, username: str) -> list[str]:
        fingers = _load_state().get("user") or []
        if not fingers:
            self._err(
                "net.reactivated.Fprint.Error.NoEnrolledPrints",
                "No enrolled fingerprints",
            )
        return fingers

    @dbus.service.method(IFACE_DEV, in_signature="s", out_signature="")
    def DeleteEnrolledFingers(self, username: str) -> None:
        _save_state({"user": []})

    @dbus.service.method(IFACE_DEV, in_signature="", out_signature="")
    def DeleteEnrolledFingers2(self) -> None:
        _save_state({"user": []})

    @dbus.service.method(IFACE_DEV, in_signature="s", out_signature="")
    def DeleteEnrolledFinger(self, finger_name: str) -> None:
        if finger_name not in FINGERS:
            self._err(
                "net.reactivated.Fprint.Error.InvalidFingername", finger_name
            )
        st = _load_state()
        st["user"] = [f for f in st.get("user", []) if f != finger_name]
        _save_state(st)

    @dbus.service.method(IFACE_DEV, in_signature="s", out_signature="")
    def Claim(self, username: str) -> None:
        if self._claimed:
            self._err(
                "net.reactivated.Fprint.Error.AlreadyInUse", "already claimed"
            )
        self._claimed = username or "user"
        self.PropertiesChanged(
            IFACE_DEV, {"num-enroll-stages": dbus.Int32(20)}, []
        )

    @dbus.service.method(IFACE_DEV, in_signature="", out_signature="")
    def Release(self) -> None:
        self._claimed = ""
        self._stop.set()
        self._busy = False

    @dbus.service.method(IFACE_DEV, in_signature="s", out_signature="")
    def EnrollStart(self, finger_name: str) -> None:
        if not self._claimed:
            self._err("net.reactivated.Fprint.Error.ClaimDevice", "not claimed")
        if finger_name not in FINGERS:
            self._err(
                "net.reactivated.Fprint.Error.InvalidFingername", finger_name
            )
        if self._busy:
            self._err("net.reactivated.Fprint.Error.AlreadyInUse", "busy")
        self._busy = True
        self._stop.clear()

        def run() -> None:
            seen = 0
            try:
                proc = subprocess.Popen(
                    [SESSION, "enroll"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                while proc.poll() is None and not self._stop.is_set():
                    tail = _dmesg_tail()
                    oks = tail.count("overlay PRESS OK rem=")
                    if oks > seen:
                        for _ in range(oks - seen):
                            GLib.idle_add(
                                self.EnrollStatus, "enroll-stage-passed", False
                            )
                        seen = oks
                    if "overlay PRESS EMPTY" in tail[-400:]:
                        GLib.idle_add(
                            self.EnrollStatus, "enroll-retry-scan", False
                        )
                    if "overlay PRESS OK rem=0" in tail or "overlay rem=0 template" in tail:
                        break
                    time.sleep(0.4)
                proc.wait(timeout=5)
                ok = os.path.isfile(MARK) or "overlay PRESS OK rem=0" in _dmesg_tail()
                if ok:
                    st = _load_state()
                    if finger_name not in st.get("user", []):
                        st.setdefault("user", []).append(finger_name)
                    _save_state(st)
                    GLib.idle_add(self.EnrollStatus, "enroll-completed", True)
                else:
                    GLib.idle_add(self.EnrollStatus, "enroll-failed", True)
            except Exception:
                GLib.idle_add(self.EnrollStatus, "enroll-unknown-error", True)
            self._busy = False

        threading.Thread(target=run, daemon=True).start()

    @dbus.service.method(IFACE_DEV, in_signature="", out_signature="")
    def EnrollStop(self) -> None:
        self._stop.set()
        subprocess.run([SESSION, "cleanup"], check=False, timeout=30)

    @dbus.service.method(IFACE_DEV, in_signature="s", out_signature="")
    def VerifyStart(self, finger_name: str) -> None:
        if not self._claimed:
            self._err("net.reactivated.Fprint.Error.ClaimDevice", "not claimed")
        if not (_load_state().get("user") or os.path.isfile(MARK)):
            self._err(
                "net.reactivated.Fprint.Error.NoEnrolledPrints", "none enrolled"
            )
        if self._busy:
            self._err("net.reactivated.Fprint.Error.AlreadyInUse", "busy")
        self._busy = True
        self._stop.clear()
        chosen = finger_name if finger_name != "any" else "right-index-finger"
        GLib.idle_add(self.VerifyFingerSelected, chosen)

        def run() -> None:
            try:
                subprocess.run(
                    [SESSION, "match"],
                    check=False,
                    timeout=60,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                tail = _dmesg_tail()
                last = tail.split("overlay AUTH")[-1] if "overlay AUTH" in tail else ""
                ok = "HIT err=0x0" in last and "FAIL" not in last
                GLib.idle_add(
                    self.VerifyStatus,
                    "verify-match" if ok else "verify-no-match",
                    True,
                )
            except Exception:
                GLib.idle_add(self.VerifyStatus, "verify-unknown-error", True)
            self._busy = False

        threading.Thread(target=run, daemon=True).start()

    @dbus.service.method(IFACE_DEV, in_signature="", out_signature="")
    def VerifyStop(self) -> None:
        self._stop.set()
        self._busy = False

    @dbus.service.signal(IFACE_DEV, signature="sb")
    def EnrollStatus(self, result: str, done: bool) -> None:
        pass

    @dbus.service.signal(IFACE_DEV, signature="sb")
    def VerifyStatus(self, result: str, done: bool) -> None:
        pass

    @dbus.service.signal(IFACE_DEV, signature="s")
    def VerifyFingerSelected(self, finger_name: str) -> None:
        pass

    @dbus.service.signal(dbus.PROPERTIES_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, interface: str, changed: dict, invalidated: list) -> None:
        pass

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface: str, prop: str) -> object:
        return self.GetAll(interface).get(prop, "")

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface: str) -> dict:
        claimed = bool(self._claimed)
        return {
            "name": dbus.String("Fairphone 5 power-button (FT9362)"),
            "num-enroll-stages": dbus.Int32(20 if claimed else -1),
            "scan-type": dbus.String("press"),
            "finger-present": dbus.Boolean(False),
            "finger-needed": dbus.Boolean(True),
        }


class Manager(dbus.service.Object):
    def __init__(self, bus: dbus.SystemBus) -> None:
        super().__init__(bus, MGR_PATH)
        self._dev = Device(bus)

    @dbus.service.method(IFACE_MGR, in_signature="", out_signature="ao")
    def GetDevices(self) -> list:
        return [dbus.ObjectPath(DEV_PATH)]

    @dbus.service.method(IFACE_MGR, in_signature="", out_signature="o")
    def GetDefaultDevice(self) -> dbus.ObjectPath:
        return dbus.ObjectPath(DEV_PATH)


def main() -> int:
    bus = dbus.SystemBus()
    name = dbus.service.BusName(
        BUS_NAME,
        bus,
        allow_replacement=True,
        replace_existing=True,
        do_not_queue=True,
    )
    mgr = Manager(bus)
    print("fp5-fprintd unique", bus.get_unique_name(), "well-known", name.get_name(), file=sys.stderr)
    GLib.MainLoop().run()
    return 0
    # keep mgr/name referenced
    _ = mgr


if __name__ == "__main__":
    raise SystemExit(main())
