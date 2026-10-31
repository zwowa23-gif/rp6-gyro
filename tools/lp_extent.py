#!/usr/bin/env python3
"""Print "<byte offset> <byte size>" of an Android dynamic partition inside the `super` block device.

usage: lp_extent.py SUPER_DEVICE PARTITION_NAME      (e.g. /dev/disk/by-partlabel/super vendor_a)
Parses the liblp metadata (geometry at 4096, primary metadata slot 0). Only linear extents on the first block
device are supported, which is what super images use.
"""
import struct
import sys

SECTOR = 512
GEOMETRY_MAGIC = 0x616C4467     # "gDLa"
HEADER_MAGIC = 0x414C5030       # "0PLA"


def main():
    dev, name = sys.argv[1], sys.argv[2]
    with open(dev, "rb") as f:
        f.seek(4096)
        geo = f.read(4096)
        magic, _size = struct.unpack_from("<II", geo, 0)
        if magic != GEOMETRY_MAGIC:
            sys.exit("not a super partition (no liblp geometry)")
        max_size, _slots, _logical = struct.unpack_from("<III", geo, 40)
        f.seek(4096 + 2 * 4096)                       # reserved + primary/backup geometry
        meta = f.read(max_size)
    (hmagic, _maj, _min, hsize) = struct.unpack_from("<IHHI", meta, 0)
    if hmagic != HEADER_MAGIC:
        sys.exit("bad liblp metadata header")
    tables_off = hsize
    # table descriptors follow the 2 checksums: partitions, extents, groups, block_devices (offset, count, entry size)
    p_off, p_n, p_sz, e_off, e_n, e_sz = struct.unpack_from("<IIIIII", meta, 12 + 32 + 4 + 32)
    for i in range(p_n):
        pent = meta[tables_off + p_off + i * p_sz: tables_off + p_off + (i + 1) * p_sz]
        pname = pent[:36].split(b"\0")[0].decode()
        first, count = struct.unpack_from("<II", pent, 40)
        if pname != name:
            continue
        if count != 1:
            sys.exit(f"{name}: {count} extents, only a single linear extent is supported")
        eent = meta[tables_off + e_off + first * e_sz: tables_off + e_off + (first + 1) * e_sz]
        sectors, ttype, tdata = struct.unpack_from("<QIQ", eent, 0)
        if ttype != 0:
            sys.exit(f"{name}: not a linear extent")
        print(tdata * SECTOR, sectors * SECTOR)
        return
    sys.exit(f"partition {name} not found")


main()
