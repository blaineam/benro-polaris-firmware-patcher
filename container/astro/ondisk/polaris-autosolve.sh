#!/bin/sh
# ============================================================================
#  polaris-autosolve.sh -- plate-solve during the app's calibration, so the
#  user does not have to eyeball the alignment star.
#
#  THE FLOW WE HOOK (captured on hardware 2026-08-18):
#     APP 527  compass:10;lat;lng
#     APP 519  state:1;yaw:169.128;pitch:32.152;track:0;speed:2   slew to star
#     APP 530  step:1;yaw:-0.000;pitch:0.000;num:1    <- ARM, dialog appears
#     APP 530  step:2;yaw:169.128;pitch:32.152;num:1  <- user taps confirm
#     APP 531  state:1;speed:2                        <- tracking
#
#  NOTE step:1 carries ZEROS and step:2 carries the real yaw/pitch. (An earlier
#  note in this repo had that backwards.) The real target is in the 519 that
#  precedes the arm, which is why we track 519 and act on 530 step:1.
#
#  WHAT WE DO, on seeing 530 step:1:
#     1. grab a LIVE VIEW frame (no shutter -- during the armed state the app
#        will not let the user take a full frame anyway, so we cannot rely on
#        one existing)
#     2. solve it, hinted by the 519 target
#     3. if the live-view solve fails, fire a FULL capture and solve that
#     4. on a good solve, correct the heading (527, via polaris-mount align)
#     5. confirm for the user (530 step:2 with the 519 values), dismissing
#        the dialog
#
#  SAFETY: a failed or low-confidence solve does NOTHING and leaves the dialog
#  alone. Falling back to the user's own tap is always safe; writing a
#  confidently wrong alignment to the mount is not. DRY_RUN=1 (default) prints
#  what it would send and sends nothing -- set DRY_RUN=0 to arm it for real.
# ============================================================================
set -u
ASTRO=${ASTRO:-/app/astro}
LOG=${LOG:-/app/sd/polaris-autosolve.log}
DRY_RUN=${DRY_RUN:-1}
FOCAL=${FOCAL_MM:-400}
MIN_LOGODDS=${MIN_LOGODDS:-100}
MIN_MATCHES=${MIN_MATCHES:-12}
LIVEVIEW_PORT=${LIVEVIEW_PORT:-8080}

# ---- TEST MODE -------------------------------------------------------------
# STUB_FRAME  solve THIS image instead of grabbing live view / firing a frame.
# STUB_UTC    pretend it is this UTC ("YYYY-MM-DDTHH:MM:SS"). Needed with
#             STUB_FRAME: a stand-in photo solves to wherever it was actually
#             taken, and alt/az only means anything at the matching time.
# Either one forces DRY_RUN unless you explicitly set DRY_RUN=0, because
# applying a correction derived from a stand-in frame would badly misalign a
# real mount.
STUB_FRAME=${STUB_FRAME:-}
STUB_UTC=${STUB_UTC:-}

# ---- THE DEVICE CLOCK IS NOT UTC -------------------------------------------
#
# The Polaris' system clock runs on LOCAL time while `date -u` reports it as
# UTC. Measured: device said 2026-08-18T19:49:48 "UTC" when true UTC was
# 2026-08-19T02:49:51 -- seven hours, i.e. the PDT offset.
#
# Seven hours is ~105 deg of hour angle, so every RA/Dec <-> alt/az conversion
# is grossly wrong. Verified against the simulator, whose clock IS correct:
#     sim truth            alt 44.988  az 23.057
#     device clock as-is   alt 16.508  az  7.148
#     device clock +7h     alt 44.947  az 22.948   <- matches
#
# Nothing running purely on the device could catch this: both halves of every
# earlier test shared the same wrong clock, so the error cancelled. It would
# have produced a confidently wrong compass correction on the first real night.
#
# TZ_OFFSET_SEC is what to ADD to the device clock to get UTC. "auto" reads it
# from the app's own 782 message (date/time/zone), which is authoritative --
# the app tells the device its timezone every time it connects.
TZ_OFFSET_SEC=${TZ_OFFSET_SEC:-auto}

