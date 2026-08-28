#!/bin/sh
# ============================================================================
#  astro-autoinstall.sh — zero-SSH boot hook for the Polaris astro stack.
#
#  The firmware patcher (patch.sh) installs this into the appfs as the boot hook
#  /app/bootapp already runs at startup. It makes the astro stack self-installing:
#  on boot it copies the SD bundle to /app/astro (once, idempotently) and runs the
#  astro boot sequence — so NO SSH and NO install_astro.sh-by-hand are needed.
#  Put the SD card in, power on, open the page, set your location in the web app.
#
#  Two hard rules:
#    * FAIL-SAFE — every step is guarded so a problem here can never stop the rest
#      of the stock boot. Worst case the astro stack simply does not come up.
#    * IDEMPOTENT — safe to run every boot. It reinstalls only when /app/astro is
#      missing or the SD carries a NEWER polaris-httpd (so swapping in a freshly
#      built card updates the device in place), and the boot sequence itself will
#      not double-bind the port.
# ============================================================================
# APP is /app on the device; overridable only so this logic can be tested against
# a fake filesystem. Everything below is relative to it.
APP="${APP_ROOT:-/app}"
cd "$APP" 2>/dev/null || true

# Chain to any hook the patcher preserved here first (e.g. an --ssh-key hook), so
# adding astro auto-install never silently removes another feature's boot action.
[ -x "$APP/astro-autoinstall.pre.sh" ] && { "$APP/astro-autoinstall.pre.sh" 2>/dev/null || true; }

SD="$APP/sd/polaris-astro"
if [ -x "$SD/install_astro.sh" ]; then
    if [ ! -x "$APP/astro/polaris-httpd" ] || [ "$SD/polaris-httpd" -nt "$APP/astro/polaris-httpd" ]; then
        # NO_BOOT=1: copy binaries+scripts to $APP/astro, DO NOT touch this hook.
        NO_BOOT=1 SRC="$SD" DEST="$APP/astro" INDEXES="$APP/sd/astrometry" \
            sh "$SD/install_astro.sh" >"${AF_LOG:-/tmp/astro-autoinstall.log}" 2>&1 || true
    fi
fi

# Run the astro boot sequence from its installed location. It starts the web
# server, wifi keepalive, etc., and is itself idempotent.
[ -x "$APP/astro/polaris-astro-boot.sh" ] && { "$APP/astro/polaris-astro-boot.sh" 2>/dev/null || true; }

exit 0
