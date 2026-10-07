#!/bin/bash
# Capture Android FP finger session (Magisk su). Run on host with adb.
set -euo pipefail
OUT=${1:-/home/kb/repos/grok_build/fp5/dumps/android/fp-finger-session-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
echo "OUT=$OUT"
adb wait-for-device
adb shell 'su -c id' | grep -q uid=0

adb shell 'su -c "cat /proc/interrupts"' > "$OUT/irq-before.txt"
adb shell 'su -c "dmesg -c >/dev/null; true"' 2>/dev/null || true

# background logcat (broad tags)
timeout 90 adb logcat -v threadtime -b all \
  '*:S' \
  FingerprintService:V fingerprint:V biometrics:V BiometricService:V \
  focal:V Focal:V fps:V Fp:V \
  QSEECOMAPI:V QSEECOM:V QSEE:V qseecom:V \
  vendor.focaltech:V android.hardware.biometrics:V \
  > "$OUT/logcat.txt" 2>&1 &
LOGPID=$!

# open enroll UI to activate HAL path
adb shell am start -a android.settings.FINGERPRINT_SETUP 2>/dev/null || \
  adb shell am start -a android.settings.FINGERPRINT_ENROLL 2>/dev/null || \
  adb shell am start -n com.android.settings/.biometrics.fingerprint.FingerprintEnrollIntroduction 2>/dev/null || true

echo ""
echo "=== ACTION: within ~60s, press the SIDE POWER BUTTON fingerprint sensor"
echo "    repeatedly as if enrolling (lift and press 5-10 times)."
echo "    Then press Enter here when done. ==="
read -r _

adb shell 'su -c "cat /proc/interrupts"' > "$OUT/irq-after.txt"
adb shell 'su -c "dmesg"' > "$OUT/dmesg-full.txt"
adb shell 'su -c "dmesg | grep -iE \"focal|finger|fp_|qsee|brl\""' > "$OUT/dmesg-fp.txt" || true
adb shell dumpsys fingerprint > "$OUT/dumpsys-fingerprint.txt" 2>&1 || true
adb shell dumpsys biometric > "$OUT/dumpsys-biometric.txt" 2>&1 || true
kill $LOGPID 2>/dev/null || true
wait $LOGPID 2>/dev/null || true

# IRQ delta for gpio 34 / focaltech
echo "=== IRQ delta (focal / gpio 34) ===" | tee "$OUT/irq-delta.txt"
paste <(grep -E 'focal| 34 ' "$OUT/irq-before.txt" || true) <(grep -E 'focal| 34 ' "$OUT/irq-after.txt" || true) | tee -a "$OUT/irq-delta.txt" || true

# summary
{
  echo "# Finger session summary"
  echo "time: $(date -Is)"
  echo "logcat lines: $(wc -l < "$OUT/logcat.txt")"
  echo "dmesg-fp lines: $(wc -l < "$OUT/dmesg-fp.txt")"
  echo "--- logcat keywords ---"
  grep -iE 'focal|fingerprint|qsee|enroll|auth|error|fail|ta |trust' "$OUT/logcat.txt" | head -80 || true
} | tee "$OUT/SUMMARY.txt"

echo "DONE -> $OUT"
