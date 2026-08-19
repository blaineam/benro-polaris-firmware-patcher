#!/bin/sh
# ===========================================================================
#  wifi-keepalive.sh -- stop the Polaris powering its wifi down.
#
#  WHAT IT WORKS AROUND (from the device's own log, captured by wifi-watch):
#      SP_ClientCtxDel: id[3], type[wifi]; WifiCount[0]     app disconnected
#      ... 60 seconds later ...
#      WifiBtTask[201]: wifi auto off
#      SP_SetWifiState[0]
#      remove@/bus/platform/drivers/bcmdhd_wlan
#      MsgFromWifiBt --> val[wifi:0; bt:1;]
#
#  Sixty seconds after the LAST wifi client disconnects, the firmware powers the
#  radio down and unloads the driver, leaving bluetooth as the wake path. The
#  SSID vanishes, and ssh and the web UI go with it. That is stock behaviour and
#  there is no setting for it.
#
#  The trigger is WifiCount reaching ZERO, so we keep one client connected: a
#  single idle TCP connection to the control port, reopened if it ever drops.
#
#  THE COST, WHICH IS REAL:
#    * The device counts this as an app connecting (SP_EVENT_APP_CONNECT). While
#      the mount is UNALIGNED that is exactly what makes the Benro app demand a
#      compass calibration, so this refuses to run until the mount is aligned.
#    * The radio stays powered, which uses battery.
#  It is therefore OFF by default and has to be turned on deliberately.
# ===========================================================================
HOST=${MOUNT_HOST:-127.0.0.1}
PORT=${MOUNT_PORT:-9090}
LOG=${KEEPALIVE_LOG:-/tmp/wifi-keepalive.log}
MLOG=/app/Mlog.txt

say() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG"; }

# Refuse while unaligned -- see above. Read passively from the device's own log,
# exactly like the web server does; this opens no connection to decide.
aligned() {
    _t=$(grep -a "code.284." "$MLOG" 2>/dev/null \
         | sed -n 's/.*track:\([0-9-][0-9]*\).*/\1/p' | tail -1)
    [ -n "$_t" ] && [ "$_t" != "3" ] && [ "$_t" != "-1" ]
}

# SINGLE INSTANCE, VIA A PIDFILE -- NOT via `ps | grep`.
# The obvious guard, `ps | grep -q '[w]ifi-keepalive' || start_it &`, does not
# work: the whole list is backgrounded into a subshell whose OWN command line
# contains "wifi-keepalive", so the grep matches itself, concludes we are
# already running, and never starts anything. (The same self-match, with pkill
# instead of grep, killed my ssh session twice today.)
PIDF=${KEEPALIVE_PIDFILE:-/tmp/wifi-keepalive.pid}
if [ -f "$PIDF" ]; then
    _old=$(cat "$PIDF" 2>/dev/null)
    if [ -n "$_old" ] && kill -0 "$_old" 2>/dev/null; then
        say "already running as pid $_old -- exiting"
        exit 0
    fi
fi
echo $$ > "$PIDF"
# TWO traps, deliberately. A handler for INT/TERM must EXIT explicitly: in
# POSIX sh execution RESUMES after the handler returns, so the obvious
# `trap 'rm -f "$PIDF"' EXIT INT TERM` catches the signal, tidies up, and then
# carries right on running -- which made "Allow Sleep" report success while the
# helper kept holding the connection open.
trap 'rm -f "$PIDF"' EXIT
trap 'say "signalled -- exiting"; rm -f "$PIDF"; exit 0' INT TERM

say "keepalive started (pid $$)"
while :; do
    if [ "${KEEPALIVE_REQUIRE_ALIGNED:-1}" = "1" ] && ! aligned; then
        say "mount not aligned -- holding off (connecting now would make the app ask for a compass calibration)"
        while [ "${KEEPALIVE_REQUIRE_ALIGNED:-1}" = "1" ] && ! aligned; do sleep 20; done
        say "mount is aligned -- connecting"
    fi
    # Hold one idle connection. `sleep` keeps nc's stdin open so it does not
    # close the socket; nc exits when the peer closes, and we reconnect.
    sleep 86400 2>/dev/null | nc "$HOST" "$PORT" >/dev/null 2>&1
    say "connection closed -- reconnecting in 5s"
    sleep 5
done
