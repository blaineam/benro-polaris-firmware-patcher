#!/bin/sh
# ============================================================================
#  polaris-autoalign — make the app's star alignment accurate, automatically.
#
#  Runs ON THE DEVICE. Watches polestar_app's own log (it records every frame
#  the phone app sends), and when the app runs its astro alignment:
#
#     app: 519 ... track:0     slews to the chosen star, then waits for you
#     ->   we capture a frame, plate solve it, and hold the true sky position
#     app: 530 step:1/step:2   sent when you tap "confirm"
#     ->   we immediately re-send 530 with the SOLVED position
#
#  The mount takes the last alignment, so yours is silently replaced by an
#  accurate one. You still tap confirm; it is simply correct afterwards.
#
#  Nothing is patched: polestar_app already logs the frames we key off.
#
#  Environment:
#     LAT, LON        observer position (REQUIRED)
#     FOCAL_MM        lens focal length, for the scale hint (default 400)
#     SENSOR_MM       sensor width (default 36)
#     CAPTURE_MODE    how to get a frame:
#                       auto     (default) TRY LIVE VIEW FIRST -- no shutter
#                                actuation, no card write, no download, a frame
#                                in well under a second. Only if that fails to
#                                solve do we fall back to a full shutter
#                                capture, which is slower and wears the shutter
#                                but gathers far more light.
#                       shutter  fire 272 and wait for the file
#                       liveview grab pgphoto's mjpg-streamer snapshot -- NO
#                                shutter actuation, no card write, no download.
#                                ~19 arcsec/px at 400mm, which is plenty for
#                                pointing; the open question is whether enough
#                                stars register in a ~1/30 s live-view frame.
#                       render   synthesise the field with polaris-skysim, for
#                                testing the whole chain with no camera at all
#     LIVEVIEW_URL    default http://127.0.0.1:8080/?action=snapshot
#     FRAME           TEST MODE: use this JPEG and do NOT fire the shutter.
#     SOLVE_FRAME     TEST MODE: DO fire the shutter and wait for the frame to
#                     land (so the capture path is genuinely exercised), but
#                     solve this substitute image instead. For daylight testing,
#                     where a real frame has no stars in it.
#                     Both imply DRY_RUN=1 unless overridden: a stand-in frame
#                     solves to wherever THAT photo was taken, so injecting it
#                     would badly misalign a real mount.
#     DRY_RUN=1       print the 530 we would send; send nothing
#     HINT_RA/HINT_DEC  force the search hint (degrees). Otherwise the mount's
#                     own pose is used -- but ONLY for a real capture, since a
#                     hint that does not match the frame is worse than none.
#     MIN_LOGODDS     quality gate (default 100)
#     MIN_MATCHES     quality gate (default 12)
#     SOLVE_TIMEOUT   seconds (default 45)
#     LOG             where to log (default /app/sd/polaris-autoalign.log)
# ============================================================================
ASTRO=${ASTRO:-/app/astro}
INDEXES=${INDEXES:-/app/sd/astrometry}
CAPTURE_DIR=${CAPTURE_DIR:-/app/sd/normal}
MOUNT_HOST=${MOUNT_HOST:-127.0.0.1}
MOUNT_PORT=${MOUNT_PORT:-9090}
APPLOG=${APPLOG:-/app/Mlog.txt}
FOCAL_MM=${FOCAL_MM:-400}
SENSOR_MM=${SENSOR_MM:-36}
DOWNSAMPLE=${DOWNSAMPLE:-4}
MAXSTARS=${MAXSTARS:-300}
MIN_LOGODDS=${MIN_LOGODDS:-100}
MIN_MATCHES=${MIN_MATCHES:-12}
SOLVE_TIMEOUT=${SOLVE_TIMEOUT:-45}
CAPTURE_WAIT=${CAPTURE_WAIT:-40}
LOG=${LOG:-/app/sd/polaris-autoalign.log}
STATE=/tmp/polaris-autoalign.solution
PENDING=/tmp/polaris-autoalign.pending

