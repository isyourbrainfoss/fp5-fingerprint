#!/bin/sh
# Always returns in well under a second. Never pkill this session binary
# (Start/cleanup call us while they are still running).
set -u
RUNDIR=/run/fp5-fp-session
killpid() {
	[ -f "$1" ] || return 0
	pid=$(cat "$1" 2>/dev/null || true)
	[ -n "$pid" ] || return 0
	kill -KILL "$pid" 2>/dev/null || true
	kill -KILL -"$pid" 2>/dev/null || true
	rm -f "$1"
}
killpid "$RUNDIR/watch.pid"
killpid "$RUNDIR/dmesg.pid"
killpid "$RUNDIR/logger.pid"
killpid "$RUNDIR/enroll.pid"
killpid "$RUNDIR/health.pid"
killpid "$RUNDIR/cue.pid"
# Cue/watch pids the enroll helper leaves in the log dir.
if [ -f "$RUNDIR/logdir" ]; then
	ld=$(cat "$RUNDIR/logdir" 2>/dev/null || true)
	if [ -n "$ld" ]; then
		killpid "$ld/cue.pid"
		killpid "$ld/enroll-watch.pid"
		killpid "$ld/dmesg-follow.pid"
		killpid "$ld/irq-series.pid"
	fi
fi
rm -f "$RUNDIR/status" "$RUNDIR/status.tmp"
exit 0
