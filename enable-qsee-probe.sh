#!/bin/sh
# Force QSEECOM foothold on FP5 pmOS and probe resident TZ apps.
# Usage (root): enable-qsee-probe.sh [/path/to/qsee_probe_fp5.ko]
set -e
MOD="${1:-/tmp/qsee_probe_fp5.ko}"
KVER="$(uname -r)"

echo "=== $(uname -a) ==="
if [ ! -f "$MOD" ]; then
	echo "missing $MOD" >&2
	exit 2
fi

rmmod qcom_qseecom_uefisecapp 2>/dev/null || true
rmmod qcom_qseecom 2>/dev/null || true
rmmod qsee_probe_fp5 2>/dev/null || true

dmesg -C 2>/dev/null || true

echo "=== insmod probe ==="
insmod "$MOD" force_pdev=1
sleep 0.3

echo "=== modprobe stock qcom_qseecom stack ==="
# Prefer packaged modules (may be compressed)
modprobe qcom_qseecom 2>&1 || insmod /lib/modules/$KVER/kernel/drivers/firmware/qcom/qcom_qseecom.ko 2>&1 || true
modprobe qcom_qseecom_uefisecapp 2>&1 || true
sleep 0.3

echo "=== lsmod ==="
lsmod | grep -iE 'qsee|qseecom|probe_fp5' || true

echo "=== sysfs ==="
find /sys -name '*qsee*' 2>/dev/null | head -40
ls -la /sys/bus/platform/devices/ 2>/dev/null | grep -i qsee || true
ls -la /sys/bus/auxiliary/devices/ 2>/dev/null | head -20

echo "=== dmesg ==="
dmesg | grep -iE 'qsee|qseecom|qsee_probe|uefisec|scm' | tail -50

echo "=== DONE enable-qsee-probe ==="
