#!/bin/sh
# ===========================================================================
#  camera-death-watch.sh -- flight recorder for "the camera battery died and
#  the mount drove itself somewhere bad".
#
#  THE PROBLEM: when the camera dies mid-session the Polaris moves its motors
#  into a bad position. To fix that we need to know what it did and why, and
#  the event is unpredictable -- it happens when a battery happens to run out.
#  So this runs continuously and cheaply, and when the camera goes away it
#  writes a self-contained dossier.
#
#  WHY A ROLLING BUFFER: /app/Mlog.txt is TRUNCATED by the device, repeatedly
#  and without warning -- found at 0 bytes minutes after holding thousands of
#  lines. Reading it after the fact gets you nothing. polaris-logwatch follows
#  it at 20 ms and is truncation-safe, so we keep our own copy and can look
#  BACKWARDS from the moment of failure.
#
#  Captures nothing but local state: log, dmesg, process table, USB tree. It
#  opens no connection to the mount, so it cannot influence what it records.
# ===========================================================================
ASTRO=${ASTRO:-/app/astro}
DIR=${CAMERA_WATCH_DIR:-/app/sd/polaris-astro}
ROLL="$DIR/camera-roll.log"          # rolling copy of the device log
STATE="$DIR/camera-death-state"
KEEP_LINES=${CAMERA_ROLL_LINES:-4000}
POST_SECONDS=${CAMERA_POST_SECONDS:-180}

mkdir -p "$DIR" 2>/dev/null

# 1. rolling copy of everything the device logs
[ -x "$ASTRO/polaris-logwatch" ] || { echo "missing polaris-logwatch" >&2; exit 2; }
"$ASTRO/polaris-logwatch" --out "$ROLL" &
LW=$!
trap 'kill $LW 2>/dev/null; exit 0' INT TERM

usb_snapshot() {
    for d in /sys/bus/usb/devices/*/; do
        [ -r "$d/idVendor" ] || continue
        printf '    %s %s:%s %s\n' "$(basename "$d")" \
            "$(cat "$d/idVendor" 2>/dev/null)" "$(cat "$d/idProduct" 2>/dev/null)" \
            "$(cat "$d/product" 2>/dev/null)"
    done
}

# Canon EOS bodies enumerate as 04a9:xxxx. Presence of ANY 04a9 device is our
# "the camera is attached" signal -- more reliable than parsing log text.
camera_present() {
    for d in /sys/bus/usb/devices/*/idVendor; do
        [ -r "$d" ] || continue
        [ "$(cat "$d" 2>/dev/null)" = "04a9" ] && return 0
    done
    return 1
}

PREV_DMESG=$(dmesg 2>/dev/null | wc -l)
WAS=0
camera_present && WAS=1
echo "$(date '+%m-%d %H:%M:%S') camera-death-watch started (camera present=$WAS)" >> "$DIR/camera-watch.log"

while :; do
    NOW=0
    camera_present && NOW=1

    if [ "$WAS" = "1" ] && [ "$NOW" = "0" ]; then
        # ---- THE EVENT ----
        TS=$(date '+%Y%m%d-%H%M%S')
        F="$DIR/camera-death-$TS.log"
        {
            echo "=============================================================="
            echo " CAMERA DISAPPEARED FROM USB"
            echo " when: $(date '+%Y-%m-%d %H:%M:%S')  (device clock)"
            echo " uptime: $(uptime 2>/dev/null | head -1)"
            echo "=============================================================="
            echo
            echo "--- what was running ---"
            ps 2>/dev/null
            echo
            echo "--- USB tree now (camera should be absent) ---"
            usb_snapshot
            echo
            echo "--- kernel messages (tail) ---"
            dmesg 2>/dev/null | tail -60
            echo
            echo "--- DEVICE LOG, THE 400 LINES BEFORE THE EVENT ---"
            echo "--- this is the part that says what the mount decided to do ---"
            tail -400 "$ROLL" 2>/dev/null
            echo
            echo "--- now recording for ${POST_SECONDS}s AFTER the event ---"
        } > "$F" 2>&1
        echo "$(date '+%m-%d %H:%M:%S') CAMERA LOST -- dossier: $F" >> "$DIR/camera-watch.log"

        MARK=$(wc -c < "$ROLL" 2>/dev/null || echo 0)
        D0=$(dmesg 2>/dev/null | wc -l)
        i=0
        while [ $i -lt "$POST_SECONDS" ]; do
            i=$((i + 5)); sleep 5
        done
        {
            echo
            echo "--- DEVICE LOG AFTER THE EVENT (${POST_SECONDS}s) ---"
            dd if="$ROLL" bs=1 skip="$MARK" 2>/dev/null
            echo
            echo "--- NEW kernel messages after the event ---"
            D1=$(dmesg 2>/dev/null | wc -l)
            [ "$D1" -gt "$D0" ] && dmesg 2>/dev/null | tail -n $((D1 - D0))
            echo
            echo "--- processes after ---"
            ps 2>/dev/null
            echo
            echo "=== end of dossier ==="
        } >> "$F" 2>&1
        echo "$(date '+%m-%d %H:%M:%S') dossier complete: $F" >> "$DIR/camera-watch.log"
    elif [ "$WAS" = "0" ] && [ "$NOW" = "1" ]; then
        echo "$(date '+%m-%d %H:%M:%S') camera reappeared on USB" >> "$DIR/camera-watch.log"
    fi
    WAS=$NOW

    # keep the rolling buffer bounded -- this runs for days
    if [ "$(wc -l < "$ROLL" 2>/dev/null || echo 0)" -gt $((KEEP_LINES * 2)) ]; then
        tail -n "$KEEP_LINES" "$ROLL" > "$ROLL.tmp" 2>/dev/null && mv "$ROLL.tmp" "$ROLL"
    fi
    sleep 2
done
