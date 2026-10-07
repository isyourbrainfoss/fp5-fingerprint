#!/bin/sh
# TrustZone register — root only (sudo -n).
# Interactive: wait for REAL finger-down. No enroll_force_cap (that froze the UI
# on fake idle CAPs). No dmesg -C (keeps evidence).
#
# Usage: sudo fp-walkthrough-enroll.sh SESSION_DIR
set -u

if [ "$(id -u)" -ne 0 ]; then
  echo "ENROLL_FAIL need_root (use: sudo -n $0 ...)"
  exit 2
fi

SESSION="${1:-}"
if [ -z "$SESSION" ] || [ ! -d "$SESSION" ]; then
  echo "usage: $0 SESSION_DIR" >&2
  echo "ENROLL_FAIL need_session_dir"
  rm -f /var/lib/fp5-fp/enrolled
  exit 2
fi
KVER="$(uname -r)"
KO=""
# extra/ first — walkthrough copy is often an older ko (M4dy HAL overlay).
for c in \
  /lib/modules/$KVER/extra/qsee_fingerpr.ko \
  /tmp/qsee_fingerpr.ko \
  /usr/local/share/fp5-fp-walkthrough/qsee_fingerpr.ko
do
  [ -f "$c" ] || continue
  need=$(modinfo -F vermagic "$c" 2>/dev/null | awk '{print $1}')
  [ -n "$need" ] && [ "$need" = "$KVER" ] || continue
  KO="$c"
  break
done

ENROLLED_MARK=/var/lib/fp5-fp/enrolled
# 1 only after overlay rem=0 this run (or Match on a kept extra). Else EXIT rm -f.
KEEP_ENROLLED_MARK=0

log() {
  echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*"
  if [ -n "$SESSION" ] && [ -d "$SESSION" ]; then
    echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" >>"$SESSION/enroll-run.txt" 2>/dev/null || true
  fi
}

clear_enrolled_mark_unless_kept() {
  if [ "${KEEP_ENROLLED_MARK:-0}" != 1 ]; then
    rm -f "$ENROLLED_MARK"
  fi
}
trap clear_enrolled_mark_unless_kept EXIT

if [ -z "$KO" ]; then
  log "ENROLL_SKIP no qsee_fingerpr.ko"
  echo "ENROLL_SKIP"
  exit 0
fi

if [ -n "$SESSION" ] && [ -d "$SESSION" ]; then
  chmod 755 "$SESSION" 2>/dev/null || true
fi

if [ -x /usr/local/share/fp5-fp-walkthrough/fp-walkthrough-prepare.sh ]; then
  /usr/local/share/fp5-fp-walkthrough/fp-walkthrough-prepare.sh all >/dev/null 2>&1 || true
