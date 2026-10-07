#!/bin/sh
# Install Phosh Settings / PAM / lock-watcher glue. Root on the phone.
set -eu
cd "$(dirname "$0")"
lib=/usr/local/lib/fp5-fp
bin=/usr/local/bin

install -d "$lib" /etc/pam.d /etc/dbus-1/system.d \
  /usr/local/share/dbus-1/system-services \
  /etc/systemd/system /etc/systemd/user \
  /etc/xdg/autostart /var/lib/fp5-fp/templates

install -m 755 fprintd/fp5-fprintd.py "$bin/fp5-fprintd"
install -m 755 fprintd/fp5-lock-watcher.py "$bin/fp5-lock-watcher"
install -m 644 fprintd/fp5-fprintd.conf /etc/dbus-1/system.d/fp5-fprintd.conf
install -m 644 fprintd/net.reactivated.Fprint.service \
  /usr/local/share/dbus-1/system-services/net.reactivated.Fprint.service
# Win over distro fprintd activation:
install -m 644 fprintd/net.reactivated.Fprint.service \
  /usr/share/dbus-1/system-services/net.reactivated.Fprint.service
install -m 644 fprintd/fp5-fprintd.service /etc/systemd/system/fp5-fprintd.service
install -m 644 fprintd/fp5-lock-watcher.service /etc/systemd/user/fp5-lock-watcher.service
install -m 644 fprintd/fp5-lock-watcher.desktop /etc/xdg/autostart/fp5-lock-watcher.desktop
install -m 755 pam/fp5-pam-exec.sh "$lib/fp5-pam-exec.sh"
install -m 644 pam/phosh /etc/pam.d/phosh
install -m 644 pam/sudo /etc/pam.d/sudo
install -m 644 pam/polkit-1 /etc/pam.d/polkit-1

systemctl daemon-reload 2>/dev/null || true
systemctl disable --now fprintd.service 2>/dev/null || true
systemctl mask fprintd.service 2>/dev/null || true
systemctl enable --now fp5-fprintd.service 2>/dev/null || true
# User lock watcher: enable for logged-in user if possible
if [ -n "${SUDO_USER:-}" ]; then
  sudo -u "$SUDO_USER" systemctl --user daemon-reload 2>/dev/null || true
  sudo -u "$SUDO_USER" systemctl --user enable --now fp5-lock-watcher.service 2>/dev/null || true
fi

echo "fp5-fprintd enabled (replaces stock fprintd on D-Bus)."
echo "Settings → Users should list Fairphone 5 power-button (FT9362)."
echo "Lock: touch sensor after lock (watcher). PIN still works."
echo "sudo: fingerprint then password if PAM is used."
