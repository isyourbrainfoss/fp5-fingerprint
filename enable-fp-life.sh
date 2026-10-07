#!/bin/sh
# Load FT9362 lab foothold module on pmOS and enable listen path.
# Usage (root): enable-fp-life.sh [path/to/focaltech_fp_life.ko]
set -e
MOD="${1:-}"
KVER="$(uname -r)"
if [ -z "$MOD" ]; then
	for c in \
		"/lib/modules/$KVER/extra/focaltech_fp_life.ko" \
		"/home/user/fp5-lab/artifacts/fp-modules/focaltech_fp_life.ko" \
		"./focaltech_fp_life.ko" \
		"/tmp/focaltech_fp_life.ko"
	do
		if [ -f "$c" ]; then MOD="$c"; break; fi
	done
fi
if [ -z "$MOD" ] || [ ! -f "$MOD" ]; then
	echo "usage: $0 /path/to/focaltech_fp_life.ko" >&2
	exit 2
fi

echo "=== kernel $(uname -a) ==="
echo "=== module $MOD ==="
modinfo "$MOD" 2>/dev/null | grep -E 'filename|vermagic|description' || true

# Free GPIOs if poke script left them (best effort)
if [ -d /sys/module/focaltech_fp_life ]; then
	echo "module already loaded"
else
	insmod "$MOD" irq_gpio=34 reset_gpio=35 vdd_gpio=60 use_sw_pdev=1
fi

sleep 0.5
echo "=== lsmod ==="
lsmod | grep -i focal || true
echo "=== device node ==="
ls -l /dev/focaltech_fp 2>&1 || true
echo "=== sysfs ==="
SYS=
for d in /sys/class/focaltech_fp/focaltech_fp /sys/devices/platform/focaltech_fp/focaltech_fp; do
	if [ -d "$d" ]; then SYS="$d"; break; fi
done
# device_create puts under class
for d in /sys/class/focaltech_fp/*; do
	[ -e "$d" ] || continue
	SYS="$d"
	break
done
if [ -n "$SYS" ]; then
	echo "sysfs $SYS"
	ls -la "$SYS"
	echo 1 > "$SYS/listen" 2>/dev/null || echo "listen store failed"
	echo "listen=$(cat "$SYS/listen" 2>/dev/null)"
	echo "vdd=$(cat "$SYS/vdd" 2>/dev/null)"
	echo "irq_count=$(cat "$SYS/irq_count" 2>/dev/null)"
else
	echo "NO_SYSFS — try read /dev/focaltech_fp"
	cat /dev/focaltech_fp 2>/dev/null || true
fi

echo "=== dmesg (focal/fp) ==="
dmesg | grep -iE 'focaltech|ft9362|ff_life|focaltech_fp' | tail -40
echo "=== /proc/interrupts (msmgpio 34 / focal) ==="
grep -iE 'focal|msmgpio[[:space:]]+34' /proc/interrupts || grep 'msmgpio  34' /proc/interrupts || true
echo "=== DONE enable-fp-life ==="
echo "Re-test: cat /sys/class/focaltech_fp/*/irq_count; press power-button FP after listen=1"