detect_tz_offset() {
    # 782 looks like: val:date:2026-08-18;time:18:09:50;zone:--25200;
    # The zone field is seconds EAST of UTC (so -25200 for PDT). Note the app
    # writes a stray extra '-'; tolerate one or two.
    _z=$(grep -a "code:782" /app/Mlog.txt 2>/dev/null \
         | sed -n 's/.*zone:-\{1,2\}\([0-9][0-9]*\).*/\1/p' | tail -1)
    if [ -n "$_z" ]; then
        echo $(( _z ))          # zone is negative -> add |zone| to reach UTC
        return 0
    fi
    return 1
}

if [ "$TZ_OFFSET_SEC" = "auto" ]; then
    if TZ_OFFSET_SEC=$(detect_tz_offset); then
        :
    else
        TZ_OFFSET_SEC=0
    fi
fi

# UTC as a string polaris-mount accepts, corrected for the device clock.
utc_now() {
    _e=$(( $(date +%s) + TZ_OFFSET_SEC ))
    date -u -d "@$_e" "+%Y-%m-%dT%H:%M:%S" 2>/dev/null \
      || date -u -r "$_e" "+%Y-%m-%dT%H:%M:%S" 2>/dev/null
}

UTCARG=""
if [ -n "$STUB_UTC" ]; then
    UTCARG="--utc $STUB_UTC"
elif [ "${TZ_OFFSET_SEC:-0}" -ne 0 ] 2>/dev/null; then
    UTCARG="--utc $(utc_now)"
fi
if [ -n "$STUB_FRAME" ] || [ -n "$STUB_UTC" ]; then
    DRY_RUN=${DRY_RUN:-1}
fi
: "${LAT:?set LAT}"; : "${LON:?set LON}"
export LAT LON

log() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; echo "$(date +%H:%M:%S) $*" >&2; }

MOUNT_HOST=${MOUNT_HOST:-127.0.0.1}
MOUNT_PORT=${MOUNT_PORT:-9090}
mount_send() { "$ASTRO/polaris-mount" --host "$MOUNT_HOST" --port "$MOUNT_PORT" \
                   send --msg "$1" >/dev/null 2>&1; }

# --- solve one jpeg; echo the JSON, or nothing --------------------------------
# Pick the downsample from the ACTUAL image size, do not inherit the default.
#
# polaris-align.sh defaults to DOWNSAMPLE=4, which is right for an 8192 px
# full-res frame and destroys a live-view one: 960x640 becomes 240x160, far too
# small to solve. Observed on hardware -- the live-view solve failed with the
# extractor reporting "240x160 decoded at 1/4 (full 960x640)".
pick_downsample() {
    _w=$("$ASTRO/polaris-extract" --jpeg "$1" --downsample 8 2>/dev/null \
         | sed -n 's/^# full resolution \([0-9]*\) x .*/\1/p')
    [ -n "$_w" ] || { echo 4; return; }
    if   [ "$_w" -ge 6000 ]; then echo 4
    elif [ "$_w" -ge 3000 ]; then echo 2
    else                          echo 1
    fi
}

