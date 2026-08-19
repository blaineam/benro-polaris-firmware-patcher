#!/bin/sh
# ===========================================================================
#  Boot hook: start the plate-solver web/Alpaca server.
#
#  Installed as /app/network_telnetd.sh, which /app/bootapp runs at boot if it
#  exists. bootapp treats this as an OPTIONAL hook and falls back to
#  /app/start_agent.sh -- so before installing, confirm start_agent.sh does not
#  exist, or you may stop it from running. (On FwVer 4.0.0.32 it does not.)
#
#  MUST NOT BLOCK. bootapp runs this inline, so everything is backgrounded and
#  nothing here waits on the network.
# ===========================================================================
ASTRO=/app/astro
LOG=/tmp/polaris-astro-boot.log
PORT=${POLARIS_HTTPD_PORT:-8090}

# Observer position lives on the microSD, in
# /app/sd/polaris-astro/site.conf:
#     LAT=35.35199
#     LON=-119.17208
#     FOCAL=400
#     CAPTURE_ALIGN_FLOW=1     (optional protocol recorder)
#
# NOTE: it is NOT read here. At boot /app/sd is not mounted yet, so reading it
# at the top of this script found nothing and the hook exited without starting
# anything -- which is exactly how the first version silently failed to survive
# a reboot. It is sourced below, after the card has actually appeared.
LAT=""; LON=""; FOCAL=400
SITE=/app/sd/polaris-astro/site.conf

