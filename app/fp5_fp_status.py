"""Plain-language fingerprint status from lab logs (no dmesg from GTK)."""

from __future__ import annotations

import json
import re
import time
from pathlib import Path
from typing import Any

RUNDIR = Path("/run/fp5-fp-session")
STATUS_NAME = "status"

RE_IRQ_COUNT = re.compile(r"sysfs_irq_count=(\d+)")
RE_LISTEN_ON = re.compile(r"IRQ \d+ ENABLED \(listen\)|l2_listen_on sysfs enable")
RE_LISTEN_OFF = re.compile(r"IRQ \d+ DISABLED")
RE_TA_OK = re.compile(r"START focal32 ret=0\b")
RE_TA_NEG22 = re.compile(r"START focal32 ret=-22")
RE_HEALTH0 = re.compile(r"HEALTH(?: final| after capture path)? er=0\b")
RE_HEALTH22 = re.compile(r"HEALTH(?: final)? er=-22")
RE_FRAME = re.compile(r"\bframe raw(?:\[\d+\])?:|\*\*\* (?:ANDROID )?FRAME RAW")
RE_AVGV = re.compile(r"frame raw\[\d+\]: avgv = (\d+)")
RE_IMAGE_PIX = re.compile(r"IMAGE PIXELS")
RE_NEW_FID = re.compile(r"new fid \(= (\d+)\)")
RE_REM1 = re.compile(r"\brem_seen=1\b")
RE_AUTH0 = re.compile(
    r"overlay AUTH HIT err=0x0|overlay AUTH er=0 err=0x0\b|AUTH er=0 rem_seen="
)
RE_AUTH_FAIL = re.compile(
    r"overlay AUTH FAIL|\bMATCH_FAIL\b|overlay AUTH kick no itype|FDT dirty"
)
RE_AUTH_KICK = re.compile(r"overlay AUTH kick")
RE_OVERLAY_AUTH0 = re.compile(r"overlay AUTH er=0 err=0x0\b")
RE_BROKEN = re.compile(r"broken off")
RE_WEDGE = re.compile(r"WEDGED_REBOOT|qsee_fingerpr (?:busy|already loaded)")
RE_HOLD_NOW = re.compile(r"HOLD FINGER NOW")
RE_PRESS_BANNER = re.compile(r"PRESS(?: then)?/?LIFT finger sample|PRESS then LIFT finger sample")
RE_LIFT_BANNER = re.compile(r"LIFT finger —")
RE_REAL_DOWN = re.compile(r"(?<!no )REAL DOWN|(?<![\w])itype=0x2\b|\[finger touch\]")
RE_NO_REAL_DOWN = re.compile(r"no REAL DOWN")
RE_PREPARE_OK = re.compile(r"FP_PREPARE_OK")
RE_ENROLL_OK = re.compile(r"^ENROLL_OK\b", re.M)
RE_TA_ENROLLING = re.compile(r"enrolled fid")
RE_OVERLAY_REM = re.compile(r"overlay rem=(\d+)")
RE_PRESS_OK = re.compile(r"overlay PRESS OK rem=(\d+)")
RE_PRESS_EMPTY = re.compile(r"overlay PRESS EMPTY")
RE_PRESS_REJECT = re.compile(r"overlay PRESS REJECT rem=(\d+)")
RE_PRESS_WEAK = re.compile(r"overlay PRESS WEAK")
# Helper windows on last `qsee_fp: init` only. `ENROLL start er=` is AFTER
# `new fid (= N)` and must not slice that line off.
RE_INIT = re.compile(r"qsee_fp: init$", re.M)
RE_KTS = re.compile(r"^\[\s*(\d+)(?:\.\d+)?\]")
RE_QUAL_ERR = re.compile(r"quality.*error")
RE_QUAL_THR = re.compile(
    r"quality = (\d+) \(thr = (\d+)\), area = (\d+) \(thr = (\d+)\)"
)
# Enroll-flag REPORT logs "quality = 85, area = 99, cond = …" (no thr).
RE_QUAL_BARE = re.compile(r"quality = (\d+), area = (\d+)")
QUAL_THR_DEFAULT = 40
AREA_THR_DEFAULT = 70
RE_ENROLL_FAIL = re.compile(r"^ENROLL_FAIL\b", re.M)
RE_ENROLL_PARTIAL = re.compile(r"^ENROLL_PARTIAL\b", re.M)
RE_ENROLL_SKIP = re.compile(r"^ENROLL_SKIP\b", re.M)
RE_VERMAGIC = re.compile(r"FP_PREPARE_FAIL vermagic")


