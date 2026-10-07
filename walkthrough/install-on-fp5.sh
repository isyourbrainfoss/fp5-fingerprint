#!/bin/sh
# Install fingerprint walkthrough app + prepare (life module) on FP5.
# Host: ./lab/fingerprint/walkthrough/install-on-fp5.sh
set -eu
FP5_SSH="${FP5_SSH:-fp5-pmos}"
ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO=$(CDPATH= cd -- "$ROOT/../../.." && pwd)
KO_CANDIDATES="
$REPO/artifacts/fp-modules/focaltech_fp_life.ko
$REPO/lab/fingerprint/oot-fp/focaltech_fp_life.ko
"

find_ko() {
  for k in $KO_CANDIDATES; do
    if [ -f "$k" ]; then echo "$k"; return 0; fi
  done
  return 1
}

if [ "$(uname -n 2>/dev/null)" = "fairphone-fp5" ] || [ -f /etc/deviceinfo ]; then
  DEST=/usr/local/share/fp5-fp-walkthrough
  sudo mkdir -p "$DEST" /usr/local/bin \
    /usr/local/share/applications \
    /home/user/.local/share/applications
  sudo cp -a "$ROOT/fp-walkthrough.py" "$ROOT/fp-walkthrough-logger.sh" \
    "$ROOT/fp-walkthrough-prepare.sh" \
    "$ROOT/fp-walkthrough-enroll.sh" \
    "$ROOT/fp-walkthrough-score.sh" \
    "$ROOT/steps.json" "$ROOT/fp-walkthrough" "$DEST/"
  sudo chmod 755 "$DEST/fp-walkthrough" "$DEST/fp-walkthrough-logger.sh" \
    "$DEST/fp-walkthrough-prepare.sh" "$DEST/fp-walkthrough-enroll.sh" \
    "$DEST/fp-walkthrough-score.sh" "$DEST/fp-walkthrough.py"
  sudo ln -sf "$DEST/fp-walkthrough" /usr/local/bin/fp-walkthrough
  sudo ln -sf "$DEST/fp-walkthrough-prepare.sh" /usr/local/bin/fp-walkthrough-prepare
  sudo cp -a "$ROOT/org.fp5.fingerprint-walkthrough.desktop" \
    /usr/local/share/applications/
  cp -a "$ROOT/org.fp5.fingerprint-walkthrough.desktop" \
    /home/user/.local/share/applications/ 2>/dev/null || true
  echo "Installed. Open app drawer → «FP Fingerprint Lab» or: fp-walkthrough"
  exit 0
fi

echo "Installing walkthrough + prepare to $FP5_SSH ..."
ssh -o BatchMode=yes "$FP5_SSH" 'mkdir -p /tmp/fp-walkthrough-install'
scp -o BatchMode=yes \
  "$ROOT/fp-walkthrough.py" \
  "$ROOT/fp-walkthrough-logger.sh" \
  "$ROOT/fp-walkthrough-prepare.sh" \
  "$ROOT/fp-walkthrough-enroll.sh" \
  "$ROOT/fp-walkthrough-cue.sh" \
  "$ROOT/fp-walkthrough-score.sh" \
  "$ROOT/steps.json" \
  "$ROOT/fp-walkthrough" \
  "$ROOT/org.fp5.fingerprint-walkthrough.desktop" \
  "$FP5_SSH:/tmp/fp-walkthrough-install/"

KO="$(find_ko || true)"
if [ -n "$KO" ]; then
  echo "Shipping life module: $KO"
  scp -o BatchMode=yes "$KO" "$FP5_SSH:/tmp/focaltech_fp_life.ko"
else
  echo "NOTE: no local focaltech_fp_life.ko — device may already have /lib/modules/.../extra/"
fi

# Also ship PATH doc for on-device reference
if [ -f "$ROOT/../PATH-TO-SENSOR.md" ]; then
  scp -o BatchMode=yes "$ROOT/../PATH-TO-SENSOR.md" "$FP5_SSH:/tmp/PATH-TO-SENSOR.md" || true
fi

ssh -o BatchMode=yes "$FP5_SSH" 'sudo -n sh -c "
  set -e
  DEST=/usr/local/share/fp5-fp-walkthrough
  KVER=\$(uname -r)
  mkdir -p \$DEST /usr/local/bin /usr/local/share/applications /home/user/.local/share/applications
  mkdir -p /lib/modules/\$KVER/extra /usr/local/share/doc/fp5
  cp -a /tmp/fp-walkthrough-install/* \$DEST/
  chmod 755 \$DEST/fp-walkthrough \$DEST/fp-walkthrough-logger.sh \
    \$DEST/fp-walkthrough-prepare.sh \$DEST/fp-walkthrough-score.sh \
    \$DEST/fp-walkthrough-enroll.sh \$DEST/fp-walkthrough-cue.sh \
    \$DEST/fp-walkthrough.py
  ln -sf \$DEST/fp-walkthrough /usr/local/bin/fp-walkthrough
  ln -sf \$DEST/fp-walkthrough-prepare.sh /usr/local/bin/fp-walkthrough-prepare
  cp -a \$DEST/org.fp5.fingerprint-walkthrough.desktop /usr/local/share/applications/
  cp -a \$DEST/org.fp5.fingerprint-walkthrough.desktop /home/user/.local/share/applications/
  chown user:user /home/user/.local/share/applications/org.fp5.fingerprint-walkthrough.desktop
  if [ -f /tmp/focaltech_fp_life.ko ]; then
    install -m 644 /tmp/focaltech_fp_life.ko /lib/modules/\$KVER/extra/
    cp -a /tmp/focaltech_fp_life.ko \$DEST/ 2>/dev/null || true
  fi
  if [ -f /tmp/PATH-TO-SENSOR.md ]; then
    install -m 644 /tmp/PATH-TO-SENSOR.md /usr/local/share/doc/fp5/
    cp -a /tmp/PATH-TO-SENSOR.md \$DEST/ 2>/dev/null || true
  fi
  if [ -d /etc/sudoers.d ]; then
    cat > /etc/sudoers.d/fp-walkthrough <<EOF
# FP lab walkthrough: logger + sensor prepare + opt-in enroll (passwordless)
user ALL=(root) NOPASSWD: \$DEST/fp-walkthrough-logger.sh
user ALL=(root) NOPASSWD: \$DEST/fp-walkthrough-prepare.sh
user ALL=(root) NOPASSWD: \$DEST/fp-walkthrough-enroll.sh
user ALL=(root) NOPASSWD: /usr/local/bin/fp-walkthrough-prepare
EOF
    chmod 440 /etc/sudoers.d/fp-walkthrough
  fi
  update-desktop-database /usr/local/share/applications 2>/dev/null || true
  # Smoke prepare once
  \$DEST/fp-walkthrough-prepare.sh all || true
  echo INSTALLED
  ls -la \$DEST /usr/local/bin/fp-walkthrough /lib/modules/\$KVER/extra/focaltech_fp_life.ko 2>/dev/null
  grep -E \"focaltech_fp|msmgpio[[:space:]]+34\" /proc/interrupts || true
"'
echo "Done. On phone: app drawer → FP Fingerprint Lab (Start loads driver)."
