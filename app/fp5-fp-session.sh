#!/bin/sh
# FP5 fingerprint session — root only (sudo -n).
# Verbs: start | health | enroll | match | cleanup
# Never qbootctl. Never auto-enroll. Never pinmux SE14.
set -u
RUNDIR=/run/fp5-fp-session
SHARE=/usr/local/share/fp5-fp-walkthrough
LIB=/usr/local/lib/fp5-fp
LOGROOT="${FP5_FP_LOGROOT:-/home/user/fp-logs}"
KVER=$(uname -r 2>/dev/null || echo unknown)

log() {
	echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*"
	if [ -n "${LOGDIR:-}" ] && [ -d "$LOGDIR" ]; then
		echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" >>"$LOGDIR/session.log" 2>/dev/null || true
	fi
}

need_root() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "FAIL: need root (sudo -n $0 ...)"
		exit 2
	fi
}

find_ko() {
	name=$1
	for c in \
		"/lib/modules/$KVER/extra/$name" \
		"$SHARE/$name" \
		"/tmp/$name"
	do
		[ -f "$c" ] || continue
		ko_matches "$c" || continue
		echo "$c"
		return 0
	done
	return 1
}

ko_matches() {
	ko=$1
	command -v modinfo >/dev/null 2>&1 || return 1
	need=$(modinfo -F vermagic "$ko" 2>/dev/null | awk '{print $1}')
	[ -n "$need" ] && [ "$need" = "$KVER" ]
}

write_status_once() {
	# Best-effort JSON so GTK is not stuck on a stale SAVED.
	py="$LIB/fp5_fp_status.py"
	[ -f "$py" ] || py="$(dirname "$0")/fp5_fp_status.py"
	[ -f "$py" ] || return 0
	python3 "$py" --write-once "${LOGDIR:-/tmp}" "${1:-fail}" >/dev/null 2>&1 || true
}

