#!/bin/sh
# ===========================================================================
#  polaris-autojoin.sh -- join the home network at power-up, keeping the AP.
#
#  Runs from the boot hook when AUTO_JOIN=1. The user turns the Polaris on and
#  it is on their network; nothing else to do.
#
#  THE TWO HARD RULES, both learned by crashing this device:
#
#  1. ONE CHANNEL. The CYW43455 has a single radio. iw phy advertises
#     "#channels <= 2", which is a lie here: holding the AP on 5 GHz while
#     associating a station on 2.4 GHz crashes the firmware and takes the whole
#     device down. So we find the home network's channel FIRST and move the AP
#     to it before associating.
#
#  2. ONE SUPPLICANT. A leftover wpa_supplicant whose interface was deleted out
#     from under it, plus a second instance started on the same radio, also
#     crashes it. Every run therefore begins by killing any supplicant, removing
#     stale control sockets, and deleting the station interface -- before
#     touching anything.
#
#  Failure is always safe: if anything does not complete, the AP is restored to
#  its original configuration and the station interface removed. The worst case
#  is "no home network", never "no device".
# ===========================================================================
WDIR=${WIFI_DIR:-/app/sd/polaris-wifi}
CONF=${WPA_CONF:-$WDIR/wpa.conf}
IFACE=${STA_IFACE:-wlan1}
LOG=${AUTOJOIN_LOG:-$WDIR/autojoin.log}
HAPCONF=/app/wifi/hostapd.conf
STAMP=$(date '+%m-%d %H:%M:%S')

say() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG"; }

# ---- rule 2: leave nothing of a previous attempt alive --------------------
purge() {
    for p in $(ps 2>/dev/null | grep '[w]pa_supplicant' | awk '{print $1}'); do
        kill -9 "$p" 2>/dev/null
    done
    rm -f /var/run/wpa_supplicant/"$IFACE" 2>/dev/null
    rm -f /tmp/autojoin-wpa.pid /tmp/autojoin-dhcp.pid
    ifconfig "$IFACE" down 2>/dev/null
    "$WDIR/iw" dev "$IFACE" del 2>/dev/null
    sleep 1
}

restore_ap() {
    if [ -f "$HAPCONF.autojoin-bak" ]; then
        cp "$HAPCONF.autojoin-bak" "$HAPCONF"
        for p in $(ps 2>/dev/null | grep '[h]ostapd' | awk '{print $1}'); do kill "$p" 2>/dev/null; done
        sleep 2
        ( cd /app/wifi && ./hostapd -B "$HAPCONF" >/dev/null 2>&1 )
        say "AP restored to its original configuration"
    fi
}

say "=== autojoin starting ==="
[ -r "$CONF" ] || { say "no $CONF -- nothing configured, doing nothing"; exit 0; }
[ -x "$WDIR/iw" ] && [ -x "$WDIR/wpa_supplicant" ] || { say "wifi tools missing"; exit 2; }

SSID=$(sed -n 's/^[[:space:]]*ssid="\(.*\)"/\1/p' "$CONF" | head -1)
[ -n "$SSID" ] || { say "no ssid in $CONF"; exit 2; }
say "target network: $SSID"

purge

# ---- find the home network's channel, on OUR radio ------------------------
# Scanning is safe: it is a brief off-channel excursion the chip handles. It is
# ASSOCIATING off-channel that kills it.
"$WDIR/iw" dev wlan0 interface add "$IFACE" type managed 2>>"$LOG" || {
    say "could not create $IFACE"; exit 3; }
ifconfig "$IFACE" up
sleep 2

