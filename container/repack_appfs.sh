#!/bin/bash
# Repack a modified appfs tree into a UBI image, reproducing the stock image's
# exact geometry (read from the stock image, never guessed).
#   $1 = stock appfs.ubifs   (for geometry + image_seq)
#   $2 = modified extracted tree root (…/ubifs)
#   $3 = output appfs.ubifs
set -euo pipefail
STOCK="$1"; TREE="$2"; OUT="$3"
mkdir -p "$(dirname "$OUT")"

read MINIO LEB MAXLEB FANOUT COMPR PEB VIDOFF IMGSEQ < <(python3 /opt/patcher/ubi_geometry.py "$STOCK")
echo "[repack] geometry: min_io=$MINIO leb=$LEB max_leb=$MAXLEB fanout=$FANOUT compr=$COMPR peb=$PEB vid_off=$VIDOFF image_seq=$IMGSEQ"

# -F / --space-fixup is REQUIRED for images flashed to an auto-resizing volume
# (our case: the volume grows to fill the 40M/81M partition). Without it the
# image boots ONCE after flashing but wedges on the next reboot, because UBIFS
# never recalculates its free space for the real volume size. The stock images
# set this flag (superblock flags=0x04); we must too.
mkfs.ubifs -r "$TREE" -o /work/out/appfs.img \
  -m "$MINIO" -e "$LEB" -c "$MAXLEB" -f "$FANOUT" -x "$COMPR" --space-fixup >/dev/null

cat > /work/out/ubinize.ini <<EOF
[ubifs]
mode=ubi
image=/work/out/appfs.img
vol_id=0
vol_type=dynamic
vol_name=ubifs
vol_flags=autoresize
EOF
ubinize -o "$OUT" -p "$PEB" -m "$MINIO" -Q "$IMGSEQ" /work/out/ubinize.ini >/dev/null
echo "[repack] wrote $OUT ($(stat -c %s "$OUT") bytes; stock was $(stat -c %s "$STOCK"))"
