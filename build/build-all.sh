#!/bin/sh
# Fetches the pinned upstream sources, applies the RP6 patches and builds everything in podman containers.
# usage: build-all.sh PREFIX      (as the user that owns PREFIX; needs git and podman)
set -e
PREFIX=${1:?usage: $0 PREFIX}
REPO=$(dirname "$(dirname "$(readlink -f "$0")")")
IMG=docker.io/library/debian:trixie-slim
mkdir -p "$PREFIX/src" "$PREFIX/out"
while read -r name url rev; do
	case "$name" in \#*|"") continue ;; esac
	if [ ! -d "$PREFIX/src/$name/.git" ]; then git clone -q "$url" "$PREFIX/src/$name"; fi
	git -C "$PREFIX/src/$name" fetch -q origin 2>/dev/null || true
	git -C "$PREFIX/src/$name" checkout -q -f "$rev"
	git -C "$PREFIX/src/$name" apply "$REPO/patches/$name-rp6.patch"
	echo "$name @ $rev patched"
done < "$REPO/UPSTREAM"
run() { podman run --rm -e DATA_DIR="$PREFIX/aimlab" -v "$PREFIX":/work:Z -v "$REPO":/repo:ro,Z "$IMG" sh /repo/build/in-container.sh "$1"; }
for t in hexagonrpc libssc imu daemon aimlab; do echo "== building $t"; run "$t"; done
podman build -q -t localhost/rp6ssc -f "$REPO/build/Containerfile" "$REPO/build"
echo "build finished"
