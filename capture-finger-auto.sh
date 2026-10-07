#!/bin/bash
# Non-interactive: arms logs for 70s; user touches power-button FP during window.
set -euo pipefail
OUT=${1:-/home/kb/repos/grok_build/fp5/dumps/android/fp-finger-session-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
adb wait-for-device
adb shell 'su -c id' | head -1 | tee "$OUT/su.txt"
adb shell 'su -c "cat /proc/interrupts"' > "$OUT/irq-before.txt"
timeout 70 adb logcat -c 2>/dev/null || true
# start enroll UI
adb shell am start -a android.settings.FINGERPRINT_ENROLL >/dev/null 2>&1 || true
echo "TOUCH power-button FP now for ~60s..."
timeout 65 adb logcat -v threadtime \
  '*:S' FingerprintService:V fingerprint:V biometrics:V \
  focal:V QSEECOM:V QSEE:V qseecom:V vendor.focaltech:V \
  > "$OUT/logcat.txt" 2>&1 &
LP=$!
# also full logcat sample
timeout 65 adb logcat -v threadtime > "$OUT/logcat-full.txt" 2>&1 &
LP2=$!
wait $LP 2>/dev/null || true
wait $LP2 2>/dev/null || true
adb shell 'su -c "cat /proc/interrupts"' > "$OUT/irq-after.txt"
adb shell 'su -c "dmesg | grep -iE \"focal|finger|fp_|qsee\" | tail -80"' > "$OUT/dmesg-fp.txt" || true
adb shell dumpsys fingerprint > "$OUT/dumpsys-fingerprint.txt" 2>&1 || true
python3 - << PY
from pathlib import Path
out=Path("$OUT")
def irq(path, name):
    for line in path.read_text(errors="replace").splitlines():
        if name in line or "focaltech_fp" in line:
            return line
    return None
b,a=out/"irq-before.txt", out/"irq-after.txt"
print("BEFORE", irq(b," 34 "))
print("AFTER ", irq(a," 34 "))
lc=out/"logcat.txt"
print("logcat lines", len(lc.read_text(errors="replace").splitlines()) if lc.exists() else 0)
print("interesting:")
import re
for line in lc.read_text(errors="replace").splitlines():
    if re.search(r"focal|Fingerprint|enroll|qsee|QSEE|error|Error|fail", line, re.I):
        print(line[:200])
PY
echo "DONE $OUT"