def _read(path: Path, limit: int = 400_000) -> str:
    try:
        data = path.read_bytes()
    except OSError:
        return ""
    if len(data) > limit:
        data = data[-limit:]
    return data.decode("utf-8", errors="ignore")


def _read_ktime(path: Path) -> int | None:
    try:
        raw = path.read_text().strip().split()[0]
        return int(float(raw))
    except (OSError, ValueError, IndexError):
        return None


def drop_old_ktime(text: str, kmin: int | None) -> str:
    """Drop kernel lines stamped before session/enroll start (leftover REAL DOWN)."""
    if kmin is None or kmin < 0:
        return text
    keep: list[str] = []
    for line in text.splitlines(True):
        m = RE_KTS.match(line)
        if m and int(m.group(1)) < kmin:
            continue
        keep.append(line)
    return "".join(keep)


def window_this_run(text: str, kmin: int | None = None) -> str:
    """Keep the suffix after the last qsee_fp init / ENROLL start."""
    text = drop_old_ktime(text, kmin)
    last = -1
    for m in RE_INIT.finditer(text):
        last = m.start()
    if last >= 0:
        return text[last:]
    return text


RE_AUTH_KICK_LIFT = "overlay AUTH kick — wait lift then PRESS"


def window_last_auth_kick(text: str) -> str:
    """Keep suffix after the last Match AUTH kick (helper awk window)."""
    last = text.rfind(RE_AUTH_KICK_LIFT)
    if last < 0:
        return text
    return text[last:]