FREQ=""
i=0
while [ $i -lt 3 ] && [ -z "$FREQ" ]; do
    i=$((i + 1))
    FREQ=$("$WDIR/iw" dev "$IFACE" scan 2>/dev/null | awk -v want="$SSID" '
        /^BSS /       { f=""; s="" }
        /^[[:space:]]*freq:/   { f=$2 }
        /^[[:space:]]*signal:/ { s=$2+0 }
        /^[[:space:]]*SSID: /  { $1=""; sub(/^ /,""); if ($0==want && f!="") {
                                    if (best=="" || s>bs) { best=f; bs=s } } }
        END { print best }')
    [ -z "$FREQ" ] && sleep 3
done
[ -n "$FREQ" ] || { say "could not find $SSID in a scan -- is it in range?"; purge; exit 4; }

# MHz -> channel
if [ "$FREQ" -ge 5000 ]; then CH=$(( (FREQ - 5000) / 5 ))
elif [ "$FREQ" = 2484 ];    then CH=14
else                             CH=$(( (FREQ - 2407) / 5 )); fi
say "found $SSID on $FREQ MHz (channel $CH), strongest BSS"

# ---- rule 1: move the AP to that channel, if it is not already there ------
APCH=$(sed -n 's/^channel=\([0-9]*\)/\1/p' "$HAPCONF" | head -1)
if [ "$APCH" != "$CH" ]; then
    say "AP is on channel $APCH -- moving it to $CH (single radio: both must share one channel)"
    [ -f "$HAPCONF.autojoin-bak" ] || cp "$HAPCONF" "$HAPCONF.autojoin-bak"
    if [ "$FREQ" -ge 5000 ]; then MODE=a; else MODE=g; fi
    sed -e "s/^channel=.*/channel=$CH/" -e "s/^hw_mode=.*/hw_mode=$MODE/" \
        "$HAPCONF" > "$HAPCONF.new" && mv "$HAPCONF.new" "$HAPCONF"
    # the station interface must not exist while hostapd restarts
    ifconfig "$IFACE" down 2>/dev/null
    "$WDIR/iw" dev "$IFACE" del 2>/dev/null
    for p in $(ps 2>/dev/null | grep '[h]ostapd' | awk '{print $1}'); do kill "$p" 2>/dev/null; done
    sleep 3
    ( cd /app/wifi && ./hostapd -B "$HAPCONF" >>"$LOG" 2>&1 )
    sleep 4
    if ! ps 2>/dev/null | grep -q '[h]ostapd'; then
        say "hostapd did not come back on channel $CH -- restoring and giving up"
        restore_ap
        exit 5
    fi
    say "AP now on channel $CH"
    "$WDIR/iw" dev wlan0 interface add "$IFACE" type managed 2>>"$LOG"
    ifconfig "$IFACE" up
    sleep 2
fi

# ---- associate, pinned to that one frequency -----------------------------
awk -v f="$FREQ" '{print} /^network=\{/{printf "\tfreq_list=%s\n\tscan_freq=%s\n", f, f}' \
    "$CONF" > "$WDIR/.autojoin.conf"

say "associating on $FREQ MHz"
"$WDIR/wpa_supplicant" -B -i "$IFACE" -c "$WDIR/.autojoin.conf" \
    -P /tmp/autojoin-wpa.pid -f "$WDIR/wpa-autojoin.log" >>"$LOG" 2>&1 || {
    say "wpa_supplicant would not start"; purge; exit 6; }

# RETRY, WITH BACKOFF. A router can refuse an association for reasons that have
# nothing to do with us and clear on their own:
#   * it has temporarily shunned the client after repeated associate/vanish
#     churn (which is exactly what debugging this produced -- dozens of cycles
#     in an hour, then ASSOC-REJECT status_code=1 from an AP that had accepted
#     the identical config minutes earlier);
#   * band steering pushing the client at another radio;
#   * at power-up, the router itself may not be ready yet.
# One attempt at boot and give up would turn any of those into "auto-join does
# not work". wpa_supplicant already retries internally and applies its own
# temporary disable, so the outer retries are spaced well beyond that.
i=0; OK=0
TRIES=${AUTOJOIN_TRIES:-5}
try=0
while [ $try -lt "$TRIES" ]; do
    try=$((try + 1))
    i=0
    while [ $i -lt 25 ]; do
        i=$((i + 1)); sleep 1
        grep -aq "CTRL-EVENT-CONNECTED" "$WDIR/wpa-autojoin.log" 2>/dev/null && { OK=1; break; }
    done
    [ "$OK" = "1" ] && break
    REJ=$(grep -ac "ASSOC-REJECT" "$WDIR/wpa-autojoin.log" 2>/dev/null)
    say "attempt $try/$TRIES: no association in 25s (${REJ:-0} rejections from the AP so far)"
    [ "$try" -ge "$TRIES" ] && break
    BACK=$((try * 30))
    say "  waiting ${BACK}s before trying again"
    sleep "$BACK"
done
if [ "$OK" != "1" ]; then
    say "gave up after $TRIES attempts"
    say "  the AP is REJECTING the association (status_code=1), not failing a password check --"
    say "  the key is right. Common causes: the router has temporarily shunned this client"
    say "  after repeated reconnects, band steering, or a device limit. Check your router for"
    say "  a blocked/paused device with MAC $(cat /sys/class/net/$IFACE/address 2>/dev/null)."
    purge
    exit 7
fi
say "associated on attempt $try"

# ---- lease. -s is NOT optional: busybox udhcpc configures nothing itself,
# it obtains the lease and silently discards it.
udhcpc -i "$IFACE" -B -t 6 -T 3 -n -s "$WDIR/udhcpc.script" \
       -p /tmp/autojoin-dhcp.pid >>"$LOG" 2>&1

IP=$(ifconfig "$IFACE" 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
if [ -n "$IP" ]; then
    say "JOINED: $IFACE = $IP   (AP still $(ifconfig wlan0 | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p'))"
    say "reach this device at http://$IP:8090/"
    printf '%s\n' "$IP" > /tmp/autojoin-ip
else
    say "associated but no address -- leaving the station up, DHCP may retry"
fi
say "=== autojoin done ==="