fi
for d in /sys/class/focaltech_fp/*; do
  [ -f "$d/listen" ] && echo 1 >"$d/listen" 2>/dev/null
  [ -f "$d/vdd" ] && echo 1 >"$d/vdd" 2>/dev/null
  [ -f "$d/spiclk" ] && echo 1 >"$d/spiclk" 2>/dev/null
done

# Extra already loaded (lab idle or stuck init). rmmod often segfaults —
# do not unload. App shows REBOOT; user normal Linux reboot, then Start → Register.
if [ "${DO_AUTH:-0}" = 1 ]; then
  if ! lsmod | grep -q '^qsee_fingerpr'; then
    log "MATCH_FAIL no kept enroll sess — extra not loaded"
    echo "MATCH_FAIL no_kept_sess"
    exit 1
  fi
  # Extra still in: previous rem=0 template is live. Do not drop the mark.
  KEEP_ENROLLED_MARK=1
  if [ ! -w /sys/module/qsee_fingerpr/parameters/auth_kick ]; then
    log "MATCH_FAIL no auth_kick sysfs"
    echo "MATCH_FAIL no_auth_kick"
    exit 1
  fi
  MATCH_KICK=1
elif lsmod | grep -q '^qsee_fingerpr'; then
  log "ENROLL_FAIL WEDGED_REBOOT (qsee_fingerpr already loaded — do not rmmod)"
  echo "ENROLL_FAIL WEDGED_REBOOT"
  exit 1
fi

# Up to 25 attempts: empty skips REPORT. Stop at rem=0. Shift finger each press.
SAMPLES="${ENROLL_SAMPLES:-60}"
WAIT="${SAMPLE_WAIT_S:-8}"
IDS_FILE="${FP5_TA_IDS:-/var/tmp/fp5-ta-ids}"

# FP6: templates live on Android persist (SFS). Mount before INIT/SET_GROUP.
mount_persist() {
  mkdir -p /mnt/persist /mnt/persist/qti_fp 2>/dev/null || true
  if [ -e /dev/disk/by-partlabel/persist ]; then
    if mountpoint -q /mnt/persist; then
      mount -o remount,rw,nosuid,nodev,noexec /mnt/persist 2>/dev/null || true
    else
      mount -o rw,nosuid,nodev,noexec /dev/disk/by-partlabel/persist /mnt/persist \
        2>/dev/null || true
    fi
    mkdir -p /mnt/persist/qti_fp /mnt/persist/persist-data /mnt/persist/tzstorage \
      /mnt/persist/misc /mnt/persist/root3 2>/dev/null || true
    log "persist $(mountpoint -q /mnt/persist && echo mounted || echo not_mounted)"
  else
    log "persist partition missing"
  fi
}
mount_persist

# App Register: one finger on the power-button sensor. Finger input is
# in the Phosh app, not SSH. enroll_force_cap=0 — no idle CAP / REPORT ev=7.
# skip_capture_path=1 skips FDT 112/96/80 (GPU hang). cap_hal_overlay=1.
# Do not pass skip_init_dev / shutdown_before_start (M4du ESD_HANDLE -22).
# skip_init_dev=1 on leftover attach: IRQ storm, itype 0x10, hung HOLD.
if [ "${DO_AUTH:-0}" = 1 ]; then
  ENROLL_OPTS="enable_fs_listener=1 enable_rpmb_listener=1 skip_enroll=1 do_auth=1 \
    enroll_needs_finger=0 wait_finger_s=0 enroll_samples=3 \
    sample_wait_s=$WAIT enroll_force_cap=0 quiet_send=1 spi_bus_num=14 \
    spi_capture_bps=8030000 \
    irq_debounce_ms=250 irq_means_down=0 skip_capture_path=1 defer_enroll=0 \
    early_cap_stages=0 heap_probe=0 idle_layout1_cap=0 cap_hal_overlay=1 \
    slim_post_base=0"
else
  ENROLL_OPTS="enable_fs_listener=1 enable_rpmb_listener=1 enable_gpfile_listener=1 defer_listeners=0 skip_enroll=0 \
    enroll_needs_finger=0 wait_finger_s=0 enroll_samples=$SAMPLES \
    sample_wait_s=$WAIT enroll_force_cap=0 quiet_send=1 spi_bus_num=14 \
    spi_capture_bps=8030000 \
    irq_debounce_ms=80 irq_means_down=0 skip_capture_path=1 defer_enroll=0 \
    early_cap_stages=0 heap_probe=0 idle_layout1_cap=0 cap_hal_overlay=1 \
    slim_post_base=0"
fi

dmesg_has() {
  dmesg 2>/dev/null | grep -qE "$1"
}

dmesg_tail_has() {
  dmesg 2>/dev/null | tail -n 120 | grep -qE "$1"
}

boot_id() {
  cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo none
}

save_ids_from_dmesg() {
  f32=$(dmesg 2>/dev/null | grep -oE 'START focal32 ret=0 .* id=0x[0-9a-fA-F]+' | tail -1 | grep -oE 'id=0x[0-9a-fA-F]+' | head -1)
  fp=$(dmesg 2>/dev/null | grep -oE 'START fingerpr ret=0 .* id=0x[0-9a-fA-F]+' | tail -1 | grep -oE 'id=0x[0-9a-fA-F]+' | head -1)
  f32n=""
  fpn=""
  [ -n "$f32" ] && f32n=$(printf '%d' "${f32#id=}" 2>/dev/null || true)
  [ -n "$fp" ] && fpn=$(printf '%d' "${fp#id=}" 2>/dev/null || true)
  if [ -n "$f32n" ]; then
    save_f32 "$f32n" "${fpn:-}"
    log "saved TA ids boot=$(boot_id) f32=$f32n fp=${fpn:-?} -> $IDS_FILE"
  fi
}

save_f32() {
  n="$1"
  fp="${2:-}"
  [ -n "$n" ] || return 1
  printf 'boot=%s\nf32=%s\nfp=%s\n' "$(boot_id)" "$n" "$fp" >"$IDS_FILE" 2>/dev/null || true
}

read_saved_f32() {
  [ -f "$IDS_FILE" ] || return 1
  saved_boot=$(grep -E '^boot=' "$IDS_FILE" 2>/dev/null | tail -1 | cut -d= -f2)
  cur=$(boot_id)
  # Stale file from a previous boot → TAs are not loaded; must APP_START.
  if [ -z "$saved_boot" ] || [ "$saved_boot" != "$cur" ]; then
    return 1
  fi
  # START ret=0 rotates out of dmesg; boot_id match is enough. Wrong id
  # is caught by ta_looks_dead (HEALTH -22) and falls back to APP_START.
  grep -E '^f32=' "$IDS_FILE" 2>/dev/null | tail -1 | cut -d= -f2
}

ta_looks_dead() {
  # Last HEALTH after this insmod: er=-22 means wrong id / TA not loaded.
  dmesg 2>/dev/null | grep 'HEALTH final er=' | tail -1 | grep -q 'er=-22'
}

unload_qsee() {
  rmmod qsee_fingerpr 2>/dev/null || true
}

progress() {
  [ -n "$SESSION" ] && [ -d "$SESSION" ] || return 0
  printf '%s\n' "$*" >>"$SESSION/enroll-progress.txt" 2>/dev/null || true
  printf '%s\n' "$*"
}

start_cues() {
  [ -n "$SESSION" ] && [ -d "$SESSION" ] || return 0
  for c in \
    "$(dirname "$0")/fp-walkthrough-cue.sh" \
    /usr/local/share/fp5-fp-walkthrough/fp-walkthrough-cue.sh
  do
    if [ -x "$c" ]; then
      nohup "$c" "$SESSION" </dev/null >>"$SESSION/cue-nohup.out" 2>&1 &
      log "CUE helper pid=$!"
      return 0
    fi
  done
  log "CUE helper missing"
}

stop_cues() {
  rm -f "$SESSION/cue.pid" 2>/dev/null || true
}

start_progress_watch() {
  [ -n "$SESSION" ] && [ -d "$SESSION" ] || return 0
  : >"$SESSION/enroll-progress.txt"
  : >"$SESSION/enroll-kernel.txt"
  printf '%s\n' "$$" >"$SESSION/enroll-watch.pid"
  start_cues
  nohup sh -c "
    while [ -f '$SESSION/enroll-watch.pid' ]; do
      dmesg 2>/dev/null | grep -E 'PRESS then LIFT finger sample|LIFT finger —|REAL DOWN|HOLD FINGER NOW|START focal32|HEALTH final|waiting up to|fingerwait|capture path done|skip_enroll' | tail -n 16 \
        >'$SESSION/enroll-kernel.txt' 2>/dev/null || true
      sleep 0.25
    done
  " </dev/null >/dev/null 2>&1 &
}

stop_progress_watch() {
  rm -f "$SESSION/enroll-watch.pid" 2>/dev/null || true
  stop_cues
}

auth_kick_window() {
  dmesg 2>/dev/null | awk '
    /overlay AUTH kick — wait lift then PRESS/ { buf = $0 ORS; next }
    { if (buf != "") buf = buf $0 ORS }
    END { printf "%s", buf }
  ' >"$SESSION/match-dmesg.txt" 2>/dev/null || true
}

if [ "${MATCH_KICK:-0}" = 1 ]; then
  log "Match AUTH kick on kept enroll sess (no insmod, no enroll score)"
  start_progress_watch
  n0=$(dmesg 2>/dev/null | grep -c 'overlay AUTH kick — wait lift then PRESS' || true)
  echo 1 >/sys/module/qsee_fingerpr/parameters/auth_kick 2>/dev/null || true
  i=0
  # Ignore leftover AUTH FAIL from a prior Match. Wait for THIS kick.
  while [ "$i" -lt 120 ]; do
    n1=$(dmesg 2>/dev/null | grep -c 'overlay AUTH kick — wait lift then PRESS' || true)
    if [ "${n1:-0}" -gt "${n0:-0}" ]; then
      auth_kick_window
      if grep -qE 'overlay AUTH HIT|overlay AUTH FAIL|overlay AUTH kick no itype' \
        "$SESSION/match-dmesg.txt" 2>/dev/null; then
        break
      fi
    fi
    sleep 0.5
    i=$((i + 1))
  done
  stop_progress_watch
  auth_kick_window
  if grep -q 'overlay AUTH HIT' "$SESSION/match-dmesg.txt" 2>/dev/null; then
    log "MATCH_OK AUTH HIT"
    echo "MATCH_OK"
    exit 0
  fi
  log "MATCH_FAIL AUTH did not HIT"
  echo "MATCH_FAIL"
  exit 1
fi

last_start_line() {
  dmesg 2>/dev/null | grep 'START focal32 ret=' | tail -1
}

load_enroll() {
  extra="$*"
  if [ "${DO_AUTH:-0}" = 1 ] && lsmod | grep -q '^qsee_fingerpr'; then
    if [ -w /sys/module/qsee_fingerpr/parameters/auth_kick ]; then
      log "Match AUTH kick on kept enroll sess (no rmmod)"
      n0=$(dmesg 2>/dev/null | grep -c 'overlay AUTH kick — wait lift then PRESS' || true)
      echo 1 >/sys/module/qsee_fingerpr/parameters/auth_kick 2>/dev/null || true
      i=0
      while [ "$i" -lt 120 ]; do
        n1=$(dmesg 2>/dev/null | grep -c 'overlay AUTH kick — wait lift then PRESS' || true)
        if [ "${n1:-0}" -gt "${n0:-0}" ]; then
          auth_kick_window
          if grep -qE 'overlay AUTH HIT|overlay AUTH FAIL|overlay AUTH kick no itype' \
            "$SESSION/match-dmesg.txt" 2>/dev/null; then
            break
          fi
        fi
        sleep 0.5
        i=$((i + 1))
      done
      return 0
    fi
  fi
  if [ "${DO_AUTH:-0}" = 1 ]; then
    log "MATCH_FAIL no auth_kick sysfs — Register this boot first"
    echo "MATCH_FAIL no_auth_kick"
    return 1
  fi
  unload_qsee
  # 6 samples + lift waits + init often exceed 75s (that killed us at 5/6).
  # shellcheck disable=SC2086
  # One REAL-DOWN CAP can take ~90s; 6 samples + init need well over 140s.
  INSMOD_ERR=$(timeout 400 insmod "$KO" $ENROLL_OPTS $extra 2>&1) || true
  if [ -n "$INSMOD_ERR" ]; then
    log "insmod: $INSMOD_ERR"
    echo "insmod: $INSMOD_ERR"
  fi
  lsmod | grep -q '^qsee_fingerpr'
}

# Fast attach probe (no finger wait). HEALTH er=0 means this app id is live.
probe_attach() {
  f32="$1"
  fp="${2:-}"
  extra="do_start=0 force_f32_id=$f32 skip_enroll=1 enroll_needs_finger=0 enroll_samples=0"
  [ -n "$fp" ] && extra="$extra force_fp_id=$fp"
  unload_qsee
  log "probe attach f32=$f32 fp=${fp:-none}"
  # shellcheck disable=SC2086
  timeout 12 insmod "$KO" enable_fs_listener=1 enable_rpmb_listener=1 quiet_send=1 \
    spi_bus_num=14 $extra >/dev/null 2>&1 || true
  ok=1
  if dmesg_tail_has 'HEALTH final er=0'; then
    log "probe HEALTH er=0 f32=$f32"
    ok=0
  elif dmesg_tail_has 'no TA ids'; then
    log "probe no TA ids f32=$f32"
  elif dmesg_tail_has 'HEALTH final er='; then
    log "probe HEALTH nonzero f32=$f32"
  else
    log "probe no HEALTH line f32=$f32"
  fi
  unload_qsee
  return $ok
}

attach_existing() {
  # START -22 = TAs already resident this boot. Attach by numeric id.
  # Prefer last successful START (id=0x4 is the usual focal32 on this unit).
  cand=""
  saved=$(read_saved_f32 || true)
  [ -n "$saved" ] && cand="$cand $saved"
  # Usual this unit: focal32 id 4; later STARTs have been 16 and 19.
  cand="$cand 19 16 6 4 5 3"
  seen=" "
  for id in $cand; do
    case "$seen" in
      *" $id "*) continue ;;
    esac
    seen="$seen$id "
    if probe_attach "$id"; then
      # stdout is mixed with log(); caller must not capture this function.
      ATTACH_ID="$id"
      return 0
    fi
  done
  ATTACH_ID=""
  return 1
}

ensure_qseecom() {
  if [ -d /sys/devices/platform/qcom_qseecom ]; then
    return 0
  fi
  for p in \
    /lib/modules/$KVER/extra/qsee_probe_fp5.ko \
    /tmp/qsee_probe_fp5.ko \
    /usr/local/share/fp5-fp-walkthrough/qsee_probe_fp5.ko
  do
    [ -f "$p" ] || continue
    insmod "$p" force_pdev=1 2>/dev/null || true
    break
  done
  modprobe qcom_qseecom 2>/dev/null || true
}

ensure_qseecom
log "ENROLL_START ko=$KO samples=$SAMPLES wait=${WAIT}s force_cap=0 hold-on-sensor"
# Drop leftover enroll-dmesg / REAL DOWN from a previous Register this session.
# Stamp from dmesg, not /proc/uptime (idle uptime can lag ~100s ahead).
dmesg 2>/dev/null | sed -n 's/^\[\s*\([0-9][0-9]*\)\..*/\1/p' | tail -1 \
	>"$SESSION/ktime-enroll.txt" 2>/dev/null || true
