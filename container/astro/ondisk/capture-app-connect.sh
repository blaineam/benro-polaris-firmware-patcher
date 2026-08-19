#!/bin/sh
# ===========================================================================
#  capture-app-connect.sh -- record a FRESH Benro app connection from the
#  first byte, to find what makes the device count it as a wifi client.
#
#  THE QUESTION: the firmware powers wifi down 60 s after WifiCount reaches 0.
#  A plain TCP connection to the control port never enters that table --
#  SP_ClientCtxAdd never fires, and closing it gives "not find this is id[N]".
#  So something else registers the app. This captures a real one connecting.
#
#  WHY IT USES polaris-logwatch AND NOT A SHELL LOOP: the device TRUNCATES
#  /app/Mlog.txt, and it does so often -- it was found at 0 bytes minutes after
#  holding thousands of lines. A `wc -c` + `dd` loop at one-second intervals
#  loses whole sessions that way, which is exactly how the first attempt at
#  this capture came back empty. logwatch polls at 20 ms and handles truncation.
# ===========================================================================
ASTRO=${ASTRO:-/app/astro}
OUT=${CAPTURE_OUT:-/app/sd/app-connect-capture.log}
CONNS=${CAPTURE_CONNS:-/app/sd/app-connect-conns.log}

echo "$(date '+%m-%d %H:%M:%S') === capture started; open the Benro app ===" >> "$OUT"

# 1. every line the device logs, unfiltered -- no --match means pass everything
[ -x "$ASTRO/polaris-logwatch" ] || { echo "missing $ASTRO/polaris-logwatch" >&2; exit 2; }
"$ASTRO/polaris-logwatch" --out "$OUT" &
LW=$!

# 2. the connection table whenever it changes, so a registration on a DIFFERENT
#    port (80/lighttpd, 8080/video) is not missed
PREV=""
{
    echo "$(date '+%m-%d %H:%M:%S') === connection tracking started ==="
    while :; do
        C=$(netstat -tn 2>/dev/null | awk '$6 == "ESTABLISHED" { print $4" <- "$5 }' | sort)
        if [ "$C" != "$PREV" ]; then
            echo "$(date '+%H:%M:%S') CONNECTIONS:"
            echo "$C" | sed 's/^/    /'
            PREV="$C"
        fi
        sleep 1
    done
} >> "$CONNS" 2>&1 &
CT=$!

trap 'kill $LW $CT 2>/dev/null; exit 0' INT TERM
wait