solve_frame() {
    _f=$1; _hra=$2; _hdec=$3
    [ -s "$_f" ] || return 1
    _ds=$(pick_downsample "$_f")
    # polaris-align.sh takes the hint POSITIONALLY: <img> <focal> <ra> <dec> <radius>.
    # HINT_RA/HINT_DEC env vars are NOT read -- setting those silently solved
    # blind, and blind is ~50x slower at this focal length (361.7s vs 7.3s
    # measured on this device), which would hang the calibration dialog.
    if [ -n "$_hra" ]; then
        # HINT_RADIUS must cover the COMPASS ERROR, which is the whole reason we
        # are plate-solving. Measured against the simulator with a 37.5 deg
        # error (hint 39.2 deg from truth) on a 960x640 frame:
        #     20 deg  -> FAILED after 120 s (burned the whole cpulimit)
        #     45 deg  -> solved in 1 s
        #     90 deg  -> solved in 3 s
        #     blind   -> solved in 3 s
        # So a too-narrow hint is WORSE THAN NO HINT: it fails slowly. The old
        # default of 20 deg could never have aligned a cold-start compass.
        _out=$(DOWNSAMPLE="$_ds" FOCAL_MM="$FOCAL" \
            sh "$ASTRO/polaris-align.sh" "$_f" "$FOCAL" "$_hra" "$_hdec" "${HINT_RADIUS:-60}" 2>>"$LOG")
        if echo "$_out" | grep -q '"solved":true'; then
            echo "$_out"; return 0
        fi
        # Hinted solve failed: the hint itself may be the problem. Retry blind.
        # At live-view resolution that costs seconds, not minutes -- the project's
        # old 361 s blind figure was measured on full-res 8192 px frames.
        log "hinted solve failed -- retrying blind (the hint may be the problem)"
        DOWNSAMPLE="$_ds" FOCAL_MM="$FOCAL" MOUNT_HINT=0 \
            sh "$ASTRO/polaris-align.sh" "$_f" "$FOCAL" 2>>"$LOG"
    else
        DOWNSAMPLE="$_ds" FOCAL_MM="$FOCAL" MOUNT_HINT=0 \
            sh "$ASTRO/polaris-align.sh" "$_f" "$FOCAL" 2>>"$LOG"
    fi
}

good_solve() {
    echo "$1" | grep -q '"solved":true' || return 1
    _lo=$(echo "$1" | sed -n 's/.*"logodds":\([0-9.]*\).*/\1/p')
    _nm=$(echo "$1" | sed -n 's/.*"nmatch":\([0-9]*\).*/\1/p')
    [ -n "$_lo" ] && [ -n "$_nm" ] || return 1
    # integer compare; logodds is large when the solve is real
    [ "${_lo%%.*}" -ge "$MIN_LOGODDS" ] && [ "$_nm" -ge "$MIN_MATCHES" ]
}


# ---- correction, computed WITHOUT needing the mount's pose -------------------
#
# polaris-mount's `align` derives its 527 from the solved position alone:
#     compass = (sky_az - 180) mod 360
# Verified against align's own output at five widely separated positions,
# agreeing to 0.0001 deg. It reads 518 only for the error DIAGNOSTICS and its
# zenith rail, not to build the command.
#
# That matters because 518 is silent at track:3 -- the never-aligned state after
# a cold boot, which is exactly when a first calibration happens. Computing the
# correction ourselves means auto-solve works there too, instead of aborting
# with "no 518 pose message arrived".
#
# The zenith rail is kept, because it is a real hazard and not a diagnostic:
# azimuth is degenerate near the zenith and a heading derived there is amplified
# by 1/cos(alt).
MAX_ALIGN_ALT=${MAX_ALIGN_ALT:-65}

# echo the compass value to send, or nothing if the geometry is unsafe
compass_for_solve() {
    _sra=$1; _sdec=$2
    [ -n "$STUB_UTC" ] || [ "${TZ_OFFSET_SEC:-0}" -eq 0 ] 2>/dev/null || UTCARG="--utc $(utc_now)"
    _j=$("$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" $UTCARG \
            radec2altaz --ra "$_sra" --dec "$_sdec" 2>/dev/null)
    _salt=$(echo "$_j" | sed -n 's/.*"alt_deg":\([-0-9.]*\).*/\1/p')
    _saz=$(echo  "$_j" | sed -n 's/.*"az_deg":\([-0-9.]*\).*/\1/p')
    [ -n "$_saz" ] || { log "could not convert the solve to alt/az"; return 1; }
    log "solved sky position: alt=$_salt az=$_saz"
    _too_high=$(awk -v a="$_salt" -v m="$MAX_ALIGN_ALT" 'BEGIN{print (a>m)?1:0}')
    if [ "$_too_high" = "1" ]; then
        log "REFUSING: solved altitude ${_salt} is above ${MAX_ALIGN_ALT} deg."
        log "  azimuth is degenerate near the zenith (error amplified by 1/cos(alt));"
        log "  a heading derived here would be worse than no correction."
        return 1
    fi
    awk -v a="$_saz" 'BEGIN{c=a-180; while(c<0)c+=360; while(c>=360)c-=360; printf "%.5f", c}'
}

