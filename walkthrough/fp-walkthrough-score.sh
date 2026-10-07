#!/bin/sh
# Score a walkthrough session: quiet vs press IRQ rates + next matrix hint.
# Usage: fp-walkthrough-score.sh SESSION_DIR
set -u
SESSION="${1:-}"
if [ -z "$SESSION" ] || [ ! -d "$SESSION" ]; then
  echo "usage: $0 SESSION_DIR" >&2
  exit 2
fi

OUT="$SESSION/score.json"
MARK="$SESSION/irq-markers.txt"
[ -f "$MARK" ] || MARK="$SESSION/markers.txt"

# Parse sysfs_irq_count at named markers
count_at() {
  name="$1"
  # find block starting with === NAME
  awk -v n="$name" '
    $0 ~ "=== " n { grab=1; next }
    grab && /sysfs_irq_count=/ {
      sub(/.*sysfs_irq_count=/,""); print $1+0; exit
    }
    grab && /^=== / { exit }
  ' "$MARK" 2>/dev/null
}

# Prefer new marker names; fall back to old walkthrough names
Q0=$(count_at "QUIET_START"); [ -n "$Q0" ] || Q0=$(count_at "BASELINE_START")
Q1=$(count_at "QUIET_END");   [ -n "$Q1" ] || Q1=$(count_at "BASELINE_END")
P0=$(count_at "PROBE_START"); [ -n "$P0" ] || P0=$(count_at "PRESS5_START")
P1=$(count_at "PROBE_END");   [ -n "$P1" ] || P1=$(count_at "PRESS5_END")
R0=$(count_at "REG_SAMPLES_START"); [ -n "$R0" ] || R0=$(count_at "PRESS10_START")
R1=$(count_at "REG_SAMPLES_END");   [ -n "$R1" ] || R1=$(count_at "PRESS10_END")
A0=$(count_at "QUIET_AFTER_START"); [ -n "$A0" ] || A0=$(count_at "IDLE_AFTER_START")
A1=$(count_at "QUIET_AFTER_END");   [ -n "$A1" ] || A1=$(count_at "IDLE_AFTER_END")

ts_at() {
  name="$1"
  grep -E "^[0-9T:-]+Z $name" "$SESSION/markers.txt" 2>/dev/null | head -1 | awk '{print $1}'
}

# Wall seconds between markers via python if available
secs_between() {
  a="$1"; b="$2"
  python3 -c "
from datetime import datetime
def p(s):
  s=s.replace('Z','+00:00')
  return datetime.fromisoformat(s)
try:
  print(max(0.1, (p('$b')-p('$a')).total_seconds()))
except Exception:
  print(1.0)
" 2>/dev/null || echo 1
}

rate() {
  c0="$1"; c1="$2"; t0="$3"; t1="$4"
  if [ -z "$c0" ] || [ -z "$c1" ] || [ -z "$t0" ] || [ -z "$t1" ]; then
    echo "null"
    return
  fi
  d=$((c1 - c0))
  s=$(secs_between "$t0" "$t1")
  python3 -c "print(round($d/float('$s'), 2))" 2>/dev/null || echo "null"
}

Tq0=$(ts_at "QUIET_START"); [ -n "$Tq0" ] || Tq0=$(ts_at "BASELINE_START")
Tq1=$(ts_at "QUIET_END");   [ -n "$Tq1" ] || Tq1=$(ts_at "BASELINE_END")
Tp0=$(ts_at "PROBE_START"); [ -n "$Tp0" ] || Tp0=$(ts_at "PRESS5_START")
Tp1=$(ts_at "PROBE_END");   [ -n "$Tp1" ] || Tp1=$(ts_at "PRESS5_END")
Tr0=$(ts_at "REG_SAMPLES_START"); [ -n "$Tr0" ] || Tr0=$(ts_at "PRESS10_START")
Tr1=$(ts_at "REG_SAMPLES_END");   [ -n "$Tr1" ] || Tr1=$(ts_at "PRESS10_END")
Ta0=$(ts_at "QUIET_AFTER_START"); [ -n "$Ta0" ] || Ta0=$(ts_at "IDLE_AFTER_START")
Ta1=$(ts_at "QUIET_AFTER_END");   [ -n "$Ta1" ] || Ta1=$(ts_at "IDLE_AFTER_END")

