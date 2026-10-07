#!/usr/bin/env python3
"""FP5 fingerprint lab — hold the power-button sensor when the screen flashes.

Does not unlock Phosh until Match AUTH HIT. Close keeps extra if a finger is saved.
"""

from __future__ import annotations

import atexit
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

for p in (
    os.path.dirname(os.path.abspath(__file__)),
    "/usr/local/lib/fp5-fp",
    "/usr/local/bin",
):
    if p not in sys.path:
        sys.path.insert(0, p)

from fp5_fp_status import (  # noqa: E402
    match_hero_while_matching,
    match_should_leave_wait,
    next_enroll_mode,
    read_status,
)

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk, Gdk, Pango  # noqa: E402

APP_ID = "org.fp5.fp"
APP_TITLE = "FP Fingerprint"
ENROLLED_MARK = Path("/var/lib/fp5-fp/enrolled")


def extra_loaded() -> bool:
    return Path("/sys/module/qsee_fingerpr").is_dir()


def live_saved_finger() -> bool:
    """Marker is stale after extra unload; Match only if extra is still in."""
    return ENROLLED_MARK.is_file() and extra_loaded()

CSS = """
window { background-color: #101418; }
.coach-hero {
  font-size: 28px;
  font-weight: 800;
  color: #f5f7fa;
}
.coach-sub { font-size: 15px; font-weight: 600; color: #c5d0dc; }
.coach-meta { font-size: 12px; color: #8a97a6; }
.coach-idle { color: #7dd3fc; }
.coach-tap { color: #4ade80; }
.coach-lift { color: #f472b6; }
.coach-warn { color: #fca5a5; }
.coach-done { color: #a3e635; }
.strip { font-size: 13px; font-weight: 700; color: #e2e8f0; }
.btn-bar { padding: 8px 10px 14px 10px; }
"""


def session_bin() -> str:
    for p in (Path("/usr/local/bin/fp5-fp-session"), Path(__file__).resolve().parent / "fp5-fp-session.sh"):
        if p.is_file():
            return str(p)
    return "fp5-fp-session"


def kill_bin() -> str:
    p = Path("/usr/local/bin/fp5-fp-kill")
    return str(p) if p.is_file() else session_bin()


def _sudo_env() -> dict[str, str]:
    env = os.environ.copy()
    if not env.get("XDG_RUNTIME_DIR") and Path("/run/user/10000").is_dir():
        env["XDG_RUNTIME_DIR"] = "/run/user/10000"
    return env


