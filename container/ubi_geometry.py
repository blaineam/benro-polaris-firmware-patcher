#!/usr/bin/env python3
"""Read UBI/UBIFS geometry from a stock appfs.ubifs image and print, on one line:
   MINIO LEB MAXLEB FANOUT COMPR PEB VIDOFF IMAGESEQ
so mkfs.ubifs/ubinize can reproduce it exactly."""
import struct, sys

COMPR = {0: "none", 1: "lzo", 2: "zlib", 3: "zstd"}

def main():
    d = open(sys.argv[1], "rb").read()
    assert d[:4] == b"UBI#", "not a UBI image"
    vid_off  = struct.unpack_from(">I", d, 16)[0]
    data_off = struct.unpack_from(">I", d, 20)[0]
    img_seq  = struct.unpack_from(">I", d, 24)[0]

    # PEB size: distance between consecutive EC headers
    peb = d.find(b"UBI#", 4)
    if peb <= 0:
        peb = 131072

    # UBIFS superblock node (magic 0x06101831), first data LEB
    i = d.find(struct.pack("<I", 0x06101831))
    assert i >= 0, "UBIFS superblock not found"
    o = i + 24                      # skip ubifs_ch common header
    o += 4                          # padding[2]+key_hash+key_fmt
    flags, min_io, leb_size, leb_cnt, max_leb_cnt = struct.unpack_from("<IIIII", d, o); o += 20
    o += 8                          # max_bud_bytes
    log_lebs, lpt_lebs, orph_lebs, jhead_cnt, fanout, lsave_cnt, fmt = \
        struct.unpack_from("<IIIIIII", d, o); o += 28
    default_compr = struct.unpack_from("<H", d, o)[0]

    print(min_io, leb_size, max_leb_cnt, fanout,
          COMPR.get(default_compr, "lzo"), peb, vid_off, img_seq)

if __name__ == "__main__":
    main()
