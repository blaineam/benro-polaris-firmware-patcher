#!/bin/sh
# ===========================================================================
#  polaris-trackwatch.sh -- keep /tmp/polaris-track current.
#
#  WHY: the mount's alignment state is only visible in /app/Mlog.txt, and the
#  device TRUNCATES that file -- it has been found holding 2711 bytes and ZERO
#  284 lines minutes after an alignment, with the mount reporting
#  {"mode":8,"track":1,"aligned":true}. Anything that greps the log on demand
#  therefore concludes "not aligned" for a mount that is aligned, which is
#  exactly what kept the wifi keepalive parked.
#
#  polaris-logwatch polls at 20 ms and is truncation-safe, so following the log
#  continuously catches every track value the moment it appears, even though the
#  file it came from is wiped seconds later.
#
#  Costs nothing and opens no connections: it reads a file.
#
#  track values: 3 = never aligned, -1 = unknown, anything else = aligned.
#  0 means the compass alignment is set but the celestial confirm has not
#  happened -- which is still "safe to talk to", and is the point at which the
#  keepalive may connect.
# ===========================================================================
ASTRO=${ASTRO:-/app/astro}
OUT=${TRACK_FILE:-/tmp/polaris-track}

[ -x "$ASTRO/polaris-logwatch" ] || { echo "missing $ASTRO/polaris-logwatch" >&2; exit 2; }

"$ASTRO/polaris-logwatch" --match "track:" | while read -r line; do
    t=$(printf '%s' "$line" | sed -n 's/.*track:\([0-9-][0-9]*\).*/\1/p' | tail -1)
    [ -n "$t" ] || continue
    prev=$(cat "$OUT" 2>/dev/null)
    [ "$t" = "$prev" ] && continue
    printf '%s\n' "$t" > "$OUT"
done
