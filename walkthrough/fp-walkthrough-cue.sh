#!/bin/sh
# One flash + beep = touch the power-button sensor now.
# No flash at start, HOLD, LIFT, or finish.
# Usage: fp-walkthrough-cue.sh SESSION_DIR
# Stop: rm SESSION_DIR/cue.pid
set -u
SESSION="${1:-}"
[ -n "$SESSION" ] && [ -d "$SESSION" ] || exit 2

echo $$ >"$SESSION/cue.pid"
LOG="$SESSION/enroll-cues.txt"
FOLLOW="$SESSION/dmesg-follow.txt"
UID_USER=10000
RUNTIME="/run/user/$UID_USER"
GO=/usr/share/sounds/freedesktop/stereo/complete.oga

as_user() {
	XDG_RUNTIME_DIR="$RUNTIME" \
	DBUS_SESSION_BUS_ADDRESS="unix:path=$RUNTIME/bus" \
	sudo -u user -E env XDG_RUNTIME_DIR="$RUNTIME" \
		DBUS_SESSION_BUS_ADDRESS="unix:path=$RUNTIME/bus" \
		"$@" >/dev/null 2>&1 || true
}

flash_once() {
	echo 1 >/sys/class/leds/white:flash/brightness 2>/dev/null || true
	sleep 0.15
	echo 0 >/sys/class/leds/white:flash/brightness 2>/dev/null || true
}

press_cue() {
	ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	echo "$ts CUE press" >>"$LOG" 2>/dev/null || true
	as_user paplay "$GO"
	as_user fbcli -E phone-incoming-call -t 1
	flash_once
}

echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) CUE wait (no flash — hands off)" >>"$LOG" 2>/dev/null || true

# Match AUTH logs "wait lift then PRESS" first; dirty-fail never prints
# "PRESS then LIFT finger sample" (no LED). Flash on either.
CUE_RE='PRESS then LIFT finger sample|wait lift then PRESS'
# Always live dmesg. dmesg-follow freezes at enroll end so Match PRESS
# (sample 0/1) never appears there — no LED.
last=$(dmesg 2>/dev/null | grep -E "$CUE_RE" | tail -1)
while [ -f "$SESSION/cue.pid" ]; do
	line=$(dmesg 2>/dev/null | grep -E "$CUE_RE" | tail -1)
	if [ -n "$line" ] && [ "$line" != "$last" ]; then
		last="$line"
		echo "$line" >>"$LOG" 2>/dev/null || true
		press_cue
	fi
	sleep 0.15
done
exit 0
