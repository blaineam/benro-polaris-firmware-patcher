#!/bin/sh
# ===========================================================================
#  capture-calibration.sh -- record the app <-> device protocol conversation.
#
#  polestar_app already logs every message in both directions:
#     msg_rcv_from_app_process: rcv msg from App[..]:type:T;code:C;val:V
#     SP_SendMsgToApp:          type[T],code[C],val[V]
#     SP_sendMsg:               key[..],type[T],code[C],val[V]
#
#  so we do NOT need to MITM port 9090 -- just follow the log and pull those
#  lines out. Mlog.txt is TRUNCATED by the app every few seconds, so follow by
#  SIZE and reset when it shrinks; `tail -f` follows by descriptor and silently
#  loses everything after the first truncation.
#
#     capture-calibration.sh [seconds]      (default 300)
#
#  Then run the calibration in the Benro app. Output:
#     /app/sd/calibration-flow.log     every app<->device message, timestamped
# ===========================================================================
DUR=${1:-300}
OUT=${OUT:-/app/sd/calibration-flow.log}
SRC=/app/Mlog.txt

: > "$OUT"
echo "recording app<->device messages for ${DUR}s -> $OUT" >&2
echo "run the calibration in the Benro app now" >&2

last=0
end=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$end" ]; do
    cur=$(wc -c < "$SRC" 2>/dev/null || echo 0)
    [ "$cur" -lt "$last" ] && last=0          # app truncated the log
    if [ "$cur" -gt "$last" ]; then
        tail -c +$((last + 1)) "$SRC" 2>/dev/null \
          | sed 's/\x1b\[[0-9;]*m//g' \
          | grep -aE "rcv msg from App|SP_SendMsgToApp|SP_sendMsg|MsgFromCamera|Align|align|compass|Compass|530|527|518|517|519" \
          >> "$OUT"
        last=$cur
    fi
    sleep 1
done

echo >&2
echo "captured $(wc -l < "$OUT") lines -> $OUT" >&2
