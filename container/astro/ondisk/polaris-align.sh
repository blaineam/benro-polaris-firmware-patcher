#!/bin/sh
# ============================================================================
#  polaris-align.sh -- ONE frame in, one sky position out. Runs ON THE DEVICE.
#
#     polaris-align.sh <image.jpg> [focal_mm] [ra_hint dec_hint radius_deg]
#
#  Chains our two binaries: polaris-extract (JPEG -> star list) and
#  polaris-solve (star list -> RA/Dec/roll/scale). Prints the solver's JSON on
#  stdout and nothing else, so a caller can parse it.
#
#  Defaults assume the Polaris' own layout:
#     ASTRO=/app/astro          binaries
#     INDEXES=/app/sd/astrometry   index files (on the microSD)
#     SENSOR_MM=36              full frame
#  Override any of them with environment variables.
#
#  A position hint is worth ~50x on a narrow field: pass the mount's rough
#  RA/Dec and a generous radius whenever you have one.
# ============================================================================
ASTRO=${ASTRO:-/app/astro}
INDEXES=${INDEXES:-/app/sd/astrometry}
SENSOR_MM=${SENSOR_MM:-36}
DOWNSAMPLE=${DOWNSAMPLE:-4}
MAXSTARS=${MAXSTARS:-200}

IMG="$1"
FOCAL="${2:-}"
RA="${3:-}"; DEC="${4:-}"; RADIUS="${5:-}"

[ -n "$IMG" ] || { echo "usage: $0 <image.jpg> [focal_mm] [ra dec radius]" >&2; exit 2; }
[ -f "$IMG" ] || { echo "no such image: $IMG" >&2; exit 2; }
[ -x "$ASTRO/polaris-extract" ] || { echo "missing $ASTRO/polaris-extract" >&2; exit 2; }

IDXARGS=""
for f in "$INDEXES"/index-*.fits; do
    [ -f "$f" ] || continue
    IDXARGS="$IDXARGS --index $f"
done
[ -n "$IDXARGS" ] || { echo "no index files in $INDEXES" >&2; exit 2; }

STARS=$(mktemp /tmp/polaris-stars.XXXXXX) || exit 1
trap 'rm -f "$STARS"' EXIT

"$ASTRO/polaris-extract" --jpeg "$IMG" --downsample "$DOWNSAMPLE" \
    --max-stars "$MAXSTARS" --stats > "$STARS" 2>&2 || {
    echo '{"solved":false,"error":"extract failed"}'; exit 3; }

# the extractor records the full-resolution size in a comment; reuse it so the
# scale hint is in full-res pixels even though we detected on a reduced image
W=$(sed -n 's/^# full resolution \([0-9]*\) x .*/\1/p' "$STARS")
H=$(sed -n 's/^# full resolution [0-9]* x \([0-9]*\)/\1/p' "$STARS")
[ -n "$W" ] && [ -n "$H" ] || { echo '{"solved":false,"error":"no image size"}'; exit 3; }

set -- $IDXARGS --stars "$STARS" --width "$W" --height "$H"
[ -n "$FOCAL" ] && set -- "$@" --focal-mm "$FOCAL" --sensor-mm "$SENSOR_MM"
[ -n "$RA" ] && [ -n "$DEC" ] && [ -n "$RADIUS" ] && \
    set -- "$@" --ra "$RA" --dec "$DEC" --radius "$RADIUS"

exec "$ASTRO/polaris-solve" "$@"