if [ ! -s "$SESSION/ktime-enroll.txt" ]; then
	cut -d. -f1 /proc/uptime >"$SESSION/ktime-enroll.txt" 2>/dev/null || true
fi
: >"$SESSION/enroll-dmesg.txt"
: >"$SESSION/enroll-hits.txt"
progress "PHASE starting — wait for PRESS cue"
start_progress_watch

# m4fi REPORT er=0 was a warm attach (TA already STARTed this boot).
# Cold START + enroll in one insmod has been REPORT -22. INIT skip_enroll
# without overlay idle CAP, unload extra, then attach for enroll.
if [ -z "$(read_saved_f32 || true)" ]; then
  log "warm INIT skip_enroll (no overlay idle CAP) then attach"
  progress "PHASE warming"
  timeout 90 insmod "$KO" do_start=1 skip_enroll=1 skip_capture_path=1 \
    cap_hal_overlay=0 overlay_idle_scan=0 overlay_idle_writereg=0 \
    early_cap_stages=0 idle_layout1_cap=0 heap_probe=0 enroll_samples=0 \
    quiet_send=1 spi_bus_num=14 spi_capture_bps=8030000 \
    enable_fs_listener=1 enable_rpmb_listener=1 >/dev/null 2>&1 || true
  save_ids_from_dmesg
  unload_qsee
  if lsmod | grep -q '^qsee_fingerpr'; then
    log "ENROLL_FAIL WEDGED_REBOOT (warm INIT extra stuck — do not rmmod)"
    echo "ENROLL_FAIL WEDGED_REBOOT"
    stop_progress_watch
    exit 1
  fi
