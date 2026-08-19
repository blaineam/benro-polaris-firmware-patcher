#!/bin/sh
# ============================================================================
#  polaris-guide.sh -- watch for tracking drift and correct it mid-session.
#
#     LAT=.. LON=.. polaris-guide.sh --ra D --dec D [--focal MM]
#     LAT=.. LON=.. polaris-guide.sh --from-solve        (use the last solve)
#
#  HOW IT DETECTS DRIFT, and why not by plate-solving every time: a solve costs
#  1-4 s and pegs the CPU. Guiding only needs to know how far the SAME field has
#  shifted, which is a translation between two star lists -- milliseconds via
#  polaris-match. Full solves are reserved for RE-ANCHORING when matching fails
#  (cloud, a slew, drift too large to match).
#
#  HOW IT CORRECTS: only goto/goto-radec move the motors on this mount -- there
#  is no axis-rate primitive exposed. So a correction is a small goto-radec back
#  to the target, which also restores tracking. After correcting we re-take the
#  reference frame, because the field has moved.
#
#  PIXEL SCALE comes from the anchoring SOLVE, never a constant. Deriving it
#  from focal length and sensor width by hand is how a 5% error crept into
#  testing (20.35 vs the true 19.32 arcsec/px at 400mm on 960 px).
#
#  SAFETY: DRY_RUN=1 (default) logs corrections without sending them. A
#  correction larger than MAX_CORRECTION_DEG is refused outright -- a big jump
#  means the match or the anchor is wrong, and slewing on bad data is worse than
#  drifting.
# ============================================================================
set -u
ASTRO=${ASTRO:-/app/astro}
LOG=${LOG:-/app/sd/polaris-guide.log}
DRY_RUN=${DRY_RUN:-1}
INTERVAL=${INTERVAL:-30}              # seconds between checks
THRESH_ARCSEC=${THRESH_ARCSEC:-60}    # correct once drift exceeds this
MAX_CORRECTION_DEG=${MAX_CORRECTION_DEG:-2.0}
FOCAL=${FOCAL_MM:-400}
LIVEVIEW_PORT=${LIVEVIEW_PORT:-8080}
INDEXES=${INDEXES:-/app/sd/astrometry}
MATCH_TOL=${MATCH_TOL:-2.5}
MOUNT_HOST=${MOUNT_HOST:-127.0.0.1}
MOUNT_PORT=${MOUNT_PORT:-9090}
SIM_STATUS=${SIM_STATUS:-}
CAPTURE_MODE=${CAPTURE_MODE:-liveview}
: "${LAT:?set LAT}"; : "${LON:?set LON}"
export LAT LON

# The Polaris' system clock runs LOCAL time while reporting itself as UTC (see
# polaris-autosolve.sh). goto-radec converts RA/Dec to alt/az, so an uncorrected
# clock sends the mount to the wrong place -- 7 hours is ~105 deg of hour angle.
TZ_OFFSET_SEC=${TZ_OFFSET_SEC:-auto}
if [ "$TZ_OFFSET_SEC" = "auto" ]; then
    TZ_OFFSET_SEC=$(grep -a "code:782" /app/Mlog.txt 2>/dev/null \
        | sed -n 's/.*zone:-\{1,2\}\([0-9][0-9]*\).*/\1/p' | tail -1)
    [ -n "$TZ_OFFSET_SEC" ] || TZ_OFFSET_SEC=0
fi
utc_now() {
    _e=$(( $(date +%s) + TZ_OFFSET_SEC ))
    date -u -d "@$_e" "+%Y-%m-%dT%H:%M:%S" 2>/dev/null \
      || date -u -r "$_e" "+%Y-%m-%dT%H:%M:%S" 2>/dev/null
}
utcarg() { [ "${TZ_OFFSET_SEC:-0}" -eq 0 ] 2>/dev/null || printf -- "--utc %s" "$(utc_now)"; }

RA=""; DEC=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ra) RA="$2"; shift 2;;
    --dec) DEC="$2"; shift 2;;
    --focal) FOCAL="$2"; shift 2;;
    --from-solve)
        RA=$(sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p'  /tmp/polaris-job.result 2>/dev/null)
        DEC=$(sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p' /tmp/polaris-job.result 2>/dev/null)
        shift;;
    -h|--help) sed -n '2,26p' "$0"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[ -n "$RA" ] && [ -n "$DEC" ] || { echo "need --ra/--dec or --from-solve" >&2; exit 2; }

log() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; echo "$(date +%H:%M:%S) $*" >&2; }

idx_args() { for f in "$INDEXES"/index-*.fits; do [ -f "$f" ] && printf ' --index %s' "$f"; done; }

