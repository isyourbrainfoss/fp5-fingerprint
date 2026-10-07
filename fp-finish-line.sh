#!/bin/sh
# FP5 fingerprint finish-line orchestrator (postmarketOS).
#
# Stages:
#   A  L2 life + press_signal (always safe)
#   B  QSEE TA presence + cold APP_START smoke (if modules present)
#   C  Enroll with finger press/lift (needs you at the power-button)
#   D  AUTH attempt after enroll (same finger)
#
# Usage (root on phone):
#   sudo fp-finish-line.sh prepare     # A only
#   sudo fp-finish-line.sh health      # A + B
#   sudo fp-finish-line.sh enroll      # A + C (interactive cues)
#   sudo fp-finish-line.sh full        # A + B + C + D
#   sudo fp-finish-line.sh cleanup
#
# Safety: never qbootctl; cleanup unloads modules; enroll is explicit.
set -u

KVER=$(uname -r)
OUT="${OUTDIR:-/var/tmp/fp-finish-line-$(date -u +%Y%m%dT%H%M%SZ)}"
SAMPLES="${SAMPLES:-8}"
SAMPLE_WAIT="${SAMPLE_WAIT:-5}"

LIFE=""
for c in \
  /usr/local/share/fp5-fp-walkthrough/focaltech_fp_life.ko \
  /lib/modules/$KVER/extra/focaltech_fp_life.ko \
  /tmp/focaltech_fp_life.ko
do
  [ -f "$c" ] && LIFE="$c" && break
done

QSEE=""
for c in \
  /usr/local/share/fp5-fp-walkthrough/qsee_fingerpr.ko \
  /lib/modules/$KVER/extra/qsee_fingerpr.ko \
  /tmp/qsee_fingerpr.ko
do
  [ -f "$c" ] && QSEE="$c" && break
done

PROBE=""
for c in \
  /usr/local/share/fp5-fp-walkthrough/qsee_probe_fp5.ko \
  /lib/modules/$KVER/extra/qsee_probe_fp5.ko \
  /tmp/qsee_probe_fp5.ko \
  artifacts/qsee-modules/qsee_probe_fp5.ko
do
  [ -f "$c" ] && PROBE="$c" && break
done

log() {
  echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*"
  mkdir -p "$OUT" 2>/dev/null || true
  echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" >>"$OUT/finish-line.log" 2>/dev/null || true
}

need_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "FAIL: need root (sudo -n $0 ...)"
    exit 2
  fi
}

