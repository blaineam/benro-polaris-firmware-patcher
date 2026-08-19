#!/bin/sh
# ============================================================================
#  solve-now.sh -- point the mount, run this, get your true sky position.
#  Runs ON THE DEVICE. One shot, one solve, no daemon, no state machine.
#
#     solve-now.sh                 capture a frame, solve it, print position
#     solve-now.sh --apply         ...and push the result to the mount (530)
#     solve-now.sh --frame F.jpg   solve an existing frame instead of capturing
#     solve-now.sh --focal 400     focal length in mm (default 400)
#
#  Requires LAT/LON for the mount hint:  LAT=35.35199 LON=-119.17208 solve-now.sh
#
#  Capture uses opcode 264 (single shot). It does NOT use the 272 step:1/2/3
#  lapse sequence -- with photoCnt:-1 that is an UNLIMITED timelapse and fired
#  28 unwanted frames during development.
# ============================================================================
set -u
ASTRO=${ASTRO:-/app/astro}
# "auto" (the default) means: let polaris-align.sh work the focal out from the
# frame's EXIF, the cached focal, or the range search. A number forces it.
FOCAL=auto; APPLY=0; FRAME=""; WAIT=${CAPTURE_WAIT:-25}

while [ $# -gt 0 ]; do
  case "$1" in
    --apply) APPLY=1; shift;;
    --latest) FRAME="LATEST"; shift;;
    --wait) FRAME="WAIT"; shift;;
    --frame) FRAME="$2"; shift 2;;
    --focal) FOCAL="$2"; shift 2;;
    -h|--help) sed -n '2,17p' "$0"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

: "${LAT:?set LAT (e.g. LAT=35.35199)}"
: "${LON:?set LON (e.g. LON=-119.17208)}"
export LAT LON

newest_frame() {
    for d in /app/sd/normal /app/sd/starskyStack /app/sd/HDR /app/sd/panorama \
             /app/sd/focusStack /app/sd/sun /app/sd/Lapse/class_*; do
        [ -d "$d" ] && ls -t "$d"/*.jpg "$d"/*.JPG 2>/dev/null | head -1
    done | while read -r f; do
        [ -n "$f" ] && printf "%s %s\n" "$(date -r "$f" +%s 2>/dev/null || echo 0)" "$f"
    done | sort -rn | head -1 | cut -d" " -f2-
}

# --latest: solve the newest frame already on disk. --wait: sit until a NEW one
# appears, so YOU trigger the shot in the Benro app and we solve it.
#
# Both exist because astro mode will not accept an externally-initiated capture:
# opcode 264 is ignored outright, and the 272 lapse sequence worked once and was
# then refused while tracking was running. Rather than keep guessing at the
# astro-mode capture opcode, let the app do what it is good at and have the
# solver follow.
if [ "$FRAME" = "LATEST" ]; then
    FRAME=$(newest_frame)
    [ -n "$FRAME" ] || { echo "[solve-now] no frames found" >&2; exit 1; }
    echo "[solve-now] newest frame: $FRAME" >&2
elif [ "$FRAME" = "WAIT" ]; then
    before=$(newest_frame)
    echo "[solve-now] waiting for a new frame -- take a shot in the Benro app" >&2
    i=0
    while [ $i -lt "${WAIT_FOR_FRAME:-300}" ]; do
        sleep 1; i=$((i+1))
        FRAME=$(newest_frame)
        [ -n "$FRAME" ] && [ "$FRAME" != "$before" ] && break
        FRAME=""
    done
    [ -n "$FRAME" ] || { echo "[solve-now] no new frame appeared" >&2; exit 1; }
    sleep 2                      # let the write finish
    echo "[solve-now] got: $FRAME" >&2
elif [ -z "$FRAME" ]; then
    before=$(newest_frame)
    echo "[solve-now] firing one frame..." >&2
    "$ASTRO/polaris-mount" --host 127.0.0.1 send --msg '1&264&2&state:1;bulb:0;c:-1#' >/dev/null 2>&1
    i=0
    while [ $i -lt "$WAIT" ]; do
        sleep 1; i=$((i+1))
        FRAME=$(newest_frame)
        [ -n "$FRAME" ] && [ "$FRAME" != "$before" ] && break
        FRAME=""
    done
    [ -n "$FRAME" ] || { echo "[solve-now] no frame appeared in ${WAIT}s" >&2; exit 1; }
    sleep 1     # let the write finish
    echo "[solve-now] frame: $FRAME" >&2
fi

echo "[solve-now] solving (hint comes from the mount)..." >&2
[ "$FOCAL" = "auto" ] && FOCAL=""
SOL=$(FOCAL_MM="$FOCAL" sh "$ASTRO/polaris-align.sh" "$FRAME" "$FOCAL") || {
    echo "[solve-now] solver failed" >&2; exit 1; }
echo "$SOL"

echo "$SOL" | grep -q '"solved":true' || { echo "[solve-now] NOT SOLVED" >&2; exit 1; }

RA=$(echo "$SOL"  | sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p')
DEC=$(echo "$SOL" | sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p')
echo "[solve-now] SOLVED  RA=$RA  Dec=$DEC" >&2

if [ "$APPLY" = "1" ]; then
    echo "[solve-now] applying to mount (530)..." >&2
    "$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" align \
        --solved-ra "$RA" --solved-dec "$DEC"
else
    echo "[solve-now] dry run -- pass --apply to push this to the mount" >&2
fi