# grab a frame; echo its path
#
# CAPTURE_MODE=liveview (default) pulls pgphoto's MJPG snapshot -- no shutter.
# CAPTURE_MODE=render is the SIMULATOR camera: ask the sim where it is REALLY
# pointing and render that patch of sky with polaris-skysim. That closes the
# loop with no hardware and no sky, so motor commands can be exercised for real
# -- the daemon sends genuine 527/519/530, the sim moves, and the next rendered
# frame reflects where it actually ended up.
SIM_STATUS=${SIM_STATUS:-}                 # host:port of the sim's status port
RENDER_W=${RENDER_W:-960}
RENDER_H=${RENDER_H:-640}

grab_frame() {
    if [ "${CAPTURE_MODE:-liveview}" = "render" ]; then
        [ -n "$SIM_STATUS" ] || { log "CAPTURE_MODE=render needs SIM_STATUS=host:port"; return 1; }
        _sh=$(echo "$SIM_STATUS" | cut -d: -f1)
        _sp=$(echo "$SIM_STATUS" | cut -d: -f2)
        rm -f /tmp/simstatus.json
        "$ASTRO/polaris-mount" fetch --host "$_sh" --port "$_sp" \
            --url-path "/" --out /tmp/simstatus.json >/dev/null 2>&1
        _tra=$(sed -n 's/.*"true_ra_deg": *\([-0-9.]*\).*/\1/p'  /tmp/simstatus.json)
        _tdec=$(sed -n 's/.*"true_dec_deg": *\([-0-9.]*\).*/\1/p' /tmp/simstatus.json)
        [ -n "$_tra" ] || { log "could not read the sim's true pointing"; return 1; }
        _idx=""
        for f in "${INDEXES:-/app/sd/astrometry}"/index-*.fits; do
            [ -f "$f" ] && _idx="$_idx --index $f"
        done
        rm -f /tmp/autosolve-lv.jpg
        "$ASTRO/polaris-skysim" $_idx --ra "$_tra" --dec "$_tdec" \
            --focal-mm "$FOCAL" --sensor-mm 36 \
            --width "$RENDER_W" --height "$RENDER_H" \
            --out /tmp/autosolve-lv.jpg >/dev/null 2>&1
        [ -s /tmp/autosolve-lv.jpg ] || { log "render failed"; return 1; }
        log "  [sim camera] rendered the sky at ra=$_tra dec=$_tdec"
        echo /tmp/autosolve-lv.jpg
        return 0
    fi
    rm -f /tmp/autosolve-lv.jpg
    "$ASTRO/polaris-mount" fetch --host 127.0.0.1 --port "$LIVEVIEW_PORT" \
        --url-path "/?action=snapshot" --out /tmp/autosolve-lv.jpg >/dev/null 2>&1
    [ -s /tmp/autosolve-lv.jpg ] || return 1
    echo /tmp/autosolve-lv.jpg
}

# --- the whole reaction to one armed alignment --------------------------------
# The protocol's yaw is a WIRE azimuth, not a compass azimuth:
#   az > 180  ->  wire =  360 - az        az <= 180  ->  wire = -az
# Verified against captured traffic: 519 yaw:173.624 while the mount reported
# az_deg 186.377 (360 - 173.624 = 186.376). Feeding yaw straight in as an
# azimuth puts the search hint ~13 deg off, and a wrong hint makes the solve
# FAIL rather than merely run slow.
wire_to_az() {
    awk -v y="$1" 'BEGIN{ if (y >= 0) a = 360 - y; else a = -y;
                          while (a < 0) a += 360; while (a >= 360) a -= 360;
                          printf "%.6f", a }'
}

