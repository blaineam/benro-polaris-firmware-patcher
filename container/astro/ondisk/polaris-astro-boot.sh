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
            cp /app/sd/polaris-astro/polaris-httpd \
               /app/sd/polaris-astro/polaris-solve \
               /app/sd/polaris-astro/polaris-extract \
               /app/sd/polaris-astro/polaris-logwatch \
               /app/sd/polaris-astro/polaris-mount "$ASTRO"/ 2>/dev/null
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

    echo "starting polaris-httpd on :$PORT (lat $LAT lon $LON focal ${FOCAL}mm)"
    setsid "$ASTRO/polaris-httpd" --port "$PORT" --lat "$LAT" --lon "$LON" \
        --focal "$FOCAL" </dev/null >/tmp/httpd.log 2>&1 &

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
        echo "starting autosolve (dry_run=${AUTOSOLVE_DRY_RUN:-1})"
        LAT="$LAT" LON="$LON" FOCAL_MM="$FOCAL" \
        DRY_RUN="${AUTOSOLVE_DRY_RUN:-1}" \
        MIN_LOGODDS="${MIN_LOGODDS:-100}" MIN_MATCHES="${MIN_MATCHES:-12}" \
        setsid sh "$ASTRO/polaris-autosolve.sh" </dev/null >/tmp/autosolve.out 2>&1 &
    fi
} >> "$LOG" 2>&1 &

exit 0
