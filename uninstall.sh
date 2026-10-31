#!/bin/sh
# Removes services, udev rule and plugin. Keeps the install directory (profiles, builds); delete it by hand if wanted.
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
USER_NAME=${1:-${SUDO_USER:-}}
systemctl disable --now rp6-deckproxy rp6-hexagonrpcd 2>/dev/null || true
rm -f /etc/systemd/system/rp6-deckproxy.service /etc/systemd/system/rp6-hexagonrpcd.service /etc/udev/rules.d/99-rp6-gyro-proxy.rules /etc/rp6-gyro.env
rm -f /run/rp6-gyro-proxy.active
systemctl daemon-reload; udevadm control --reload; udevadm trigger --action=change --subsystem-match=hidraw
if [ -n "$USER_NAME" ]; then
	H=$(getent passwd "$USER_NAME" | cut -d: -f6)
	runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR=/run/user/$(id -u "$USER_NAME") systemctl --user disable --now rp6-motion 2>/dev/null || true
	rm -f "$H/.config/systemd/user/rp6-motion.service"; rm -rf "$H/homebrew/plugins/rp6-gyro"
fi
systemctl restart plugin_loader 2>/dev/null || true
echo "removed; reboot to give the ADSP its original state"
