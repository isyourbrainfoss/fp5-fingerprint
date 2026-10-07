#!/bin/sh
# Bring up QSEE + load FocalTech TAs + prove focal32 dialogue on pmOS.
# Usage (root): enable-qsee-session.sh
set -e
KVER=$(uname -r)
MODDIR=${MODDIR:-/tmp}
FW=/lib/firmware/qsee
mkdir -p "$FW" /mnt/modem_a
if ! mountpoint -q /mnt/modem_a 2>/dev/null; then
	mount -o ro /dev/disk/by-partlabel/modem_a /mnt/modem_a
fi
cp -a /mnt/modem_a/image/fingerpr* /mnt/modem_a/image/focal32* "$FW/" 2>/dev/null || true

if [ ! -d /sys/devices/platform/qcom_qseecom ]; then
	insmod "$MODDIR/qsee_probe_fp5.ko" force_pdev=1
	modprobe qcom_qseecom 2>/dev/null || true
fi

# Prefer demo module that prints focaltech:ta:base
if [ -f "$MODDIR/qsee_focal_ok.ko" ]; then
	rmmod qsee_focal_ok 2>/dev/null || true
	# May need reboot if TAs already loaded
	insmod "$MODDIR/qsee_focal_ok.ko" ta_dir="$FW" || {
		echo "insmod failed — if START -22, reboot then re-run" >&2
		exit 1
	}
	dmesg | grep qsee_ok | tail -30
	echo "Expect: f32-base with string focaltech:ta:base"
	exit 0
fi
echo "missing $MODDIR/qsee_focal_ok.ko" >&2
exit 2
