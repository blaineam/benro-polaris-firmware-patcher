#!/bin/sh
# ============================================================================
#  uninstall_astro.sh -- remove the plate solver. Runs ON THE DEVICE.
#
#  Restores whatever boot hook was there before (the patcher's ssh hook, if you
#  used it), stops everything this project runs, and deletes /app/astro.
#  Leaves the microSD alone: your site.conf, index files, logs and frames stay.
# ============================================================================
HOOK=/app/network_telnetd.sh
PRE=/app/network_telnetd.pre-astro.sh

echo "[astro] stopping daemons"
# NOTE: match on the script/binary names, never with a bare pkill on a pattern
# that also appears in THIS script's own command line -- that kills the shell
# running the uninstall halfway through.
ME=$$
ps 2>/dev/null | grep -E "polaris-(httpd|autosolve|logwatch|guide|match)|wifi-watch" \
  | grep -v grep | while read -r P _; do
    [ "$P" = "$ME" ] || kill "$P" 2>/dev/null
done
sleep 2

if [ -f "$HOOK" ] && grep -q "polaris-astro boot hook" "$HOOK" 2>/dev/null; then
    if [ -f "$PRE" ]; then
        mv "$PRE" "$HOOK"
        chmod 755 "$HOOK"
        echo "[astro] restored the previous boot hook (ssh access preserved)"
    else
        rm -f "$HOOK"
        echo "[astro] removed $HOOK"
    fi
else
    echo "[astro] $HOOK is not ours -- left alone"
fi

rm -rf /app/astro
echo "[astro] removed /app/astro"
echo "[astro] the microSD is untouched: site.conf, indexes, logs and frames remain"
echo "[astro] reboot to be sure nothing of ours is running"