def scan_text(text: str) -> dict[str, Any]:
    fids = RE_NEW_FID.findall(text)
    overlay_rems = [int(x) for x in RE_OVERLAY_REM.findall(text) if x.isdigit()]
    overlay_rem = overlay_rems[-1] if overlay_rems else None
    press_result = None
    last_pr = -1
    for kind, cre in (
        ("ok", RE_PRESS_OK),
        ("empty", RE_PRESS_EMPTY),
        ("reject", RE_PRESS_REJECT),
        ("weak", RE_PRESS_WEAK),
    ):
        for m in cre.finditer(text):
            if m.start() >= last_pr:
                last_pr = m.start()
                press_result = kind
    last_banner = -1
    for m in RE_PRESS_BANNER.finditer(text):
        last_banner = m.start()
    # Later PRESS banner means the loop is still armed; do not stick on MISS.
    if press_result in ("empty", "reject", "weak") and last_banner > last_pr:
        press_result = None
    avgvs = [int(x) for x in RE_AVGV.findall(text) if x.isdigit()]
    last_avgv = avgvs[-1] if avgvs else None
    last_q = None
    qual_pass = False
    qual_err = False
    last_q_line = None
    for line in text.splitlines():
        if RE_QUAL_ERR.search(line) or RE_QUAL_THR.search(line) or RE_QUAL_BARE.search(line):
            last_q_line = line
    if last_q_line:
        if RE_QUAL_ERR.search(last_q_line):
            qual_err = True
        else:
            m = RE_QUAL_THR.search(last_q_line)
            if m:
                q, qt, a, at = (int(x) for x in m.groups())
                last_q = q
                qual_pass = q >= qt and a >= at
            else:
                m = RE_QUAL_BARE.search(last_q_line)
                if m:
                    q, a = (int(x) for x in m.groups())
                    last_q = q
                    qual_pass = q >= QUAL_THR_DEFAULT and a >= AREA_THR_DEFAULT
    # Leftover ion QUAL-MARK after REPORT SCM -22 is not a live quality pass.
    if not re.search(r"ION HIT dev-REPORT-ev5 ret=0", text):
        qual_pass = False
    # Idle empty ~800–900. Low avgv can still be a real finger (try1 avgv=55, q=68).
    empty_sensor = last_avgv is not None and last_avgv >= 700
    if last_avgv is not None and last_avgv < 80 and not qual_pass:
        empty_sensor = True
    auth_text = window_last_auth_kick(text)
    last_hit = -1
    last_fail = -1
    for m in RE_AUTH0.finditer(auth_text):
        last_hit = m.start()
    for m in RE_OVERLAY_AUTH0.finditer(auth_text):
        if m.start() >= last_hit:
            last_hit = m.start()
    for m in RE_AUTH_FAIL.finditer(auth_text):
        last_fail = m.start()
    auth_er0 = last_hit >= 0 and last_hit > last_fail
    auth_fail = last_fail >= 0 and last_fail > last_hit
    return {
        "listen_on": bool(RE_LISTEN_ON.search(text)),
        "listen_off": bool(RE_LISTEN_OFF.search(text)),
        "ta_started": bool(RE_TA_OK.search(text)),
        "ta_resident_neg22": bool(RE_TA_NEG22.search(text)),
        "health_er0": bool(RE_HEALTH0.search(text)),
        "health_er22": bool(RE_HEALTH22.search(text)),
        "frame_raw": len(set(re.findall(r"frame raw\[(\d+)\]", text)))
        or len(RE_FRAME.findall(text)),
        "image_pixels": bool(RE_IMAGE_PIX.search(text)),
        "new_fid": fids[-1] if fids else None,
        "fid_count": len(fids),
        "rem_seen": bool(RE_REM1.search(text)),
        # leftover "enroll sample loop done rem_seen=0" is not TA rem=0
        "rem_zero": overlay_rem == 0,
        "auth_er0": auth_er0,
        "auth_fail": auth_fail,
        "auth_kick": bool(RE_AUTH_KICK.search(auth_text)),
        "broken_off": bool(RE_BROKEN.search(text)),
        "wedge": bool(RE_WEDGE.search(text)),
        "hold_now": bool(RE_HOLD_NOW.search(text)),
        "press_banner": bool(RE_PRESS_BANNER.search(text)),
        "lift_banner": bool(RE_LIFT_BANNER.search(text)),
        "real_down": bool(RE_REAL_DOWN.search(text)) and not bool(RE_NO_REAL_DOWN.search(text)),
        "prepare_ok": bool(RE_PREPARE_OK.search(text)),
        "enroll_ok_line": bool(RE_ENROLL_OK.search(text)),
        "ta_enrolling": bool(RE_TA_ENROLLING.search(text)),
        "qual_err": qual_err,
        "enroll_fail": bool(RE_ENROLL_FAIL.search(text)),
        "enroll_partial": bool(RE_ENROLL_PARTIAL.search(text)),
        "enroll_skip": bool(RE_ENROLL_SKIP.search(text)),
        "vermagic": bool(RE_VERMAGIC.search(text)),
        "frame_avgv": last_avgv,
        "empty_sensor": empty_sensor,
        "qual_pass": qual_pass,
        "frame_quality": last_q,
        "overlay_rem": overlay_rem,
        "press_result": press_result,
    }


def irq_count_from_series(text: str) -> int | None:
    hits = RE_IRQ_COUNT.findall(text)
    if not hits:
        return None
    try:
        return int(hits[-1])
    except ValueError:
        return None


def score(events: dict[str, Any], irq_delta_per_s: float = 0.0) -> dict[str, bool]:
    press_ok = bool(events.get("real_down") or events.get("press_banner"))
    tz_up = bool(
        (events.get("health_er0") and not events.get("health_er22"))
        or (events.get("ta_started") and not events.get("health_er22"))
    )
    # overlay rem=0 + SAVE is the enroll finish (new fid may be outside ktime window).
    template = bool(
        events.get("frame_raw", 0) > 0
        and events.get("overlay_rem") == 0
        and not events.get("empty_sensor")
        and not events.get("broken_off")
        and not events.get("qual_err")
    ) or bool(
        events.get("new_fid")
        and events.get("frame_raw", 0) > 0
        and events.get("ta_enrolling")
        and not events.get("empty_sensor")
        and not events.get("broken_off")
        and not events.get("qual_err")
    )
    match = bool(
        events.get("auth_er0")
        and not events.get("auth_fail")
        and not events.get("broken_off")
    )
    return {
        "press_ok": press_ok,
        "tz_up": tz_up,
        "template": template,
        "match": match,
        "fid_only": bool(events.get("new_fid") and not template),
    }