handle_arm() {
    _yaw=$1; _pitch=$2
    _az=$(wire_to_az "$_yaw")
    log "ARMED: app target yaw=$_yaw (az=$_az) pitch=$_pitch -- solving"

    # the app's target, as a search hint
    [ -n "$STUB_UTC" ] || [ "${TZ_OFFSET_SEC:-0}" -eq 0 ] 2>/dev/null || UTCARG="--utc $(utc_now)"
    _hint=$("$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" $UTCARG \
              altaz2radec --alt "$_pitch" --az "$_az" 2>/dev/null)
    _hra=$(echo "$_hint"  | sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p')
    _hdec=$(echo "$_hint" | sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p')
    log "hint from app target: ra=$_hra dec=$_hdec"

    # TEST MODE: solve the stand-in and skip both camera stages. The camera
    # stages were verified separately in daylight (live-view fetch returns a
    # 960x640 frame; a full capture lands and is picked up), so this exercises
    # everything downstream of the pixels.
    if [ -n "$STUB_FRAME" ]; then
        log "STUB: solving $STUB_FRAME (utc=${STUB_UTC:-now})"
        _sol=$(solve_frame "$STUB_FRAME" "" "")
        if good_solve "$_sol"; then
            log "stub solve OK"
        else
            log "stub solve FAILED -- leaving the dialog alone"
            log "  solver said: $(echo "$_sol" | tail -c 200)"
            return 1
        fi
        _ra=$(echo  "$_sol" | sed -n "s/.*\"ra_deg\":\([-0-9.]*\).*/\1/p")
        _dec=$(echo "$_sol" | sed -n "s/.*\"dec_deg\":\([-0-9.]*\).*/\1/p")
        log "SOLVED ra=$_ra dec=$_dec"
        apply_correction "$_ra" "$_dec" "$_yaw" "$_pitch"
        return $?
    fi

    # 1. LIVE VIEW first -- no shutter, and during the armed state the app will
    #    not let the user take a full frame anyway.
    _sol=$(solve_frame "$(grab_frame)" "$_hra" "$_hdec")
    if good_solve "$_sol"; then
        log "live-view solve OK"
    else
        # 2. fall back to a full capture, as specified.
        #
        # DRY RUN MUST TOUCH NOTHING. Firing the shutter is an action on the
        # user's camera, so it belongs behind the same guard as the alignment
        # writes -- the first version fired it before the DRY_RUN check, which
        # made "safe mode" command the camera anyway.
        if [ "$DRY_RUN" = "1" ]; then
            log "live-view solve failed -- DRY RUN, not firing a capture; stopping here"
            return 1
        fi
        log "live-view solve failed -- falling back to a full capture"
        _before=$(ls -t /app/sd/normal/*.jpg /app/sd/Lapse/class_*/*.jpg 2>/dev/null | head -1)
        mount_send '1&272&2&step:1#'
        mount_send '1&272&2&step:2;point:1;time:0;para:1,-1;bulb:0;#'
        mount_send '1&272&2&step:3;point:1;time:0;photoCnt:1;#'
        _i=0; _f=""
        while [ $_i -lt 40 ]; do
            sleep 1; _i=$((_i+1))
            _f=$(ls -t /app/sd/normal/*.jpg /app/sd/Lapse/class_*/*.jpg 2>/dev/null | head -1)
            [ -n "$_f" ] && [ "$_f" != "$_before" ] && break
            _f=""
        done
        if [ -z "$_f" ]; then
            log "no full frame either -- LEAVING THE DIALOG ALONE (user taps confirm)"
            return 1
        fi
        sleep 2
        log "full frame: $_f"
        _sol=$(solve_frame "$_f" "$_hra" "$_hdec")
        good_solve "$_sol" || {
            log "full-frame solve failed too -- LEAVING THE DIALOG ALONE"
            log "  solver said: $(echo "$_sol" | tail -c 200)"
            return 1
        }
        log "full-frame solve OK"
    fi

    _ra=$(echo  "$_sol" | sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p')
    _dec=$(echo "$_sol" | sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p')
    log "SOLVED ra=$_ra dec=$_dec"

    apply_correction "$_ra" "$_dec" "$_yaw" "$_pitch"
    return $?
}

# Hand off to the guider once the target is centred and confirmed.
#
# GUIDE=1 in site.conf enables it. It runs as its own process so a guiding
# failure can never wedge the alignment path, and only ONE runs at a time --
# a second guider would fight the first for the mount.
start_guiding() {
    [ "${GUIDE:-0}" = "1" ] || return 0
    if ps 2>/dev/null | grep -q "[p]olaris-guide"; then
        log "guider already running -- not starting a second"
        return 0
    fi
    log "starting the guider on ra=$1 dec=$2 (dry_run=${GUIDE_DRY_RUN:-$DRY_RUN})"
    LAT="$LAT" LON="$LON" FOCAL_MM="$FOCAL" \
    DRY_RUN="${GUIDE_DRY_RUN:-$DRY_RUN}" \
    INTERVAL="${GUIDE_INTERVAL:-30}" THRESH_ARCSEC="${GUIDE_THRESH:-60}" \
        setsid sh "$ASTRO/polaris-guide.sh" --ra "$1" --dec "$2" \
        </dev/null >/tmp/guide.out 2>&1 &
}

# CORRECT -> CENTRE -> VERIFY -> CONFIRM
#
# The app has already slewed to the target's alt/az. Those coordinates are
# correct (they come from ephemeris, not the compass); what is wrong is where
# the mount PHYSICALLY lands for a commanded azimuth, because the compass
# heading is off. So:
#
#   1. correct the heading (527) -- now the mount's frame is truthful
#   2. re-issue the SAME goto (519) -- in the corrected frame it lands on the
#      target, centring it
#   3. solve again to check it really is centred
#   4. repeat while the residual is still large (bounded)
#   5. confirm (530 step:2), dismissing the dialog
#
# Step 3 is not optional decoration: without it we would confirm on faith. If
# the residual never comes inside tolerance we confirm nothing and leave the
# dialog for the user.
CENTRE_TOL_DEG=${CENTRE_TOL_DEG:-0.15}     # ~9 arcmin; well inside a 5.4 deg field
CENTRE_TRIES=${CENTRE_TRIES:-2}
SLEW_SETTLE=${SLEW_SETTLE:-6}

# angular separation between two alt/az pairs, degrees
sep_deg() {
    awk -v a1="$1" -v z1="$2" -v a2="$3" -v z2="$4" 'BEGIN{
        d=3.14159265358979/180;
        x=sin(a1*d)*sin(a2*d)+cos(a1*d)*cos(a2*d)*cos((z1-z2)*d);
        if(x>1)x=1; if(x<-1)x=-1; printf "%.4f", atan2(sqrt(1-x*x),x)/d }'
}

apply_correction() {
    _ra=$1; _dec=$2; _yaw=$3; _pitch=$4
    _compass=$(compass_for_solve "$_ra" "$_dec") || return 1
    _az=$(wire_to_az "$_yaw")
    log "heading correction: compass=$_compass"

    if [ "$DRY_RUN" = "1" ]; then
        log "DRY RUN -- would send:"
        log "    ->> 1&527&3&compass:$_compass;lat:$LAT;lng:$LON;#      (fix heading)"
        log "    ->> 1&519&3&state:1;yaw:$_yaw;pitch:$_pitch;lat:$LAT;track:0;speed:2;lng:$LON;#   (re-centre)"
        log "    then re-solve, and once inside ${CENTRE_TOL_DEG} deg:"
        log "    ->> 1&530&3&step:2;yaw:$_yaw;pitch:$_pitch;lat:$LAT;num:1;lng:$LON;#   (confirm)"
        return 0
    fi

    mount_send "1&527&3&compass:$_compass;lat:$LAT;lng:$LON;#"

    _try=0
    while [ $_try -lt "$CENTRE_TRIES" ]; do
        _try=$((_try + 1))
        log "centring pass $_try: re-issuing the goto in the corrected frame"
        mount_send "1&519&3&state:1;yaw:$_yaw;pitch:$_pitch;lat:$LAT;track:0;speed:2;lng:$LON;#"
        sleep "$SLEW_SETTLE"

        _f=$(grab_frame) || { log "no frame to verify centring"; break; }
        _v=$(solve_frame "$_f" "" "")
        good_solve "$_v" || { log "verification solve failed"; break; }
        _vra=$(echo  "$_v" | sed -n 's/.*"ra_deg":\([-0-9.]*\).*/\1/p')
        _vdec=$(echo "$_v" | sed -n 's/.*"dec_deg":\([-0-9.]*\).*/\1/p')
        [ -n "$STUB_UTC" ] || [ "${TZ_OFFSET_SEC:-0}" -eq 0 ] 2>/dev/null || UTCARG="--utc $(utc_now)"
        _vj=$("$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" $UTCARG \
                radec2altaz --ra "$_vra" --dec "$_vdec" 2>/dev/null)
        _valt=$(echo "$_vj" | sed -n 's/.*"alt_deg":\([-0-9.]*\).*/\1/p')
        _vaz=$(echo  "$_vj" | sed -n 's/.*"az_deg":\([-0-9.]*\).*/\1/p')
        _err=$(sep_deg "$_pitch" "$_az" "$_valt" "$_vaz")
        log "  centring residual: ${_err} deg (tolerance ${CENTRE_TOL_DEG})"
        _ok=$(awk -v e="$_err" -v t="$CENTRE_TOL_DEG" 'BEGIN{print (e<=t)?1:0}')
        if [ "$_ok" = "1" ]; then
            log "centred -- confirming (530 step:2)"
            mount_send "1&530&3&step:2;yaw:$_yaw;pitch:$_pitch;lat:$LAT;num:1;lng:$LON;#"
            log "done -- dialog should be dismissed"
            start_guiding "$_vra" "$_vdec"
            return 0
        fi
        # still off: refine the heading from this newer solve and try again
        _compass=$(compass_for_solve "$_vra" "$_vdec") || break
        log "  refining heading to compass=$_compass"
        mount_send "1&527&3&compass:$_compass;lat:$LAT;lng:$LON;#"
    done

    log "NOT confirming: could not verify the target is centred."
    log "  the heading correction was applied; tap confirm yourself if it looks right."
    return 1
}

log "clock: device is $(date "+%H:%M:%S"), TZ_OFFSET_SEC=$TZ_OFFSET_SEC -> UTC $(utc_now)"
log "autosolve watching (dry_run=$DRY_RUN focal=${FOCAL}mm gates: logodds>=$MIN_LOGODDS matches>=$MIN_MATCHES)"

LAST_YAW=""; LAST_PITCH=""
"$ASTRO/polaris-logwatch" --match "code:519" --match "code:530" | while read -r line; do
    case "$line" in
      *"code:519"*)
        LAST_YAW=$(echo   "$line" | sed -n 's/.*yaw:\([-0-9.]*\).*/\1/p')
        LAST_PITCH=$(echo "$line" | sed -n 's/.*pitch:\([-0-9.]*\).*/\1/p')
        [ -n "$LAST_YAW" ] && log "target from 519: yaw=$LAST_YAW pitch=$LAST_PITCH"
        ;;
      *"code:530"*"step:1"*)
        if [ -n "$LAST_YAW" ]; then
            handle_arm "$LAST_YAW" "$LAST_PITCH"
        else
            log "530 step:1 seen but no preceding 519 -- cannot know the target; ignoring"
        fi
        ;;
    esac
done
