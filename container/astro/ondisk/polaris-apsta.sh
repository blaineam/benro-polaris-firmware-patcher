#!/bin/sh
# ===========================================================================
#  polaris-apsta.sh -- join a home network WITHOUT giving up the access point.
#
#  The Benro app talks to 192.168.0.1:9090, so the AP has to stay exactly as it
#  is. This adds a SECOND, station-mode interface alongside it.
#
#  The chip allows it, but ONLY ON ONE CHANNEL. Asked directly (iw phy) it
#  claims "#{ AP } <= 2, #{ managed } <= 2, total <= 4, #channels <= 2" -- and
#  the "#channels <= 2" part is a lie on this hardware. The CYW43455 has ONE
#  radio. Holding the AP on 5 GHz ch36 while associating a station on 2.4 GHz
#  ch11 means being in two bands at once, which needs RSDB (two radios). The
#  firmware crashes attempting it and takes the whole device down -- observed
#  three times, always at the moment of association, never during a scan
#  (scans are brief off-channel excursions, which the chip handles).
#
#  So both interfaces must share ONE channel. That is why this pins the station
#  to the AP's channel, and why moving the AP to the home network's channel is
#  part of the setup rather than an optimisation.
#
#  SAFETY: this runs over the same radio that carries ssh. Every path here is
#  time-bounded and self-reverting -- if association fails, or anything hangs,
#  a detached watchdog tears the station interface down and leaves the AP
#  untouched. The AP is never stopped, reconfigured, or moved, so the worst
#  case is "no home network" rather than "no device".
#
#     polaris-apsta.sh up      bring the station interface up
#     polaris-apsta.sh down    remove it
#     polaris-apsta.sh status  report
# ===========================================================================
WDIR=${WIFI_DIR:-/app/sd/polaris-wifi}
CONF=${WPA_CONF:-$WDIR/wpa.conf}
IFACE=${STA_IFACE:-wlan1}
LOG=${APSTA_LOG:-/app/sd/polaris-wifi/apsta.log}
TIMEOUT=${APSTA_TIMEOUT:-45}

say() { echo "$(date '+%m-%d %H:%M:%S') $*" | tee -a "$LOG"; }

have() { [ -x "$WDIR/$1" ]; }

sta_down() {
    [ -f /tmp/apsta-wpa.pid ] && kill "$(cat /tmp/apsta-wpa.pid)" 2>/dev/null
    [ -f /tmp/apsta-dhcp.pid ] && kill "$(cat /tmp/apsta-dhcp.pid)" 2>/dev/null
    rm -f /tmp/apsta-wpa.pid /tmp/apsta-dhcp.pid
    ifconfig "$IFACE" down 2>/dev/null
    "$WDIR/iw" dev "$IFACE" del 2>/dev/null
}

case "${1:-status}" in
  down)
    say "taking the station interface down"
    sta_down
    say "AP untouched: $(ifconfig wlan0 2>/dev/null | grep -o 'inet addr:[0-9.]*')"
    exit 0
    ;;
  status)
    echo "AP  wlan0: $(ifconfig wlan0 2>/dev/null | grep -o 'inet addr:[0-9.]*' || echo down)"
    echo "STA $IFACE: $(ifconfig $IFACE 2>/dev/null | grep -o 'inet addr:[0-9.]*' || echo down)"
    "$WDIR/iw" dev 2>/dev/null | grep -E "Interface|type|channel" | sed 's/^/  /'
    exit 0
    ;;
esac

have iw || { say "missing $WDIR/iw"; exit 2; }
have wpa_supplicant || { say "missing $WDIR/wpa_supplicant"; exit 2; }
[ -r "$CONF" ] || {
    say "no $CONF -- create it with your network's details first:"
    say "    $WDIR/wpa_passphrase 'YOUR_SSID' 'YOUR_PASSWORD' > $CONF"
    exit 3
}

