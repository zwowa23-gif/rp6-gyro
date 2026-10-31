#!/bin/sh
# Health check: prints what works and what to do about what does not.
ok() { printf '  ok    %s\n' "$1"; }
bad() { printf '  FAIL  %s\n        -> %s\n' "$1" "$2"; FAILED=1; }
FAILED=0
DIR=$(sed -n 's/^RP6_GYRO_DIR=//p' /etc/rp6-gyro.env 2>/dev/null)
echo "RP6 Gyro doctor"
[ -n "$DIR" ] && ok "install dir $DIR" || bad "/etc/rp6-gyro.env missing" "run install.sh again"
[ -n "$DIR" ] && { [ -d "$DIR" ] && ok "install dir present" || bad "$DIR not found" "mount the SD card / run install.sh"; }
[ -e /dev/fastrpc-adsp ] && ok "/dev/fastrpc-adsp" || bad "/dev/fastrpc-adsp missing" "kernel without ADSP FastRPC node"
for u in rp6-hexagonrpcd rp6-deckproxy; do
	[ "$(systemctl is-active $u)" = active ] && ok "$u running" || bad "$u not running" "journalctl -u $u -b; systemctl restart $u"
done
[ "$(systemctl --user is-active rp6-motion 2>/dev/null)" = active ] && ok "rp6-motion (IMU) running" || \
	bad "rp6-motion not running" "systemctl --user status rp6-motion; podman image exists? (podman images | grep rp6ssc)"
S=/run/rp6-gyro/status.json
if [ -f "$S" ]; then
	python3 - "$S" <<'PY'
import json, sys, time
d = json.load(open(sys.argv[1]))
age = time.time() - d["time"]
print("  %s  daemon status %.0f s old, IMU %.0f Hz, reports %.0f Hz, game %s, mode %s" % (
    "ok  " if age < 8 else "FAIL", age, d["imu_hz"], d["reports_hz"], d.get("appid") or "-", d.get("mode")))
if d["imu_hz"] < 10:
    print("        -> sensors silent: reboot (hexagonrpcd must run while the ADSP boots); check journalctl -u rp6-hexagonrpcd")
PY
else
	bad "no /run/rp6-gyro/status.json" "rp6-deckproxy is not running or cannot find the controller (28de:12f0 from InputPlumber)"
fi
ls /dev/hidraw* >/dev/null 2>&1 && grep -l 'rp6-gyro-proxy' /sys/class/hidraw/*/device/uevent >/dev/null 2>&1 && ok "proxy controller visible to Steam" || \
	bad "proxy controller not found" "systemctl restart rp6-deckproxy; is InputPlumber using the deck-uhid target?"
[ -f "$HOME/homebrew/plugins/rp6-gyro/main.py" ] && ok "Decky plugin installed" || bad "Decky plugin missing" "install.sh copies it; restart plugin_loader"
exit $FAILED
