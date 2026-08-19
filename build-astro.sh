#!/usr/bin/env sh
# ============================================================================
#  Build the on-device plate solver bundle (macOS / Linux).
#
#     ./build-astro.sh [--focal-min 14] [--focal-max 400] [--sensor-mm 36]
#                      [--out DIR] [--no-indexes] [--image NAME]
#
#  Produces out/astro-bundle/ :
#     COPY-TO-SD-CARD-ROOT/     <- drag the CONTENTS of this onto the microSD
#         astrometry/           index files      -> /app/sd/astrometry
#         polaris-astro/        binaries+scripts -> /app/sd/polaris-astro
#     README.txt                how to install and test it
#
#  Put the card back in the Polaris, run
#  /app/sd/polaris-astro/install_astro.sh once, and point polaris-align.sh at a
#  frame in /app/sd/DCIM/100SPCAM/.
#
#  NOTHING here is flashed. It is purely additive files under /app/astro, and
#  `rm -rf /app/astro` removes it. See docs/PLATE-SOLVING.md.
# ============================================================================
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
FMIN=14; FMAX=400; SENSOR=36; OUT="$HERE/out/astro-bundle"; IMG="polaris-patcher"; WANT_IDX=1

while [ $# -gt 0 ]; do
  case "$1" in
    --focal-min) FMIN="$2"; shift 2;;
    --focal-max) FMAX="$2"; shift 2;;
    --sensor-mm) SENSOR="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --no-indexes) WANT_IDX=0; shift;;
    --image) IMG="$2"; shift 2;;
    -h|--help) sed -n '2,22p' "$0"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 1;;
  esac
done

command -v docker >/dev/null 2>&1 || { echo "error: docker not found." >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: docker daemon not running." >&2; exit 1; }

echo "[*] building docker image '$IMG' (first run only)…"
docker build -q -t "$IMG" -f "$HERE/docker/Dockerfile" "$HERE" >/dev/null

mkdir -p "$OUT"
echo "[*] cross-building polaris-solve + polaris-extract for the device…"
docker run --rm \
  -v "$HERE/container":/opt/patcher:ro \
  -v "$OUT":/out \
  --entrypoint /bin/bash "$IMG" -c \
  'bash /opt/patcher/astro/build_solver.sh arm /work/out/astro && mkdir -p /out/COPY-TO-SD-CARD-ROOT/polaris-astro && arm-linux-gnueabi-gcc -O2 -std=gnu11 -mfloat-abi=soft -Wall -Wextra /opt/patcher/astro/polaris-httpd.c -o /work/out/astro/polaris-httpd -lm && cp /work/out/astro/polaris-solve /work/out/astro/polaris-extract /work/out/astro/polaris-mount /work/out/astro/polaris-httpd /out/COPY-TO-SD-CARD-ROOT/polaris-astro/ && cp /opt/patcher/astro/ondisk/*.sh /out/COPY-TO-SD-CARD-ROOT/polaris-astro/'

if [ "$WANT_IDX" = "1" ]; then
  echo "[*] selecting + downloading index files for ${FMIN}-${FMAX}mm…"
  sh "$HERE/container/astro/fetch-indexes.sh" --focal-min "$FMIN" --focal-max "$FMAX" \
     --sensor-mm "$SENSOR" --out "$OUT/COPY-TO-SD-CARD-ROOT/astrometry"
fi

cat > "$OUT/README.txt" <<EOF
Benro Polaris — on-device plate solver bundle
=============================================
Built for ${FMIN}-${FMAX}mm on a ${SENSOR}mm-wide sensor.

Put it on the device
--------------------
Drag the CONTENTS of COPY-TO-SD-CARD-ROOT/ onto the root of the Polaris'
microSD card, so the card ends up with:

    <SD root>/astrometry/index-41xx.fits
    <SD root>/polaris-astro/polaris-solve, polaris-extract, polaris-mount, *.sh

Put the card back in the Polaris and run once:

    /app/sd/polaris-astro/install_astro.sh

(That copies the three binaries to /app/astro. The index files are read
straight off the card at /app/sd/astrometry — they are never copied into
flash.) Over ssh instead of sneakernet, the same layout works:

    scp -r COPY-TO-SD-CARD-ROOT/. root@<polaris ip>:/app/sd/
    ssh root@<polaris ip> /app/sd/polaris-astro/install_astro.sh

Test it on a real frame
-----------------------
Shoot with the Polaris as usual. Frames land in /app/sd/DCIM/100SPCAM/.

  /app/astro/polaris-align.sh /app/sd/DCIM/100SPCAM/IMG_0001.JPG 400

Pass the mount's rough pointing when you have it — on a long lens it is worth
about 10x:

  /app/astro/polaris-align.sh /app/sd/DCIM/100SPCAM/IMG_0001.JPG 400 <ra_deg> <dec_deg> 20

Output is one JSON line: ra_deg, dec_deg, roll_deg, pixscale_arcsec, field size,
parity, the CD matrix, match odds and how long the solve took.

Tuning
------
  DOWNSAMPLE=2 ...   detect on a bigger image (slower, slightly better centroids)
  MAXSTARS=300 ...   feed the solver more stars
  SENSOR_MM=23.5 ... APS-C body

Moving the mount (simulated first — see docs/PLATE-SOLVING.md)
------------------------------------------------------------
  /app/astro/polaris-mount --lat <deg> --lon <deg> pose
  /app/astro/polaris-mount --lat <deg> --lon <deg> align --solved-ra R --solved-dec D
  /app/astro/polaris-mount --lat <deg> --lon <deg> goto-radec --ra R --dec D
  /app/astro/polaris-mount --lat <deg> --lon <deg> track on

Every motion refuses to run outside a safe envelope (--min-alt/--max-alt,
--max-slew) and alignment refuses within --max-align-alt of the zenith, where
azimuth is degenerate. Add --dry-run to see the exact commands without sending
them.

Removing it
-----------
  rm -rf /app/astro /app/sd/astrometry /app/sd/polaris-astro

Licensing: polaris-solve is built from astrometry.net and is GPL v2-or-later;
polaris-extract, polaris-align.sh and install_astro.sh are MIT. They are
separate programs that talk through files — see docs/LICENSE-AUDIT.md.
EOF

echo
echo "[✓] bundle ready: $OUT"
ls -la "$OUT" | sed 's/^/    /'
echo "    next: read $OUT/README.txt"
