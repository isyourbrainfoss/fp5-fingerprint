#!/bin/bash
# Fresh Android fingerprint enroll capture for CAPTURE-sequence RE.
# Run on host with: adb authorized, screen unlocked, Magisk root preferred.
#
# Usage:
#   ./android-enroll-capture.sh              # arm loggers, wait for you
#   ./android-enroll-capture.sh --arm-only   # start loggers, print OUT dir
#   ./android-enroll-capture.sh --config-only  # HAL restart + factory.* dump, no enroll
#   STOP: touch /tmp/fp-enroll-capture-stop  or Ctrl-C
#
# Rem-gap (do NOT recapture rem countdown logcat — we have 20260725).
# Need: factory.* HAL defaults, coverage/quality inte lines, QSEECom ioctl
# hex of CAPTURE 0x1013 after finger-down (0x24 capture_ctx).
set -euo pipefail

PASS_HINT="${PASS_HINT:-}"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${OUT:-/home/kb/repos/grok_build/fp5/dumps/android/fp-enroll-${STAMP}}"
mkdir -p "$OUT"
STOP_FILE=/tmp/fp-enroll-capture-stop
rm -f "$STOP_FILE"

log() { echo "[$(date +%H:%M:%S)] $*"; }

need_adb() {
  adb wait-for-device
  adb get-state | grep -q device
}

root_ok() {
  adb shell su -c id 2>/dev/null | grep -q uid=0 || \
    adb root >/dev/null 2>&1 && sleep 1 && adb wait-for-device
  adb shell su -c id 2>/dev/null | head -1 || adb shell id
}

log "OUT=$OUT"
need_adb
root_ok | tee "$OUT/root.txt"
adb shell getprop ro.build.display.id 2>/dev/null | tee "$OUT/build.txt"
adb shell getprop ro.boot.slot_suffix 2>/dev/null | tee -a "$OUT/build.txt"
adb shell 'dumpsys fingerprint' 2>/dev/null | tee "$OUT/dumpsys-fingerprint-before.txt" | tail -20

dump_hal_config() {
  log "HAL restart to dump full config (factory.* was truncated in 20260726)"
  adb logcat -c 2>/dev/null || true
  adb shell su -c 'setprop persist.vendor.focaltech.log 1' 2>/dev/null || true
  adb shell su -c 'stop vendor.fps_hal; stop vendor.focaltech.fps_hal; sleep 1; start vendor.fps_hal' \
    2>/dev/null || adb shell su -c 'killall android.hardware.biometrics.fingerprint@2.1-service' 2>/dev/null || true
  sleep 3
  adb logcat -d -v threadtime 2>/dev/null | tee "$OUT/logcat-hal-restart.txt" | \
    grep -E 'focaltech:config|factory\.|other_press_inte|dead_pixel_inte|inte time' \
    | tee "$OUT/hal-config-factory.txt" || true
  adb shell su -c 'ls -la /data/vendor/focaltech /data/vendor_de/0/fpdata /data/vendor/fpdata 2>/dev/null' \
    | tee "$OUT/fpdata-ls-before.txt" || true
  adb shell su -c 'cat /data/vendor/focaltech/ff_config.json 2>/dev/null' | tee "$OUT/ff_config.json" || true
  adb shell su -c 'cat /vendor/etc/ff_config.json 2>/dev/null; cat /vendor/focaltech/ff_config.json 2>/dev/null' \
    | tee -a "$OUT/ff_config.json" || true
}

dump_hal_config

if [ "${1:-}" = "--config-only" ]; then
  log "config-only done → $OUT"
  echo "$OUT"
  exit 0
fi

# Optional musl strace of fps_hal (glibc strace fails on this unit).
(
  HALPID=$(adb shell su -c 'pidof android.hardware.biometrics.fingerprint@2.1-service vendor.fps.fingerprint-default' 2>/dev/null | tr ' ' '\n' | head -1 | tr -d '\r')
  echo "halpid=$HALPID" | tee "$OUT/hal-pid.txt"
  if [ -n "$HALPID" ] && adb shell su -c 'test -x /data/local/tmp/muslroot/bin/strace' 2>/dev/null; then
    log "strace fps_hal pid=$HALPID (ioctl, 8 min)"
    timeout 480 adb shell su -c "/data/local/tmp/muslroot/bin/strace -f -s 512 -e ioctl,write -p $HALPID" \
      >"$OUT/strace-fps-hal.txt" 2>&1 || true
  else
    echo "no musl strace or no HAL pid" | tee "$OUT/strace-fps-hal.txt"
  fi
) &
STRACE_PID=$!

