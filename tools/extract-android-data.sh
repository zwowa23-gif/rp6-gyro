#!/bin/sh
# Copies the files hexagonrpcd must serve to the ADSP out of the device's own Android partitions:
#   sensors/config    <- vendor_a:/etc/sensors/config           (SSC sensor configuration)
#   sensors/sns_reg.conf <- vendor_a:/etc/sensors/sns_reg_config
#   sensors/registry  <- persist:/sensors/registry/registry     (calibration/registry written by Android)
#   dsp/adsp          <- vendor_a:/lib/rfsa/adsp                (ADSP user-space libraries, no audio blobs)
#   socinfo           <- fake /sys/devices/soc0 values (the sensors PD checks the platform)
# The files are Qualcomm/Retroid property of your own device and are NOT part of this repository.
# usage: extract-android-data.sh DEST_DIR     (run as root; the partitions are only mounted read-only)
set -e
DEST=${1:?usage: $0 DEST_DIR}
HERE=$(dirname "$(readlink -f "$0")")
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
SUPER=/dev/disk/by-partlabel/super
PERSIST=/dev/disk/by-partlabel/persist
[ -b "$SUPER" ] && [ -b "$PERSIST" ] || { echo "super/persist partitions not found (is this a Retroid Pocket 6 with the Android slot intact?)" >&2; exit 1; }
set -- $(python3 -I "$HERE/lp_extent.py" "$SUPER" vendor_a)
OFF=$1; SIZE=$2
TMP=$(mktemp -d); LOOP=
cleanup() { umount "$TMP/vendor" 2>/dev/null || true; umount "$TMP/persist" 2>/dev/null || true; [ -z "$LOOP" ] || losetup -d "$LOOP"; rm -rf "$TMP"; }
trap cleanup EXIT
mkdir -p "$TMP/vendor" "$TMP/persist"
LOOP=$(losetup -r -f --show -o "$OFF" --sizelimit "$SIZE" "$SUPER")
mount -o ro "$LOOP" "$TMP/vendor"
mount -o ro "$PERSIST" "$TMP/persist"

rm -rf "$DEST/sensors/config" "$DEST/sensors/registry" "$DEST/dsp" "$DEST/socinfo"
mkdir -p "$DEST/sensors" "$DEST/dsp/adsp" "$DEST/socinfo"
cp -r "$TMP/vendor/etc/sensors/config" "$DEST/sensors/config"
cp "$TMP/vendor/etc/sensors/sns_reg_config" "$DEST/sensors/sns_reg.conf"
cp -r "$TMP/persist/sensors/registry/registry" "$DEST/sensors/registry"
# audio firmware blobs (bm3*.bin etc.) are not needed for the sensors
(cd "$TMP/vendor/lib/rfsa/adsp" && find . -maxdepth 1 -type f ! -name 'bm3*' ! -name '*.bin' -exec cp {} "$DEST/dsp/adsp/" \;)
printf '2\n' > "$DEST/sensors/sns_reg_version"
for f in family machine revision serial_number soc_id; do cat /sys/devices/soc0/$f > "$DEST/socinfo/$f" 2>/dev/null || true; done
printf 'HDK\n' > "$DEST/socinfo/hw_platform"; printf 'Unknown\n' > "$DEST/socinfo/platform_subtype"
printf '0\n' > "$DEST/socinfo/platform_subtype_id"; printf '65536\n' > "$DEST/socinfo/platform_version"
echo "extracted: $(ls "$DEST/sensors/registry" | wc -l) registry files, $(ls "$DEST/sensors/config" | wc -l) sensor configs, $(ls "$DEST/dsp/adsp" | wc -l) DSP libraries"