{ [ -n "${FRAME:-}" ] || [ -n "${SOLVE_FRAME:-}" ]; } && DRY_RUN=${DRY_RUN:-1}
DRY_RUN=${DRY_RUN:-0}

# NB: stderr, never stdout. capture() returns a filename ON STDOUT, so anything
# logged to stdout would be concatenated into the path.
log() { msg="$(date -u '+%H:%M:%S') $*"; echo "$msg" >> "$LOG"; echo "$msg" >&2; }

[ -n "${LAT:-}" ] && [ -n "${LON:-}" ] || { echo "set LAT and LON" >&2; exit 2; }
[ -x "$ASTRO/polaris-solve" ] || { echo "missing $ASTRO/polaris-solve" >&2; exit 2; }

idx_args() { for f in "$INDEXES"/index-*.fits; do [ -f "$f" ] && printf ' --index %s' "$f"; done; }

# ---- fire the shutter, wait for the frame to land -------------------------
capture() {
    if [ -n "${FRAME:-}" ]; then echo "$FRAME"; return 0; fi

    if [ "${CAPTURE_MODE:-shutter}" = "liveview" ]; then
        out=/tmp/polaris-liveview.jpg
        rm -f "$out"
        # the device has neither curl nor wget; polaris-mount does the GET
        "$ASTRO/polaris-mount" --host "${LIVEVIEW_HOST:-127.0.0.1}" \
            --port "${LIVEVIEW_PORT:-8080}" fetch \
            --url-path "${LIVEVIEW_PATH:-/?action=snapshot}" --out "$out" >/dev/null 2>&1
        if [ -s "$out" ]; then log "grabbed a live-view frame (no shutter)"; echo "$out"; return 0; fi
        log "live view returned nothing"; return 1
    fi

    if [ "${CAPTURE_MODE:-shutter}" = "render" ]; then
        out=/tmp/polaris-rendered.jpg
        pose=$("$ASTRO/polaris-mount" --host "$MOUNT_HOST" --port "$MOUNT_PORT" \
               --lat "$LAT" --lon "$LON" pose 2>/dev/null)
        rra=$(echo "$pose"  | sed -n "s/.*\"ra_deg\":\([-0-9.]*\).*/\1/p")
        rdec=$(echo "$pose" | sed -n "s/.*\"dec_deg\":\([-0-9.]*\).*/\1/p")
        # deliberately offset the render to simulate a mount that is WRONG by a
        # known amount, so the correction can be checked against a known answer
        rra=$(awk -v r="$rra" -v o="${RENDER_OFFSET_RA:-0}" 'BEGIN{printf "%.6f", (r+o)%360}')
        rdec=$(awk -v d="$rdec" -v o="${RENDER_OFFSET_DEC:-0}" 'BEGIN{printf "%.6f", d+o}')
        [ -n "$rra" ] && [ -n "$rdec" ] || { log "no pose to render from"; return 1; }
        log "rendering the sky at ra=$rra dec=$rdec (no camera involved)"
        "$ASTRO/polaris-skysim" $(idx_args) --ra "$rra" --dec "$rdec" \
            --focal-mm "$FOCAL_MM" --sensor-mm "$SENSOR_MM" \
            --width "${RENDER_W:-4096}" --height "${RENDER_H:-2732}" \
            --out "$out" >/dev/null 2>&1 || { log "render failed"; return 1; }
        echo "$out"; return 0
    fi
    before=$(ls -t "$CAPTURE_DIR"/*.jpg 2>/dev/null | head -1)
    "$ASTRO/polaris-mount" --host "$MOUNT_HOST" --port "$MOUNT_PORT" send \
        --msg '1&272&2&step:1#' \
        --msg '1&272&2&step:2;point:1;time:0;para:1,-1;bulb:0;#' \
        --msg '1&272&2&step:3;point:1;time:-1;photoCnt:-1;#' >/dev/null 2>&1
    i=0
    while [ $i -lt "$CAPTURE_WAIT" ]; do
        sleep 1; i=$((i + 1))
        newest=$(ls -t "$CAPTURE_DIR"/*.jpg 2>/dev/null | head -1)
        if [ -n "$newest" ] && [ "$newest" != "$before" ]; then
            sleep 1                       # let the write finish
            log "shutter fired; frame landed: $newest"
            # Daylight testing: the real frame has no stars, so solve a
            # substitute. The capture path above was still exercised for real.
            [ -n "${SOLVE_FRAME:-}" ] && { log "substituting $SOLVE_FRAME for the solve"; echo "$SOLVE_FRAME"; return 0; }
            echo "$newest"; return 0
        fi
    done
    log "shutter fired but no frame appeared within ${CAPTURE_WAIT}s"
    [ -n "${SOLVE_FRAME:-}" ] && { log "solving the substitute anyway so the rest of the chain is tested"; echo "$SOLVE_FRAME"; return 0; }
    return 1
}

# ---- solve one frame; echoes "ra dec logodds nmatch" on success -----------
solve_frame() {
    frame="$1"
    log "solving $frame"

    # A pose hint is ONLY valid if the frame came from the current pointing.
    # It is worth ~50x when right and worse than useless when wrong (the solver
    # searches the hinted region, fails, then grinds). So: never hint from the
    # mount when we are solving a substitute image.
    hint=""
    if [ -n "${HINT_RA:-}" ] && [ -n "${HINT_DEC:-}" ]; then
        hint="--ra $HINT_RA --dec $HINT_DEC --radius ${HINT_RADIUS:-30}"
    elif [ -z "${SOLVE_FRAME:-}" ] && [ -z "${FRAME:-}" ]; then
        pose=$("$ASTRO/polaris-mount" --host "$MOUNT_HOST" --port "$MOUNT_PORT" \
               --lat "$LAT" --lon "$LON" pose 2>/dev/null)
        hra=$(echo "$pose" | sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p')
        hdec=$(echo "$pose" | sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p')
        [ -n "$hra" ] && [ -n "$hdec" ] && hint="--ra $hra --dec $hdec --radius ${HINT_RADIUS:-30}"
    else
        log "substitute frame: skipping the mount pose hint (it would not match)"
    fi

    stars=$(mktemp /tmp/aa-stars.XXXXXX) || return 1
    "$ASTRO/polaris-extract" --jpeg "$frame" --downsample "$DOWNSAMPLE" \
        --max-stars "$MAXSTARS" > "$stars" 2>/dev/null || { rm -f "$stars"; return 1; }
    W=$(sed -n 's/^# full resolution \([0-9]*\) x .*/\1/p' "$stars")
    H=$(sed -n 's/^# full resolution [0-9]* x \([0-9]*\)/\1/p' "$stars")
    [ -n "$W" ] && [ -n "$H" ] || { rm -f "$stars"; return 1; }

    sol=$("$ASTRO/polaris-solve" $(idx_args) --stars "$stars" --width "$W" --height "$H" \
          --focal-mm "$FOCAL_MM" --sensor-mm "$SENSOR_MM" --scale-tol 0.35 \
          --cpulimit "$SOLVE_TIMEOUT" $hint 2>/dev/null)
    rm -f "$stars"
    echo "$sol" | grep -q '"solved":true' || { log "no solve: $sol"; return 1; }

    ra=$(echo  "$sol" | sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p')
    dec=$(echo "$sol" | sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p')
    lo=$(echo  "$sol" | sed -n 's/.*"logodds":\([-0-9.]*\).*/\1/p')
    nm=$(echo  "$sol" | sed -n 's/.*"nmatch":\([0-9]*\).*/\1/p')

    # Quality gate. A BAD solve pushed into 530 corrupts a good compass
    # alignment -- strictly worse than doing nothing. So refuse anything
    # marginal and leave the user's own confirm standing.
    if [ "$(echo "$lo" | cut -d. -f1)" -lt "$MIN_LOGODDS" ] 2>/dev/null || \
       [ "${nm:-0}" -lt "$MIN_MATCHES" ] 2>/dev/null; then
        log "REJECTED by quality gate: logodds=$lo nmatch=$nm (need >=$MIN_LOGODDS/$MIN_MATCHES)"
        return 1
    fi

    altaz=$("$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" \
            radec2altaz --ra "$ra" --dec "$dec" 2>/dev/null)
    alt=$(echo "$altaz" | sed -n 's/.*"alt_deg":\([-0-9.]*\).*/\1/p')
    az=$(echo  "$altaz" | sed -n 's/.*"az_deg":\([-0-9.]*\).*/\1/p')
    [ -n "$alt" ] && [ -n "$az" ] || return 1

    echo "$alt $az" > "$STATE"
    log "SOLVED ra=$ra dec=$dec (logodds=$lo nmatch=$nm) -> alt=$alt az=$az"
    return 0
}

# ---- try the cheap frame first, fall back to the expensive one ------------
solve_now() {
    rm -f "$STATE"
    mode=${CAPTURE_MODE:-auto}

    if [ "$mode" = "auto" ]; then
        log "trying live view first (no shutter)"
        if f=$(CAPTURE_MODE=liveview capture); then
            if solve_frame "$f"; then
                [ -f "$PENDING" ] && { rm -f "$PENDING"; inject; }
                return 0
            fi
            log "live view did not solve -- falling back to a full capture"
        else
            log "no live-view frame -- falling back to a full capture"
        fi
        mode=shutter
    fi

    f=$(CAPTURE_MODE=$mode capture) || { log "capture produced no frame"; return 1; }
    solve_frame "$f" || return 1
    [ -f "$PENDING" ] && { rm -f "$PENDING"; inject; }
    return 0
}

# ---- push the solved position into the mount's alignment ------------------
inject() {
    [ -f "$STATE" ] || { log "nothing solved yet; leaving the user's alignment alone"; return 1; }
    read -r alt az < "$STATE"
    if [ "$DRY_RUN" = "1" ]; then
        log "DRY RUN: would 530 alt=$alt az=$az"
        "$ASTRO/polaris-mount" --host "$MOUNT_HOST" --port "$MOUNT_PORT" \
            --lat "$LAT" --lon "$LON" --dry-run star-align --alt "$alt" --az "$az" 2>&1 \
            | grep '^->>' >> "$LOG" 2>&1
    else
        log "INJECTING plate-solved alignment: alt=$alt az=$az"
        "$ASTRO/polaris-mount" --host "$MOUNT_HOST" --port "$MOUNT_PORT" \
            --lat "$LAT" --lon "$LON" star-align --alt "$alt" --az "$az" 2>/dev/null \
            >> "$LOG" 2>&1
    fi
    rm -f "$STATE"
}

# polestar_app TRUNCATES this log continuously -- measured: 13439 bytes down to
# 979 in twenty seconds. `tail -f` follows by DESCRIPTOR, so the first truncation
# leaves it reading a stale offset and it silently never reports another line.
# In the field that looks exactly like "the solve never fired". So follow by
# size instead, and reset to the start whenever the file shrinks.
follow_log() {
    last=$(wc -c < "$APPLOG" 2>/dev/null || echo 0)
    while :; do
        cur=$(wc -c < "$APPLOG" 2>/dev/null || echo 0)
        if [ "$cur" -lt "$last" ]; then          # truncated: start over
            last=0
        fi
        if [ "$cur" -gt "$last" ]; then
            tail -c +$((last + 1)) "$APPLOG" 2>/dev/null
            last=$cur
        fi
        sleep 0.3 2>/dev/null || sleep 1
    done
}

log "watching $APPLOG  (dry_run=$DRY_RUN frame=${FRAME:-none} focal=${FOCAL_MM}mm)"
follow_log | while read -r line; do
    case "$line" in
        *"code:519"*"track:0"*)
            log "app is aligning: slewing to its star"
            ( sleep 6; solve_now ) &          # settle, then capture+solve in background
            ;;
        *"code:530"*"step:2"*)
            if [ -f "$STATE" ]; then
                log "app confirmed its alignment -> replacing it with the solved one"
                inject
            else
                log "app confirmed before our solve finished; will inject as soon as it lands"
                touch "$PENDING"
            fi
            ;;
    esac
done
