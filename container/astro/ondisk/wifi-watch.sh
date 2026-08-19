#!/bin/sh
# ===========================================================================
#  wifi-watch.sh -- record what happens to the access point.
#
#  WHY THIS EXISTS: the AP disappears when the phone disconnects, and the
#  Benro app cannot bring it back without a power cycle. It cannot be observed
#  live over ssh, because ssh runs over that same AP -- the moment the fault
#  occurs the observer is disconnected too. So we log to the microSD, which
#  survives both the fault and the power cycle, and read it afterwards.
#
#  Records ONLY local state: process table, interface flags, ARP, and the
#  device's own log. It opens no sockets and touches nothing, so it cannot be
#  the cause of what it is measuring.
# ===========================================================================
OUT=${WIFI_WATCH_LOG:-/app/sd/wifi-watch.log}
INT=${WIFI_WATCH_INTERVAL:-2}
MLOG=/app/Mlog.txt

# keep the file bounded across long runs
if [ -f "$OUT" ] && [ "$(wc -c < "$OUT" 2>/dev/null || echo 0)" -gt 4000000 ]; then
    mv "$OUT" "$OUT.1" 2>/dev/null
fi

say() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$OUT"; }

say "=== wifi-watch started (pid $$) ==="
say "hostapd: $(ps 2>/dev/null | grep '[h]ostapd' | head -1)"
say "wifi_bt: $(ps 2>/dev/null | grep '[p]olaris_wifi_bt' | head -1)"

PREV=""
PREV_DMESG=$(dmesg 2>/dev/null | wc -l)
PREV_MLOG=$(wc -c < "$MLOG" 2>/dev/null || echo 0)

while :; do
    HAP=$(ps 2>/dev/null | grep -c '[h]ostapd')
    WBT=$(ps 2>/dev/null | grep -c '[p]olaris_wifi_bt')
    UDH=$(ps 2>/dev/null | grep -c '[u]dhcpd')
    PSA=$(ps 2>/dev/null | grep -c '[p]olestar_app')
    # interface flags: UP / RUNNING tell us if the radio is still carrying
    IFF=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*\(UP\)\?.*\(RUNNING\)\?.*MTU.*/&/p' | head -1 \
          | tr -s ' ' | sed 's/^ *//')
    [ -z "$IFF" ] && IFF="wlan0 ABSENT"
    STA=$(grep -c "wlan0" /proc/net/arp 2>/dev/null)
    CONN=$(netstat -tn 2>/dev/null | grep -c ':9090.*ESTABLISHED')
    CUR="hostapd=$HAP wifi_bt=$WBT udhcpd=$UDH polestar=$PSA sta=$STA conn9090=$CONN | $IFF"

    # only write when something CHANGES, plus a heartbeat every 60 samples
    if [ "$CUR" != "$PREV" ]; then
        say "CHANGE $CUR"
        PREV="$CUR"
    fi

    # new kernel messages (driver-level AP teardown shows up here)
    NOW_DMESG=$(dmesg 2>/dev/null | wc -l)
    if [ "$NOW_DMESG" -gt "$PREV_DMESG" ]; then
        dmesg 2>/dev/null | tail -n $((NOW_DMESG - PREV_DMESG)) | while read -r l; do
            say "  dmesg: $l"
        done
        PREV_DMESG=$NOW_DMESG
    fi

    # the device's own log -- polaris_wifi_bt writes here, so its decision to
    # take the radio down (and any bluetooth wake attempt) lands in it
    NOW_MLOG=$(wc -c < "$MLOG" 2>/dev/null || echo 0)
    if [ "$NOW_MLOG" -lt "$PREV_MLOG" ]; then PREV_MLOG=0; fi     # truncated
    if [ "$NOW_MLOG" -gt "$PREV_MLOG" ]; then
        dd if="$MLOG" bs=1 skip="$PREV_MLOG" 2>/dev/null \
          | grep -aiE "wifi|wlan|hostapd|bt_|bluetooth|ble|disconnect|connect|sleep|power|ap_" \
          | head -20 | while read -r l; do
            say "  mlog: $(echo "$l" | cut -c1-160)"
        done
        PREV_MLOG=$NOW_MLOG
    fi

    sleep "$INT"
done
