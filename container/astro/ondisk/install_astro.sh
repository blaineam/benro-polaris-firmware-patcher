#!/bin/sh
# ============================================================================
#  install_astro.sh -- put the plate solver on the device. Runs ON THE DEVICE.
#
#  Purely additive: writes /app/astro, creates a site.conf if there is none,
#  and installs the boot hook. Undo with `uninstall_astro.sh`, or by hand:
#      rm -rf /app/astro && rm -f /app/network_telnetd.sh
#      (then restore /app/network_telnetd.pre-astro.sh if one exists)
#
#     SRC        bundle dir (default: this script's dir)
#     DEST       install dir (default /app/astro)
#     INDEXES    where index files live (default /app/sd/astrometry)
#     NO_BOOT=1  install the files but NOT the boot hook
# ============================================================================
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HERE}
DEST=${DEST:-/app/astro}
INDEXES=${INDEXES:-/app/sd/astrometry}
HOOK=/app/network_telnetd.sh
CONF=/app/sd/polaris-astro/site.conf

mkdir -p "$DEST" "$INDEXES"

# ---- binaries -------------------------------------------------------------
# Every one of these is load-bearing for some feature, and a missing one fails
# SILENTLY at runtime (the feature simply never happens), so they are checked
# rather than best-effort copied.
for b in polaris-solve polaris-extract polaris-mount polaris-httpd \
         polaris-logwatch polaris-match; do
    [ -f "$SRC/$b" ] || { echo "[astro] MISSING $SRC/$b" >&2; exit 1; }
    cp "$SRC/$b" "$DEST/$b"; chmod +x "$DEST/$b"
done
# optional: only needed for simulated-sky testing
[ -f "$SRC/polaris-skysim" ] && { cp "$SRC/polaris-skysim" "$DEST"/; chmod +x "$DEST/polaris-skysim"; }

# ---- scripts --------------------------------------------------------------
for s in "$SRC"/*.sh; do
    [ -f "$s" ] || continue
    cp "$s" "$DEST"/; chmod +x "$DEST/$(basename "$s")"
done
echo "[astro] installed -> $DEST"

# ---- config ---------------------------------------------------------------
if [ ! -f "$CONF" ]; then
    mkdir -p "$(dirname "$CONF")"
    if [ -f "$SRC/site.conf.example" ]; then
        cp "$SRC/site.conf.example" "$CONF"
        echo "[astro] created $CONF from the example"
        echo "[astro] *** EDIT IT: LAT and LON are REQUIRED and are not guessable."
        echo "[astro]     Without them the solver has no hint and Alpaca reports"
        echo "[astro]     the wrong site."
    fi
else
    echo "[astro] kept your existing $CONF"
fi

# ---- boot hook ------------------------------------------------------------
# /app/bootapp runs /app/network_telnetd.sh at boot if it exists. The SSH
# option of the firmware patcher uses THE SAME FILE, so overwriting it blindly
# would silently remove ssh access -- which is how you would then be locked out
# of fixing it. Anything already there is preserved and chained instead.
if [ "${NO_BOOT:-0}" = "1" ]; then
    echo "[astro] NO_BOOT=1 -- boot hook not installed; start things by hand"
elif [ -f "$SRC/polaris-astro-boot.sh" ]; then
    if [ -f "$HOOK" ] && ! grep -q "polaris-astro boot hook" "$HOOK" 2>/dev/null; then
        cp "$HOOK" /app/network_telnetd.pre-astro.sh
        chmod +x /app/network_telnetd.pre-astro.sh
        echo "[astro] existing $HOOK preserved as /app/network_telnetd.pre-astro.sh"
        echo "[astro]   (it will be run first at every boot, so ssh keeps working)"
    fi
    cp "$SRC/polaris-astro-boot.sh" "$HOOK"
    chmod 755 "$HOOK"
    echo "[astro] boot hook installed -> $HOOK"
else
    echo "[astro] WARNING: no polaris-astro-boot.sh in the bundle; nothing will"
    echo "[astro]          start at boot."
fi

# ---- index files ----------------------------------------------------------
if ls "$INDEXES"/index-*.fits >/dev/null 2>&1; then
    echo "[astro] found $(ls "$INDEXES"/index-*.fits | wc -l) index file(s) in $INDEXES"
else
    echo "[astro] NO index files in $INDEXES -- NOTHING WILL SOLVE until they are"
    echo "[astro] there. Copy the astrometry/ directory from the bundle to the card."
fi

cat <<TXT

[astro] Next:
    1. Edit $CONF  (LAT, LON are required)
    2. Reboot, or run: $HOOK
    3. Open http://<polaris ip>:8090/

[astro] Solve a frame by hand (focal is read from EXIF; pass one to override):
    $DEST/polaris-align.sh /app/sd/normal/SP_0001.jpg

[astro] Talk to the mount (add --dry-run to send nothing):
    $DEST/polaris-mount --lat <deg> --lon <deg> pose
TXT
