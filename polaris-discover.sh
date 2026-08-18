#!/usr/bin/env bash
# ============================================================================
#  polaris-discover.sh — find the Polaris and see what it offers.
#
#  Run AFTER joining the Polaris' WiFi. Read-only: it pings and opens TCP
#  connections, and sends no mount commands.
#
#     ./polaris-discover.sh            # look on the Wi-Fi interface
#     ./polaris-discover.sh 192.168.1.1
#     ./polaris-discover.sh --sweep    # also ping-sweep the Wi-Fi subnet
#
#  It looks at the WI-FI interface's router, not the default route: with an
#  ethernet cable plugged in, the default route stays on the wire and the
#  Polaris is invisible to it.
# ============================================================================
set -uo pipefail

SWEEP=0; IP=""
for a in "$@"; do
  case "$a" in
    --sweep) SWEEP=1;;
    -h|--help) sed -n '2,16p' "$0"; exit 0;;
    *) IP="$a";;
  esac
done

# which interface is the Wi-Fi?
WIFI="$(networksetup -listallhardwareports 2>/dev/null \
        | awk '/^Hardware Port: Wi-Fi/{getline; print $2; exit}')"
WIFI="${WIFI:-en0}"

echo "[*] interfaces with an address:"
for i in $(ifconfig -l); do
  a="$(ipconfig getifaddr "$i" 2>/dev/null)"
  [ -n "$a" ] || continue
  r="$(ipconfig getoption "$i" router 2>/dev/null)"
  mark=""; [ "$i" = "$WIFI" ] && mark="   <- Wi-Fi"
  printf '    %-6s %-15s router=%-15s%s\n' "$i" "$a" "${r:-?}" "$mark"
done

WIFI_IP="$(ipconfig getifaddr "$WIFI" 2>/dev/null)"
if [ -z "$IP" ]; then
  if [ -z "$WIFI_IP" ]; then
    echo
    echo "[!] $WIFI has no address — you are not joined to the Polaris' WiFi yet." >&2
    echo "    Join it (System Settings > Wi-Fi), then run this again." >&2
    echo "    Current network: $(networksetup -getairportnetwork "$WIFI" 2>&1 | tail -1)" >&2
    exit 1
  fi
  IP="$(ipconfig getoption "$WIFI" router 2>/dev/null)"
  [ -n "$IP" ] || IP="$(echo "$WIFI_IP" | awk -F. '{print $1"."$2"."$3".1"}')"
  echo "[*] Wi-Fi is $WIFI ($WIFI_IP); assuming the Polaris is its router: $IP"
fi

echo "[*] target: $IP"
ping -c 2 -t 2 "$IP" >/dev/null 2>&1 && echo "    ping: replies" || echo "    ping: no reply (many devices ignore ping — keep going)"

probe() {
  if nc -z -G 2 -w 2 "$IP" "$1" 2>/dev/null; then printf '    %-6s OPEN    %s\n' "$1" "$2"
  else printf '    %-6s closed  %s\n' "$1" "$2"; fi
}
echo "[*] ports of interest:"
probe 22   "ssh — live debugging (stock firmware starts sshd from rcS)"
probe 23   "telnet"
probe 80   "http — /app/bin/lighttpd exists but nothing starts it in stock"
probe 9090 "the control port this project ASSUMES for the mount protocol"
probe 8080 "alternate http"
probe 554  "rtsp"

echo "[*] wider sweep (the control port is an assumption until we see it here):"
OPEN=""
for p in 21 25 53 111 443 1024 1025 1234 2000 3000 4000 5000 5555 6000 7000 7777 \
         8000 8081 8443 8888 9000 9001 9091 9100 9999 10000 20000 30000 50000; do
  nc -z -G 1 -w 1 "$IP" "$p" 2>/dev/null && OPEN="$OPEN $p"
done
echo "    additionally open:${OPEN:- none found}"

if [ "$SWEEP" = "1" ] && [ -n "${WIFI_IP:-}" ]; then
  echo "[*] ping-sweeping $(echo "$WIFI_IP" | awk -F. '{print $1"."$2"."$3".0/24"}') for other hosts:"
  base="$(echo "$WIFI_IP" | awk -F. '{print $1"."$2"."$3}')"
  for n in $(seq 1 254); do (ping -c 1 -t 1 "$base.$n" >/dev/null 2>&1 && echo "    alive: $base.$n") & done
  wait
  echo "    (also check: arp -an | grep -v incomplete)"
fi

echo
echo "[*] next, WITHOUT sending any command:"
echo "      ./out/polaris-mount --host $IP --port <port> --lat <deg> --lon <deg> --verbose pose"