# CAPTURE_MODE=render is the SIMULATOR camera: ask the sim where it is REALLY
# pointing and draw that patch of sky. Lets guiding be exercised with real motor
# commands and no hardware.
grab() {
    if [ "$CAPTURE_MODE" = "render" ]; then
        [ -n "$SIM_STATUS" ] || return 1
        _sh=$(echo "$SIM_STATUS" | cut -d: -f1); _sp=$(echo "$SIM_STATUS" | cut -d: -f2)
        rm -f /tmp/guide_sim.json
        "$ASTRO/polaris-mount" fetch --host "$_sh" --port "$_sp" \
            --url-path "/" --out /tmp/guide_sim.json >/dev/null 2>&1
        _tra=$(sed -n 's/.*"true_ra_deg": *\([-0-9.]*\).*/\1/p'  /tmp/guide_sim.json)
        _tdec=$(sed -n 's/.*"true_dec_deg": *\([-0-9.]*\).*/\1/p' /tmp/guide_sim.json)
        [ -n "$_tra" ] || return 1
        rm -f "$1"
        "$ASTRO/polaris-skysim" $(idx_args) --ra "$_tra" --dec "$_tdec" \
            --focal-mm "$FOCAL" --sensor-mm 36 --width 960 --height 640 \
            --out "$1" >/dev/null 2>&1
        [ -s "$1" ]
        return $?
    fi
    rm -f "$1"
    "$ASTRO/polaris-mount" fetch --host 127.0.0.1 --port "$LIVEVIEW_PORT" \
        --url-path "/?action=snapshot" --out "$1" >/dev/null 2>&1
    [ -s "$1" ]
}

extract() { "$ASTRO/polaris-extract" --jpeg "$1" --downsample 1 > "$2" 2>/dev/null; }

# Anchor: solve the current frame to learn the true pixel scale, and keep that
# frame as the reference the matcher compares against.
PIXSCALE=""
anchor() {
    log "anchoring: solving the current field"
    grab /tmp/guide_ref.jpg || { log "  no live-view frame"; return 1; }
    extract /tmp/guide_ref.jpg /tmp/guide_ref.txt
    W=$(sed -n 's/^# full resolution \([0-9]*\) x .*/\1/p' /tmp/guide_ref.txt)
    H=$(sed -n 's/^# full resolution [0-9]* x \([0-9]*\)/\1/p' /tmp/guide_ref.txt)
    J=$("$ASTRO/polaris-solve" $(idx_args) --stars /tmp/guide_ref.txt \
          --width "$W" --height "$H" --focal-mm "$FOCAL" --sensor-mm 36 \
          --cpulimit 60 2>/dev/null | tail -1)
    echo "$J" | grep -q '"solved":true' || { log "  anchor solve failed"; return 1; }
    PIXSCALE=$(echo "$J" | sed -n 's/.*"pixscale_arcsec":\([0-9.]*\).*/\1/p')
    ARA=$(echo  "$J" | sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p')
    ADEC=$(echo "$J" | sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p')
    log "  anchored at ra=$ARA dec=$ADEC, pixel scale ${PIXSCALE}\"/px ($(grep -vc '^#' /tmp/guide_ref.txt) stars)"
    return 0
}

correct() {
    _arcsec=$1
    _deg=$(awk -v a="$_arcsec" 'BEGIN{printf "%.4f", a/3600.0}')
    _too_big=$(awk -v d="$_deg" -v m="$MAX_CORRECTION_DEG" 'BEGIN{print (d>m)?1:0}')
    if [ "$_too_big" = "1" ]; then
        log "  REFUSING correction of ${_deg} deg (limit ${MAX_CORRECTION_DEG})."
        log "    a jump that large means the match or the anchor is wrong;"
        log "    slewing on bad data is worse than drifting. Re-anchoring."
        anchor
        return 1
    fi
    if [ "$DRY_RUN" = "1" ]; then
        log "  DRY RUN -- would goto-radec --ra $RA --dec $DEC to re-centre"
        return 0
    fi
    log "  correcting: goto-radec --ra $RA --dec $DEC"
    "$ASTRO/polaris-mount" --host "$MOUNT_HOST" --port "$MOUNT_PORT" \
        --lat "$LAT" --lon "$LON" $(utcarg) \
        goto-radec --ra "$RA" --dec "$DEC" >>"$LOG" 2>&1
    sleep 5
    anchor            # field moved; take a fresh reference
}

log "clock: TZ_OFFSET_SEC=$TZ_OFFSET_SEC -> UTC $(utc_now)"
log "guiding target ra=$RA dec=$DEC (dry_run=$DRY_RUN interval=${INTERVAL}s threshold=${THRESH_ARCSEC}\")"
anchor || { log "could not anchor -- nothing to guide against"; exit 1; }

while :; do
    sleep "$INTERVAL"
    grab /tmp/guide_cur.jpg || { log "no frame; skipping this check"; continue; }
    extract /tmp/guide_cur.jpg /tmp/guide_cur.txt
    M=$("$ASTRO/polaris-match" --ref /tmp/guide_ref.txt --cur /tmp/guide_cur.txt \
          --tol "$MATCH_TOL" --top 50 2>/dev/null)
    if ! echo "$M" | grep -q '"matched":true'; then
        log "match failed ($M) -- re-anchoring with a full solve"
        anchor
        continue
    fi
    DX=$(echo "$M" | sed -n 's/.*"dx":\([-0-9.]*\).*/\1/p')
    DY=$(echo "$M" | sed -n 's/.*"dy":\([-0-9.]*\).*/\1/p')
    DRIFT=$(awk -v x="$DX" -v y="$DY" -v p="$PIXSCALE" 'BEGIN{printf "%.1f", sqrt(x*x+y*y)*p}')
    OVER=$(awk -v d="$DRIFT" -v t="$THRESH_ARCSEC" 'BEGIN{print (d>t)?1:0}')
    if [ "$OVER" = "1" ]; then
        log "drift ${DRIFT}\" (dx=$DX dy=$DY px) exceeds ${THRESH_ARCSEC}\""
        correct "$DRIFT"
    else
        log "drift ${DRIFT}\" (dx=$DX dy=$DY px) -- within tolerance"
    fi
done
