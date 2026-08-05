#!/bin/sh
# install_stage2.sh  --  reversible on-device install of the Stage-2 full-core
# swap (self-driving wrapper form).  Runs ON THE DEVICE.  Idempotent.
#
# It (1) backs up the real /app/bin/pgphoto to /app/sd/pgphoto.prestage2.bak
# (ONLY if that backup does not already exist -- so re-runs never overwrite the
# genuine stock binary with an already-installed wrapper), (2) populates
# /app/lib/stage2 with the loader + fresh core/port/ptp2/usb1 + the on-disk-
# trampolined binary, (3) installs the sh-wrapper as /app/bin/pgphoto, and
# (4) prints how to resume polestar.
#
# Nothing is deleted; restore_stock.sh reverses it.  Parameterised by env:
#   SRC     bundle root holding the files (default: this script's dir + ..)
#   STAGE2  install dir           (default /app/lib/stage2)
#   BINP    launched path         (default /app/bin/pgphoto)
#   BACKUP  stock backup path     (default /app/sd/pgphoto.prestage2.bak)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$(cd "$HERE/.." && pwd)}          # bundle root (parent of ondisk/)
STAGE2=${STAGE2:-/app/lib/stage2}
BINP=${BINP:-/app/bin/pgphoto}
BACKUP=${BACKUP:-/app/sd/pgphoto.prestage2.bak}

echo "[install] SRC=$SRC  STAGE2=$STAGE2  BINP=$BINP  BACKUP=$BACKUP"

# --- locate bundle files (accept ondisk/ subdir or flat layout) --------------
find_one() {                                  # find_one <relpath...> ; echoes first hit
    for r in "$@"; do
        [ -f "$SRC/$r" ] && { echo "$SRC/$r"; return 0; }
    done
    echo "[install] MISSING: none of: $* (under $SRC)" >&2; return 1
}
LOADER=$(find_one ondisk/libpolaris_stage2.so libpolaris_stage2.so)
STG2BIN=$(find_one ondisk/pgphoto.stage2ondisk pgphoto.stage2ondisk)
CORE=$(find_one libgphoto2.so.6)
PORT=$(find_one libgphoto2_port.so.12)
PTP2=$(find_one libgphoto2/2.5.34/ptp2.so)
USB1=$(find_one libgphoto2_port/0.12.2/usb1.so)
WRAP=$(find_one ondisk/pgphoto.wrapper pgphoto.wrapper)

# --- 1. back up the REAL stock binary (only once) ----------------------------
if [ -e "$BACKUP" ]; then
    echo "[install] backup already present ($BACKUP) -- NOT overwriting"
else
    cp "$BINP" "$BACKUP"
    echo "[install] backed up $BINP -> $BACKUP"
fi

# --- 2. populate /app/lib/stage2 ---------------------------------------------
mkdir -p "$STAGE2/libgphoto2/2.5.34" "$STAGE2/libgphoto2_port/0.12.2"
cp "$LOADER"  "$STAGE2/libpolaris_stage2.so"
cp "$STG2BIN" "$STAGE2/pgphoto.stage2ondisk";  chmod +x "$STAGE2/pgphoto.stage2ondisk"
cp "$CORE"    "$STAGE2/libgphoto2.so.6"
cp "$PORT"    "$STAGE2/libgphoto2_port.so.12"
cp "$PTP2"    "$STAGE2/libgphoto2/2.5.34/ptp2.so"
cp "$USB1"    "$STAGE2/libgphoto2_port/0.12.2/usb1.so"
echo "[install] populated $STAGE2 (loader + core/port + ptp2/usb1 + stage2 binary)"

# --- 2b. also place fresh ptp2/usb1 at the STOCK camlib/iolib paths -----------
# The swapped 2.5.34 core dlopens its camlib from the stock on-disk layout
# (/app/lib/libgphoto2/<rev>/ptp2.so), NOT from the CAMLIBS the wrapper exports,
# and the port loader dlopens usb1 from /app/lib/libgphoto2_port/<rev>/usb1.so.
# So the fresh driver must live there too or the core loads the stale stock one.
# Back up the stock files once so restore_stock.sh can reverse this.
STOCK_PTP2=$(ls /app/lib/libgphoto2/*/ptp2.so 2>/dev/null | head -1 || true)
STOCK_USB1=$(ls /app/lib/libgphoto2_port/*/usb1.so 2>/dev/null | head -1 || true)
if [ -n "$STOCK_PTP2" ]; then
    [ -e "$STOCK_PTP2.prestage2.bak" ] || cp "$STOCK_PTP2" "$STOCK_PTP2.prestage2.bak"
    cp "$PTP2" "$STOCK_PTP2"
    echo "[install] placed fresh ptp2 at stock camlib path $STOCK_PTP2 (backup kept)"
fi
if [ -n "$STOCK_USB1" ]; then
    [ -e "$STOCK_USB1.prestage2.bak" ] || cp "$STOCK_USB1" "$STOCK_USB1.prestage2.bak"
    cp "$USB1" "$STOCK_USB1"
    echo "[install] placed fresh usb1 at stock iolib path $STOCK_USB1 (backup kept)"
fi

# --- 3. install the self-driving wrapper as /app/bin/pgphoto -----------------
cp "$WRAP" "$BINP";  chmod +x "$BINP"
echo "[install] installed wrapper -> $BINP"

# --- 4. how to resume --------------------------------------------------------
cat <<EOF

[install] DONE.  To (re)start with the Stage-2 core:
    pkill -f pgphoto          # stop the running stock pgphoto (+ its watchdog if any)
    /app/restart_gphoto       # or reboot -- polestar relaunches $BINP (now the wrapper)

  Watch the first launch:
    tail -f /tmp/stage2ondisk.log 2>/dev/null   # if you tee; else check console
  Success markers (in order):
    [stage2] mmap slot page @0x30000000 ok
    [stage2] dlopen core ok / dlopen port ok
    [stage2] slots filled 64/64
    Setting abilities ('Canon EOS R5m2')  ->  gp_camera_init ret 0

  Revert at any time:   sh $HERE/restore_stock.sh
EOF