def sudo_spawn(args: list[str]) -> None:
    cmd = ["sudo", "-n", *args]
    try:
        subprocess.Popen(
            cmd,
            env=_sudo_env(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
    except OSError:
        pass


def live_logdir() -> str:
    try:
        return Path("/run/fp5-fp-session/logdir").read_text().strip()
    except OSError:
        return ""


def _kill_args() -> list[str]:
    kb = kill_bin()
    if kb.endswith("kill"):
        return [kb]
    return [session_bin(), "cleanup"]


def _cleanup_best_effort() -> None:
    sudo_spawn(_kill_args())
    sudo_spawn([session_bin(), "cleanup"])


def _style_for(hero: str) -> str:
    h = hero.upper()
    if h in ("PRESS", "PRESS OK", "SENSOR READY", "HOLD FINGER", "ACCEPTED", "ENROLLING"):
        return "coach-tap"
    if h == "LIFT":
        return "coach-lift"
    if h in ("SAVED", "MATCH", "GOT IT", "DONE"):
        return "coach-done"
    if h in (
        "HOLD POWER",
        "FAILED",
        "NO TZ MODULE",
        "NO IMAGE",
        "NO FRAME",
        "USE PIN",
        "NO FINGER",
        "FINGER SEEN",
        "MISS",
        "TRY AGAIN",
        "NO MATCH",
        "REBOOT",
    ):
        return "coach-warn"
    return "coach-idle"


class FpWindow(Adw.ApplicationWindow):
    def __init__(self, app: Adw.Application) -> None:
        super().__init__(application=app, title=APP_TITLE)
        self.set_default_size(400, 720)
        self._mode = "idle"  # idle|starting|ready|enroll|match|need_reboot
        self._await_start = False
        self._start_deadline = 0.0
        self._ignore_logdir = ""
        self._hero_key: tuple[str, str, str] | None = None
        self._strip_key = ""
        self._match_t = 0.0
        self._match_saw_phase = False
        self._match_fail_at_click = False

        css = Gtk.CssProvider()
        css.load_from_data(CSS.encode())
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(),
            css,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )

        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.set_content(outer)
        header = Adw.HeaderBar()
        header.set_title_widget(Gtk.Label(label=APP_TITLE))
        outer.append(header)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        box.set_margin_top(12)
        box.set_margin_bottom(8)
        box.set_margin_start(14)
        box.set_margin_end(14)
        box.set_vexpand(True)
        outer.append(box)

        self._strip = Gtk.Label(label="L2 · TZ · IRQ")
        self._strip.add_css_class("strip")
        self._strip.set_wrap(True)
        box.append(self._strip)

        self._hero = Gtk.Label(label="POWER BUTTON")
        self._hero.add_css_class("coach-hero")
        self._hero.add_css_class("coach-idle")
        self._hero.set_wrap(True)
        self._hero.set_justify(Gtk.Justification.CENTER)
        self._hero.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        box.append(self._hero)

        self._sub = Gtk.Label(label="Start loads the sensor. Register is a second tap.")
        self._sub.add_css_class("coach-sub")
        self._sub.set_wrap(True)
        self._sub.set_justify(Gtk.Justification.CENTER)
        box.append(self._sub)

        self._plain = Gtk.Label(label="Does not unlock Phosh. Close unloads the driver.")
        self._plain.add_css_class("coach-meta")
        self._plain.set_wrap(True)
        self._plain.set_justify(Gtk.Justification.CENTER)
        self._plain.set_selectable(True)
        box.append(self._plain)

        btn_bar = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        btn_bar.add_css_class("btn-bar")
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        row.set_homogeneous(True)
        self._primary = Gtk.Button(label="Start")
        self._primary.add_css_class("suggested-action")
        self._primary.add_css_class("pill")
        self._primary.connect("clicked", self.on_primary)
        self._abort = Gtk.Button(label="Abort")
        self._abort.add_css_class("destructive-action")
        self._abort.set_sensitive(False)
        self._abort.connect("clicked", self.on_abort)
        row.append(self._primary)
        row.append(self._abort)
        btn_bar.append(row)
        outer.append(btn_bar)

        self.connect("close-request", self.on_close)
        GLib.timeout_add(300, self.tick)
        atexit.register(_cleanup_best_effort)
        signal.signal(signal.SIGTERM, lambda *_a: (_cleanup_best_effort(), sys.exit(0)))
        self._set_hero(
            "POWER BUTTON",
            "Start, then one Register. Hands off until the LED flashes.",
            "coach-idle",
        )

    def _adopt_live_or_ready(self) -> None:
        """Match-only needs extra in memory; the mark file alone is stale."""
        if live_saved_finger():
            self._mode = "enrolled"
            self._primary.set_sensitive(True)
            self._primary.set_label("Match this finger")
            self._abort.set_sensitive(True)
            self._abort.set_label("Register again")
            return
        self._mode = "ready"
        self._abort.set_sensitive(True)
        self._abort.set_label("Abort")
        if extra_loaded():
            self._primary.set_sensitive(False)
            self._primary.set_label("Reboot first")
        else:
            self._primary.set_sensitive(True)
            self._primary.set_label("Register fingerprint")

    def _wedge_ready_copy(self) -> tuple[str, str]:
        return (
            "REBOOT",
            "Extra still loaded. Close and reboot — do not Register. "
            "Hold Power ~30s only if the screen is frozen.",
        )

    def _clear_hero_style(self) -> None:
        for c in ("coach-idle", "coach-tap", "coach-lift", "coach-warn", "coach-done"):
            self._hero.remove_css_class(c)

    def _set_hero(self, hero: str, sub: str, style: str) -> None:
        key = (hero, sub, style)
        if key == self._hero_key:
            return
        self._hero_key = key
        self._clear_hero_style()
        self._hero.add_css_class(style)
        self._hero.set_text(hero)
        self._sub.set_text(sub)

    def on_primary(self, *_a) -> None:
        if self._mode == "idle":
            self._mode = "starting"
            self._await_start = True
            self._ignore_logdir = live_logdir()
            self._start_deadline = time.monotonic() + 20.0
            self._primary.set_sensitive(False)
            self._primary.set_label("Starting…")
            self._abort.set_sensitive(True)
            self._set_hero("STARTING…", "Loading the power-button sensor.", "coach-idle")
            # Paint first; insmod on the GTK thread (via a sync spawn) froze Phosh.
            GLib.timeout_add(150, lambda: (sudo_spawn([session_bin(), "start"]), False)[1])
            return
        if self._mode == "no_image":
            # Empty-fid lock was a dead end: next driver cut could not Register.
            sudo_spawn(_kill_args())
            sudo_spawn([session_bin(), "cleanup"])
            self._mode = "starting"
            self._await_start = True
            self._ignore_logdir = live_logdir()
            self._start_deadline = time.monotonic() + 20.0
            self._primary.set_sensitive(False)
            self._primary.set_label("Starting…")
            self._abort.set_sensitive(True)
            self._abort.set_label("Abort")
            self._set_hero("STARTING…", "Loading the power-button sensor.", "coach-idle")
            GLib.timeout_add(150, lambda: (sudo_spawn([session_bin(), "start"]), False)[1])
            return
        if self._mode == "ready":
            if live_saved_finger():
                self._adopt_live_or_ready()
                self._set_hero(
                    "SAVED",
                    "Match this finger, or tap Register again. Not Phosh unlock.",
                    "coach-done",
                )
                return
            if extra_loaded():
                hero, sub = self._wedge_ready_copy()
                self._primary.set_sensitive(False)
                self._primary.set_label("Reboot first")
                self._set_hero(hero, sub, "coach-warn")
                return
            self._mode = "confirm"
            self._primary.set_label("Yes, register")
            self._abort.set_sensitive(True)
            self._abort.set_label("Not now")
            self._set_hero(
                "REGISTER THIS FINGER?",
                "Hands off until the LED flashes. Do not touch the power-button yet.",
                "coach-warn",
            )
            return
        if self._mode == "confirm":
            self._mode = "enroll"
            self._primary.set_sensitive(False)
            self._primary.set_label("Wait…")
            self._abort.set_sensitive(True)
            self._abort.set_label("Abort")
            self._set_hero(
                "WAIT",
                "Hands off until the LED flashes. Do not touch the power-button yet.",
                "coach-idle",
            )
            sudo_spawn([session_bin(), "enroll"])
            return
        if self._mode in ("enrolled", "matched"):
            if not extra_loaded():
                self._adopt_live_or_ready()
                self._set_hero(
                    "SENSOR READY",
                    "Register again when you want. Hands off until the LED flashes.",
                    "coach-tap",
                )
                return
            self._mode = "match"
            self._match_t = time.monotonic()
            self._match_saw_phase = False
            st0 = read_status() or {}
            self._match_fail_at_click = bool((st0.get("events") or {}).get("auth_fail"))
            self._primary.set_sensitive(False)
            self._primary.set_label("Matching…")
            self._abort.set_sensitive(True)
            self._abort.set_label("Abort")
            self._set_hero("WAIT", "Flash = same finger on the power-button sensor, hold.", "coach-idle")
            sudo_spawn([session_bin(), "match"])
            GLib.timeout_add_seconds(8, self._match_watchdog)
            return

    def _match_watchdog(self) -> bool:
        if self._mode != "match":
            return False
        self._mode = "enrolled"
        self._primary.set_sensitive(True)
        self._primary.set_label("Match again")
        self._abort.set_label("Register again")
        self._set_hero(
            "NO MATCH",
            "No compare that time. Close the app if WAIT sticks. Not Phosh unlock.",
            "coach-warn",
        )
        return False

    def on_abort(self, *_a) -> None:
        if self._mode in ("enrolled", "matched"):
            self._mode = "confirm"
            self._primary.set_sensitive(True)
            self._primary.set_label("Yes, register")
            self._abort.set_sensitive(True)
            self._abort.set_label("Not now")
            self._set_hero(
                "REGISTER THIS FINGER?",
                "Starts a new enroll. The previous print is only kept if the extra is still loaded.",
                "coach-warn",
            )
            return
        if self._mode == "confirm":
            self._adopt_live_or_ready()
            if self._mode == "enrolled":
                self._set_hero(
                    "SAVED",
                    "Match this finger, or tap Register again. Not Phosh unlock.",
                    "coach-done",
                )
            elif extra_loaded():
                hero, sub = self._wedge_ready_copy()
                self._set_hero(hero, sub, "coach-warn")
            else:
                self._set_hero(
                    "SENSOR READY",
                    "Register again when you want. Hands off until the LED flashes.",
                    "coach-tap",
                )
            return
        if self._mode == "need_reboot":
            sudo_spawn(_kill_args())
            sudo_spawn([session_bin(), "cleanup"])
            self._primary.set_sensitive(False)
            self._primary.set_label("Reboot first")
            self._abort.set_sensitive(False)
            return
        sudo_spawn(_kill_args())
        if self._mode in ("enroll", "match", "starting", "no_image", "need_reboot"):
            sudo_spawn([session_bin(), "cleanup"])
        self._mode = "idle"
        self._await_start = False
        self._primary.set_sensitive(True)
        self._primary.set_label("Start")
        self._abort.set_sensitive(False)
        self._abort.set_label("Abort")
        self._set_hero("STOPPED", "Driver unloading. Start again when ready.", "coach-warn")

    def on_close(self, *_a) -> bool:
        sudo_spawn(_kill_args())
        sudo_spawn([session_bin(), "cleanup"])
        return False

    def tick(self) -> bool:
        if self._await_start:
            logdir = live_logdir()
            if logdir and logdir != self._ignore_logdir and Path(logdir).is_dir():
                self._await_start = False
            elif time.monotonic() > self._start_deadline:
                self._await_start = False
                self._mode = "idle"
                self._primary.set_sensitive(True)
                self._primary.set_label("Start")
                self._abort.set_sensitive(False)
                self._set_hero("NO SESSION", "Could not start. Close and try Start again.", "coach-warn")
                return True
        if self._mode == "idle":
            return True
        st = read_status()
        if not st:
            return True
        if self._mode == "parked":
            irq = st.get("irq_count")
            listen = st.get("listen")
            self._strip.set_text(
                f"listen {listen} · IRQ {irq if irq is not None else '—'} · L2 · parked"
            )
            return True
        self._apply_status(st)
        return True

    def _apply_status(self, st: dict) -> None:
        score = st.get("score") or {}
        events = st.get("events") or {}
        hero = str(st.get("hero") or "")
        plain = str(st.get("plain") or "")
        phase = str(st.get("phase") or "")
        irq = st.get("irq_count")
        listen = st.get("listen")
        tz = "TZ" if score.get("tz_up") else ("TA?" if events.get("ta_resident_neg22") else "—")
        fr = events.get("frame_raw") or 0
        fid = events.get("new_fid") or "—"
        strip = (
            f"listen {listen} · IRQ {irq if irq is not None else '—'} · {tz} · frame {fr} · fid {fid}"
        )
        if strip != self._strip_key:
            self._strip_key = strip
            self._strip.set_text(strip)
        meta = plain + "\nDoes not unlock Phosh. Close unloads the driver."
        if self._plain.get_text() != meta:
            self._plain.set_text(meta)

        # Do not touch sysfs while Matching — IRQ storm blocked GLib here
        # and the button stayed Matching… after MATCH_FAIL.
        if self._mode != "match":
            if self._mode in ("enrolled", "matched") and not extra_loaded():
                self._adopt_live_or_ready()
            elif self._mode == "ready" and live_saved_finger():
                self._adopt_live_or_ready()

        if self._mode == "starting" and phase in ("ready", "health") and (
            events.get("prepare_ok") or score.get("tz_up") or score.get("press_ok")
        ):
            self._adopt_live_or_ready()
        if self._mode == "starting" and phase == "ready":
            self._adopt_live_or_ready()

        nxt = next_enroll_mode(self._mode, phase, hero, score, events)
        # Mark file without extra is stale (helper deletes it; GTK still must not Match-only).
        if nxt == "enrolled" and not extra_loaded():
            nxt = "ready"
        if self._mode == "enroll" and nxt != self._mode:
            if nxt == "enrolled":
                self._mode = "enrolled"
                self._primary.set_sensitive(True)
                self._primary.set_label("Match this finger")
                self._abort.set_sensitive(True)
                self._abort.set_label("Register again")
            elif nxt == "ready":
                self._adopt_live_or_ready()
            else:
                self._mode = nxt
                if nxt == "need_reboot":
                    self._primary.set_sensitive(False)
                    self._primary.set_label("Reboot first")
                    self._abort.set_sensitive(True)

        if self._mode == "match" and score.get("match"):
            self._mode = "matched"
            self._primary.set_sensitive(True)
            self._primary.set_label("Match again")
            self._abort.set_label("Register again")
        elif self._mode == "match":
            if phase == "match":
                self._match_saw_phase = True
            if hero == "NO MATCH" or score.get("match"):
                self._mode = "enrolled" if not score.get("match") else "matched"
                self._primary.set_sensitive(True)
                self._primary.set_label("Match again")
                self._abort.set_label("Register again")
            auth_fail = bool(events.get("auth_fail"))
            if getattr(self, "_match_fail_at_click", False) and not auth_fail:
                self._match_fail_at_click = False
            new_fail = auth_fail and not getattr(self, "_match_fail_at_click", False)
            if match_should_leave_wait(
                phase=phase,
                saw_match_phase=self._match_saw_phase,
                match_t=self._match_t,
                now=time.monotonic(),
                score_match=bool(score.get("match")),
                auth_fail=new_fail,
                cap_s=15.0,
            ):
                self._mode = "enrolled"
                self._primary.set_sensitive(True)
                self._primary.set_label("Match again")
                self._abort.set_label("Register again")

        if self._mode in ("enroll", "match", "starting"):
            # Ready leftover / prior Register must not look like this press.
            if self._mode == "enroll" and hero in ("FINGER SEEN", "SENSOR READY", "SAVED"):
                hero = "WAIT"
                plain = "Hands off until the LED flashes. Do not touch the power-button yet."
            # Leftover SAVED/SENSOR READY → WAIT only while matching.
            if self._mode == "match":
                hero = match_hero_while_matching("SAVED")
                plain = "Match: wait for the LED, then press and lift like Register."
            self._set_hero(hero or "WAIT", plain, _style_for(hero))
        elif self._mode == "need_reboot":
            if hero == "FINGER SEEN":
                self._set_hero(
                    "FINGER SEEN",
                    plain
                    or "Finger captured, no template. Register again — no reboot.",
                    "coach-warn",
                )
            elif hero == "HOLD POWER" or events.get("wedge"):
                self._set_hero(
                    "REBOOT",
                    plain
                    or "Normal Linux reboot, then Start → Register. Hold Power ~30s only if the screen is frozen.",
                    "coach-warn",
                )
            else:
                self._set_hero(
                    "REBOOT",
                    plain
                    or "Normal Linux reboot, then Start → Register. Hold Power ~30s only if the screen is frozen.",
                    "coach-warn",
                )
        elif self._mode == "no_image":
            self._set_hero(
                hero or "NO IMAGE",
                plain or "No frame. Start over loads the sensor. Register is a second tap.",
                "coach-warn",
            )
        elif self._mode == "ready":
            if extra_loaded() and not live_saved_finger():
                self._primary.set_sensitive(False)
                self._primary.set_label("Reboot first")
                wh, ws = self._wedge_ready_copy()
                self._set_hero(wh, ws, "coach-warn")
            else:
                if hero in ("SAVED", "MATCH") and not live_saved_finger():
                    hero = "SENSOR READY"
                    plain = (
                        "TrustZone up. Register when you want. "
                        "Hands off until the LED flashes."
                    )
                self._primary.set_sensitive(True)
                if self._primary.get_label() == "Reboot first":
                    self._primary.set_label("Register fingerprint")
                self._set_hero(
                    hero or "SENSOR READY",
                    plain
                    or "TrustZone up. Register when you want. Hands off until the LED flashes.",
                    _style_for(hero or "SENSOR READY"),
                )
        elif self._mode == "enrolled":
            if events.get("auth_fail") and hero != "MATCH":
                hero = "NO MATCH"
            self._set_hero(
                hero or "SAVED",
                "Match this finger, or tap Register again. Not Phosh unlock.",
                _style_for(hero or "SAVED"),
            )
        elif self._mode == "matched":
            self._set_hero(
                hero or "MATCH",
                "Match again, or Register again. Still not Phosh unlock.",
                _style_for(hero or "MATCH"),
            )


class FpApp(Adw.Application):
    def __init__(self) -> None:
        super().__init__(application_id=APP_ID)
        self.connect("activate", self.on_activate)

    def on_activate(self, *_a) -> None:
        win = FpWindow(self)
        win.present()


def main() -> int:
    return FpApp().run(None)


if __name__ == "__main__":
    raise SystemExit(main())