fi

# If we already know the TA id this boot, skip do_start=1 (always -22 and
# costs a second GPU-stressing init). One enroll insmod only.
ATTACHED=0
NEED_LIVE=0
SKIP_CASE=0
saved_id=$(read_saved_f32 || true)
if [ -n "$saved_id" ] && [ "$saved_id" -eq "$saved_id" ] 2>/dev/null; then
  log "skip cold START — using saved f32=$saved_id (this boot)"
  progress "PHASE attaching"
  if load_enroll do_start=0 force_f32_id="$saved_id"; then
    if ta_looks_dead; then
      log "saved f32=$saved_id HEALTH -22 — TA not live, falling back to APP_START"
      unload_qsee
    else
      ATTACHED=1
      NEED_LIVE=1
      SKIP_CASE=1
      log "saved-id enroll insmod returned"
    fi
  fi
fi
if [ "$SKIP_CASE" != 1 ] && [ "$ATTACHED" != 1 ]; then
  if ! load_enroll do_start=1; then
    log "insmod did not stay loaded (timeout or error) — will score"
  fi
fi

if [ "$SKIP_CASE" = 1 ]; then
  STARTL="saved-id"
else
STARTL=$(last_start_line)
case "$STARTL" in
  *ret=0*)
    save_ids_from_dmesg
    ATTACHED=1
    log "cold START focal32 ok ($STARTL)"
    ;;
  *ret=-22*)
    log "START -22 — TAs already in TZ, attaching (no reboot)"
    progress "PHASE attaching"
    unload_qsee
    # Skip the extra 70s HEALTH probe (that plus enroll hung the GPU/display).
    # Use last known id, then 4/6/5 — one enroll insmod only.
    aid=$(read_saved_f32 || true)
    [ -z "$aid" ] && aid=4
    log "reattach f32=$aid — loading enroll (no probe)"
    progress "PHASE waiting f32=$aid — PRESS when cued"
    if load_enroll do_start=0 force_f32_id="$aid"; then
      ATTACHED=1
      NEED_LIVE=1
    else
      ATTACH_ID=""
      attach_existing || true
      aid="$ATTACH_ID"
      if [ -n "$aid" ] && [ "$aid" -eq "$aid" ] 2>/dev/null; then
        save_f32 "$aid"
        log "probe found f32=$aid — loading enroll"
        if load_enroll do_start=0 force_f32_id="$aid"; then
          ATTACHED=1
          NEED_LIVE=1
        fi
      fi
    fi
    ;;
  *)
    log "no START line yet — scoring whatever ran"
    ;;