prune_logs() {
	[ -d "$LOGROOT" ] || return 0
	# Keep last 3 stamp dirs.
	old=$(ls -1dt "$LOGROOT"/* 2>/dev/null | tail -n +4 || true)
	[ -n "$old" ] || return 0
	# shellcheck disable=SC2086
	rm -rf $old
}

saved_f32() {
	f=/var/tmp/fp5-ta-ids
	[ -f "$f" ] || return 1
	saved_boot=$(grep -E '^boot=' "$f" 2>/dev/null | tail -1 | cut -d= -f2)
	cur=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo none)
	[ -n "$saved_boot" ] && [ "$saved_boot" = "$cur" ] || return 1
	grep -E '^f32=' "$f" 2>/dev/null | tail -1 | cut -d= -f2
}

find_helper() {
	name=$1
	for c in "$SHARE/$name" "/usr/local/bin/$name" "$(dirname "$0")/../walkthrough/$name"; do
		if [ -f "$c" ]; then
			echo "$c"
			return 0
		fi
	done
	return 1
}

set_phase() {
	mkdir -p "$RUNDIR"
	echo "$1" >"$RUNDIR/phase"
}

current_logdir() {
	if [ -f "$RUNDIR/logdir" ]; then
		cat "$RUNDIR/logdir"
	fi
}

bg_detach() {
	# $1 = pid file, rest = command
	pf=$1
	shift
	setsid "$@" </dev/null >/dev/null 2>&1 &
	echo $! >"$pf"
}

ensure_tas() {
	mkdir -p /lib/firmware/qsee
	if [ -f /lib/firmware/qsee/fingerpr.mdt ] && [ -f /lib/firmware/qsee/focal32.mdt ]; then
		return 0
	fi
	if [ -b /dev/disk/by-partlabel/modem_a ]; then
		mkdir -p /mnt/modem_a
		mountpoint -q /mnt/modem_a 2>/dev/null || mount -o ro /dev/disk/by-partlabel/modem_a /mnt/modem_a
		cp -a /mnt/modem_a/image/fingerpr* /mnt/modem_a/image/focal32* /lib/firmware/qsee/ 2>/dev/null || true
	fi
}

cmd_start() {
	need_root
	# Fast kill leftover helpers; do not unload life yet (start will reload).
	if [ -x /usr/local/bin/fp5-fp-kill ]; then
		/usr/local/bin/fp5-fp-kill
	fi
	mkdir -p "$RUNDIR" "$LOGROOT"
	chmod 0755 "$RUNDIR" 2>/dev/null || true
	prune_logs
	stamp=$(date -u +%Y%m%dT%H%M%SZ)
	LOGDIR="$LOGROOT/$stamp-$$"
	mkdir -p "$LOGDIR"
	chmod 0777 "$LOGDIR" 2>/dev/null || true
	echo "$LOGDIR" >"$RUNDIR/logdir"
	chmod 0644 "$RUNDIR/logdir"
	set_phase start
	log "start logdir=$LOGDIR kver=$KVER"

	life=$(find_ko focaltech_fp_life.ko || true)
	if [ -z "$life" ] || ! ko_matches "$life"; then
		need=$(modinfo -F vermagic "$life" 2>/dev/null | awk '{print $1}' || echo unknown)
		log "FAIL vermagic $need vs running $KVER"
		printf '%s\n' "FP_PREPARE_FAIL vermagic $need vs $KVER" \
			"Rebuild kos or boot 7.1.2 hybrid. Do not flash to 'fix' this." \
			>"$LOGDIR/prepare.txt"
		set_phase fail
		write_status_once fail
		exit 1
	fi
	prep=$(find_helper fp-walkthrough-prepare.sh || true)
	if [ -z "$prep" ]; then
		log "FAIL no prepare helper"
		echo "FP_PREPARE_FAIL no_prepare" >"$LOGDIR/prepare.txt"
		set_phase fail
		exit 1
	fi
	"$prep" all >"$LOGDIR/prepare.txt" 2>&1 || true
	logger=$(find_helper fp-walkthrough-logger.sh || true)
	if [ -n "$logger" ]; then
		"$logger" start "$LOGDIR" >>"$LOGDIR/session.log" 2>&1 || true
	fi
	# Live dmesg for the watcher. Cap the file; do not dump the whole boot ring.
	# ktime-start drops leftover REAL DOWN from earlier Registers this boot.
	# Stamp from dmesg, not /proc/uptime (idle uptime can lag ~100s ahead).
	dmesg 2>/dev/null | sed -n 's/^\[\s*\([0-9][0-9]*\)\..*/\1/p' | tail -1 \
		>"$LOGDIR/ktime-start.txt" 2>/dev/null || true
	if [ ! -s "$LOGDIR/ktime-start.txt" ]; then
		cut -d. -f1 /proc/uptime >"$LOGDIR/ktime-start.txt" 2>/dev/null || true
	fi
	rm -f "$RUNDIR/status" "$RUNDIR/status.tmp"
	write_status_once ready
	if command -v dmesg >/dev/null 2>&1; then
		: >"$LOGDIR/dmesg-follow.txt"
		bg_detach "$RUNDIR/dmesg.pid" sh -c "dmesg -w >>\"$LOGDIR/dmesg-follow.txt\""
	fi
	statuspy="$LIB/fp5_fp_status.py"
	if [ ! -f "$statuspy" ]; then
		statuspy="$(dirname "$0")/fp5_fp_status.py"
	fi
	if [ -f "$statuspy" ]; then
		bg_detach "$RUNDIR/watch.pid" python3 "$statuspy" --watch "$LOGDIR" --phase-file "$RUNDIR/phase"
	fi
	# FP6: Android persist holds TEE templates. Mount before Register/INIT.
	mkdir -p /mnt/persist 2>/dev/null || true
	if [ -e /dev/disk/by-partlabel/persist ]; then
		if mountpoint -q /mnt/persist; then
			mount -o remount,rw,nosuid,nodev,noexec /mnt/persist 2>/dev/null || true
		else
			mount -o rw,nosuid,nodev,noexec /dev/disk/by-partlabel/persist /mnt/persist \
				2>/dev/null || true
		fi
		mkdir -p /mnt/persist/qti_fp 2>/dev/null || true
		log "persist $(mountpoint -q /mnt/persist && echo mounted || echo not_mounted)"
	fi
	# Life only. qsee_fingerpr insmod stalls Phosh/phoc — TZ waits for Register.
	set_phase ready
	log "start done phase=ready (life; TZ on register)"
}

cmd_health() {
	need_root
	LOGDIR=$(current_logdir)
	[ -n "$LOGDIR" ] || LOGDIR=/tmp/fp5-fp-health
	mkdir -p "$LOGDIR"
	set_phase health
	ensure_tas || true
	QSEE=$(find_ko qsee_fingerpr.ko || true)
	PROBE=$(find_ko qsee_probe_fp5.ko || true)
	if [ ! -d /sys/devices/platform/qcom_qseecom ] && [ -n "$PROBE" ]; then
		insmod "$PROBE" force_pdev=1 2>/dev/null || true
		modprobe qcom_qseecom 2>/dev/null || true
	fi
	if [ -z "$QSEE" ]; then
		log "HEALTH skip: no qsee_fingerpr.ko"
		echo "HEALTH_SKIP no_module" >"$LOGDIR/health.txt"
		return 0
	fi
	if ! ko_matches "$QSEE"; then
		log "HEALTH skip: qsee vermagic mismatch"
		echo "HEALTH_SKIP vermagic" >"$LOGDIR/health.txt"
		return 0
	fi
	rmmod qsee_fingerpr 2>/dev/null || true
	f32=$(saved_f32 || true)
	if [ -n "$f32" ]; then
		log "health attach f32=$f32 (no second APP_START)"
		start_args="do_start=0 force_f32_id=$f32"
	else
		log "health insmod skip_enroll=1 do_start=1"
		start_args="do_start=1"
	fi
	if ! insmod "$QSEE" $start_args enable_fs_listener=1 enable_rpmb_listener=1 \
		skip_enroll=1 skip_capture_path=1 quiet_send=1 spi_bus_num=14 \
		>"$LOGDIR/health-insmod.err" 2>&1
	then
		log "health insmod failed (see health-insmod.err) — TAs may already be up"
		dmesg 2>/dev/null | tail -n 80 >>"$LOGDIR/health.txt" || true
		rmmod qsee_fingerpr 2>/dev/null || true
		return 0
	fi
	n=0
	while [ "$n" -lt 12 ]; do
		if dmesg 2>/dev/null | tail -n 80 | grep -qE 'HEALTH final er=0|sn num|START focal32 ret=0'; then
			break
		fi
		n=$((n + 1))
		sleep 1
	done
	dmesg 2>/dev/null | grep -E 'START focal32|HEALTH final|sn num' | tail -n 40 >"$LOGDIR/health.txt" || true
	# Persist TA ids for later attach (full dmesg, not a 120-line window).
	f32=$(dmesg 2>/dev/null | grep -oE 'START focal32 ret=0 .* id=0x[0-9a-fA-F]+' | tail -1 | grep -oE 'id=0x[0-9a-fA-F]+' | head -1)
	if [ -n "$f32" ]; then
		f32n=$(printf '%d' "${f32#id=}" 2>/dev/null || true)
		boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo none)
		printf 'boot=%s\nf32=%s\n' "$boot" "$f32n" >/var/tmp/fp5-ta-ids 2>/dev/null || true
		log "saved TA id f32=$f32n"
	fi
	# Unload QSEE module only; keep life + TZ apps.
	rmmod qsee_fingerpr 2>/dev/null || true
	log "health done"
}