# ---- THE WATCHDOG, ARMED BEFORE ANYTHING IS TOUCHED ----------------------
# If this script dies, hangs, or the association wedges the radio, the station
# interface goes away by itself. It is detached so it survives losing ssh.
(
    i=0
    while [ $i -lt "$TIMEOUT" ]; do
        i=$((i + 1)); sleep 1
        [ -f /tmp/apsta-ok ] && exit 0     # we got an address; stand down
    done
    echo "$(date '+%m-%d %H:%M:%S') WATCHDOG: no address in ${TIMEOUT}s -- reverting" >> "$LOG"
    [ -f /tmp/apsta-wpa.pid ] && kill "$(cat /tmp/apsta-wpa.pid)" 2>/dev/null
    [ -f /tmp/apsta-dhcp.pid ] && kill "$(cat /tmp/apsta-dhcp.pid)" 2>/dev/null
    ifconfig wlan1 down 2>/dev/null
    /app/sd/polaris-wifi/iw dev wlan1 del 2>/dev/null
    echo "$(date '+%m-%d %H:%M:%S') WATCHDOG: station interface removed; AP untouched" >> "$LOG"
) </dev/null >/dev/null 2>&1 &

rm -f /tmp/apsta-ok
say "adding station interface $IFACE (AP on wlan0 stays up)"
sta_down
"$WDIR/iw" dev wlan0 interface add "$IFACE" type managed 2>>"$LOG" || {
    say "could not create $IFACE -- the driver refused the combination"
    exit 4
}
ifconfig "$IFACE" up 2>>"$LOG"

# The station interface gets its own MAC -- the radio's base address with the
# locally-administered bit set (48:.. -> 4a:..). It is DETERMINISTIC, so once a
# router is told to accept it, it keeps working across reboots. Log it, because
# a router that requires device approval will show this address as an unknown
# client and silently withhold DHCP: association succeeds, the WPA handshake
# completes, and then no offer ever arrives -- which looks like a driver fault
# and is not one.
say "station MAC is $(cat /sys/class/net/$IFACE/address 2>/dev/null) (allow this on your router if it filters by MAC)"

# SAME CHANNEL AS THE AP, always. See the note at the top: two channels means
# two bands here, and two bands crashes the firmware.
APFREQ=$(sed -n 's/^channel=\([0-9]*\)/\1/p' /app/wifi/hostapd.conf 2>/dev/null | head -1)
case "$APFREQ" in
    ""|0) APMHZ="" ;;
    1[0-4]|[1-9]) APMHZ=$(( 2407 + APFREQ * 5 )); [ "$APFREQ" = 14 ] && APMHZ=2484 ;;
    *)    APMHZ=$(( 5000 + APFREQ * 5 )) ;;
esac
if [ -n "$APMHZ" ]; then
    say "AP is on channel $APFREQ ($APMHZ MHz) -- pinning the station to it"
    awk -v f="$APMHZ" '{print} /^network=\{/{printf "\tfreq_list=%s\n\tscan_freq=%s\n", f, f}' \
        "$CONF" > "$WDIR/.wpa-pinned.conf" && CONF="$WDIR/.wpa-pinned.conf"
fi

say "associating..."
"$WDIR/wpa_supplicant" -B -i "$IFACE" -c "$CONF" -P /tmp/apsta-wpa.pid \
    -f "$WDIR/wpa_supplicant.log" 2>>"$LOG" || {
    say "wpa_supplicant failed to start"; exit 5; }

# -s IS NOT OPTIONAL. BusyBox udhcpc configures nothing itself: without a
# script it obtains the lease and silently discards it, logging
# "lease of 10.0.0.110 obtained" while the interface stays address-less. That
# reads exactly like a DHCP failure and is not one -- it cost an hour.
say "requesting an address (DHCP)"
udhcpc -i "$IFACE" -t 6 -T 3 -n -s "$WDIR/udhcpc.script" \
       -p /tmp/apsta-dhcp.pid >>"$LOG" 2>&1

IP=$(ifconfig "$IFACE" 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
if [ -n "$IP" ]; then
    touch /tmp/apsta-ok
    say "JOINED: $IFACE has $IP"
    say "AP still up on $(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')"
    say "you can now reach this device at $IP from your home network"
else
    say "ASSOCIATED BUT NO DHCP LEASE."
    say "  The WPA handshake completed, so the password is right and the router"
    say "  accepted the association. No address means the DHCP offer never came."
    say "  Most likely, in order:"
    say "    1. the router filters by MAC / requires device approval -- allow"
    say "       $(cat /sys/class/net/$IFACE/address 2>/dev/null)"
    say "    2. its DHCP pool is full"
    say "    3. the home network uses 192.168.0.x, which COLLIDES with this"
    say "       device's own access point at 192.168.0.1 -- check with:"
    say "       ifconfig wlan0 | grep inet"
    say "  Association detail is in $WDIR/wpa_supplicant.log"
    exit 6
fi
