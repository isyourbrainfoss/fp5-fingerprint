#!/bin/sh
# pam_exec helper: wait for overlay AUTH HIT. Exit 0 = match.
# Used as: auth sufficient pam_exec.so expose_authtok /usr/local/lib/fp5-fp/fp5-pam-exec.sh
set -eu
KICK=/sys/module/qsee_fingerpr/parameters/auth_kick
if [ -w "$KICK" ]; then
  echo 1 >"$KICK" 2>/dev/null || true
else
  /usr/local/bin/fp5-fp-session match >/dev/null 2>&1 || true
fi
i=0
while [ "$i" -lt 40 ]; do
  if dmesg 2>/dev/null | tail -n 30 | grep -q "overlay AUTH HIT"; then
    exit 0
  fi
  if dmesg 2>/dev/null | tail -n 30 | grep -q "overlay AUTH FAIL"; then
    exit 1
  fi
  sleep 0.5
  i=$((i + 1))
done
exit 1
