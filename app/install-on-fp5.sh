#!/bin/sh
# Install FP Fingerprint (multi-step) on the phone.
# Host: ./lab/fingerprint/app/install-on-fp5.sh
set -eu
FP5_SSH="${FP5_SSH:-fp5-pmos}"
ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO=$(CDPATH= cd -- "$ROOT/../../.." && pwd)
WALK="$REPO/lab/fingerprint/walkthrough"
LIB=/usr/local/lib/fp5-fp
BIN=/usr/local/bin
SHARE=/usr/local/share/fp5-fp-walkthrough
APP=/usr/local/share/applications

ko_vermagic() {
	modinfo -F vermagic "$1" 2>/dev/null | awk '{print $1}'
}

find_ko() {
	name=$1
	want=${FP5_KVER:-}
	for k in \
		${want:+"$REPO/artifacts/fp-modules-$want/$name"} \
		${want:+"$REPO/artifacts/qsee-modules-$want/$name"} \
		"$REPO/artifacts/fp-modules/$name" \
		"$REPO/artifacts/qsee-modules/$name" \
		"$REPO/lab/fingerprint/oot-fp/$name" \
		"$REPO/lab/fingerprint/oot-qsee/$name" \
		"/tmp/fp-oot-fp/$name" \
		"/tmp/fp-oot-qsee/$name"
	do
		[ -n "$k" ] && [ -f "$k" ] || continue
		if [ -n "$want" ]; then
			need=$(ko_vermagic "$k")
			[ "$need" = "$want" ] || continue
		fi
		echo "$k"
		return 0
	done
	return 1
}

install_local() {
	DEST=$SHARE
	sudo mkdir -p "$DEST" "$LIB" "$BIN" "$APP" \
		/home/user/.local/share/applications /home/user/fp-logs
	sudo cp -a "$ROOT/fp5_fp_status.py" "$ROOT/fp5-fp-lab.py" "$LIB/"
	sudo install -m755 "$ROOT/fp5-fp-session.sh" "$BIN/fp5-fp-session"
	sudo install -m755 "$ROOT/fp5-fp-kill.sh" "$BIN/fp5-fp-kill"
	sudo install -m644 "$ROOT/org.fp5.fp.desktop" "$APP/org.fp5.fp.desktop"
	cp -a "$ROOT/org.fp5.fp.desktop" /home/user/.local/share/applications/ 2>/dev/null || true
	# Walkthrough helpers (prepare/enroll/logger/cue)
	if [ -d "$WALK" ]; then
		sudo cp -a "$WALK/fp-walkthrough-prepare.sh" "$WALK/fp-walkthrough-enroll.sh" \
			"$WALK/fp-walkthrough-logger.sh" "$WALK/fp-walkthrough-score.sh" \
			"$WALK/fp-walkthrough-cue.sh" "$DEST/" 2>/dev/null || true
		sudo chmod 755 "$DEST"/fp-walkthrough-*.sh 2>/dev/null || true
	fi
	sudo tee "$BIN/fp5-fp-lab" >/dev/null <<'EOF'
#!/bin/sh
export PYTHONUNBUFFERED=1
export PYTHONPATH="/usr/local/lib/fp5-fp${PYTHONPATH:+:$PYTHONPATH}"
if [ -z "$XDG_RUNTIME_DIR" ] && [ -d /run/user/10000 ]; then
  export XDG_RUNTIME_DIR=/run/user/10000
fi
if [ -z "$WAYLAND_DISPLAY" ] && [ -S "${XDG_RUNTIME_DIR:-}/wayland-0" ]; then
  export WAYLAND_DISPLAY=wayland-0
fi
exec python3 /usr/local/lib/fp5-fp/fp5-fp-lab.py "$@"
EOF
	sudo chmod 755 "$BIN/fp5-fp-lab"
	KVER=$(uname -r)
	FP5_KVER=${FP5_KVER:-$KVER}
	sudo mkdir -p "/lib/modules/$KVER/extra"
	for name in focaltech_fp_life.ko qsee_fingerpr.ko qsee_probe_fp5.ko; do
		k=$(find_ko "$name" || true)
		if [ -z "$k" ]; then
			continue
		fi
		sudo cp -a "$k" "$DEST/"
		need=$(modinfo -F vermagic "$k" 2>/dev/null | awk '{print $1}' || true)
		if [ -n "$need" ] && [ "$need" = "$KVER" ]; then
			sudo install -m644 "$k" "/lib/modules/$KVER/extra/$name"
		else
			echo "NOTE: not installing $name into extra/ (vermagic ${need:-?} vs $KVER)"
		fi
	done
	if [ -d /etc/sudoers.d ]; then
		sudo tee /etc/sudoers.d/fp5-fp >/dev/null <<'EOF'
# FP fingerprint lab: session + kill only
user ALL=(root) NOPASSWD: /usr/local/bin/fp5-fp-session, /usr/local/bin/fp5-fp-kill
EOF
		sudo chmod 440 /etc/sudoers.d/fp5-fp
	fi
	# Hide the old walkthrough icon (FP Fingerprint Lab).
	sudo rm -f "$APP/org.fp5.fingerprint-walkthrough.desktop" \
		/home/user/.local/share/applications/org.fp5.fingerprint-walkthrough.desktop
	echo "Installed. Drawer → FP Fingerprint  (or fp5-fp-lab)"
}

