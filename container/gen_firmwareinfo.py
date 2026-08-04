#!/usr/bin/env python3
"""Regenerate firmwareInfo for the repacked FwPkt.

The device recomputes MD5s on-board (getFwInfo.sh -> crcInfo) and string-compares
each 'X MD5:' field against firmwareInfo. So firmwareInfo must carry the true
MD5 (and size) of every shipped file. We preserve the stock line ORDER/format and
only the values change (in practice just appfs).

    gen_firmwareinfo.py <stock_firmwareInfo> <FwPkt_dir>   > new_firmwareInfo
"""
import hashlib, os, re, sys

def md5(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()

# component key -> path within FwPkt (globbed for gimbal)
def resolve(key, root):
    m = {
        "config":     "camera/config",
        "uImage":     "camera/uImage",
        "rootfs":     "camera/rootfs.ubifs",
        "appfs":      "camera/appfs.ubifs",
    }
    if key in m:
        return os.path.join(root, m[key])
    if key.startswith("polaris") or key == "oms":
        import glob
        g = glob.glob(os.path.join(root, "gimbal", key + "_*.bin"))
        return g[0] if g else None
    return None

def main():
    stock, root = sys.argv[1], sys.argv[2]
    out = []
    for line in open(stock, "r"):
        raw = line.rstrip("\n")
        m = re.match(r"^(\w+)\s+size:(\d+);\1\s+MD5:([0-9a-fA-F]+);", raw)
        if not m:
            out.append(raw)          # preserve blank/other lines verbatim
            continue
        key = m.group(1)
        p = resolve(key, root)
        if p and os.path.exists(p):
            out.append("%s size:%d;%s MD5:%s;" % (key, os.path.getsize(p), key, md5(p)))
        else:
            out.append(raw)          # keep stock line if file not present
    sys.stdout.write("\n".join(out) + "\n")

if __name__ == "__main__":
    main()
