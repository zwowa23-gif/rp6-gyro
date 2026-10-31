#!/bin/sh
# RP6 Gyro installer for the Retroid Pocket 6 running the Armada Linux image.
#
#   sudo ./install.sh [--prefix DIR] [--user NAME] [--skip-build] [--skip-extract]
#
# --prefix   where sources, binaries, profiles and the extracted Android data live (default /var/lib/rp6-gyro;
#            a directory on the SD card works too, the services wait for the mount)
# --user     the desktop user (default: the user that ran sudo)
# Idempotent: running it again updates the installed files and keeps your settings and profiles.
set -e
PREFIX=/var/lib/rp6-gyro; SKIP_BUILD=0; SKIP_EXTRACT=0; USER_NAME=${SUDO_USER:-}
while [ $# -gt 0 ]; do
	case "$1" in
	--prefix) PREFIX=$2; shift ;;
	--user) USER_NAME=$2; shift ;;
	--skip-build) SKIP_BUILD=1 ;;
	--skip-extract) SKIP_EXTRACT=1 ;;
	-h|--help) sed -n '2,10p' "$0"; exit 0 ;;
	*) echo "unknown option $1" >&2; exit 1 ;;
	esac
	shift
done
HERE=$(dirname "$(readlink -f "$0")")
die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || die "cannot tell the desktop user; pass --user NAME"
UID_N=$(id -u "$USER_NAME"); HOME_D=$(getent passwd "$USER_NAME" | cut -d: -f6)
[ "$(uname -m)" = aarch64 ] || die "aarch64 only"
for t in podman git python3 systemctl; do command -v $t >/dev/null || die "$t is required"; done
[ -e /dev/fastrpc-adsp ] || die "/dev/fastrpc-adsp missing: this needs the RP6 kernel with the ADSP FastRPC node"
[ -b /dev/disk/by-partlabel/super ] && [ -b /dev/disk/by-partlabel/persist ] || [ "$SKIP_EXTRACT" = 1 ] || \
	die "Android super/persist partitions not found; keep the Android slot, or copy the data by hand and use --skip-extract"
case "$PREFIX" in /*) ;; *) die "--prefix must be absolute" ;; esac

echo "== directories ($PREFIX)"
mkdir -p "$PREFIX/motion/profiles" "$PREFIX/aimlab" "$PREFIX/qcom" "$PREFIX/out" "$PREFIX/src"
chown -R "$USER_NAME": "$PREFIX"

if [ "$SKIP_BUILD" = 0 ]; then
	echo "== building (podman, takes a while)"
	runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR=/run/user/$UID_N sh "$HERE/build/build-all.sh" "$PREFIX"
fi
if [ "$SKIP_EXTRACT" = 0 ]; then
	echo "== extracting sensor data from the Android partitions"
	sh "$HERE/tools/extract-android-data.sh" "$PREFIX/qcom"
	chown -R "$USER_NAME": "$PREFIX/qcom"
fi

echo "== configuration"
[ -f "$PREFIX/motion/proxy.conf" ] || cp "$HERE/conf/proxy.conf.example" "$PREFIX/motion/proxy.conf"
[ -f "$PREFIX/motion/imu.env" ] || cp "$HERE/conf/imu.env" "$PREFIX/motion/imu.env"
[ -f "$PREFIX/aimlab/prefs.conf" ] || cp "$HERE/aimlab/prefs.conf.example" "$PREFIX/aimlab/prefs.conf"
chown "$USER_NAME": "$PREFIX/motion/proxy.conf" "$PREFIX/motion/imu.env" "$PREFIX/aimlab/prefs.conf"
printf 'RP6_GYRO_DIR=%s\n' "$PREFIX" > /etc/rp6-gyro.env

echo "== services"
for u in rp6-hexagonrpcd rp6-deckproxy; do sed "s#@PREFIX@#$PREFIX#g" "$HERE/systemd/$u.service" > /etc/systemd/system/$u.service; done
install -m 644 "$HERE/udev/99-rp6-gyro-proxy.rules" /etc/udev/rules.d/
UDIR=$HOME_D/.config/systemd/user
mkdir -p "$UDIR"; sed "s#@PREFIX@#$PREFIX#g" "$HERE/systemd/rp6-motion.service" > "$UDIR/rp6-motion.service"
chown -R "$USER_NAME": "$HOME_D/.config/systemd"
systemctl daemon-reload; udevadm control --reload
loginctl enable-linger "$USER_NAME"
systemctl enable rp6-hexagonrpcd rp6-deckproxy
runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR=/run/user/$UID_N systemctl --user daemon-reload || true
runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR=/run/user/$UID_N systemctl --user enable rp6-motion || \
	echo "note: enable the user service after the first login: systemctl --user enable --now rp6-motion"

echo "== Decky plugin"
PL=$HOME_D/homebrew/plugins/rp6-gyro
if [ -d "$HOME_D/homebrew/plugins" ]; then
	rm -rf "$PL"; mkdir -p "$PL"; cp -r "$HERE/decky/rp6-gyro/." "$PL/"
	systemctl restart plugin_loader 2>/dev/null || echo "restart Decky (plugin_loader) to load the plugin"
else
	echo "Decky Loader not found; copy decky/rp6-gyro to <homebrew>/plugins/rp6-gyro later"
fi

cat <<MSG

Installed. REBOOT NOW: the ADSP sensor firmware must find the file server (hexagonrpcd) while it boots.
Afterwards run: $HERE/tools/doctor.sh
Add $PREFIX/out/rp6aim to Steam as a non-Steam game named "RP6 Aim Lab" for the full calibration.
MSG
