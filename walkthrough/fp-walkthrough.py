#!/usr/bin/env python3
"""FP5 fingerprint lab walkthrough — GTK4 guided coach.

Big on-screen cues ("DO NOT TOUCH", "TAP NOW — 4 left", "HOLD 5…") with
timed auto-advance so the user can watch the power-button sensor, not the
«Mark done» button.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import traceback
from datetime import datetime, timezone
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk, Gdk, Pango  # noqa: E402

APP_ID = "org.fp5.fingerprint-walkthrough"
# Interactive enroll: do not wait forever (module_init can wedge).
ENROLL_TIMEOUT_SEC = 420
SESSION_KEEP = 3
LOW_SPACE_WARN_MB = 250
LOW_SPACE_BLOCK_MB = 40

CSS = """
window {
  background-color: #101418;
}
/* Compact coach: must fit Phosh with bottom bar + home indicator. */
.coach-hero {
  font-size: 22px;
  font-weight: 800;
  letter-spacing: 0.1px;
  color: #f5f7fa;
  line-height: 1.1;
}
.coach-sub {
  font-size: 14px;
  font-weight: 600;
  color: #c5d0dc;
}
.coach-meta {
  font-size: 12px;
  color: #8a97a6;
}
.coach-idle { color: #7dd3fc; }
.coach-tap { color: #4ade80; }
.coach-hold { color: #fbbf24; }
.coach-lift { color: #f472b6; }
.coach-done { color: #a3e635; }
.coach-warn { color: #fca5a5; }
.progress-chip {
  font-size: 13px;
  font-weight: 700;
  color: #e2e8f0;
  padding: 2px 6px;
}
.btn-bar {
  padding: 8px 10px 14px 10px;
  background-color: #101418;
}
"""


def _bundle_dirs() -> list[Path]:
    here = Path(__file__).resolve().parent
    return [
        here,
        Path("/usr/local/share/fp5-fp-walkthrough"),
        Path("/usr/share/fp5-fp-walkthrough"),
        Path.home() / ".local/share/fp5-fp-walkthrough",
    ]


def find_resource(name: str) -> Path:
    for d in _bundle_dirs():
        p = d / name
        if p.is_file():
            return p
    raise FileNotFoundError(name)


def load_steps() -> dict:
    path = find_resource("steps.json")
    return json.loads(path.read_text())


def disk_free_mb(path: Path | None = None) -> int | None:
    try:
        st = os.statvfs(str(path or "/"))
        return int(st.f_bavail * st.f_frsize // (1024 * 1024))
    except OSError:
        return None


def prune_old_sessions(root: Path, keep: int = SESSION_KEEP) -> int:
    """Delete oldest session-* dirs so lab logs cannot fill the 3.3G root."""
    if keep < 0 or not root.is_dir():
        return 0
    sessions = sorted(
        (p for p in root.iterdir() if p.is_dir() and p.name.startswith("session-")),
        key=lambda p: p.name,
    )
    extra = sessions[:-keep] if keep else sessions
    removed = 0
    for p in extra:
        try:
            shutil.rmtree(p)
            removed += 1
        except OSError:
            continue
    return removed


def logger_script() -> Path:
    for d in _bundle_dirs():
        p = d / "fp-walkthrough-logger.sh"
        if p.is_file():
            return p
    return Path(__file__).resolve().parent / "fp-walkthrough-logger.sh"


def prepare_script() -> Path:
    for d in _bundle_dirs():
        p = d / "fp-walkthrough-prepare.sh"
        if p.is_file():
            return p
    return Path(__file__).resolve().parent / "fp-walkthrough-prepare.sh"


def run_sudo_script(
    script: Path,
    *args: str,
    timeout: float = 12.0,
    allow_user_fallback: bool = True,
) -> subprocess.CompletedProcess:
    """Run script with sudo -n first. Optionally fall back to plain user."""
    attempts: list[list[str]] = [["sudo", "-n", str(script), *args]]
    if allow_user_fallback:
        attempts.append([str(script), *args])
    last = subprocess.CompletedProcess(args, 127, "", "no runner tried")
    for cmd in attempts:
        try:
            last = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
                start_new_session=True,
            )
            if last.returncode == 0 or "OK" in (last.stdout or "") or "FP_PREPARE_OK" in (
                last.stdout or ""
            ):
                return last
            # Root-only tools: do not fall through to user after any sudo result
            if cmd[0] == "sudo" and not allow_user_fallback:
                return last
        except subprocess.TimeoutExpired as e:
            last = subprocess.CompletedProcess(
                cmd,
                124,
                (e.stdout or "") if isinstance(e.stdout, str) else "",
                f"TIMEOUT after {timeout}s",
            )
            if cmd[0] == "sudo" and not allow_user_fallback:
                return last
        except FileNotFoundError:
            continue
        except Exception as e:  # noqa: BLE001
            last = subprocess.CompletedProcess(cmd, 125, "", str(e))
    return last


def run_logger(*args: str, timeout: float = 8.0) -> subprocess.CompletedProcess:
    """Run logger. Never hang the UI: short timeout, no pkexec, detach children."""
    return run_sudo_script(logger_script(), *args, timeout=timeout)


def prepare_sensor() -> tuple[bool, str]:
    """Load life module + listen. Returns (ok, status_text)."""
    script = prepare_script()
    if not script.is_file():
        return False, f"prepare script missing: {script}"
    r = run_sudo_script(script, "all", timeout=15.0)
    out = ((r.stdout or "") + "\n" + (r.stderr or "")).strip()
    ok = r.returncode == 0 or "FP_PREPARE_OK" in out
    return ok, out[-900:]


def pump_ui() -> None:
    ctx = GLib.MainContext.default()
    for _ in range(50):
        if not ctx.pending():
            break
        ctx.iteration(False)


class WalkthroughWindow(Adw.ApplicationWindow):
    def __init__(self, app: Adw.Application):
        super().__init__(application=app, title="FP5 Fingerprint Lab")
        self.set_default_size(400, 780)
        try:
            self.cfg = load_steps()
        except Exception as e:  # noqa: BLE001
            self.cfg = {
                "title": "FP walkthrough (error loading steps)",
                "intro": f"Could not load steps.json:\n{e}",
                "steps": [],
                "session_root": str(Path.home() / "fp-walkthrough"),
                "outro": "Fix install and reopen.",
            }
        self.steps = list(self.cfg.get("steps") or [])
        self.idx = -1
        self.session: Path | None = None
        self._busy = False
        self._coach_source: int | None = None
        self._coach_gen = 0
        self._phase_queue: list[tuple[str, str, str, float]] = []
        # (hero, sub, css_class, seconds)

        css = Gtk.CssProvider()
        css.load_from_data(CSS.encode())
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(),
            css,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )

        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        header.set_title_widget(
            Gtk.Label(label=self.cfg.get("title", "FP walkthrough"))
        )
        toolbar.add_top_bar(header)

        # Compact vertical stack; no giant vexpand (was pushing UI off-screen).
        self.box = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            spacing=6,
            margin_top=8,
            margin_bottom=4,
            margin_start=12,
            margin_end=12,
        )
        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroller.set_child(self.box)
        scroller.set_vexpand(True)
        toolbar.set_content(scroller)

        self.progress_lbl = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER)
        self.progress_lbl.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.progress_lbl.add_css_class("progress-chip")

        self.coach_hero = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER)
        self.coach_hero.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.coach_hero.set_max_width_chars(16)
        self.coach_hero.set_lines(2)
        self.coach_hero.add_css_class("coach-hero")
        self.coach_hero.set_hexpand(True)
        self.coach_hero.set_vexpand(False)
        self.coach_hero.set_valign(Gtk.Align.CENTER)

        self.coach_sub = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER)
        self.coach_sub.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.coach_sub.set_max_width_chars(28)
        self.coach_sub.set_lines(2)
        self.coach_sub.add_css_class("coach-sub")
        self.coach_sub.set_hexpand(True)

        self.title_lbl = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER)
        self.title_lbl.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.title_lbl.set_max_width_chars(28)
        self.title_lbl.add_css_class("title-4")

        self.body_lbl = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER)
        self.body_lbl.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.body_lbl.set_max_width_chars(32)
        self.body_lbl.set_lines(3)
        self.body_lbl.add_css_class("coach-meta")
        self.body_lbl.set_selectable(True)

        self.path_lbl = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER)
        self.path_lbl.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.path_lbl.set_max_width_chars(32)
        self.path_lbl.set_lines(2)
        self.path_lbl.add_css_class("dim-label")
        self.path_lbl.add_css_class("coach-meta")

        self.status_lbl = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER)
        self.status_lbl.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.status_lbl.set_max_width_chars(32)
        self.status_lbl.set_lines(2)
        self.status_lbl.add_css_class("coach-meta")
        self.status_lbl.set_selectable(True)

        self.box.append(self.progress_lbl)
        self.box.append(self.coach_hero)
        self.box.append(self.coach_sub)
        self.box.append(self.title_lbl)
        self.box.append(self.body_lbl)
        self.box.append(self.path_lbl)
        self.box.append(self.status_lbl)

        # Bottom action bar — always on-screen above home indicator.
        btn_bar = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        btn_bar.add_css_class("btn-bar")
        btn_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        btn_row.set_homogeneous(True)
        self.btn_primary = Gtk.Button(label="Start logging")
        self.btn_primary.add_css_class("suggested-action")
        self.btn_primary.add_css_class("pill")
        self.btn_primary.connect("clicked", self.on_primary)
        self.btn_skip = Gtk.Button(label="Skip")
        self.btn_skip.connect("clicked", self.on_skip)
        self.btn_redo = Gtk.Button(label="Redo")
        self.btn_redo.connect("clicked", self.on_redo)
        self.btn_enroll = Gtk.Button(label="Try register")
        self.btn_enroll.add_css_class("destructive-action")
        self.btn_enroll.connect("clicked", self.on_enroll)
        self.btn_enroll.set_visible(False)
        self.btn_enroll.set_sensitive(False)
        btn_row.append(self.btn_primary)
        btn_row.append(self.btn_redo)
        btn_row.append(self.btn_skip)
        btn_bar.append(btn_row)
        btn_bar.append(self.btn_enroll)
        toolbar.add_bottom_bar(btn_bar)

        self._press_ok = False
        self._enroll_busy = False
        self._enroll_t0 = 0.0
        self._enroll_watch = None
        self.connect("close-request", self.on_close_request)
        self.set_content(toolbar)
        self.show_intro()

    # ---- display helpers ----
    def _clear_coach_style(self) -> None:
        for c in (
            "coach-idle",
            "coach-tap",
            "coach-hold",
            "coach-lift",
            "coach-done",
            "coach-warn",
        ):
            self.coach_hero.remove_css_class(c)

    def set_coach(
        self, hero: str, sub: str = "", style: str = "coach-idle", *, pump: bool = True
    ) -> None:
        self._clear_coach_style()
        if style:
            self.coach_hero.add_css_class(style)
        self.coach_hero.set_text(hero)
        self.coach_sub.set_text(sub)
        if pump:
            pump_ui()

    def set_status(self, msg: str) -> None:
        # Keep status short so it does not dominate the coach screen.
        lines = (msg or "").strip().splitlines()
        short = "\n".join(lines[:2])
        if len(short) > 160:
            short = short[:157] + "…"
        self.status_lbl.set_text(short)
        pump_ui()

    def set_progress(self, text: str) -> None:
        self.progress_lbl.set_text(text)

    def _set_detail_visible(self, visible: bool) -> None:
        """Hide long detail widgets during timed coach steps (saves vertical space)."""
        for w in (self.title_lbl, self.body_lbl, self.path_lbl):
            w.set_visible(visible)

    def cancel_coach(self) -> None:
        self._coach_gen += 1
        if self._coach_source is not None:
            try:
                GLib.source_remove(self._coach_source)
            except Exception:
                pass
            self._coach_source = None
        self._phase_queue.clear()

    # ---- screens ----
    def show_intro(self) -> None:
        self.cancel_coach()
        self.idx = -1
        self._set_detail_visible(True)
        self.title_lbl.set_text(self.cfg.get("title", "FP walkthrough"))
        self.body_lbl.set_text(
            self.cfg.get("intro")
            or "Ignore a stuck 5s clock. After Register: short beep = started. "
            "Wait ~70s for a loud beep + vibe + flash, then PRESS ~1s and LIFT. "
            "Six times. Screen often freezes — trust the beep."
        )
        self.path_lbl.set_text(
            "Logs: " + str(self.cfg.get("session_root", "/var/tmp/fp-walkthrough"))
        )
        stage = self.cfg.get("matrix_stage") or "register path"
        self.set_progress(str(stage)[:48])
        self.set_coach("IGNORE THE CLOCK", "Beep+flash = PRESS now", "coach-warn")
        self.btn_primary.set_label("Start")
        self.btn_primary.set_sensitive(True)
        self.btn_skip.set_sensitive(False)
        self.btn_redo.set_sensitive(False)
        self.btn_enroll.set_visible(True)
        self.btn_enroll.set_sensitive(True)
        self.btn_enroll.set_label("Register now")
        self._press_ok = False
        self._skip_coach = False
        free = disk_free_mb(Path(self.cfg.get("session_root", "/var/tmp")))
        if free is not None and free < LOW_SPACE_WARN_MB:
            self.set_status(f"LOW SPACE {free} MB free — Start prunes old sessions")
        elif free is not None:
            self.set_status(f"Start → driver + gated quiet/press · {free} MB free")
        else:
            self.set_status("Start → driver + gated quiet/press + samples")
        # Best-effort status probe without insmod (fast)
        GLib.idle_add(self._probe_sensor_idle)

    def _probe_sensor_idle(self) -> bool:
        try:
            r = run_sudo_script(prepare_script(), "status", timeout=6.0)
            out = (r.stdout or "").strip()
            if "FP_PREPARE_OK" in out:
                self.set_progress("Sensor ready")
                self.set_status("Sensor stack already up. Tap Start when ready.\n" + out[-200:])
            elif "FP_PREPARE" in out:
                self.set_progress("Sensor needs load")
                self.set_status(
                    "Sensor driver not active yet — Start will load it.\n" + out[-240:]
                )
        except Exception:  # noqa: BLE001
            pass
        return GLib.SOURCE_REMOVE

    def on_primary(self, *_args) -> None:
        if self._busy:
            self.set_status("Still working… please wait a few seconds.")
            return
        if self.idx < 0:
            self.set_status("Loading sensor driver + logger…")
            self.set_coach("LOADING…", "Driver + IRQ listen", "coach-idle")
            self.btn_primary.set_sensitive(False)
            self.btn_primary.set_label("Working…")
            pump_ui()
            GLib.idle_add(self._start_session_idle)
            return
        if self.idx >= len(self.steps):
            if self._enroll_busy:
                self.abort_enroll("Close — aborting register")
                return
            self.do_cleanup_and_close()
            return
        # Manual complete (user finished early or coach done)
        self.set_status("Recording step complete…")
        self.btn_primary.set_sensitive(False)
        self.cancel_coach()
        pump_ui()
        GLib.idle_add(self._complete_step_idle)

    def on_skip(self, *_args) -> None:
        if self.idx < 0 or self.idx >= len(self.steps) or self._busy:
            return
        step = self.steps[self.idx]
        self.cancel_coach()
        self.set_status("Skipping step…")
        if self.session:
            run_logger("marker", str(self.session), f"SKIP_{step.get('id', self.idx)}")
        self.advance()

    def on_redo(self, *_args) -> None:
        if self.idx < 0 or self.idx >= len(self.steps) or self._busy:
            return
        self.start_step_coach()

    def abort_enroll(self, why: str = "aborted") -> None:
        """Stop waiting; best-effort kill helper (kernel init may still need reboot)."""
        proc = getattr(self, "_enroll_proc", None)
        if proc and proc.poll() is None:
            try:
                proc.terminate()
            except Exception:
                pass
            try:
                subprocess.run(
                    ["sudo", "-n", "pkill", "-f", "qsee_fingerpr.ko"],
                    timeout=3,
                    check=False,
                    capture_output=True,
                )
            except Exception:
                pass
        self._enroll_cues = []
        self._enroll_last_cue = ""
        self.cancel_coach()
        self._enroll_busy = False
        self._enroll_proc = None
        self.set_coach("STOPPED", why[:48], "coach-warn")
        self.set_status("Stopped. Retry register, or Close.")
        self.btn_enroll.set_sensitive(True)
        self.btn_enroll.set_label("Retry register")
        self.btn_primary.set_label("Close")
        self.btn_primary.set_sensitive(True)

    def do_cleanup_and_close(self) -> None:
        if self.session:
            run_logger("cleanup", str(self.session), timeout=8.0)
        self.close()

    def on_close_request(self, *_a) -> bool:
        if self._enroll_busy:
            self.abort_enroll("window close")
        if self.session:
            run_logger("cleanup", str(self.session), timeout=8.0)
        return False  # allow destroy

    def on_enroll(self, *_args) -> None:
        """Opt-in TrustZone register — abortable; cues follow the kernel, not a timer."""
        if self._enroll_busy:
            self.abort_enroll("user abort")
            return
        if not self.session:
            # Coach is optional. Register now: session + enroll, no TAP/HOLD script.
            self._skip_coach = True
            self.set_status("Skipping coach — starting register…")
            self.set_coach("ARMING…", "No coach this time", "coach-idle")
            self.btn_enroll.set_sensitive(False)
            GLib.idle_add(self._start_session_idle)
            return
        self._enroll_busy = True
        self._enroll_t0 = GLib.get_monotonic_time() / 1e6
        self._enroll_last_cue = ""
        self.btn_enroll.set_sensitive(True)
        self.btn_enroll.set_label("Abort")
        self.btn_primary.set_sensitive(True)
        self.btn_primary.set_label("Close")
        self.set_coach("ARMING…", "Do not press yet", "coach-idle")
        self.set_status("Connecting — PRESS cue comes when the TA is ready.")
        GLib.idle_add(self._start_enroll_idle)

    def _start_enroll_idle(self) -> bool:
        if not self._enroll_busy or not self.session:
            return GLib.SOURCE_REMOVE
        script = None
        for d in _bundle_dirs():
            p = d / "fp-walkthrough-enroll.sh"
            if p.is_file():
                script = p
                break
        if not script:
            self.set_coach("NO SCRIPT", "enroll helper missing", "coach-warn")
            self._enroll_busy = False
            self.btn_enroll.set_label("Try register")
            return GLib.SOURCE_REMOVE

        # Re-arm listen only if needed (full prepare blocked the UI for ~15s).
        r = run_sudo_script(prepare_script(), "status", timeout=6.0)
        if "FP_PREPARE_OK" not in (r.stdout or ""):
            prepare_sensor()
        try:
            self._enroll_proc = subprocess.Popen(
                ["sudo", "-n", str(script), str(self.session)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
        except OSError as e:
            self.set_coach("FAIL", str(e)[:40], "coach-warn")
            self._enroll_busy = False
            self.btn_enroll.set_label("Try register")
            return GLib.SOURCE_REMOVE
        self.set_coach("STARTING…", "Screen may stutter 3–5s — keep waiting", "coach-idle")
        GLib.timeout_add(1000, self._tick_enroll_clock)
        GLib.timeout_add(1500, self._poll_enroll_proc)
        return GLib.SOURCE_REMOVE

    def _tick_enroll_clock(self) -> bool:
        """Cheap 1s heartbeat so ATTACHING/PREPARING does not look frozen."""
        if not getattr(self, "_enroll_busy", False):
            return GLib.SOURCE_REMOVE
        if getattr(self, "_enroll_proc", None) is None:
            return GLib.SOURCE_REMOVE
        if self._enroll_proc.poll() is not None:
            return GLib.SOURCE_REMOVE
        elapsed = int((GLib.get_monotonic_time() / 1e6) - getattr(self, "_enroll_t0", 0))
        hero = (self.coach_hero.get_text() or "") if hasattr(self, "coach_hero") else ""
        if hero in ("ATTACHING…", "PREPARING…", "STARTING…", "ARMING…"):
            self.coach_sub.set_text(f"{elapsed}s · wait for PRESS (stutter at 3–5s is GPU)")
        return True

    def _apply_enroll_progress(self) -> None:
        """Drive PRESS/LIFT from kernel banners so the timer cannot skip samples."""
        if not self.session:
            return
        chunks: list[str] = []
        for name in ("enroll-progress.txt", "enroll-kernel.txt", "dmesg-follow.txt"):
            p = self.session / name
            try:
                chunks.append(p.read_text(errors="ignore"))
            except OSError:
                continue
        text = "\n".join(chunks)
        if not text:
            return
        lines = text.splitlines()
        press = [ln for ln in lines if "PRESS then LIFT finger sample" in ln]
        hold = [ln for ln in lines if "HOLD FINGER NOW" in ln]
        waiting = [ln for ln in lines if "waiting up to" in ln and "finger" in ln]
        phase = [ln for ln in lines if ln.startswith("PHASE ")]
        last_press_i = text.rfind("PRESS then LIFT finger sample")
        last_lift_i = text.rfind("LIFT finger —")
        last_down_i = text.rfind("REAL DOWN")
        elapsed = int((GLib.get_monotonic_time() / 1e6) - getattr(self, "_enroll_t0", 0))

        hero, sub, style = "", "", "coach-idle"
        if press and last_press_i >= last_lift_i and last_press_i >= last_down_i:
            m = re.search(r"sample (\d+)/(\d+)", press[-1])
            if m:
                n, tot = int(m.group(1)), int(m.group(2))
                shown = n + 1 if n < tot else n
                hero, sub, style = "PRESS", f"Sample {shown}/{tot} · ~1s then LIFT", "coach-tap"
        elif last_lift_i > last_press_i:
            hero, sub, style = "LIFT", "Fully off the sensor", "coach-lift"
        elif last_down_i > last_press_i:
            hero, sub, style = "GOT IT", "Lift, then next PRESS", "coach-done"
        elif hold or waiting:
            hero, sub, style = "PRESS", "Sensor ready · ~1s then LIFT", "coach-tap"
        elif phase and "attaching" in phase[-1].lower():
            hero, sub, style = "ATTACHING…", f"{elapsed}s · wait for PRESS", "coach-idle"
        elif "START focal32 ret=0" in text:
            hero, sub, style = "PREPARING…", f"{elapsed}s · wait for PRESS", "coach-idle"
        elif "START focal32 ret=-22" in text:
            hero, sub, style = "ATTACHING…", f"{elapsed}s · wait for PRESS", "coach-idle"
        elif elapsed >= 2:
            hero, sub, style = "STARTING…", f"{elapsed}s · wait for PRESS", "coach-idle"
        else:
            return

        key = f"{hero}|{sub}"
        if key == getattr(self, "_enroll_last_cue", ""):
            return
        self._enroll_last_cue = key
        self.set_coach(hero, sub, style, pump=False)
        if press:
            self.set_status(press[-1][-80:])
        elif "no finger — not ENROLLing" in text:
            self.set_status("Init finished with no finger window — next build always samples.")

    def _poll_enroll_proc(self) -> bool:
        proc = getattr(self, "_enroll_proc", None)
        if proc is None:
            return GLib.SOURCE_REMOVE
        elapsed = (GLib.get_monotonic_time() / 1e6) - getattr(self, "_enroll_t0", 0)
        if elapsed > ENROLL_TIMEOUT_SEC and proc.poll() is None:
            self.abort_enroll(f"timeout {int(elapsed)}s — reboot if still wedged")
            return GLib.SOURCE_REMOVE
        if proc.poll() is None:
            self._apply_enroll_progress()
            return True
        rc = proc.poll()
        if rc is None:
            return True  # keep polling
        out = ""
        try:
            out, _ = proc.communicate(timeout=1)
        except Exception:
            pass
        try:
            if self.session:
                (self.session / "enroll-ui.txt").write_text((out or "") + "\n")
        except OSError:
            pass
        self._enroll_cues = []
        self.cancel_coach()
        if out and ("ENROLL_OK" in out or "new fid" in out.lower()):
            self.set_coach("FID OK", "Template id generated!", "coach-done")
            self.set_status("Register progress — pull enroll-hits.txt")
        elif out and "WEDGED_REBOOT" in out:
            self.set_coach("REBOOT", "Hold Power — module stuck", "coach-warn")
            self.set_status((out or "")[-200:])
        elif out and "ENROLL_FAIL ATTACH" in out:
            self.set_coach("TA BUSY", "Retry register — no reboot", "coach-hold")
            self.set_status("TAs already in TZ. Retry attaches by id. Reboot only if Retry stays dead.")
        elif out and "ENROLL_FAIL" in out:
            self.set_coach("FAILED", "insmod/root issue", "coach-warn")
            self.set_status((out or "")[-200:])
        elif out and "ENROLL_PARTIAL" in out:
            if "HEALTH final er=-22" in out or "er=-22" in (out or ""):
                self.set_coach("TA DEAD", "Need a fresh START — tap Register again", "coach-warn")
            else:
                self.set_coach("PARTIAL", "TA ran · no solid fid", "coach-hold")
            self.set_status((out or "")[-200:])
        else:
            self.set_coach("DONE", f"rc={rc}", "coach-idle")
            self.set_status((out or "no output")[-200:])
        self._enroll_busy = False
        self._enroll_proc = None
        self.btn_enroll.set_sensitive(True)
        self.btn_enroll.set_label("Retry register")
        self.btn_primary.set_label("Close")
        self.btn_primary.set_sensitive(True)
        return GLib.SOURCE_REMOVE

    def _start_session_idle(self) -> bool:
        try:
            self.start_session()
        except Exception:  # noqa: BLE001
            self.set_status("ERROR starting session:\n" + traceback.format_exc()[-800:])
            self.btn_primary.set_sensitive(True)
            self.btn_primary.set_label("Retry start")
            self._busy = False
        return GLib.SOURCE_REMOVE

    def _complete_step_idle(self) -> bool:
        try:
            self.complete_step()
        except Exception:  # noqa: BLE001
            self.set_status("ERROR on step:\n" + traceback.format_exc()[-500:])
            self.btn_primary.set_sensitive(True)
        return GLib.SOURCE_REMOVE

    def start_session(self) -> None:
        self._busy = True
        self.set_coach("LOADING…", "Sensor driver + IRQ", "coach-idle")
        self.set_status("Creating session directory…")
        root = Path(self.cfg.get("session_root", "/var/tmp/fp-walkthrough"))
        n_pruned = prune_old_sessions(root, keep=SESSION_KEEP)
        free = disk_free_mb(root if root.exists() else Path("/var/tmp"))
        if free is not None and free < LOW_SPACE_WARN_MB:
            n_pruned += prune_old_sessions(root, keep=1)
            free = disk_free_mb(root if root.exists() else Path("/var/tmp"))
        if free is not None and free < LOW_SPACE_BLOCK_MB:
            self.set_coach("LOW SPACE", f"{free} MB free", "coach-warn")
            self.set_status("Need ~40 MB. Close other apps or reboot.")
            self.btn_primary.set_sensitive(True)
            self.btn_primary.set_label("Retry start")
            self._busy = False
            return
        if n_pruned:
            space = f"{free} MB free" if free is not None else "space ok"
            self.set_status(f"Pruned {n_pruned} old session(s) · {space}")
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        self.session = root / f"session-{stamp}"
        try:
            self.session.mkdir(parents=True, exist_ok=True)
            os.chmod(self.session, 0o755)
        except OSError as e:
            self.set_status(f"Cannot create {self.session}: {e}\nTrying home…")
            self.session = Path.home() / "fp-walkthrough" / f"session-{stamp}"
            self.session.mkdir(parents=True, exist_ok=True)

        try:
            (self.session / "steps-used.json").write_text(
                json.dumps(self.cfg, indent=2) + "\n"
            )
        except OSError as e:
            self.set_status(f"Warning: could not write steps-used.json: {e}")

        self.path_lbl.set_text(f"Session:\n{self.session}")

        # --- Load life module + listen (required for non-empty IRQ series) ---
        self.set_status("Loading focaltech_fp_life + listen=1…")
        prep_ok, prep_out = prepare_sensor()
        try:
            (self.session / "prepare.txt").write_text(prep_out + "\n")
        except OSError:
            pass
        if prep_ok:
            self.set_progress("Sensor ready")
            self.set_coach("SENSOR OK", "IRQ 34 · listen", "coach-done")
            self.set_status("Sensor OK")
        else:
            self.set_progress("No driver")
            self.set_coach("NO DRIVER", "Coach only", "coach-warn")
            self.set_status("Driver failed · coach continues")
        pump_ui()

        self.set_status("Starting logger…")
        r = run_logger("start", str(self.session), timeout=8.0)
        out = ((r.stdout or "") + "\n" + (r.stderr or "")).strip()
        if r.returncode == 0 or "OK" in out:
            self.set_status("Logger OK" if prep_ok else "Logger OK · no sensor")
        else:
            self.set_status(f"Logger warn rc={r.returncode}")

        self._busy = False
        self.btn_primary.set_sensitive(True)
        if getattr(self, "_skip_coach", False):
            self._skip_coach = False
            self._press_ok = True
            self.idx = len(self.steps)
            self.cancel_coach()
            self._set_detail_visible(True)
            self.set_progress("Coach skipped")
            self.btn_primary.set_label("Close")
            self.btn_skip.set_sensitive(False)
            self.btn_redo.set_sensitive(False)
            self.btn_enroll.set_visible(True)
            self.btn_enroll.set_sensitive(True)
            self.btn_enroll.set_label("Abort")
            # Start register immediately (no TAP/HOLD script).
            self.on_enroll()
            return
        self.idx = 0
        self.show_step()

    def show_step(self) -> None:
        if self.idx >= len(self.steps):
            self.show_outro()
            return
        step = self.steps[self.idx]
        n = len(self.steps)
        # Coach mode: only progress + hero + sub + short status (hide paragraphs).
        self._set_detail_visible(False)
        self.set_progress(f"Step {self.idx + 1}/{n}")
        self.title_lbl.set_text(step.get("title", f"Step {self.idx + 1}"))
        self.body_lbl.set_text(step.get("instruction") or "")
        self.btn_primary.set_label("Done")
        self.btn_primary.set_sensitive(True)
        self.btn_skip.set_sensitive(True)
        self.btn_redo.set_sensitive(True)

        markers = step.get("markers") or []
        if markers and self.session:
            run_logger("marker", str(self.session), markers[0], timeout=5.0)

        self.start_step_coach()

    def start_step_coach(self) -> None:
        """Build timed cue phases from step action and run them."""
        self.cancel_coach()
        self._set_detail_visible(False)
        step = self.steps[self.idx]
        action = (step.get("action") or "wait").lower()
        phases: list[tuple[str, str, str, float]] = []

        if action == "wait":
            secs = int(step.get("duration_sec") or 3)
            phases.append(("GET READY", "Hands off", "coach-idle", 0.8))
            for left in range(secs, 0, -1):
                phases.append(
                    ("DO NOT TOUCH", f"{left}s left", "coach-idle", 1.0)
                )
            phases.append(("DONE", "OK", "coach-done", 0.4))

        elif action == "hold":
            secs = int(step.get("duration_sec") or 3)
            # Extra second to get finger on the power-button sensor before countdown.
            phases.append(("GET READY", "Finger ready", "coach-idle", 1.0))
            phases.append(("GET READY", f"Hold in 1s…", "coach-idle", 1.0))
            phases.append(("HOLD", f"{secs}s", "coach-hold", 0.6))
            for left in range(secs, 0, -1):
                phases.append((f"HOLD {left}", "Keep pressing", "coach-hold", 1.0))
            phases.append(("LIFT", "Release", "coach-lift", 0.7))
            phases.append(("DONE", "OK", "coach-done", 0.4))

        elif action == "presses":
            count = int(step.get("count") or 5)
            phases.append(("GET READY", f"{count} taps", "coach-idle", 0.8))
            for i in range(1, count + 1):
                left = count - i
                phases.append(
                    (
                        "TAP NOW",
                        f"{i}/{count}" + (f" · {left} left" if left else " · last"),
                        "coach-tap",
                        1.0,
                    )
                )
                phases.append(("LIFT", "Release", "coach-lift", 0.6))
            phases.append(("DONE", f"{count} done", "coach-done", 0.4))

        else:
            phases.append(
                (
                    step.get("title") or "DO IT",
                    step.get("instruction") or "Then Done",
                    "coach-warn",
                    0.0,
                )
            )

        self._phase_queue = phases
        self.set_status("Follow cue · auto-next")
        self._run_next_phase()

    def _run_next_phase(self) -> None:
        if not self._phase_queue:
            # Coach finished → auto complete step
            self.set_coach("STEP COMPLETE", "Saving…", "coach-done")
            self.btn_primary.set_sensitive(False)
            GLib.idle_add(self._complete_step_idle)
            return

        hero, sub, style, secs = self._phase_queue.pop(0)
        self.set_coach(hero, sub, style)
        gen = self._coach_gen

        if secs <= 0:
            # static until user marks done
            self.set_status("Do the action, then tap «Mark done».")
            return

        # For multi-second hold we already emit 1s ticks; single timeout is fine
        def _tick() -> bool:
            if gen != self._coach_gen:
                return GLib.SOURCE_REMOVE
            self._coach_source = None
            self._run_next_phase()
            return GLib.SOURCE_REMOVE

        # Use milliseconds for smoother short phases
        ms = max(100, int(secs * 1000))
        self._coach_source = GLib.timeout_add(ms, _tick)

    def complete_step(self) -> None:
        self.cancel_coach()
        step = self.steps[self.idx]
        markers = step.get("markers") or []
        if self.session:
            if len(markers) >= 2:
                run_logger("marker", str(self.session), markers[1], timeout=5.0)
            run_logger(
                "snapshot",
                str(self.session),
                step.get("id", f"step{self.idx}"),
                timeout=5.0,
            )
        self.advance()

    def advance(self) -> None:
        self.idx += 1
        self.show_step()

    def show_outro(self) -> None:
        self.cancel_coach()
        self._set_detail_visible(True)
        self.set_progress("Finished")
        self.set_coach("SAVED", "Scoring…", "coach-done")
        self.set_status("Saving…")
        score_note = ""
        if self.session:
            r = run_logger("stop", str(self.session), "keep", timeout=12.0)
            # Read score if scorer wrote it
            sp = self.session / "score.json"
            if sp.is_file():
                try:
                    sc = json.loads(sp.read_text())
                    score_note = (
                        f"\n\nMatrix: {sc.get('matrix_stage')}\n"
                        f"Press signal: {sc.get('press_signal')}\n"
                        f"Next: {sc.get('next_step')}\n"
                        f"({sc.get('note')})"
                    )
                    if sc.get("press_signal"):
                        self._press_ok = True
                        self.set_coach("PRESS OK", "Quiet 0 · press live", "coach-done")
                    elif sc.get("matrix_stage") == "M0-irq-flat":
                        self.set_coach("IRQ FLAT", "Close + Start to re-arm", "coach-warn")
                    else:
                        self.set_coach("LIFE OK", "Need clearer press", "coach-hold")
                except Exception:
                    score_note = ""
            self.set_status(f"Saved rc={r.returncode}")
        self.title_lbl.set_text("Done — toward register")
        if self._press_ok:
            body_extra = (
                "\n\nPress signal OK. «Try register» cues PRESS/LIFT (6×). "
                "Wait for the PRESS cue (not a timer). Abort if it hangs. "
                "Firm press on the power-button sensor — not a rest."
            )
        else:
            body_extra = (
                "\n\nAuto-enroll stays off. Re-run coach until score shows press_signal."
            )
        self.body_lbl.set_text(
            (self.cfg.get("outro") or "Session complete.") + score_note + body_extra
        )
        if self.session:
            self.path_lbl.set_text(f"Session:\n{self.session}")
        self.btn_primary.set_label("Close")
        self.btn_primary.set_sensitive(True)
        self.btn_skip.set_sensitive(False)
        self.btn_redo.set_sensitive(False)
        # Offer enroll only after press discrimination works
        self.btn_enroll.set_visible(self._press_ok)
        self.btn_enroll.set_sensitive(self._press_ok)
        self.btn_enroll.set_label("Try register")
        self.idx = len(self.steps)


class WalkthroughApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id=APP_ID)
        self.connect("activate", self.on_activate)

    def on_activate(self, app):
        win = WalkthroughWindow(app)
        win.present()


def main() -> int:
    try:
        Adw.init()
    except Exception:
        pass
    app = WalkthroughApp()
    return app.run(sys.argv)


if __name__ == "__main__":
    raise SystemExit(main())