# Clear ring buffers
adb logcat -c 2>/dev/null || true
adb shell su -c 'dmesg -C' 2>/dev/null || adb shell dmesg -C 2>/dev/null || true

# Baseline IRQ / node
adb shell 'ls -la /dev/focaltech_fp /dev/qseecom 2>/dev/null; cat /sys/class/focaltech_fp/*/irq* 2>/dev/null; ls /sys/class/focaltech_fp 2>/dev/null' \
  2>/dev/null | tee "$OUT/baseline-nodes.txt" || true

# Background loggers
adb logcat -v threadtime \
  '*:S' \
  'focaltech:V' 'Focal:V' 'FingerprintService:V' 'BiometricService:V' \
  'FingerprintProvider:V' 'fp_hal:V' 'fps_hal:V' 'QSEECOMAPI:V' 'QSEE:V' \
  'qseecom:V' 'Trustonic:V' 'GateKeeper:V' 'Keymaster:V' \
  'focaltech:config:V' 'focaltech:hal:V' 'focaltech:ft93xx:V' \
  >"$OUT/logcat-fp.txt" 2>&1 &
LOGCAT_PID=$!

adb logcat -v threadtime >"$OUT/logcat-full.txt" 2>&1 &
LOGCAT_FULL_PID=$!

# dmesg follow (root)
( adb shell su -c 'dmesg -w' 2>/dev/null || adb shell 'dmesg -w' ) \
  >"$OUT/dmesg-follow.txt" 2>&1 &
DMESG_PID=$!

# Optional: poll fingerprint dumpsys lightly
(
  for i in $(seq 1 120); do
    [ -f "$STOP_FILE" ] && break
    echo "===== poll $i $(date -Iseconds) =====" >>"$OUT/dumpsys-fingerprint-poll.txt"
    adb shell dumpsys fingerprint 2>/dev/null >>"$OUT/dumpsys-fingerprint-poll.txt" || true
    sleep 2
  done
) &
POLL_PID=$!

cat >"$OUT/README.txt" <<EOF
Android enroll capture $STAMP
Host started: $(date -Iseconds)

DO ON PHONE (in order):
1. Unlock screen; keep awake.
2. Settings → Security → Fingerprint → Add fingerprint
  (or remove one first if full, then re-add)
3. Complete ~several presses until finish (or cancel after 4+ good presses).
4. On host: touch $STOP_FILE   OR Ctrl-C this script.

Focus tags in logcat-fp.txt: focaltech, FingerprintService, QSEE*, transmit_image.

REM GAP (must-have; skip if we only get rem countdown again):
- factory.other_press_inte / factory.dead_pixel_inte in hal-config-factory.txt
- "The coverage inte time" / "quality inte" / "snr inte" in logcat
- QSEECom_send_cmd / ioctl hex of CAPTURE after finger-down (strace-fps-hal.txt)
- capture_ctx 0x24 bytes (burst/+0x10/+0x14/+0x1c/+0x20)
Do NOT bother repeating enrolled fid rem=19→0 logcat; we have 20260725.
EOF

log "Loggers armed (logcat_fp=$LOGCAT_PID full=$LOGCAT_FULL_PID dmesg=$DMESG_PID)"
log "=============================================="
log "  ON PHONE NOW: enroll a fingerprint"
log "  Settings → Security → Fingerprint → Add"
log "  Do several presses until finish (or 4+ samples)"
log "=============================================="
log "When done: touch $STOP_FILE  (or Ctrl-C)"

if [ "${1:-}" = "--arm-only" ]; then
  echo "$OUT" > /tmp/fp-enroll-capture-out
  echo "ARMED OUT=$OUT"
  exit 0
