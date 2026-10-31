#!/bin/sh
# Runs inside docker.io/library/debian:trixie-slim (arm64). /work = install dir, /repo = this repository (read-only).
# usage: in-container.sh hexagonrpc|libssc|imu|daemon|aimlab
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
case "$1" in
hexagonrpc)
	apt-get install -y -qq meson ninja-build gcc pkg-config >/dev/null
	rm -rf /work/out/build-hrpc
	meson setup /work/out/build-hrpc /work/src/hexagonrpc --prefix=/work/out/prefix --buildtype=release >/dev/null
	ninja -C /work/out/build-hrpc >/dev/null && ninja -C /work/out/build-hrpc install >/dev/null
	ls /work/out/prefix/bin/hexagonrpcd ;;
libssc)
	apt-get install -y -qq meson ninja-build gcc pkg-config libglib2.0-dev libqmi-glib-dev libprotobuf-c-dev \
		protobuf-c-compiler protobuf-compiler python3-protobuf python3-dev libpython3-dev >/dev/null
	rm -rf /work/out/ssc-build
	meson setup /work/out/ssc-build /work/src/libssc --prefix=/work/out/ssc-prefix >/dev/null
	ninja -C /work/out/ssc-build >/dev/null && ninja -C /work/out/ssc-build install >/dev/null
	ls /work/out/ssc-prefix/lib/*/libssc* ;;
imu)
	apt-get install -y -qq gcc pkg-config libglib2.0-dev libqmi-glib-dev libprotobuf-c-dev >/dev/null
	L=$(echo /work/out/ssc-prefix/lib/*-linux-gnu)
	gcc -O2 -Wall -o /work/out/rp6imu /repo/imu/rp6imu.c -I/work/src/libssc/src -I/work/out/ssc-build/src \
		$(pkg-config --cflags --libs glib-2.0 gobject-2.0 gio-2.0) -L"$L" -lssc -Wl,-rpath,"$L"
	ls /work/out/rp6imu ;;
daemon)
	apt-get install -y -qq gcc libc6-dev linux-libc-dev >/dev/null
	gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -o /work/out/rp6gyrod /repo/daemon/rp6gyrod.c -lm
	ls /work/out/rp6gyrod ;;
aimlab)
	apt-get install -y -qq gcc libc6-dev libsdl2-dev libfreetype-dev pkg-config >/dev/null
	gcc -O2 -Wall -Wno-misleading-indentation -DDATA_DIR=\""${DATA_DIR:?}"\" -o /work/out/rp6aim /repo/aimlab/rp6aim.c \
		$(pkg-config --cflags --libs sdl2 freetype2) -lm
	ls /work/out/rp6aim ;;
*)	echo "unknown target $1" >&2; exit 1 ;;
esac