cmd_enroll() {
	need_root
	LOGDIR=$(current_logdir)
	if [ -z "$LOGDIR" ] || [ ! -d "$LOGDIR" ]; then
		rm -f /var/lib/fp5-fp/enrolled
		echo "ENROLL_FAIL need_session"
		exit 2
	fi
	set_phase enroll
	write_status_once enroll
	echo $$ >"$RUNDIR/enroll.pid"
	ensure_tas || true
	PROBE=$(find_ko qsee_probe_fp5.ko || true)
	if [ ! -d /sys/devices/platform/qcom_qseecom ] && [ -n "$PROBE" ]; then
		insmod "$PROBE" force_pdev=1 2>/dev/null || true
		modprobe qcom_qseecom 2>/dev/null || true
	fi
	enroll=$(find_helper fp-walkthrough-enroll.sh || true)
	if [ -z "$enroll" ]; then
		rm -f "$RUNDIR/enroll.pid" /var/lib/fp5-fp/enrolled
		echo "ENROLL_FAIL no_helper" | tee "$LOGDIR/enroll-ui.txt"
		exit 1
	fi
	SAMPLE_WAIT_S="${SAMPLE_WAIT_S:-8}" ENROLL_SAMPLES="${ENROLL_SAMPLES:-60}" \
		"$enroll" "$LOGDIR" >"$LOGDIR/enroll-ui.txt" 2>&1
	rc=$?
	rm -f "$RUNDIR/enroll.pid"
	if [ "$rc" -ne 0 ]; then
		rm -f /var/lib/fp5-fp/enrolled
	fi
	set_phase ready
	exit $rc
}

