#!/bin/sh
# ============================================================================
#  install_astro.sh -- put the plate solver on the device. Runs ON THE DEVICE.
#  Purely additive: it writes /app/astro and touches nothing else. Remove it
#  again with `rm -rf /app/astro`.
#
#     SRC     bundle dir (default: this script's dir)
#     DEST    install dir (default /app/astro)
#     INDEXES where index files should live (default /app/sd/astrometry)
# ============================================================================
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HERE}
DEST=${DEST:-/app/astro}
INDEXES=${INDEXES:-/app/sd/astrometry}

mkdir -p "$DEST" "$INDEXES"
for b in polaris-solve polaris-extract polaris-mount polaris-align.sh; do
    [ -f "$SRC/$b" ] || { echo "[astro] MISSING $SRC/$b" >&2; exit 1; }
    cp "$SRC/$b" "$DEST/$b"
    chmod +x "$DEST/$b"
done
echo "[astro] installed -> $DEST"
echo "[astro] index files are expected in $INDEXES"
ls "$INDEXES"/index-*.fits >/dev/null 2>&1 \
    && echo "[astro] found $(ls "$INDEXES"/index-*.fits | wc -l) index file(s)" \
    || echo "[astro] NO index files yet -- copy them to $INDEXES (see fetch-indexes.sh on the host)"
cat <<EOF

[astro] solve a frame:
    $DEST/polaris-align.sh /app/sd/DCIM/100SPCAM/IMG_0001.JPG 400
    # a rough pointing hint is worth ~10x on a long lens:
    $DEST/polaris-align.sh /app/sd/DCIM/100SPCAM/IMG_0001.JPG 400 <ra> <dec> 20

[astro] talk to the mount (add --dry-run to send nothing):
    $DEST/polaris-mount --lat <deg> --lon <deg> pose
    $DEST/polaris-mount --lat <deg> --lon <deg> align --solved-ra R --solved-dec D
    $DEST/polaris-mount --lat <deg> --lon <deg> goto-radec --ra R --dec D
    $DEST/polaris-mount --lat <deg> --lon <deg> track on
EOF