{
    echo "$(date) polaris-astro boot hook"

    # Wait for the microSD to actually mount. Bounded, so a missing card cannot
    # stall boot forever, but long enough that a slow card still works.
    i=0
    while [ $i -lt 45 ] && [ ! -f "$SITE" ]; do
        i=$((i + 1)); sleep 2
    done
    if [ -f "$SITE" ]; then
        . "$SITE"
        echo "site.conf loaded after ${i} tries (lat=$LAT lon=$LON focal=$FOCAL)"
    else
        echo "no $SITE after ${i} tries -- microSD not mounted?"
    fi

    i=0
    while [ $i -lt 30 ] && [ ! -x "$ASTRO/polaris-httpd" ]; do
        if [ -x /app/sd/polaris-astro/polaris-httpd ]; then
            mkdir -p "$ASTRO"
            # Every binary the daemons need. polaris-logwatch is autosolve's
            # trigger detector and polaris-match is the guider's drift
            # measurement -- omit either and the feature silently does nothing.
            for _b in polaris-httpd polaris-solve polaris-extract \
                      polaris-logwatch polaris-match polaris-skysim \
                      polaris-mount; do
                [ -f "/app/sd/polaris-astro/$_b" ] \
                    && cp "/app/sd/polaris-astro/$_b" "$ASTRO"/ 2>/dev/null
            done
            cp /app/sd/polaris-astro/*.sh "$ASTRO"/ 2>/dev/null
            chmod +x "$ASTRO"/* 2>/dev/null
        fi
        i=$((i + 1)); sleep 2
    done

    if [ ! -x "$ASTRO/polaris-httpd" ]; then
        echo "no $ASTRO/polaris-httpd after ${i} tries -- giving up"
        exit 0
    fi
    if [ -z "$LAT" ] || [ -z "$LON" ]; then
        echo "no LAT/LON (create /app/sd/polaris-astro/site.conf) -- not starting"
        exit 0
    fi

    # Idempotent: running this by hand while the server is already up must not
    # spawn a second instance that dies on "Address already in use".
    if netstat -ltn 2>/dev/null | grep -q ":$PORT "; then
        echo "something is already listening on :$PORT -- not starting a second"
        exit 0
    fi

    # WAIT FOR ALIGNMENT BEFORE STARTING THE WEB SERVER.
    #
    # The server answers Alpaca/LX200 position queries, which means reading the
    # mount -- and connecting to an UNALIGNED mount is what makes the Benro app
    # demand a compass calibration. The server already refuses to read while
    # unaligned, but the cleanest guarantee is simply not to be running yet.
    #
    # Alignment is detected PASSIVELY from the device's own 284 log lines
    # (mode/track), so this loop opens no connections at all. track:3 is the
    # never-aligned state; anything else means an alignment has completed.
    #
    # DEFAULT IS NOW 0: start immediately, so the page is there when you open
    # it. Safety comes from the server refusing to READ the mount until it is
    # aligned, not from the server being absent -- every endpoint that opens a
    # connection to the control port is gated on may_read_mount(), including
    # the Alpaca Altitude/Azimuth pair that used to slip through. Set
    # HTTPD_WAIT_ALIGNED=1 to go back to not running it at all until aligned.
    if [ "${HTTPD_WAIT_ALIGNED:-0}" = "1" ]; then
        echo "waiting for the mount to be aligned before starting polaris-httpd"
        (
            while :; do
                _t=$(grep -a "code\[284\]" /app/Mlog.txt 2>/dev/null \
                     | sed -n 's/.*track:\([0-9-][0-9]*\).*/\1/p' | tail -1)
                if [ -n "$_t" ] && [ "$_t" != "3" ] && [ "$_t" != "-1" ]; then
                    echo "$(date) mount reports track:$_t -- starting polaris-httpd on :$PORT"
                    setsid "$ASTRO/polaris-httpd" --port "$PORT" --lat "$LAT" \
                        --lon "$LON" --focal "$FOCAL" </dev/null >/tmp/httpd.log 2>&1 &
                    exit 0
                fi
                sleep 10
            done
        ) >> "$LOG" 2>&1 &
    else
        echo "starting polaris-httpd on :$PORT (lat $LAT lon $LON focal ${FOCAL}mm)"
        setsid "$ASTRO/polaris-httpd" --port "$PORT" --lat "$LAT" --lon "$LON" \
            --focal "$FOCAL" </dev/null >/tmp/httpd.log 2>&1 &
    fi

    # Optional protocol recorder, for working out the alignment flow. Set
    # CAPTURE_ALIGN_FLOW=1 in site.conf to enable. Uses polaris-logwatch (20 ms,
    # truncation-safe) rather than a shell poll -- Mlog.txt is truncated every
    # few seconds and a 1 s shell loop demonstrably loses whole messages.
    if [ "${CAPTURE_ALIGN_FLOW:-0}" = "1" ] && [ -x "$ASTRO/polaris-logwatch" ] \
       && ! pgrep -f "align-flow.log" >/dev/null 2>&1; then
        echo "starting align-flow recorder -> /app/sd/align-flow.log"
        setsid "$ASTRO/polaris-logwatch" \
            --match "code:530" --match "code:531" --match "code:519" \
            --match "code:527" --match "code:285" --match "code:517" \
            --match "code[530]" --match "code[531]" --match "code[519]" \
            --out /app/sd/align-flow.log </dev/null >/dev/null 2>&1 &
    fi
    # Auto-solve during the app's calibration. Set AUTOSOLVE=1 in site.conf.
    #
    # AUTOSOLVE_DRY_RUN defaults to 1 ON PURPOSE: armed, this writes a heading
    # correction and an alignment confirm to a live mount. Run it dry once,
    # confirm from /app/sd/polaris-autosolve.log that the solve is good and the
    # correction looks sane, THEN set AUTOSOLVE_DRY_RUN=0.
    if [ "${AUTOSOLVE:-0}" = "1" ] && [ -x "$ASTRO/polaris-autosolve.sh" ]; then
        # Idempotent, like the httpd guard above. Without this a second run of
        # the hook (or a manual restart that killed only ONE pid) leaves two
        # daemons reacting to the same alignment -- observed on hardware, with
        # both a stub-mode and a live-mode instance answering one injected 530.
        if pgrep -f "polaris-autosolve" >/dev/null 2>&1; then
            echo "autosolve already running -- not starting a second"
            exit 0
        fi
        # NAME TRANSLATION, do not "simplify" this: site.conf says FOCAL and
        # AUTOSOLVE_DRY_RUN, the daemon reads FOCAL_MM and DRY_RUN. Launching it
        # without this mapping silently gives focal=400mm and dry_run=1 -- it
        # looks like it started fine and then solves nothing and touches nothing.
        echo "starting autosolve (dry_run=${AUTOSOLVE_DRY_RUN:-1} focal=${FOCAL}mm)"
        LAT="$LAT" LON="$LON" FOCAL_MM="$FOCAL" \
        DRY_RUN="${AUTOSOLVE_DRY_RUN:-1}" \
        STUB_FRAME="${STUB_FRAME:-}" STUB_UTC="${STUB_UTC:-}" \
        GUIDE="${GUIDE:-0}" GUIDE_DRY_RUN="${GUIDE_DRY_RUN:-1}" \
        GUIDE_INTERVAL="${GUIDE_INTERVAL:-30}" GUIDE_THRESH="${GUIDE_THRESH:-60}" \
        CENTRE_TOL_DEG="${CENTRE_TOL_DEG:-0.15}" MAX_ALIGN_ALT="${MAX_ALIGN_ALT:-65}" \
        MIN_LOGODDS="${MIN_LOGODDS:-100}" MIN_MATCHES="${MIN_MATCHES:-12}" \
        FOCAL_MIN="${FOCAL_MIN:-8}" FOCAL_MAX="${FOCAL_MAX:-3000}" \
        RANGE_TIMEOUT="${RANGE_TIMEOUT:-240}" KEEP_FAILED="${KEEP_FAILED:-20}" \
        setsid sh "$ASTRO/polaris-autosolve.sh" </dev/null >/tmp/autosolve.out 2>&1 &
    fi
} >> "$LOG" 2>&1 &

exit 0