def next_enroll_mode(
    mode: str,
    phase: str,
    hero: str,
    scored: dict[str, Any],
    events: dict[str, Any],
) -> str:
    """GTK enroll-mode transition.

    fid_only/enroll_partial are true on every quality-pass FINGER SEEN
    (new fid, no rem). That is Register-again when TrustZone is up, not REBOOT.
    """
    if mode != "enroll":
        return mode
    if scored.get("template"):
        return "enrolled"
    if events.get("wedge"):
        return "need_reboot"
    # m4hm: leftover HEALTH + PRESS banner with no REAL DOWN painted REBOOT
    # and pinned GTK on need_reboot. Living TA is Register-again.
    if phase == "ready" and scored.get("tz_up"):
        return "ready"
    if hero in ("HOLD POWER", "REBOOT"):
        return "need_reboot"
    if phase == "ready" and (
        events.get("enroll_fail")
        or (events.get("health_er22") and not events.get("health_er0"))
    ):
        return "need_reboot"
    return mode


def match_should_leave_wait(
    *,
    phase: str,
    saw_match_phase: bool,
    match_t: float,
    now: float,
    score_match: bool,
    auth_fail: bool = False,
    cap_s: float = 20.0,
) -> bool:
    """Leave Match WAIT only after HIT, this-run FAIL, or timeout.

    Phase ready alone used to snap back to SAVED before MATCH_FAIL parsed.
    """
    if score_match:
        return True
    # Helper MATCH_FAIL can land while phase never ticked "match".
    if auth_fail and match_t and now - match_t > 1.0:
        return True
    if match_t and now - match_t > cap_s:
        return True
    return False


def match_hero_while_matching(hero: str) -> str:
    """Leftover SAVED / NO MATCH must stay WAIT until this Match finishes."""
    if hero in ("SAVED", "SENSOR READY", "NO MATCH"):
        return "WAIT"
    return hero or "WAIT"