cleanup() {
  log "cleanup: listen off + unload"
  for d in /sys/class/focaltech_fp/*; do
    [ -f "$d/listen" ] && echo 0 >"$d/listen" 2>/dev/null || true
  done
  rmmod qsee_fingerpr 2>/dev/null || true
  rmmod qsee_ta_load 2>/dev/null || true
  rmmod qsee_focal_ok 2>/dev/null || true
  # leave probe/qseecom if system needs them; life unload restores KEY_POWER path
  rmmod focaltech_fp_life 2>/dev/null || true
  log "cleanup done"
}

prepare_l2() {
  log "=== A: L2 prepare ==="
  if [ -z "$LIFE" ]; then
    log "FAIL: no focaltech_fp_life.ko"
    return 1
  fi
  if [ -x /usr/local/share/fp5-fp-walkthrough/fp-walkthrough-prepare.sh ]; then
    /usr/local/share/fp5-fp-walkthrough/fp-walkthrough-prepare.sh all 2>&1 | tee -a "$OUT/prepare.txt"
  else
    insmod "$LIFE" 2>/dev/null || true
    for d in /sys/class/focaltech_fp/*; do
      [ -f "$d/vdd" ] && echo 1 >"$d/vdd"
      [ -f "$d/listen" ] && echo 1 >"$d/listen"
      [ -f "$d/spiclk" ] && echo 1 >"$d/spiclk"
    done
  fi
  if [ ! -e /dev/focaltech_fp ]; then
    log "FAIL: /dev/focaltech_fp missing"
    return 1
  fi
  grep -E 'focaltech_fp|msmgpio.*34' /proc/interrupts | tee "$OUT/irq-line.txt" || true
  for d in /sys/class/focaltech_fp/*; do
    echo "listen=$(cat $d/listen 2>/dev/null) irq_count=$(cat $d/irq_count 2>/dev/null) vdd=$(cat $d/vdd 2>/dev/null)" \
      | tee -a "$OUT/sysfs.txt"
  done
  log "L2 OK"
  return 0
}

ensure_tas() {
  log "=== B: TA firmware ==="
  mkdir -p /lib/firmware/qsee
  if [ ! -f /lib/firmware/qsee/fingerpr.mdt ]; then
    if [ -b /dev/disk/by-partlabel/modem_a ]; then
      mkdir -p /mnt/modem_a
      mountpoint -q /mnt/modem_a 2>/dev/null || mount -o ro /dev/disk/by-partlabel/modem_a /mnt/modem_a
      cp -a /mnt/modem_a/image/fingerpr* /mnt/modem_a/image/focal32* /lib/firmware/qsee/ 2>/dev/null || true
    fi
  fi
  ls -la /lib/firmware/qsee/fingerpr.mdt /lib/firmware/qsee/focal32.mdt 2>&1 | tee "$OUT/ta-files.txt"
  if [ ! -f /lib/firmware/qsee/fingerpr.mdt ]; then
    log "WARN: fingerpr.mdt missing — enroll will fail"
    return 1
  fi
  # force qseecom pdev if needed
  if [ ! -d /sys/devices/platform/qcom_qseecom ] && [ -n "$PROBE" ]; then
    insmod "$PROBE" force_pdev=1 2>/dev/null || true
    modprobe qcom_qseecom 2>/dev/null || true
  fi
  log "TA files present"
  return 0
}

health_qsee() {
  log "=== B: QSEE module present ==="
  if [ -z "$QSEE" ]; then
    log "FAIL: no qsee_fingerpr.ko"
    return 1
  fi
  log "qsee_fingerpr.ko=$QSEE"
  # Soft smoke: do not full enroll; just confirm insmod with skip_enroll
  prepare_l2 || return 1
  ensure_tas || true
  rmmod qsee_fingerpr 2>/dev/null || true
  dmesg -C 2>/dev/null || true
  log "insmod smoke skip_enroll=1 do_start=1 …"
  if ! insmod "$QSEE" do_start=1 enable_fs_listener=1 enable_rpmb_listener=1 \
    skip_enroll=1 quiet_send=1 spi_bus_num=14 2>"$OUT/insmod-smoke.err"
  then
    log "insmod smoke FAILED (see insmod-smoke.err) — may need reboot if TA START -22"
    cat "$OUT/insmod-smoke.err" 2>/dev/null || true
    dmesg | grep -iE 'qsee|fingerpr|focal|APP_START|-22' | tail -40 | tee "$OUT/dmesg-smoke.txt"
    rmmod qsee_fingerpr 2>/dev/null || true
    return 1
  fi
  sleep 3
  dmesg | grep -iE 'qsee|fingerpr|focal|APP_START|SN|sn num|HEALTH|START' | tail -50 | tee "$OUT/dmesg-smoke.txt"
  rmmod qsee_fingerpr 2>/dev/null || true
  if grep -qiE 'APP_START.*success|sn num|HEALTH.*er=0|fdt_ok' "$OUT/dmesg-smoke.txt" 2>/dev/null; then
    log "HEALTH smoke: interesting markers found"
  else
    log "HEALTH smoke: loaded; check dmesg-smoke.txt for START -22 / SN"
  fi
  return 0
}

enroll_finger() {
  log "=== C: ENROLL with finger (press/lift) ==="
  prepare_l2 || return 1
  ensure_tas || true
  if [ -z "$QSEE" ]; then
    log "FAIL: no qsee_fingerpr.ko"
    return 1
  fi
  rmmod qsee_fingerpr 2>/dev/null || true
  dmesg -C 2>/dev/null || true

  cat <<EOF

============================================================
  FINGERPRINT ENROLL — power-button sensor
  ${SAMPLES} samples · press ~1s then LIFT fully each time
  Wall clock ~$((SAMPLES * (SAMPLE_WAIT + 2) + 20))s
  Like Android: short press, lift, repeat — not one long hold
============================================================

EOF

  # Proven lab params (run-enroll-hold-finger + walkthrough)
  if ! insmod "$QSEE" do_start=1 enable_fs_listener=1 enable_rpmb_listener=1 \
    skip_enroll=0 enroll_needs_finger=1 wait_finger_s=5 \
    enroll_samples="$SAMPLES" sample_wait_s="$SAMPLE_WAIT" \
    enroll_force_cap=1 quiet_send=1 spi_bus_num=14 \
    irq_debounce_ms=250 2>"$OUT/insmod-enroll.err"
  then
    log "ENROLL insmod failed"
    cat "$OUT/insmod-enroll.err"
    dmesg | tail -60 | tee "$OUT/dmesg-enroll-fail.txt"
    return 1
  fi

  # Live progress for ~SAMPLES windows
  i=1
  while [ "$i" -le "$SAMPLES" ]; do
    echo ">>> SAMPLE $i/$SAMPLES — PRESS power-button FP now (~1s), then LIFT"
    sleep "$SAMPLE_WAIT"
    i=$((i + 1))
  done
  sleep 8

  dmesg >"$OUT/dmesg-enroll.txt" 2>/dev/null || true
  grep -iE 'new fid|frame raw|rem_seen|enrolling|ENROLL|IMAGE PIXELS|broken|AUTH|fdt_ok|vio 1.8|HEALTH|APP_START|-22|sn num' \
    "$OUT/dmesg-enroll.txt" >"$OUT/enroll-hits.txt" 2>/dev/null || true

  FID=$(grep -oE 'new fid \(= [0-9]+\)' "$OUT/dmesg-enroll.txt" 2>/dev/null | tail -1 || true)
  PIX=$(grep -c 'frame raw' "$OUT/dmesg-enroll.txt" 2>/dev/null || echo 0)
  REM=$(grep -c 'rem_seen=1' "$OUT/dmesg-enroll.txt" 2>/dev/null || echo 0)
  BROKEN=$(grep -c 'broken off' "$OUT/dmesg-enroll.txt" 2>/dev/null || echo 0)

  rmmod qsee_fingerpr 2>/dev/null || true

  {
    echo "fid=$FID"
    echo "frame_raw_hits=$PIX"
    echo "rem_seen_hits=$REM"
    echo "broken_off_hits=$BROKEN"
  } | tee "$OUT/enroll-score.txt"

  if [ -n "$FID" ]; then
    log "ENROLL_OK $FID pix=$PIX rem=$REM"
    echo "ENROLL_OK $FID"
    return 0
  fi
  if [ "${PIX:-0}" -gt 0 ] || [ "${REM:-0}" -gt 0 ]; then
    log "ENROLL_PARTIAL pix=$PIX rem=$REM"
    echo "ENROLL_PARTIAL pix=$PIX rem=$REM"
    return 1
  fi
  log "ENROLL_FAIL no fid / no frames — check enroll-hits.txt (START -22? finger timing?)"
  echo "ENROLL_FAIL"
  return 1
}

auth_try() {
  log "=== D: AUTH try (same finger; optional) ==="
  # Full AUTH path lives inside qsee_fingerpr after enroll in same insmod.
  # Separate AUTH-only not always exported — point at hits file.
  if [ -f "$OUT/enroll-hits.txt" ]; then
    grep -iE 'AUTH|broken|match|identify' "$OUT/enroll-hits.txt" | tee "$OUT/auth-hits.txt" || true
  fi
  log "AUTH residual: without rem_seen frames, AUTH stays broken off (lab truth)"
}

write_summary() {
  cat >"$OUT/RESULTS.md" <<EOF
# FP finish-line run

- Out: \`$OUT\`
- life: \`$LIFE\`
- qsee: \`$QSEE\`

## Commands

\`\`\`
sudo OUTDIR=$OUT $(basename "$0") prepare|health|enroll|full|cleanup
\`\`\`

## Honest finish lines

| Goal | Status |
|------|--------|
| A Sensor life / press_signal | Coach M0-press-ok |
| B TA load + enroll protocol | Lab has loaded TAs; enroll needs finger |
| C Phosh unlock | Still blocked without stable AUTH + fprintd glue |

See enroll-score.txt / enroll-hits.txt in this directory.
EOF
  log "RESULTS=$OUT/RESULTS.md"
}

cmd="${1:-health}"
need_root
mkdir -p "$OUT"
log "start cmd=$cmd out=$OUT"

case "$cmd" in
  prepare)
    prepare_l2
    write_summary
    ;;
  health)
    prepare_l2
    health_qsee || true
    cleanup
    write_summary
    ;;
  enroll)
    enroll_finger
    cleanup
    write_summary
    ;;
  full)
    prepare_l2
    health_qsee || log "health soft-fail; still trying enroll"
    enroll_finger || true
    auth_try || true
    cleanup
    write_summary
    ;;
  cleanup)
    cleanup
    ;;
  *)
    echo "Usage: $0 prepare|health|enroll|full|cleanup"
    exit 2
    ;;
esac

echo "OUTDIR=$OUT"
exit 0