esac
fi

# Successful cold START already ran the sample loop inside the first insmod.
# Do not treat a finished/timeout module as ATTACH failure (that showed REBOOT
# / skip-to-4/6 last time).
if [ "$ATTACHED" != 1 ] && [ "$NEED_LIVE" = 1 ]; then
  stop_progress_watch
  log "ENROLL_FAIL attach failed (wrong id or TA dead)"
  echo "ENROLL_FAIL ATTACH"
  unload_qsee
  exit 1
fi

if [ "$ATTACHED" != 1 ] && ! dmesg 2>/dev/null | grep -q 'START focal32 ret=0'; then
  if dmesg 2>/dev/null | grep -q 'START focal32 ret=-22'; then
    stop_progress_watch
    log "ENROLL_FAIL attach failed (wrong id or TA dead)"
    echo "ENROLL_FAIL ATTACH"
    unload_qsee
    exit 1
  fi
fi

log "insmod returned — scoring dmesg (press/lift should already have happened)"
progress "PHASE scoring"
stop_progress_watch

# If insmod returned quickly, wait remaining sample window (timeout already capped)
sleep 2

if [ -n "$SESSION" ] && [ -d "$SESSION" ]; then
  # This insmod only (last qsee_fp: init). Full-boot tail mixed leftover fids/quality.
  dmesg 2>/dev/null | awk '
    /qsee_fp: init$/ { n=0 }
    { lines[++n] = $0 }
    END { for (i = 1; i <= n; i++) print lines[i] }
  ' >"$SESSION/enroll-dmesg.txt" || true
  grep -iE 'new fid|frame raw|rem_seen|enrolling|ENROLL|IMAGE PIXELS|broken|AUTH|fdt_ok|vio 1.8|HEALTH|START|APP_START|-22|no TA|no REAL|irq_means|sn num|finger|quality|enroll-flag|REPORT|SAVE-data|persist WRITE|req_resp=0x3|SET_GROUP persist|itype' \
    "$SESSION/enroll-dmesg.txt" >"$SESSION/enroll-hits.txt" 2>/dev/null || true
  cat "$SESSION/enroll-hits.txt" 2>/dev/null || true