fi

# Wait until stop or 8 minutes
END=$((SECONDS + 480))
while [ $SECONDS -lt $END ]; do
  if [ -f "$STOP_FILE" ]; then
    log "stop file seen"
    break
  fi
  # detect enroll finish in logcat
  if grep -qE 'FingerprintEnrollFinish|enroll.*complete|fingerprint_cancel' "$OUT/logcat-fp.txt" 2>/dev/null; then
    # give a few more seconds of tail
    sleep 5
    if grep -qE 'FingerprintEnrollFinish|fingerprint_cancel' "$OUT/logcat-fp.txt" 2>/dev/null; then
      log "enroll finish markers seen — stopping soon"
      sleep 3
      break
    fi
  fi
  sleep 1
done

log "Stopping loggers..."
kill $LOGCAT_PID $LOGCAT_FULL_PID $DMESG_PID $POLL_PID $STRACE_PID 2>/dev/null || true
wait $LOGCAT_PID $LOGCAT_FULL_PID $DMESG_PID $POLL_PID $STRACE_PID 2>/dev/null || true
sleep 1

# After snapshots
adb shell 'dumpsys fingerprint' 2>/dev/null | tee "$OUT/dumpsys-fingerprint-after.txt" | tail -30
adb shell 'dumpsys biometric' 2>/dev/null | tee "$OUT/dumpsys-biometric-after.txt" | tail -40 || true
adb shell su -c 'dmesg' 2>/dev/null | tail -500 >"$OUT/dmesg-tail.txt" || true
adb shell su -c 'ls -la /data/vendor_de/0/fpdata 2>/dev/null; ls -la /data/vendor/fpdata 2>/dev/null' \
  2>/dev/null | tee "$OUT/fpdata-ls.txt" || true

# Extract high-signal lines
grep -E 'transmit_image|frame raw|finger touch|interrupt type|enrolling|FDT_|pre_enroll|fingerprint_enroll|CAPTURE|0x1013|invoke_ta|rem|Null|array_new|Out of memory|img_scan|sensor raw|inte time|other_press_inte|dead_pixel_inte|factory\.|QSEECom_send' \
  "$OUT/logcat-fp.txt" "$OUT/logcat-full.txt" 2>/dev/null \
  | head -600 >"$OUT/focaltech-events.txt" || true

grep -E 'transmit_image|frame raw|finger touch|enrolling|FDT_|interrupt type|invoke_ta' \
  "$OUT/logcat-full.txt" 2>/dev/null | head -200 >"$OUT/focus-events.txt" || true

# Quick summary stats
{
  echo "# Capture $STAMP"
  echo "build: $(cat "$OUT/build.txt" 2>/dev/null | tr '\n' ' ')"
  echo "logcat-fp lines: $(wc -l <"$OUT/logcat-fp.txt" 2>/dev/null || echo 0)"
  echo "transmit_image hits: $(grep -c transmit_image "$OUT/logcat-full.txt" 2>/dev/null || echo 0)"
  echo "frame raw hits: $(grep -c 'frame raw' "$OUT/logcat-full.txt" 2>/dev/null || echo 0)"
  echo "finger touch hits: $(grep -c 'finger touch' "$OUT/logcat-full.txt" 2>/dev/null || echo 0)"
  echo "enrolling hits: $(grep -c enrolling "$OUT/logcat-full.txt" 2>/dev/null || echo 0)"
  echo "FingerprintEnrollFinish: $(grep -c FingerprintEnrollFinish "$OUT/logcat-full.txt" 2>/dev/null || echo 0)"
  echo "factory.* hits: $(grep -c 'factory\\.' "$OUT/hal-config-factory.txt" 2>/dev/null || echo 0)"
  echo "coverage inte hits: $(grep -c 'coverage inte' "$OUT/logcat-full.txt" 2>/dev/null || echo 0)"
  echo "strace ioctl lines: $(grep -c ioctl "$OUT/strace-fps-hal.txt" 2>/dev/null || echo 0)"
} | tee "$OUT/SUMMARY-auto.txt"

log "DONE → $OUT"
echo "$OUT"
