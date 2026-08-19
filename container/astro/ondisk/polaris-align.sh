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

# ---- pixel scale: EXIF first, then config, then a realistic lens range -----
#
# A WRONG FOCAL MAKES THE SOLVE IMPOSSIBLE, NOT SLOW. The scale search is
# derived from it, so a 70 mm frame searched at 400 mm hunts 2.3 arcsec/pix
# when the truth is 13.4 and no quad can ever match. An entire night of frames
# failed this way -- config said 400, the zoom was at 70. Those same frames
# solved in 5 s once the focal was right.
#
# So the frame's own EXIF wins over any configured value: a config number
# cannot follow a zoom ring. If there is no EXIF (live-view frames carry none)
# we use the configured focal, and if there is neither -- or the focal we tried
# does not solve -- we search the whole range of focal lengths that could
# plausibly be on this mount rather than guessing a single wrong one.
EXIF_FOCAL=$(sed -n 's/^# exif focal-mm \([0-9][0-9.]*\).*/\1/p' "$STARS" | head -1)
FOCAL_MIN=${FOCAL_MIN:-8}          # ultra-wide
FOCAL_MAX=${FOCAL_MAX:-3000}       # long telephoto; nothing realistic exceeds this

# The range search is the SAFETY NET, not the fast path: measured 148 s on this
# device versus 5 s with a known focal. It therefore gets its own, longer
# budget -- with the caller's 45 s it would always time out and the net would
# never actually catch anything.
RANGE_TIMEOUT=${RANGE_TIMEOUT:-240}

# LIVE-VIEW FRAMES CARRY NO EXIF, so without this every calibration solve would
# take the 148 s path. The lens does not change between a capture and the live
# view seconds later, so remember the last focal a real frame reported and use
# it for frames that cannot say. If the lens HAS been changed since, that focal
# simply fails to solve and we fall through to the range search anyway.
FOCAL_CACHE=${FOCAL_CACHE:-/app/sd/polaris-astro/last-focal}
if [ -n "$EXIF_FOCAL" ]; then
    mkdir -p "$(dirname "$FOCAL_CACHE")" 2>/dev/null
    printf '%s\n' "$EXIF_FOCAL" > "$FOCAL_CACHE" 2>/dev/null
fi
CACHED_FOCAL=""
[ -z "$EXIF_FOCAL" ] && [ -r "$FOCAL_CACHE" ] && \
    CACHED_FOCAL=$(sed -n 's/^\([0-9][0-9.]*\)$/\1/p' "$FOCAL_CACHE" | head -1)

solve_with() {
    _scale=$1
    set -- $IDXARGS --stars "$STARS" --width "$W" --height "$H" --cpulimit "$SOLVE_TIMEOUT"
    [ -n "$_scale" ] && set -- "$@" $_scale
    [ -n "$RA" ] && [ -n "$DEC" ] && [ -n "$RADIUS" ] && \
        set -- "$@" --ra "$RA" --dec "$DEC" --radius "$RADIUS"
    "$ASTRO/polaris-solve" "$@"
}

# arcsec/pixel bounds implied by the focal range. Long lens -> small scale.
range_args() {
    awk -v s="$SENSOR_MM" -v w="$W" -v fmin="$FOCAL_MIN" -v fmax="$FOCAL_MAX" 'BEGIN{
        pix = s / w;
        lo  = 206264.806 * atan2(pix / fmax, 1) * 0.9;
        hi  = 206264.806 * atan2(pix / fmin, 1) * 1.1;
        printf "--scale-low %.6f --scale-high %.6f", lo, hi }'
}

TRIED=""
if [ -n "$EXIF_FOCAL" ]; then
    if [ -n "$FOCAL" ] && [ "$FOCAL" != "$EXIF_FOCAL" ]; then
        echo "[polaris-align] focal: EXIF says ${EXIF_FOCAL}mm, config says ${FOCAL}mm -- trusting EXIF" >&2
    else
        echo "[polaris-align] focal from EXIF: ${EXIF_FOCAL}mm" >&2
    fi
    TRIED="${EXIF_FOCAL}mm (exif)"
    OUT=$(solve_with "--focal-mm $EXIF_FOCAL --sensor-mm $SENSOR_MM")
    case "$OUT" in *'"solved":true'*) echo "$OUT"; exit 0;; esac
elif [ -n "$CACHED_FOCAL" ]; then
    echo "[polaris-align] no EXIF (live view?) -- using ${CACHED_FOCAL}mm from the last frame that had it" >&2
    TRIED="${CACHED_FOCAL}mm (cached)"
    OUT=$(solve_with "--focal-mm $CACHED_FOCAL --sensor-mm $SENSOR_MM")
    case "$OUT" in *'"solved":true'*) echo "$OUT"; exit 0;; esac
    # a cached focal that no longer solves is worse than none -- drop it so we
    # do not pay for it on every subsequent frame
    rm -f "$FOCAL_CACHE" 2>/dev/null
elif [ -n "$FOCAL" ]; then
    echo "[polaris-align] focal from config: ${FOCAL}mm (frame has no EXIF)" >&2
    TRIED="${FOCAL}mm (config)"
    OUT=$(solve_with "--focal-mm $FOCAL --sensor-mm $SENSOR_MM")
    case "$OUT" in *'"solved":true'*) echo "$OUT"; exit 0;; esac
fi

[ -n "$TRIED" ] && echo "[polaris-align] $TRIED did not solve -- retrying across ${FOCAL_MIN}-${FOCAL_MAX}mm" >&2
[ -n "$TRIED" ] || echo "[polaris-align] no focal known -- searching ${FOCAL_MIN}-${FOCAL_MAX}mm" >&2
exec "$ASTRO/polaris-solve" $(range_args) $IDXARGS --stars "$STARS" \
     --width "$W" --height "$H" --cpulimit "$RANGE_TIMEOUT" \
     ${RA:+--ra "$RA"} ${DEC:+--dec "$DEC"} ${RADIUS:+--radius "$RADIUS"}