if [ "$(uname -n 2>/dev/null)" = "fairphone-fp5" ] || [ -f /etc/deviceinfo ]; then
	install_local
	exit 0
fi

echo "Installing FP Fingerprint to $FP5_SSH ..."
FP5_KVER=${FP5_KVER:-$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$FP5_SSH" uname -r)}
export FP5_KVER
echo "Phone running $FP5_KVER"
ssh -o BatchMode=yes "$FP5_SSH" 'mkdir -p /tmp/fp5-fp-install'
scp -o BatchMode=yes \
	"$ROOT/fp5_fp_status.py" \
	"$ROOT/fp5-fp-lab.py" \
	"$ROOT/fp5-fp-session.sh" \
	"$ROOT/fp5-fp-kill.sh" \
	"$ROOT/org.fp5.fp.desktop" \
	"$FP5_SSH:/tmp/fp5-fp-install/"
# Helpers + modules
if [ -d "$WALK" ]; then
	scp -o BatchMode=yes \
		"$WALK/fp-walkthrough-prepare.sh" \
		"$WALK/fp-walkthrough-enroll.sh" \
		"$WALK/fp-walkthrough-logger.sh" \
		"$WALK/fp-walkthrough-score.sh" \
		"$WALK/fp-walkthrough-cue.sh" \
		"$FP5_SSH:/tmp/fp5-fp-install/" || true
fi
if ! find_ko focaltech_fp_life.ko >/dev/null; then
	echo "FAIL: no focaltech_fp_life.ko matching $FP5_KVER"
	exit 1
fi
for name in focaltech_fp_life.ko qsee_fingerpr.ko qsee_probe_fp5.ko; do
	k=$(find_ko "$name" || true)
	if [ -n "$k" ]; then
		echo "Shipping $name  ($(ko_vermagic "$k")  $k)"
		scp -o BatchMode=yes "$k" "$FP5_SSH:/tmp/fp5-fp-install/$name"
	else
		echo "NOTE: no $name matching $FP5_KVER"
	fi
done

ssh -o BatchMode=yes "$FP5_SSH" 'sudo -n sh -s' <<'REMOTE'
set -e
LIB=/usr/local/lib/fp5-fp
BIN=/usr/local/bin
SHARE=/usr/local/share/fp5-fp-walkthrough
APP=/usr/local/share/applications
KVER=$(uname -r)
mkdir -p "$LIB" "$BIN" "$SHARE" "$APP" \
	/home/user/.local/share/applications /home/user/fp-logs \
	/lib/modules/$KVER/extra
chown user:user /home/user/fp-logs 2>/dev/null || true
cp -a /tmp/fp5-fp-install/fp5_fp_status.py /tmp/fp5-fp-install/fp5-fp-lab.py "$LIB/"
install -m755 /tmp/fp5-fp-install/fp5-fp-session.sh "$BIN/fp5-fp-session"
install -m755 /tmp/fp5-fp-install/fp5-fp-kill.sh "$BIN/fp5-fp-kill"
install -m644 /tmp/fp5-fp-install/org.fp5.fp.desktop "$APP/org.fp5.fp.desktop"
cp -a "$APP/org.fp5.fp.desktop" /home/user/.local/share/applications/
chown user:user /home/user/.local/share/applications/org.fp5.fp.desktop
for s in fp-walkthrough-prepare.sh fp-walkthrough-enroll.sh fp-walkthrough-logger.sh \
	fp-walkthrough-score.sh fp-walkthrough-cue.sh; do
	if [ -f /tmp/fp5-fp-install/$s ]; then
		install -m755 /tmp/fp5-fp-install/$s "$SHARE/$s"
	fi
done
for name in focaltech_fp_life.ko qsee_fingerpr.ko qsee_probe_fp5.ko; do
	if [ -f /tmp/fp5-fp-install/$name ]; then
		cp -a /tmp/fp5-fp-install/$name "$SHARE/"
		need=$(modinfo -F vermagic /tmp/fp5-fp-install/$name 2>/dev/null | awk '{print $1}' || true)
		if [ -n "$need" ] && [ "$need" = "$KVER" ]; then
			install -m644 /tmp/fp5-fp-install/$name /lib/modules/$KVER/extra/$name
		else
			echo "NOTE: not installing $name into extra/ (vermagic ${need:-?} vs $KVER)"
		fi
	fi
done
cat >"$BIN/fp5-fp-lab" <<'EOF'
#!/bin/sh
export PYTHONUNBUFFERED=1
export PYTHONPATH="/usr/local/lib/fp5-fp${PYTHONPATH:+:$PYTHONPATH}"
if [ -z "$XDG_RUNTIME_DIR" ] && [ -d /run/user/10000 ]; then
  export XDG_RUNTIME_DIR=/run/user/10000
fi
if [ -z "$WAYLAND_DISPLAY" ] && [ -S "${XDG_RUNTIME_DIR:-}/wayland-0" ]; then
  export WAYLAND_DISPLAY=wayland-0
fi
exec python3 /usr/local/lib/fp5-fp/fp5-fp-lab.py "$@"
EOF
chmod 755 "$BIN/fp5-fp-lab"
if [ -d /etc/sudoers.d ]; then
	printf '%s\n' \
		'user ALL=(root) NOPASSWD: /usr/local/bin/fp5-fp-session, /usr/local/bin/fp5-fp-kill' \
		>/etc/sudoers.d/fp5-fp
	chmod 440 /etc/sudoers.d/fp5-fp
fi
rm -f "$APP/org.fp5.fingerprint-walkthrough.desktop" \
	/home/user/.local/share/applications/org.fp5.fingerprint-walkthrough.desktop
update-desktop-database "$APP" 2>/dev/null || true
echo INSTALLED
ls -l "$BIN/fp5-fp-lab" "$BIN/fp5-fp-session" "$BIN/fp5-fp-kill" \
	/lib/modules/$KVER/extra/focaltech_fp_life.ko \
	/lib/modules/$KVER/extra/qsee_fingerpr.ko 2>/dev/null || true
REMOTE
echo "Done. On phone: app drawer → FP Fingerprint"
echo "Start → Register → wait for flash → hold finger on power-button sensor. Close unloads."