fi

HITS="${SESSION}/enroll-dmesg.txt"
[ -f "$HITS" ] || HITS="/dev/null"
FID=$(grep 'dev-ENROLL-start' "$HITS" 2>/dev/null | grep -oE 'new fid \(= [0-9]+\)' | tail -1 || true)
[ -n "$FID" ] || FID=$(grep -oE 'new fid \(= [0-9]+\)' "$HITS" 2>/dev/null | tail -1 || true)
AVG=$(sed -n 's/.*frame raw\[0\]: avgv = \([0-9][0-9]*\).*/\1/p' "$HITS" 2>/dev/null | tail -1 || true)
REM=$(grep -c 'enroll sample loop done rem_seen=1' "$HITS" 2>/dev/null || true)
TAREM=$(grep -c 'enrolled fid' "$HITS" 2>/dev/null || true)
LASTQ=$(grep -E 'error, ret = -3|quality = [0-9]+ \(thr' "$HITS" 2>/dev/null | tail -1 || true)
QUALERR=0
case "$LASTQ" in
  *error*) QUALERR=1 ;;
esac
DOWN=$(grep -c 'no REAL DOWN' "$HITS" 2>/dev/null || true)
BROKEN=$(grep -c 'broken off' "$HITS" 2>/dev/null || true)

