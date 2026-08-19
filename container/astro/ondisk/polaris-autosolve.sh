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
UTCARG=""
[ -n "$STUB_UTC" ] && UTCARG="--utc $STUB_UTC"
if [ -n "$STUB_FRAME" ] || [ -n "$STUB_UTC" ]; then
    DRY_RUN=${DRY_RUN:-1}
fi
: "${LAT:?set LAT}"; : "${LON:?set LON}"
export LAT LON

log() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; echo "$(date +%H:%M:%S) $*" >&2; }

mount_send() { "$ASTRO/polaris-mount" --host 127.0.0.1 send --msg "$1" >/dev/null 2>&1; }

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
    if [ -n "$_hra" ]; then
        DOWNSAMPLE="$_ds" FOCAL_MM="$FOCAL" HINT_RA="$_hra" HINT_DEC="$_hdec" \
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
        if [ "$DRY_RUN" = "1" ]; then
            log "DRY RUN -- would correct heading then confirm:"
            "$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" $UTCARG --dry-run \
                align --solved-ra "$_ra" --solved-dec "$_dec" 2>&1 | sed "s/^/    /" >> "$LOG"
            log "    ->> 1&530&3&step:2;yaw:$_yaw;pitch:$_pitch;lat:$LAT;num:1;lng:$LON;#"
            return 0
        fi
        "$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" $UTCARG \
            align --solved-ra "$_ra" --solved-dec "$_dec" >>"$LOG" 2>&1
        mount_send "1&530&3&step:2;yaw:$_yaw;pitch:$_pitch;lat:$LAT;num:1;lng:$LON;#"
        log "done"
        return 0
    fi

    # 1. LIVE VIEW first -- no shutter, and during the armed state the app will
    #    not let the user take a full frame anyway.
    rm -f /tmp/autosolve-lv.jpg
    "$ASTRO/polaris-mount" fetch --host 127.0.0.1 --port "$LIVEVIEW_PORT" \
        --url-path "/?action=snapshot" --out /tmp/autosolve-lv.jpg >/dev/null 2>&1
    _sol=$(solve_frame /tmp/autosolve-lv.jpg "$_hra" "$_hdec")
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

    if [ "$DRY_RUN" = "1" ]; then
        log "DRY RUN -- would correct heading then confirm:"
        "$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" $UTCARG --dry-run \
            align --solved-ra "$_ra" --solved-dec "$_dec" 2>&1 | sed "s/^/    /" >> "$LOG"
        log "    ->> 1&530&3&step:2;yaw:$_yaw;pitch:$_pitch;lat:$LAT;num:1;lng:$LON;#"
        return 0
    fi

    # 4. correct the heading (527)
    log "correcting heading"
    "$ASTRO/polaris-mount" --lat "$LAT" --lon "$LON" $UTCARG \
        align --solved-ra "$_ra" --solved-dec "$_dec" >>"$LOG" 2>&1

    # 5. confirm for the user -- same form the app itself sends
    log "confirming (530 step:2)"
    mount_send "1&530&3&step:2;yaw:$_yaw;pitch:$_pitch;lat:$LAT;num:1;lng:$LON;#"
    log "done -- dialog should be dismissed"
}

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
