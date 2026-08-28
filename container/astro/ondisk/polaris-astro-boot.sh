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

    # CHAIN ANY HOOK THAT WAS HERE FIRST. /app/network_telnetd.sh is a single
    # slot and the patcher's ssh option uses the same file, so install_astro.sh
    # moves an existing one aside rather than deleting it. Run it before we do
    # anything, so ssh comes up even if everything below fails.
    if [ -x /app/network_telnetd.pre-astro.sh ]; then
        echo "running pre-existing hook /app/network_telnetd.pre-astro.sh"
        /app/network_telnetd.pre-astro.sh || echo "  (it returned $?)"
    fi

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

    # SILENCE THE KERNEL CONSOLE. FIRST, before anything else runs.
    #
    # The wifi driver floods the log with
    #     dhd_tcpdata_info_get 1056: No more free tdata_psh_info!!
    # when its TCP-flow pool is exhausted, which on this firmware is most of
    # the time. The kernel command line is console=ttyAMA0,115200, so EVERY one
    # of those lines is written out a 115200-baud serial port -- about 5 ms of
    # blocked kernel time each. A few per second is invisible; under TCP load
    # the rate explodes and the kernel spends its life in serial output.
    #
    # The symptom is distinctive and was observed twice: ping keeps answering
    # (ICMP is handled in softirq) while EVERY TCP service stops -- ssh accepts
    # the connection and never sends a banner, the web server and the Benro
    # control port both go silent, and eventually the box reboots itself.
    #
    # console_loglevel 1 stops them reaching the serial port. They are still
    # recorded in the kernel ring buffer, so dmesg still has them for
    # diagnosis; they simply no longer block the system to print.
    #
    # PRINTK_QUIET=0 in site.conf leaves the console alone.
    if [ "${PRINTK_QUIET:-1}" = "1" ]; then
        echo 1 > /proc/sys/kernel/printk 2>/dev/null \
            && echo "console loglevel -> 1 (driver log flood no longer blocks on the serial port)"
    fi

    # TRACK WATCHER -- always. Keeps /tmp/polaris-track current by following the
    # device log continuously, because the log is truncated too fast for anyone
    # to grep it on demand. Everything that gates on "is the mount aligned"
    # reads that file. No connections, no cost.
    if [ -x /app/sd/polaris-astro/polaris-trackwatch.sh ]; then
        mkdir -p "$ASTRO"
        cp /app/sd/polaris-astro/polaris-trackwatch.sh "$ASTRO"/ 2>/dev/null
        chmod 755 "$ASTRO/polaris-trackwatch.sh" 2>/dev/null
        echo "starting trackwatch"
        setsid "$ASTRO/polaris-trackwatch.sh" </dev/null >/dev/null 2>&1 &
    fi

    # AUTO-JOIN THE HOME NETWORK. Set from the dashboard; the flag lives on the
    # microSD so the choice survives a power cycle. Runs in the background --
    # it moves the AP to the home network's channel and restarts hostapd, which
    # briefly drops every client, so it must not block the rest of boot.
    #
    # Deliberately AFTER the astro daemons start: if the join fails or the radio
    # misbehaves, plate solving and the app path are already up and unaffected.
    if [ -f /app/sd/polaris-wifi/autojoin ] && [ -x /app/sd/polaris-wifi/polaris-autojoin.sh ]; then
        echo "auto-join enabled -- joining the home network in the background"
        setsid /app/sd/polaris-wifi/polaris-autojoin.sh </dev/null >/dev/null 2>&1 &
    fi

    # CAMERA FLIGHT RECORDER. Catches the "camera battery died and the mount
    # drove somewhere bad" event, which cannot be reproduced on demand -- it
    # happens when a battery happens to run out. CAMERA_WATCH=0 to skip.
    if [ "${CAMERA_WATCH:-1}" = "1" ] && [ -x /app/sd/polaris-astro/camera-death-watch.sh ]; then
        mkdir -p "$ASTRO"
        cp /app/sd/polaris-astro/camera-death-watch.sh "$ASTRO"/ 2>/dev/null
        chmod 755 "$ASTRO/camera-death-watch.sh" 2>/dev/null
        echo "starting camera-death-watch"
        setsid "$ASTRO/camera-death-watch.sh" </dev/null >/dev/null 2>&1 &
    fi

    # WIFI WATCHDOG -- always, and first.
    #
    # The AP disappears when the phone disconnects and only a power cycle
    # brings it back. That cannot be observed over ssh, because ssh dies with
    # the AP, so this records local state to the microSD where it survives both
    # the fault and the power cycle. It opens no sockets and connects to
    # nothing, so it cannot be causing what it measures. WIFI_WATCH=0 to skip.
    if [ "${WIFI_WATCH:-1}" = "1" ] && [ -x /app/sd/polaris-astro/wifi-watch.sh ]; then
        mkdir -p "$ASTRO"
        cp /app/sd/polaris-astro/wifi-watch.sh "$ASTRO"/ 2>/dev/null
        chmod 755 "$ASTRO/wifi-watch.sh" 2>/dev/null
        if ! ps 2>/dev/null | grep -q "[w]ifi-watch"; then
            echo "starting wifi-watch"
            setsid "$ASTRO/wifi-watch.sh" </dev/null >/dev/null 2>&1 &
        fi
    fi

    # MASTER SWITCH. POLARIS_ASTRO=0 in site.conf starts nothing below this
    # point -- no web server, no autosolve, no guider -- which gives a clean
    # baseline for deciding whether a device-level fault has anything to do
    # with this project at all. The watchdog above still runs, so the baseline
    # is still recorded.
    if [ "${POLARIS_ASTRO:-1}" != "1" ]; then
        echo "POLARIS_ASTRO=0 -- starting nothing else (baseline mode)"
        exit 0
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
    # The observing position is OPTIONAL now. Without it the server still starts,
    # so the page is reachable and the location can be set there (Settings >
    # Observing site, via the browser GPS or by hand) -- /api/site persists it
    # back to this site.conf, so the next boot picks it up. Only pass --lat/--lon
    # when we actually have both; polaris-httpd starts fine without them.
    POS=""
    if [ -n "$LAT" ] && [ -n "$LON" ]; then POS="--lat $LAT --lon $LON"; fi

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
                    setsid "$ASTRO/polaris-httpd" --port "$PORT" $POS \
                        --focal "$FOCAL" </dev/null >/tmp/httpd.log 2>&1 &
                    exit 0
                fi
                sleep 10
            done
        ) >> "$LOG" 2>&1 &
    else
        echo "starting polaris-httpd on :$PORT (lat ${LAT:-unset} lon ${LON:-unset} focal ${FOCAL}mm)"
        setsid "$ASTRO/polaris-httpd" --port "$PORT" $POS \
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
    # Keep the wifi radio awake, if it was turned on from the web UI. The flag
    # lives on the microSD so the choice survives a reboot. The helper itself
    # waits for the mount to be aligned before it connects to anything.
    if [ -f /app/sd/polaris-astro/keep-wifi-awake ] && [ -x "$ASTRO/wifi-keepalive.sh" ]; then
        if ! ps 2>/dev/null | grep -q "[w]ifi-keepalive"; then
            echo "starting wifi-keepalive (keep-wifi-awake is set)"
            setsid "$ASTRO/wifi-keepalive.sh" </dev/null >/dev/null 2>&1 &
        fi
    fi

    # Protocol capture of a fresh app connection. CAPTURE_APP_CONNECT=1 in
    # site.conf. Survives a power cycle, which matters because the fault being
    # investigated (wifi auto-off) ends in one.
    if [ "${CAPTURE_APP_CONNECT:-0}" = "1" ] && [ -x "$ASTRO/capture-app-connect.sh" ]; then
        echo "starting app-connect capture"
        setsid "$ASTRO/capture-app-connect.sh" </dev/null >/dev/null 2>&1 &
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
        GUIDE_ON_TRACK="${GUIDE_ON_TRACK:-1}" \
        GUIDE_INTERVAL="${GUIDE_INTERVAL:-30}" GUIDE_THRESH="${GUIDE_THRESH:-60}" \
        CENTRE_TOL_DEG="${CENTRE_TOL_DEG:-0.15}" MAX_ALIGN_ALT="${MAX_ALIGN_ALT:-65}" \
        MIN_LOGODDS="${MIN_LOGODDS:-100}" MIN_MATCHES="${MIN_MATCHES:-12}" \
        FOCAL_MIN="${FOCAL_MIN:-8}" FOCAL_MAX="${FOCAL_MAX:-3000}" \
        RANGE_TIMEOUT="${RANGE_TIMEOUT:-240}" KEEP_FAILED="${KEEP_FAILED:-20}" \
        setsid sh "$ASTRO/polaris-autosolve.sh" </dev/null >/tmp/autosolve.out 2>&1 &
    fi
} >> "$LOG" 2>&1 &

exit 0