RQ=$(rate "$Q0" "$Q1" "$Tq0" "$Tq1")
RP=$(rate "$P0" "$P1" "$Tp0" "$Tp1")
RR=$(rate "$R0" "$R1" "$Tr0" "$Tr1")
RA=$(rate "$A0" "$A1" "$Ta0" "$Ta1")

PREP_OK=0
grep -q FP_PREPARE_OK "$SESSION/prepare.txt" 2>/dev/null && PREP_OK=1

# Press quality: press rate should clearly exceed quiet (lab bar: +15% or +3/s)
python3 - <<PY
import json, math
from pathlib import Path
rq, rp, rr, ra = $(echo "$RQ" | sed 's/null/None/'), $(echo "$RP" | sed 's/null/None/'), $(echo "$RR" | sed 's/null/None/'), $(echo "$RA" | sed 's/null/None/')
prep = $PREP_OK
def ok(x): return x is not None and not (isinstance(x, float) and math.isnan(x))
press_signal = False
irq_flat = False
note = "insufficient markers"
if ok(rq) and ok(rp):
    if rq == 0 and rp == 0:
        irq_flat = True
        note = "IRQ count frozen (0/s quiet and press) — sensor not generating edges; re-arm listen/power or reload life module"
    elif rp >= rq * 1.15 + 0.5 or rp >= rq + 3:
        press_signal = True
        note = f"press rate {rp}/s > quiet {rq}/s — useful finger signal"
    elif rq >= 5 and abs(rp - rq) < max(2.0, rq * 0.1):
        note = f"press rate {rp}/s ≈ quiet {rq}/s — free-running IRQ noise; need edge filter before enroll"
    else:
        note = f"quiet={rq}/s press={rp}/s — weak discrimination"
elif prep:
    note = "driver OK but could not score rates from markers"
else:
    note = "driver prepare failed"

# Matrix progression hint
if not prep:
    stage, next_step = "M0-fail", "Fix prepare / load focaltech_fp_life"
elif irq_flat:
    stage, next_step = "M0-irq-flat", "Re-arm: listen off→on + VDD toggle or rmmod/insmod life; then re-run coach"
elif press_signal:
    stage, next_step = "M0-press-ok", "Next: careful opt-in TrustZone ENROLL (FP_RUN_ENROLL=1) with finger on samples"
else:
    stage, next_step = "M0-life-noisy", "Next: reduce free-running IRQ (debounce) so presses stand out, then ENROLL"

out = {
  "prep_ok": bool(prep),
  "irq_rate_quiet_per_s": rq,
  "irq_rate_press_probe_per_s": rp,
  "irq_rate_register_samples_per_s": rr,
  "irq_rate_quiet_after_per_s": ra,
  "press_signal": press_signal,
  "matrix_stage": stage,
  "next_step": next_step,
  "note": note,
  "counts": {"quiet": [ $([ -n "$Q0" ] && echo "$Q0" || echo null), $([ -n "$Q1" ] && echo "$Q1" || echo null) ],
             "probe": [ $([ -n "$P0" ] && echo "$P0" || echo null), $([ -n "$P1" ] && echo "$P1" || echo null) ],
             "register": [ $([ -n "$R0" ] && echo "$R0" || echo null), $([ -n "$R1" ] && echo "$R1" || echo null) ]},
}
Path("$OUT").write_text(json.dumps(out, indent=2) + "\n")
print(json.dumps(out, indent=2))
print("SCORE_OK", stage)
PY
