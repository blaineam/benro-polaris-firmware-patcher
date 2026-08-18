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
#     LAT / LON                 observer position; REQUIRED for the mount hint
#     ASTRO=/app/astro          binaries
#     INDEXES=/app/sd/astrometry   index files (on the microSD)
#     SENSOR_MM=36              full frame
#  Override any of them with environment variables.
#
#  A position hint is worth ~50x on a narrow field -- MEASURED ON THE DEVICE:
#  a 4.5 deg field (400mm on full frame) solves in 7.3 s with a 15 deg hint and
#  361.7 s without. So if no hint is passed we ASK THE MOUNT where it thinks it
#  is pointing and use that; the mount always knows roughly, even unaligned.
#  Pass MOUNT_HINT=0 to disable, or give ra/dec/radius explicitly.
# ============================================================================
ASTRO=${ASTRO:-/app/astro}
INDEXES=${INDEXES:-/app/sd/astrometry}
SENSOR_MM=${SENSOR_MM:-36}
DOWNSAMPLE=${DOWNSAMPLE:-4}
MAXSTARS=${MAXSTARS:-200}
# never let one frame block the loop; a mismatched hint can grind for minutes
SOLVE_TIMEOUT=${SOLVE_TIMEOUT:-45}

IMG="$1"
FOCAL="${2:-}"
RA="${3:-}"; DEC="${4:-}"; RADIUS="${5:-}"

[ -n "$IMG" ] || { echo "usage: $0 <image.jpg> [focal_mm] [ra dec radius]" >&2; exit 2; }
[ -f "$IMG" ] || { echo "no such image: $IMG" >&2; exit 2; }
[ -x "$ASTRO/polaris-extract" ] || { echo "missing $ASTRO/polaris-extract" >&2; exit 2; }

# ---- derive a pointing hint from the mount if we were not given one --------
MOUNT_HINT=${MOUNT_HINT:-1}
MOUNT_HOST=${MOUNT_HOST:-127.0.0.1}
MOUNT_PORT=${MOUNT_PORT:-9090}
HINT_RADIUS=${HINT_RADIUS:-20}
if [ -z "$RA" ] && [ "$MOUNT_HINT" = "1" ] && [ -n "${LAT:-}" ] && [ -n "${LON:-}" ] \
   && [ -x "$ASTRO/polaris-mount" ]; then
    POSE=$("$ASTRO/polaris-mount" --host "$MOUNT_HOST" --port "$MOUNT_PORT" \
           --lat "$LAT" --lon "$LON" pose 2>/dev/null)
    RA=$(echo "$POSE" | sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p')
    DEC=$(echo "$POSE" | sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p')
    if [ -n "$RA" ] && [ -n "$DEC" ]; then
        RADIUS=$HINT_RADIUS
        echo "[polaris-align] hint from mount: ra=$RA dec=$DEC r=${RADIUS}deg" >&2
    else
        RA=""; DEC=""
        echo "[polaris-align] no pose from the mount; solving blind (slow at long focal lengths)" >&2
    fi
fi

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

set -- $IDXARGS --stars "$STARS" --width "$W" --height "$H" --cpulimit "$SOLVE_TIMEOUT"
[ -n "$FOCAL" ] && set -- "$@" --focal-mm "$FOCAL" --sensor-mm "$SENSOR_MM"
[ -n "$RA" ] && [ -n "$DEC" ] && [ -n "$RADIUS" ] && \
    set -- "$@" --ra "$RA" --dec "$DEC" --radius "$RADIUS"

exec "$ASTRO/polaris-solve" "$@"