cmd_match() {
	need_root
	LOGDIR=$(current_logdir)
	if [ -z "$LOGDIR" ] || [ ! -d "$LOGDIR" ]; then
		echo "MATCH_FAIL need_session"
		exit 2
	fi
	set_phase match
	echo $$ >"$RUNDIR/enroll.pid"
	ensure_tas || true
	enroll=$(find_helper fp-walkthrough-enroll.sh || true)
	if [ -z "$enroll" ]; then
		rm -f "$RUNDIR/enroll.pid"
		echo "MATCH_FAIL no_helper"
		exit 1
	fi
	: >"$LOGDIR/match-ui.txt"
	DO_AUTH=1 ENROLL_SAMPLES=3 SAMPLE_WAIT_S=8 \
		"$enroll" "$LOGDIR" >>"$LOGDIR/match-ui.txt" 2>&1
	rc=$?
	rm -f "$RUNDIR/enroll.pid"
	set_phase ready
	write_status_once ready
	exit $rc
}

cmd_cleanup() {
	need_root
	LOGDIR=$(current_logdir)
	set_phase cleanup
	if [ -x /usr/local/bin/fp5-fp-kill ]; then
		/usr/local/bin/fp5-fp-kill
	fi
	for d in /sys/class/focaltech_fp/*; do
		[ -f "$d/listen" ] && echo 0 >"$d/listen" 2>/dev/null || true
	done
	logger=$(find_helper fp-walkthrough-logger.sh || true)
	if [ -n "$logger" ] && [ -n "$LOGDIR" ] && [ -d "$LOGDIR" ]; then
		"$logger" cleanup "$LOGDIR" >/dev/null 2>&1 || true
	fi
	extra_in=0
	lsmod | grep -q '^qsee_fingerpr' && extra_in=1
	# Keep extra only if mark is present AND extra is loaded. Stale file ≠ keep.
	# Never rmmod a loaded extra (segfaults).
	if [ -f /var/lib/fp5-fp/enrolled ] && [ "$extra_in" = 1 ]; then
		log "cleanup keep extra+life (enrolled — Match AUTH kick)"
	else
		if [ -f /var/lib/fp5-fp/enrolled ]; then
			rm -f /var/lib/fp5-fp/enrolled
			log "cleanup drop stale enrolled mark (extra not loaded)"
		fi
		if [ "$extra_in" = 1 ]; then
			log "cleanup skip rmmod qsee_fingerpr (often segfaults) — reboot to unload"
		fi
		rmmod qsee_ta_load 2>/dev/null || true
		rmmod qsee_focal_ok 2>/dev/null || true
		rmmod focaltech_fp_life 2>/dev/null || true
	fi
	rm -f "$RUNDIR/logdir" "$RUNDIR/phase" "$RUNDIR/status" "$RUNDIR/status.tmp"
	log "cleanup done"
	exit 0
}

cmd=${1:-}
case "$cmd" in
start) cmd_start ;;
health) cmd_health ;;
enroll) cmd_enroll ;;
match) cmd_match ;;
cleanup) cmd_cleanup ;;
*)
	echo "usage: $0 start|health|enroll|match|cleanup" >&2
	exit 2
	;;
esac
