#!/bin/sh
# restore_stock.sh  --  reverse install_stage2.sh.  Runs ON THE DEVICE.
#
# Copies the preserved stock binary back over /app/bin/pgphoto (undoing the
# wrapper).  The /app/lib/stage2 tree is left in place (harmless; nothing
# launches it once the wrapper is gone) -- pass PURGE=1 to also remove it.
#
# Parameterised by env (must match install_stage2.sh):
#   BINP    launched path      (default /app/bin/pgphoto)
#   BACKUP  stock backup path  (default /app/sd/pgphoto.prestage2.bak)
#   STAGE2  install dir        (default /app/lib/stage2)
#   PURGE   if 1, also rm -rf $STAGE2
set -e

BINP=${BINP:-/app/bin/pgphoto}
BACKUP=${BACKUP:-/app/sd/pgphoto.prestage2.bak}
STAGE2=${STAGE2:-/app/lib/stage2}

if [ ! -e "$BACKUP" ]; then
    echo "[restore] FATAL: no backup at $BACKUP -- cannot restore stock." >&2
    echo "[restore] (was install_stage2.sh ever run? is BACKUP set correctly?)" >&2
    exit 1
fi

cp "$BACKUP" "$BINP";  chmod +x "$BINP"
echo "[restore] restored stock $BACKUP -> $BINP"

# restore the stock ptp2/usb1 placed at the stock camlib/iolib paths (if backed up)
for f in /app/lib/libgphoto2/*/ptp2.so /app/lib/libgphoto2_port/*/usb1.so; do
    [ -e "$f.prestage2.bak" ] && { cp "$f.prestage2.bak" "$f"; echo "[restore] restored stock $f"; }
done

if [ "${PURGE:-0}" = "1" ]; then
    rm -rf "$STAGE2"
    echo "[restore] purged $STAGE2"
else
    echo "[restore] left $STAGE2 in place (pass PURGE=1 to remove it)"
fi

cat <<EOF

[restore] DONE.  To relaunch the STOCK core:
    pkill -f pgphoto
    /app/restart_gphoto        # or reboot -- polestar relaunches the stock $BINP
EOF
