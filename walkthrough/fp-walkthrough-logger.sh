#!/bin/sh
# Background logger for fingerprint walkthrough sessions.
# Must exit quickly even under sudo (background jobs fully detached).
# Usage:
#   fp-walkthrough-logger.sh start SESSION_DIR
#   fp-walkthrough-logger.sh marker SESSION_DIR MARKER_NAME
#   fp-walkthrough-logger.sh snapshot SESSION_DIR LABEL
#   fp-walkthrough-logger.sh stop SESSION_DIR [keep|unload]
#   fp-walkthrough-logger.sh cleanup SESSION_DIR
set -u

cmd="${1:-}"
SESSION="${2:-}"
ARG3="${3:-}"

if [ -z "$cmd" ] || [ -z "$SESSION" ]; then
  echo "usage: $0 start|marker|snapshot|stop SESSION_DIR [arg]" >&2
  exit 2
fi

mkdir -p "$SESSION"
LOG="$SESSION/session.log"
# Prefer real sensor names (after fp-walkthrough-prepare.sh loads life module).
IRQ_FOCAL='focaltech_fp|msmgpio[[:space:]]+34'

sample_irq_sysfs() {
  for d in /sys/class/focaltech_fp/*; do
    [ -e "$d/irq_count" ] || continue
    printf 'sysfs_irq_count=%s listen=%s vdd=%s\n' \
      "$(cat "$d/irq_count" 2>/dev/null)" \
      "$(cat "$d/listen" 2>/dev/null)" \
      "$(cat "$d/vdd" 2>/dev/null)"
    return 0
  done
  echo "sysfs_irq_count=missing"
}

# Gate IRQ listen by coach phase: quiet=off, press/hold=on (cuts free-running baseline).
set_listen() {
  want="$1"
  for d in /sys/class/focaltech_fp/*; do
    [ -f "$d/listen" ] || continue
    echo "$want" >"$d/listen" 2>/dev/null || true
    return 0
  done
}

listen_for_marker() {
  name="$1"
  case "$name" in
    QUIET_*|BASELINE_*|IDLE_*)
      set_listen 0
      ;;
    PROBE_*|PRESS*|REG_*|HOLD_*)
      set_listen 1
      ;;
  esac
}

ts() { date -u +%Y-%m-%dT%H:%M:%SZ; }

# Cap dumps — full dmesg + old sessions filled the 3.3G root.
save_dmesg() {
  dest="$1"
  dmesg 2>/dev/null | tail -n 4000 >"$dest" || true
}

log() {
  printf '%s %s\n' "$(ts)" "$*" >>"$LOG" 2>/dev/null || true
  printf '%s %s\n' "$(ts)" "$*"
}

# Detach so `sudo` does not wait on long-lived children (was freezing the GUI app).
bg_detach() {
  # $1 = pidfile basename, rest = command
  _pf="$SESSION/$1"
  shift
  # shellcheck disable=SC2086
  nohup "$@" </dev/null >>"$SESSION/nohup-$1.out" 2>&1 &
  echo $! >"$_pf"
}

case "$cmd" in
start)
  log "LOGGER_START session=$SESSION"
  # Persist TA ids only from a START ret=0 in this boot's dmesg.
  # Copying an old f32 onto a new boot_id makes Register skip APP_START
  # and attach a dead id (HEALTH -22) before the real enroll insmod.
  boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo none)
  f32=$(dmesg 2>/dev/null | grep -oE 'START focal32 ret=0 .* id=0x[0-9a-fA-F]+' | tail -1 | grep -oE 'id=0x[0-9a-fA-F]+' | head -1 || true)
  f32n=""
  [ -n "$f32" ] && f32n=$(printf '%d' "${f32#id=}" 2>/dev/null || true)
  saved_boot=$(grep -E '^boot=' /var/tmp/fp5-ta-ids 2>/dev/null | tail -1 | cut -d= -f2 || true)
  if [ -n "$f32n" ]; then
    printf 'boot=%s\nf32=%s\n' "$boot" "$f32n" >/var/tmp/fp5-ta-ids 2>/dev/null || true
  elif [ -n "$saved_boot" ] && [ "$saved_boot" != "$boot" ]; then
    rm -f /var/tmp/fp5-ta-ids 2>/dev/null || true
  fi
  # Do not dmesg -C — that hid leftover START ids and older enroll evidence.
  cat /proc/interrupts >"$SESSION/irq-before.txt" 2>/dev/null || true
  {
    echo "=== uname ==="
    uname -a
    echo "=== modules ==="
    lsmod 2>/dev/null | grep -iE 'focal|fp_|qsee' || true
    echo "=== devnodes ==="
    ls -la /dev/focaltech* /dev/qsee* 2>/dev/null || true
    echo "=== uid ==="
    id
  } >"$SESSION/device-info.txt" 2>&1

  : >"$SESSION/dmesg-follow.txt"
  # Fully detach dmesg follow (sudo must not wait). Rotate if it grows.
  nohup sh -c "
    dmesg -w 2>/dev/null >>'$SESSION/dmesg-follow.txt' &
    wpid=\$!
    echo \$wpid >'$SESSION/dmesg-follow.writer.pid'
    while [ -f '$SESSION/dmesg-follow.pid' ] && kill -0 \$wpid 2>/dev/null; do
      sz=\$(wc -c <'$SESSION/dmesg-follow.txt' 2>/dev/null || echo 0)
      if [ \"\$sz\" -gt 800000 ]; then
        # rewrite same inode so dmesg -w keeps the open fd
        tail -c 400000 '$SESSION/dmesg-follow.txt' >'$SESSION/dmesg-follow.txt.tmp' 2>/dev/null \
          && cat '$SESSION/dmesg-follow.txt.tmp' >'$SESSION/dmesg-follow.txt' \
          && rm -f '$SESSION/dmesg-follow.txt.tmp'
      fi
      sleep 5
    done
    kill \$wpid 2>/dev/null || true
  " </dev/null >/dev/null 2>&1 &
  echo $! >"$SESSION/dmesg-follow.pid"

  nohup sh -c "
    while [ -f '$SESSION/dmesg-follow.pid' ]; do
      date -u +%Y-%m-%dT%H:%M:%SZ >>'$SESSION/irq-series.txt'
      grep -E '$IRQ_FOCAL' /proc/interrupts 2>/dev/null >>'$SESSION/irq-series.txt' || echo 'irq_line=none' >>'$SESSION/irq-series.txt'
      c=missing
      for d in /sys/class/focaltech_fp/*; do
        [ -e \"\$d/irq_count\" ] || continue
        c=\$(cat \"\$d/irq_count\" 2>/dev/null)
        break
      done
      echo \"sysfs_irq_count=\$c\" >>'$SESSION/irq-series.txt'
      echo '---' >>'$SESSION/irq-series.txt'
      sleep 1
    done
  " </dev/null >/dev/null 2>&1 &
  echo $! >"$SESSION/irq-series.pid"

  log "LOGGER_RUNNING dmesg_pid=$(cat "$SESSION/dmesg-follow.pid" 2>/dev/null) irq_pid=$(cat "$SESSION/irq-series.pid" 2>/dev/null)"
  echo "OK $SESSION"
  exit 0
  ;;

marker)
  name="${ARG3:-MARK}"
  log "MARKER $name"
  echo "$(ts) $name" >>"$SESSION/markers.txt"
  # Apply listen gate *before* sampling so quiet windows stay flat.
  listen_for_marker "$name"
  sleep 0.05
  {
    echo "=== $name $(ts) ==="
    grep -E "$IRQ_FOCAL" /proc/interrupts 2>/dev/null || echo "irq_line=none"
    sample_irq_sysfs
  } >>"$SESSION/irq-markers.txt"
  dmesg 2>/dev/null | tail -80 >>"$SESSION/dmesg-at-${name}.txt" 2>/dev/null || true
  # keep per-marker files from growing if the user taps Redo a lot
  if [ -f "$SESSION/dmesg-at-${name}.txt" ]; then
    tail -n 200 "$SESSION/dmesg-at-${name}.txt" >"$SESSION/dmesg-at-${name}.txt.tmp" 2>/dev/null \
      && mv "$SESSION/dmesg-at-${name}.txt.tmp" "$SESSION/dmesg-at-${name}.txt"
  fi
  echo "OK marker=$name"
  exit 0
  ;;

snapshot)
  label="${ARG3:-snap}"
  log "SNAPSHOT $label"
  cat /proc/interrupts >"$SESSION/irq-${label}.txt" 2>/dev/null || true
  {
    sample_irq_sysfs
    grep -E "$IRQ_FOCAL" /proc/interrupts 2>/dev/null || true
  } >"$SESSION/irq-count-${label}.txt" 2>/dev/null || true
  save_dmesg "$SESSION/dmesg-${label}.txt"
  echo "OK snapshot=$label"
  exit 0
  ;;

cleanup)
  # Unload lab modules (power-button path). Call from app Close only.
  log "LOGGER_CLEANUP"
  set_listen 0
  extra_in=0
  lsmod | grep -q '^qsee_fingerpr' && extra_in=1
  # Keep extra only if mark is present AND extra is loaded. Never rmmod extra.
  if [ -f /var/lib/fp5-fp/enrolled ] && [ "$extra_in" = 1 ]; then
    log "cleanup keep extra (enrolled — Match AUTH kick)"
  else
    if [ -f /var/lib/fp5-fp/enrolled ]; then
      rm -f /var/lib/fp5-fp/enrolled
      log "cleanup drop stale enrolled mark (extra not loaded)"
    fi
    if [ "$extra_in" = 1 ]; then
      log "cleanup skip rmmod qsee_fingerpr (often segfaults)"
    fi
    rmmod focaltech_fp_life 2>/dev/null && log "rmmod focaltech_fp_life OK" || log "rmmod focaltech_fp_life skipped/busy"
  fi
  echo "OK cleanup"
  exit 0
  ;;

stop)
  log "LOGGER_STOP"
  # ARG3=keep → leave life module loaded so Try register can run.
  # ARG3=unload or omitted after crash: still default keep (app Close does cleanup).
  KEEP="${ARG3:-keep}"
  if [ -f "$SESSION/dmesg-follow.pid" ]; then
    kill "$(cat "$SESSION/dmesg-follow.pid")" 2>/dev/null || true
    rm -f "$SESSION/dmesg-follow.pid"
  fi
  if [ -f "$SESSION/dmesg-follow.writer.pid" ]; then
    kill "$(cat "$SESSION/dmesg-follow.writer.pid")" 2>/dev/null || true
    rm -f "$SESSION/dmesg-follow.writer.pid"
  fi
  if [ -f "$SESSION/irq-series.pid" ]; then
    kill "$(cat "$SESSION/irq-series.pid")" 2>/dev/null || true
    rm -f "$SESSION/irq-series.pid"
  fi
  # orphan nohup children matching session path
  pkill -f "dmesg-follow.txt" 2>/dev/null || true
  cat /proc/interrupts >"$SESSION/irq-after.txt" 2>/dev/null || true
  save_dmesg "$SESSION/dmesg-full.txt"
  dmesg 2>/dev/null | grep -iE 'focal|finger|fp_|qsee|gpio' | tail -n 800 >"$SESSION/dmesg-fp.txt" || true
  # dmesg-follow grows for the whole coach; keep a tail only
  if [ -f "$SESSION/dmesg-follow.txt" ]; then
    tail -n 4000 "$SESSION/dmesg-follow.txt" >"$SESSION/dmesg-follow.txt.tmp" 2>/dev/null \
      && mv "$SESSION/dmesg-follow.txt.tmp" "$SESSION/dmesg-follow.txt"
  fi
  {
    echo "# Walkthrough session summary"
    echo "ended: $(ts)"
    echo "markers:"
    cat "$SESSION/markers.txt" 2>/dev/null || true
    echo "--- irq focal/34 before ---"
    grep -E "$IRQ_FOCAL" "$SESSION/irq-before.txt" 2>/dev/null || true
    echo "--- irq focal/34 after ---"
    grep -E "$IRQ_FOCAL" "$SESSION/irq-after.txt" 2>/dev/null || true
    echo "--- sysfs after ---"
    sample_irq_sysfs
  } | tee "$SESSION/SUMMARY.txt"
  # Score quiet vs press (matrix progress) if scorer present
  for sc in \
    "$(dirname "$0")/fp-walkthrough-score.sh" \
    /usr/local/share/fp5-fp-walkthrough/fp-walkthrough-score.sh
  do
    if [ -x "$sc" ]; then
      "$sc" "$SESSION" >>"$SESSION/score-run.txt" 2>&1 || true
      break
    fi
  done
  # TrustZone enroll is OFF by default (GPU/display hangs seen with auto-enroll).
  # Opt-in only: FP_RUN_ENROLL=1 on the logger env, or run enroll script manually.
  if [ "${FP_RUN_ENROLL:-0}" = "1" ] && [ "${FP_SKIP_ENROLL:-0}" != "1" ]; then
    for en in \
      "$(dirname "$0")/fp-walkthrough-enroll.sh" \
      /usr/local/share/fp5-fp-walkthrough/fp-walkthrough-enroll.sh
    do
      if [ -x "$en" ]; then
        log "ENROLL_BACKEND background (opt-in FP_RUN_ENROLL=1)"
        nohup "$en" "$SESSION" >>"$SESSION/enroll-run.txt" 2>&1 </dev/null &
        echo $! >"$SESSION/enroll.pid" 2>/dev/null || true
        break
      fi
    done
  else
    log "ENROLL_SKIPPED (safe default; set FP_RUN_ENROLL=1 to enable)"
  fi
  # Coach stop: leave L2 loaded so Try register can run. Close → cleanup.
  if [ "$KEEP" = "unload" ]; then
    set_listen 0
    log "LISTEN_OFF after stop (unload)"
    rmmod qsee_fingerpr 2>/dev/null && log "rmmod qsee_fingerpr OK" || log "rmmod qsee_fingerpr skipped/busy"
    if [ ! -f "$SESSION/enroll.pid" ] || ! kill -0 "$(cat "$SESSION/enroll.pid" 2>/dev/null)" 2>/dev/null; then
      rmmod focaltech_fp_life 2>/dev/null && log "rmmod focaltech_fp_life OK" || log "rmmod focaltech_fp_life skipped/busy"
    fi
  else
    set_listen 1
    log "KEEP_LIFE after coach stop (Try register ready; Close will unload)"
  fi
  log "LOGGER_DONE"
  echo "OK stopped"
  exit 0
  ;;

*)
  echo "unknown cmd $cmd" >&2
  exit 2
  ;;
esac
