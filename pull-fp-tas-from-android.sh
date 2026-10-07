#!/bin/sh
# Run while phone is on rooted stock Android (adb + Magisk su).
# Pulls full fingerpr + focal32 TA segments into lab dumps.
set -e
OUT="${1:-dumps/android/fp-tas-full-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
adb wait-for-device
adb shell 'su -c "ls -la /vendor/firmware_mnt/image/fingerpr* /vendor/firmware_mnt/image/focal32*"'
for p in fingerpr focal32; do
  adb shell "su -c 'cat /vendor/firmware_mnt/image/${p}.mdt'" > "$OUT/${p}.mdt"
  for i in 0 1 2 3 4 5 6 7; do
    f="${p}.b0${i}"
    adb shell "su -c 'cat /vendor/firmware_mnt/image/${f}'" > "$OUT/${f}" || true
  done
done
( cd "$OUT" && sha256sum fingerpr* focal32* > sha256.txt )
ls -lh "$OUT"
echo "Wrote $OUT — copy to pmOS /lib/firmware/qsee/ when ready"
