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
#  single TCP connection to the control port that REGISTERS ITSELF the way the
#  app does (opcode 808, type:0), reopened if it ever drops. An unregistered
#  connection does not count -- see below.
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

# Refuse while unaligned -- see above. Decided passively; this opens no
# connection.
#
# THE LOG IS NOT ENOUGH ON ITS OWN. The device truncates /app/Mlog.txt, and it
# has been observed holding no 284 lines at all minutes after an alignment. A
# grep of the log therefore reports "not aligned" for a mount that is aligned
# and tracking -- which is exactly what kept this helper parked. So prefer the
# state file that polaris-httpd maintains (it records every track value it ever
# learns, from the log or from a real read), and fall back to the log only if
# the file is missing.
TRACKF=${TRACK_FILE:-/tmp/polaris-track}
aligned() {
    _t=""
    [ -r "$TRACKF" ] && _t=$(sed -n 's/^\([0-9-][0-9]*\)$/\1/p' "$TRACKF" | head -1)
    if [ -z "$_t" ]; then
        _t=$(grep -a "code.284." "$MLOG" 2>/dev/null \
             | sed -n 's/.*track:\([0-9-][0-9]*\).*/\1/p' | tail -1)
    fi
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
    # REGISTER, then hold. An idle connection is NOT a counted client: the
    # device assigns it an id but never adds it to the context table, and
    # closing it gives "SP_ClientCtxDel: not find this is id[N]". Captured from
    # a real app connecting, the registration is opcode 808 with type:0 --
    #
    #     rcv msg from App[5]: type:2; code:808; val:type:0;
    #     SP_MsgSysFromAppProc: client type[0]; id[5];
    #     SP_ClientCtxAdd: id[5]; type[wifi]; WifiCount[1];
    #     SP_SendMsgToApp: code[808], val[ret:0;]
    #
    # -- and sending exactly that from here produces the same three lines and
    # increments WifiCount, which is what keeps the auto-off timer from firing.
    # `sleep` holds nc's stdin open so the socket stays up; nc exits when the
    # peer closes, and we re-register on reconnect.
    { printf '1&808&2&type:0;#'; sleep 86400 2>/dev/null; } | nc "$HOST" "$PORT" >/dev/null 2>&1
    say "connection closed -- reconnecting in 5s"
    sleep 5
done
