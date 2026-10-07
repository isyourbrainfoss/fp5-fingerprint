#!/bin/sh
# Install TA blobs, force QSEECOM, attempt APP_START for fingerpr (and optional focal32).
# Usage (root): enable-qsee-ta-load.sh [path/to/qsee_ta_load.ko] [path/to/qsee_probe_fp5.ko]
set -e
TA_KO="${1:-/tmp/qsee_ta_load.ko}"
PROBE_KO="${2:-/tmp/qsee_probe_fp5.ko}"
FWDIR=/lib/firmware/qsee
SRC="${TA_SRC:-/tmp/fp-tas}"

echo "=== $(uname -a) ==="
mkdir -p "$FWDIR"
if [ -d "$SRC" ]; then
	cp -a "$SRC"/*.mdt "$SRC"/*.b0* "$FWDIR/" 2>/dev/null || true
fi
ls -lh "$FWDIR"/fingerpr* "$FWDIR"/focal32* 2>&1 | head -20

# Ensure qseecom pdev
if ! [ -d /sys/devices/platform/qcom_qseecom ]; then
	if [ -f "$PROBE_KO" ]; then
		insmod "$PROBE_KO" force_pdev=1 2>/dev/null || true
	fi
fi
modprobe qcom_qseecom 2>/dev/null || true
modprobe qcom_qseecom_uefisecapp 2>/dev/null || true

rmmod qsee_ta_load 2>/dev/null || true
dmesg -C 2>/dev/null || true

echo "=== load fingerpr ==="
insmod "$TA_KO" ta_dir="$FWDIR" ta_name=fingerpr
sleep 0.5
dmesg | grep -iE 'qsee_ta_load|qseecom|APP_START|LOOKUP' | tail -40

echo "=== optional focal32 ==="
rmmod qsee_ta_load 2>/dev/null || true
dmesg -C 2>/dev/null || true
insmod "$TA_KO" ta_dir="$FWDIR" ta_name=focal32
sleep 0.5
dmesg | grep -iE 'qsee_ta_load|APP_START|LOOKUP' | tail -30

echo "=== DONE ==="