# Mark only overlay rem=0. ENROLL_PARTIAL / ENROLL_FAIL / non-rem=0: trap rm -f.
if grep -qE 'overlay PRESS OK rem=0|overlay rem=0 template' "$HITS" 2>/dev/null; then
  mkdir -p /var/lib/fp5-fp
  : >"$ENROLLED_MARK"
  KEEP_ENROLLED_MARK=1
  log "keep extra loaded for Match AUTH kick"
  log "ENROLL_OK overlay rem=0 avgv=${AVG:-none}"
  echo "ENROLL_OK overlay rem=0 avgv=${AVG:-none}"
  exit 0
fi
unload_qsee

# Empty-sensor frames are avgv ~800–900. A finger on Android is ~280–510.
# rem_seen=1 from frame raw is not TA rem--. Need enrolled fid / rem = N.
# "enrolling..." + quality error is a rejected empty/dark frame, not a sample.
if [ -n "$FID" ] && [ -n "$AVG" ] && [ "$AVG" -ge 80 ] && [ "$AVG" -lt 700 ] &&
   [ "${TAREM:-0}" -gt 0 ] && [ "${QUALERR:-0}" -eq 0 ] && [ "${BROKEN:-0}" -eq 0 ]; then
  log "ENROLL_OK $FID avgv=$AVG rem=$REM tarem=$TAREM (no overlay rem=0 — no mark)"
  echo "ENROLL_OK $FID avgv=$AVG"
  exit 0
fi
if [ -n "$AVG" ] && [ "$AVG" -ge 700 ]; then
  log "ENROLL_PARTIAL $FID empty_sensor avgv=$AVG (session stopped — do not Register again on this extra)"
  echo "ENROLL_PARTIAL empty_sensor avgv=$AVG"
  exit 1
fi
if [ -n "$FID" ]; then
  log "ENROLL_PARTIAL $FID avgv=${AVG:-none} rem=$REM (no TA rem)"
  echo "ENROLL_PARTIAL $FID avgv=${AVG:-none} rem=$REM"
  exit 1
fi
log "ENROLL_PARTIAL avgv=${AVG:-none} rem=$REM no_real_down=$DOWN"
echo "ENROLL_PARTIAL avgv=${AVG:-none} rem=$REM"
exit 1