def hero_plain(events: dict[str, Any], scored: dict[str, bool], phase: str = "idle") -> tuple[str, str]:
    # Register against an already-loaded extra: reboot from the phone, not SSH.
    if events.get("wedge") and (events.get("enroll_fail") or phase == "enroll"):
        return (
            "REBOOT",
            "Extra still loaded. Normal Linux reboot, then Start, one Register. Hold Power ~30s only if the screen is frozen. Never qbootctl.",
        )
    # "qsee_fingerpr busy" during health is not a reboot wedge.
    if events.get("wedge") and not scored.get("tz_up") and not events.get("prepare_ok"):
        return (
            "REBOOT",
            "Stuck. Normal Linux reboot, then Start, one Register. Hold Power ~30s only if the screen is frozen.",
        )
    if events.get("vermagic"):
        return "WRONG KERNEL", "Fingerprint kos are 7.1.2. This boot is not. Use hybrid or rebuild — do not flash."
    if events.get("enroll_skip"):
        return "NO TZ MODULE", "qsee_fingerpr.ko missing. Install the lab app again."
    if events.get("auth_fail"):
        return (
            "NO MATCH",
            "No compare that time. Wait for the LED, then press and lift. Same finger as Register.",
        )
    if scored.get("match"):
        return "MATCH", "Same finger recognized. Still not Phosh unlock."
    if scored.get("template"):
        return "SAVED", "Template stored in TrustZone. You can Match."
    rem = events.get("overlay_rem")
    pr = events.get("press_result")
    if phase == "enroll":
        if pr == "empty":
            left = rem if rem is not None else "?"
            return (
                "MISS",
                f"No finger that flash (still {left} left). Lift fully, wait for the LED, press and lift. Later flashes still count.",
            )
        if pr == "reject" or pr == "weak":
            left = rem if rem is not None else "?"
            return (
                "TRY AGAIN",
                f"Need the same finger as the first accepted press, shifted slightly (still {left} left). Other fingers do not count.",
            )
        if pr == "ok":
            if rem == 0:
                return "SAVED", "Template stored in TrustZone. You can Match."
            left = rem if rem is not None else "?"
            return (
                "ACCEPTED",
                f"Good press. {left} remaining. Shift slightly for the next LED.",
            )
        if events.get("lift_banner"):
            return "LIFT", "Lift off the power-button sensor."
        if events.get("press_banner") or events.get("real_down"):
            return (
                "HOLD FINGER",
                "Flash = ready. Press and lift at your own speed. Shift slightly each press.",
            )
        return "WAIT", "Hands off until the LED flashes. Do not touch the power-button yet."
    if rem is not None and 1 <= rem <= 19 and scored.get("tz_up"):
        if events.get("empty_sensor") or not events.get("qual_pass"):
            return (
                "NO FINGER",
                f"Stopped with {rem} remaining. Close the app.",
            )
        return (
            "ENROLLING",
            f"Sample accepted, {rem} remaining. Press and lift again — no reboot.",
        )
    if events.get("health_er22") and not events.get("health_er0"):
        return (
            "REBOOT",
            "TA is dead (HEALTH -22). Normal Linux reboot, then one Register. Do not Register again on this boot.",
        )
    if events.get("qual_pass") and not scored.get("template"):
        if scored.get("tz_up"):
            return (
                "SENSOR READY",
                "Last Register counted a finger, no template. Register again — no reboot. Hands off until the LED flashes.",
            )
        return (
            "REBOOT",
            "Last Register counted a finger, no template. TA is down. Normal Linux reboot, then one Register.",
        )
    if (
        events.get("frame_raw", 0) > 0
        and not events.get("empty_sensor")
        and not scored.get("template")
        and (events.get("real_down") or events.get("press_banner"))
    ):
        if scored.get("tz_up"):
            return (
                "SENSOR READY",
                "Last Register counted a finger, but TrustZone did not store a template (no rem). Register again — no reboot. Hands off until the LED flashes.",
            )
        return (
            "REBOOT",
            "Finger captured, no template. TA is down. Normal Linux reboot, then one Register.",
        )
    if events.get("empty_sensor"):
        return (
            "NO FINGER",
            "Capture ran with nothing on the sensor. Start over. When it flashes, hold a finger on the power button.",
        )
    if scored.get("fid_only"):
        if scored.get("tz_up"):
            return (
                "NO FRAME",
                "No finger frame that time. Register again — no reboot. Hands off until the LED flashes.",
            )
        return (
            "NO IMAGE",
            "A fid was minted with no finger frame. Reboot, then Start → Register. More taps on this boot will not save a template.",
        )
    if events.get("enroll_fail") and not events.get("wedge"):
        return (
            "REBOOT",
            "Register did not finish. TA is dead. Normal Linux reboot, then one Register.",
        )
    if events.get("enroll_partial") and not (
        events.get("press_banner") or events.get("real_down") or events.get("new_fid")
    ):
        return (
            "REBOOT",
            "Register ended before the flash. TA is dead. Normal Linux reboot, then one Register.",
        )
    if events.get("enroll_partial"):
        if scored.get("tz_up"):
            return (
                "NO FRAME",
                "No finger frame that time. Register again — no reboot. Hands off until the LED flashes.",
            )
        return (
            "REBOOT",
            "Path ran, no frame. More Register taps will not save a template. Normal Linux reboot, then one Register.",
        )
    if phase == "match":
        return "PRESS", "Same finger. Press and lift at your own speed."
    if scored.get("tz_up"):
        return "SENSOR READY", "TrustZone up. Register when you want."
    if scored.get("press_ok"):
        return "PRESS OK", "Power-button sensor sees you. Not enrolled yet."
    if events.get("prepare_ok") or events.get("listen_on"):
        return "LISTEN", "Hands off until you Register."
    if phase in ("start", "health"):
        return "STARTING…", "Loading driver. Screen may stutter — wait."
    return "READY", "Power-button sensor. Start loads the driver."


