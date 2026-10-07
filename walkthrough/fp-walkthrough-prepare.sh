#!/bin/sh
# Prepare FP sensor life stack for the walkthrough app.
# Loads OOT focaltech_fp_life (if needed), enables listen, prints machine-readable status.
#
# Usage:
#   fp-walkthrough-prepare.sh              # load + listen + status
#   fp-walkthrough-prepare.sh status       # status only
#   fp-walkthrough-prepare.sh load         # load + listen
#
# Exit: 0 if /dev/focaltech_fp + gpio34 IRQ present; 1 otherwise.
set -u

cmd="${1:-all}"
KVER="$(uname -r)"
MOD=""
for c in \
  "/lib/modules/$KVER/extra/focaltech_fp_life.ko" \
  "/usr/local/share/fp5-fp-walkthrough/focaltech_fp_life.ko" \
  "/usr/local/lib/fp5/focaltech_fp_life.ko" \
  "/tmp/focaltech_fp_life.ko"
do
  if [ -f "$c" ]; then MOD="$c"; break; fi
done

find_sysfs() {
  SYS=""
  for d in /sys/class/focaltech_fp/*; do
    [ -e "$d" ] || continue
    SYS="$d"
    break
  done
  echo "$SYS"
}

status_json_ish() {
  dev=0
  [ -e /dev/focaltech_fp ] && dev=1
  irq_line="$(grep -E 'focaltech_fp|msmgpio[[:space:]]+34' /proc/interrupts 2>/dev/null | head -1 || true)"
  irq_present=0
  [ -n "$irq_line" ] && irq_present=1
  SYS="$(find_sysfs)"
  listen="?"
  irq_count="?"
  vdd="?"
  if [ -n "$SYS" ] && [ -d "$SYS" ]; then
    listen="$(cat "$SYS/listen" 2>/dev/null || echo '?')"
    irq_count="$(cat "$SYS/irq_count" 2>/dev/null || echo '?')"
    vdd="$(cat "$SYS/vdd" 2>/dev/null || echo '?')"
  fi
  mod_loaded=0
  lsmod 2>/dev/null | grep -q focaltech_fp_life && mod_loaded=1
  echo "FP_PREPARE dev=$dev irq=$irq_present mod=$mod_loaded listen=$listen irq_count=$irq_count vdd=$vdd sysfs=${SYS:-none}"
  if [ -n "$irq_line" ]; then
    echo "FP_IRQ_LINE $irq_line"
  fi
  if [ "$dev" = 1 ] && [ "$irq_present" = 1 ]; then
    echo "FP_PREPARE_OK"
    return 0
  fi
  echo "FP_PREPARE_FAIL"
  return 1
}

do_load() {
  if lsmod 2>/dev/null | grep -q focaltech_fp_life; then
    echo "FP_LOAD already_loaded"
  else
    if [ -z "$MOD" ] || [ ! -f "$MOD" ]; then
      echo "FP_LOAD no_module (need focaltech_fp_life.ko under /lib/modules/$KVER/extra/)"
      return 1
    fi
    echo "FP_LOAD insmod $MOD"
    if ! insmod "$MOD" irq_gpio=34 reset_gpio=35 vdd_gpio=60 use_sw_pdev=1 2>/tmp/fp-prep-insmod.err; then
      echo "FP_LOAD insmod_failed $(cat /tmp/fp-prep-insmod.err 2>/dev/null)"
      # already loaded race
      lsmod | grep -q focaltech_fp_life || return 1
    fi
    sleep 0.4
  fi
  SYS="$(find_sysfs)"
  if [ -n "$SYS" ]; then
    # Full re-arm: listen=1 alone does NOT power-cycle if already listening
    # (driver skips cycle when irq_enabled). Drop listen + toggle VDD first.
    echo 0 >"$SYS/listen" 2>/dev/null || true
    if [ -f "$SYS/vdd" ]; then
      echo 0 >"$SYS/vdd" 2>/dev/null || true
      sleep 0.05
      echo 1 >"$SYS/vdd" 2>/dev/null || true
    fi
    sleep 0.1
    echo 1 >"$SYS/listen" 2>/dev/null || echo "FP_LISTEN store_failed"
    if [ -f "$SYS/spiclk" ]; then
      echo 1 >"$SYS/spiclk" 2>/dev/null || true
    fi
    c0=$(cat "$SYS/irq_count" 2>/dev/null || echo 0)
    sleep 0.35
    c1=$(cat "$SYS/irq_count" 2>/dev/null || echo 0)
    echo "FP_LISTEN listen=$(cat "$SYS/listen" 2>/dev/null) irq_count=$c1 delta_0.35s=$((c1 - c0))"
    # If IRQ still dead after re-arm, reload the life module once
    if [ "$c1" -eq "$c0" ] 2>/dev/null; then
      echo "FP_REARM irq_flat — reloading life module"
      rmmod focaltech_fp_life 2>/dev/null || true
      sleep 0.2
      if [ -n "$MOD" ] && [ -f "$MOD" ]; then
        insmod "$MOD" irq_gpio=34 reset_gpio=35 vdd_gpio=60 use_sw_pdev=1 2>/tmp/fp-prep-reload.err || \
          echo "FP_RELOAD failed $(cat /tmp/fp-prep-reload.err 2>/dev/null)"
        sleep 0.4
        SYS="$(find_sysfs)"
        if [ -n "$SYS" ]; then
          echo 1 >"$SYS/vdd" 2>/dev/null || true
          echo 1 >"$SYS/listen" 2>/dev/null || true
          echo "FP_RELOAD irq_count=$(cat "$SYS/irq_count" 2>/dev/null)"
        fi
      fi
    fi
  else
    echo "FP_LISTEN no_sysfs"
  fi
  return 0
}

case "$cmd" in
status)
  status_json_ish
  exit $?
  ;;
load)
  do_load || exit 1
  status_json_ish
  exit $?
  ;;
all|*)
  do_load || true
  status_json_ish
  exit $?
  ;;
esac