def build_status(
    session: Path,
    phase: str = "idle",
    irq_count: int | None = None,
    irq_delta_per_s: float = 0.0,
    listen: int | None = None,
) -> dict[str, Any]:
    blobs = []
    enroll_dmesg = session / "enroll-dmesg.txt"
    if enroll_dmesg.is_file() and enroll_dmesg.stat().st_size > 0:
        # Helper already windowed this insmod. Do not mix dmesg-follow boot ring.
        for name in (
            "prepare.txt",
            "enroll-dmesg.txt",
            "enroll-hits.txt",
            "enroll-run.txt",
            "enroll-progress.txt",
            "enroll-ui.txt",
            "match-ui.txt",
            "match-dmesg.txt",
        ):
            blobs.append(_read(session / name))
    else:
        for name in (
            "prepare.txt",
            "dmesg-follow.txt",
            "dmesg.log",
            "enroll-kernel.txt",
            "enroll-progress.txt",
            "enroll-hits.txt",
            "enroll-run.txt",
            "enroll-ui.txt",
            "match-ui.txt",
            "match-dmesg.txt",
            "health.txt",
            "irq-series.txt",
            "finish-line.log",
        ):
            blobs.append(_read(session / name))
    kmin = _read_ktime(session / "ktime-start.txt")
    if phase == "enroll":
        ke = _read_ktime(session / "ktime-enroll.txt")
        if ke is not None:
            kmin = ke
    # Helper enroll-dmesg is already last `qsee_fp: init`. /proc/uptime can
    # sit ~100s ahead of dmesg and drop this-run frames (false NO IMAGE).
    if enroll_dmesg.is_file() and enroll_dmesg.stat().st_size > 0:
        kmin = None
    text = window_this_run("\n".join(blobs), kmin=kmin)
    series = _read(session / "irq-series.txt", limit=32_000)
    if irq_count is None:
        irq_count = irq_count_from_series(series)
    events = scan_text(text)
    scored = score(events, irq_delta_per_s=irq_delta_per_s)
    hero, plain = hero_plain(events, scored, phase=phase)
    if listen is None:
        listen = 1 if events.get("listen_on") and not events.get("listen_off") else 0
    return {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "session": str(session),
        "phase": phase,
        "listen": listen,
        "irq_count": irq_count,
        "irq_delta_per_s": irq_delta_per_s,
        "events": events,
        "score": scored,
        "hero": hero,
        "plain": plain,
        "hint": "Does not unlock Phosh. Close unloads the driver.",
    }


def write_status(payload: dict[str, Any], rundir: Path = RUNDIR) -> None:
    rundir.mkdir(parents=True, exist_ok=True)
    raw = json.dumps(payload, indent=0) + "\n"
    tmp = rundir / (STATUS_NAME + ".tmp")
    dest = rundir / STATUS_NAME
    tmp.write_text(raw)
    tmp.replace(dest)
    session = payload.get("session")
    if session:
        try:
            Path(session, "status.json").write_text(raw)
        except OSError:
            pass
        try:
            (rundir / "logdir").write_text(str(session) + "\n")
        except OSError:
            pass


def read_status(rundir: Path = RUNDIR) -> dict[str, Any] | None:
    for p in (rundir / STATUS_NAME,):
        try:
            return json.loads(p.read_text())
        except (OSError, json.JSONDecodeError):
            continue
    return None


def _irq_sysfs() -> int | None:
    root = Path("/sys/class/focaltech_fp")
    if not root.is_dir():
        return None
    for d in root.iterdir():
        f = d / "irq_count"
        if f.is_file():
            try:
                return int(f.read_text().strip())
            except (OSError, ValueError):
                return None
    return None


def watch_loop(session: Path, phase_file: Path | None = None) -> None:
    prev = None
    prev_t = time.monotonic()
    while True:
        phase = "idle"
        if phase_file and phase_file.is_file():
            try:
                phase = phase_file.read_text().strip() or "idle"
            except OSError:
                pass
        irq = _irq_sysfs()
        now = time.monotonic()
        delta = 0.0
        if irq is not None and prev is not None and now > prev_t:
            delta = max(0.0, (irq - prev) / (now - prev_t))
        prev, prev_t = irq, now
        payload = build_status(session, phase=phase, irq_count=irq, irq_delta_per_s=round(delta, 2))
        write_status(payload)
        time.sleep(0.3)


def main(argv: list[str] | None = None) -> int:
    import argparse

    p = argparse.ArgumentParser()
    p.add_argument("--watch", metavar="SESSION")
    p.add_argument("--phase-file", default=str(RUNDIR / "phase"))
    p.add_argument("--once", metavar="SESSION")
    p.add_argument("--write-once", nargs=2, metavar=("SESSION", "PHASE"))
    args = p.parse_args(argv)
    if args.watch:
        watch_loop(Path(args.watch), Path(args.phase_file))
        return 0
    if args.write_once:
        payload = build_status(Path(args.write_once[0]), phase=args.write_once[1])
        write_status(payload)
        return 0
    if args.once:
        payload = build_status(Path(args.once))
        print(json.dumps(payload, indent=2))
        return 0
    p.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
