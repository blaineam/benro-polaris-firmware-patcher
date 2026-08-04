#!/bin/bash
# Cross-build the ptp2 camera driver from a chosen libgphoto2 release.
#   $1 = libgphoto2 version (e.g. 2.5.34)
#   $2 = trampoline address (hex, e.g. 0x0003fa00) — pgphoto's set_info_dirty
#   $3 = fix R5m2 typo (1/0)
#
# The driver is:
#   * soft-float EABI, glibc-2.24 ceiling (debian:9 arm-linux-gnueabi toolchain)
#   * linked against the device's OWN libexif/libltdl sonames (staged in /work/devlibs)
#   * built WITHOUT libxml2/jpeg/curl — the stock pgphoto's ptp2 links none of them,
#     and Canon USB capture needs none; this keeps the on-device dependency set minimal.
set -euo pipefail
VER="$1"; TRAMP="$2"; FIXTYPO="${3:-1}"
XT=arm-linux-gnueabi
DEV=/work/devlibs
SRC=/work/src
mkdir -p "$SRC" /work/out

echo "[build] fetching libgphoto2 $VER"
cd "$SRC"
if [ ! -d "libgphoto2-$VER" ]; then
  for u in \
    "https://github.com/gphoto/libgphoto2/releases/download/v$VER/libgphoto2-$VER.tar.xz" \
    "https://github.com/gphoto/libgphoto2/releases/download/v$VER/libgphoto2-$VER.tar.bz2"; do
    if wget -q -O lg.tar "$u"; then break; fi
  done
  tar xf lg.tar
fi
cd "libgphoto2-$VER"

# --- inject the trampoline shim so gp_filesystem_set_info_dirty resolves to
#     pgphoto's own implementation (see docs/HOW-IT-WORKS.md) ---
if ! grep -q "polaris-patcher trampoline shim" camlibs/ptp2/library.c; then
  sed "s|@TRAMP@|$TRAMP|g" /opt/patcher/polaris_shim.c.in >> camlibs/ptp2/library.c
fi

# --- optional: fix the upstream "Canon:EOS 5Rm2" model-name typo (R5 Mark II) ---
if [ "$FIXTYPO" = "1" ]; then
  sed -i 's/"Canon:EOS 5Rm2"/"Canon:EOS R5m2"/' camlibs/ptp2/library.c || true
fi

# --- optional: lengthen the Canon cold-start OpenSession timeout (ms) ---------
#  The R5 Mark II's USB bulk endpoint is not ready to accept the first
#  OpenSession request for a few seconds after it (re-)enumerates. With the
#  stock 1500ms timeout the write times out, the port is reset, and the camera
#  re-enumerates — restarting its readiness clock. That cold-start loop can grind
#  for minutes. A longer timeout lets a single OpenSession write stay pending
#  (the camera NAKs, it does not stall) until the camera becomes ready, so init
#  succeeds on the first enumeration instead of storming resets.
if [ -n "${COLD_START_TIMEOUT_MS:-}" ]; then
  sed -i "s/#define USB_CANON_START_TIMEOUT 1500.*/#define USB_CANON_START_TIMEOUT ${COLD_START_TIMEOUT_MS}\t\/* polaris: cold-start endpoint-ready window *\//" camlibs/ptp2/library.c
  echo "[build] USB_CANON_START_TIMEOUT set to ${COLD_START_TIMEOUT_MS}ms"
fi

# --- optional: disable the EOS keep-device-on heartbeat -----------------------
#  camera_keep_device_on() clobbers params->starttime every ~10s, which also
#  drives the viewfinder settle wait (`while time_since(starttime) < 3000`).
#  Removing it avoids that interaction during live-view startup.
if [ "${REMOVE_KEEP_DEVICE_ON:-0}" = "1" ]; then
  sed -i 's/CR (camera_keep_device_on (camera));/\/* polaris: keepdeviceon disabled *\//g' camlibs/ptp2/library.c
  echo "[build] camera_keep_device_on() calls neutralized"
fi

# --- optional: drop the SetRemoteMode toggle in camera_exit (2.5.27 parity) ----
#  2.5.34 ADDED `ptp_canon_eos_setremotemode(params,1)` to camera_exit ("switches
#  the display back on"); stock 2.5.27 — which cold-connected R5 II live view
#  reliably — has no such call in camera_exit. During pgphoto's cold-start retry
#  loop, each failed camera_init is followed by camera_exit, so this re-toggles
#  the EOS remote mode and the camera RE-ENUMERATES again, restarting the churn.
#  Neutralize ONLY the camera_exit instance (identified by its unique preceding
#  comment) — the camera_init SetRemoteMode calls are required and left intact.
if [ "${REMOVE_EXIT_REMOTEMODE:-0}" = "1" ]; then
  perl -0pi -e 's/(\/\* this switches the display back on \.\.\. \*\/\s*\n\s*if \(ptp_operation_issupported\(params, PTP_OC_CANON_EOS_SetRemoteMode\)\) \{\s*\n)\s*C_PTP \(ptp_canon_eos_setremotemode\(params, 1\)\);/$1\t\t\t\t\/* polaris: skip exit-side remote-mode toggle (2.5.27 parity) *\//' camlibs/ptp2/library.c
  grep -q 'skip exit-side remote-mode toggle' camlibs/ptp2/library.c && echo "[build] camera_exit SetRemoteMode toggle removed" || { echo "[build] ERROR: exit-remotemode patch did not match"; exit 1; }
fi

# --- usb1 iolib (port/USB transport) -----------------------------------------
#  The stock on-disk usb1.so is a STANDARD libgphoto2 usb1 iolib built against
#  libusb-1.0 (its DT_NEEDED lists libusb-1.0.so.0, which the device ships in
#  /app/lib and pgphoto's port loader dlopen's at runtime — unlike ptp2, the
#  port layer is NOT statically dispatched). So the ABI-faithful replacement is
#  ALSO libusb-based, linked against the DEVICE's own libusb-1.0.so.0 soname
#  (staged by patch.sh in $DEV). We enable it here and harvest usb1.so below.
#
#  The device libusb-1.0.so.0 itself NEEDs libudev.so.1 (which lives in the
#  rootfs, not staged here). That only matters for configure's libusb_init
#  link-test; `-Wl,--allow-shlib-undefined` lets that test pass. usb1.so never
#  references any udev symbol, so udev does NOT enter usb1.so's own DT_NEEDED
#  (verified against stock in patch.sh).
#
#  usb1 is built ONLY when patch.sh staged the device libusb in $DEV (i.e. the
#  usb1 swap is enabled). Otherwise libusb-1.0 is explicitly turned OFF so the
#  build stays ptp2-only and byte-identical to the legacy path — even though the
#  image now ships libusb headers, they must not auto-enable a usb1 we can't link.
CONF_ARGS=(--host="$XT" --prefix=/opt/lg
  --disable-static --disable-nls --disable-rpath --disable-docs
  --with-camlibs=ptp2 --without-libxml-2.0 --without-jpeg --without-libcurl
  CC="${XT}-gcc" CXX="${XT}-g++" AR="${XT}-ar" RANLIB="${XT}-ranlib"
  STRIP="${XT}-strip" LD="${XT}-ld"
  LIBEXIF_CFLAGS="-I/usr/include" LIBEXIF_LIBS="-L$DEV -lexif")
if [ -f "$DEV/libusb-1.0.so.0" ]; then
  CONF_ARGS+=(CPPFLAGS="-I/usr/include -I/usr/include/libusb-1.0"
    LDFLAGS="-L$DEV -Wl,-rpath-link,$DEV -Wl,--allow-shlib-undefined"
    LIBUSB_CFLAGS="-I/usr/include/libusb-1.0" LIBUSB_LIBS="-L$DEV -lusb-1.0")
else
  CONF_ARGS+=(CPPFLAGS="-I/usr/include"
    LDFLAGS="-L$DEV -Wl,-rpath-link,$DEV"
    --with-libusb-1.0=no --with-libusb=no)
fi
if [ ! -f config.status ]; then
  ./configure "${CONF_ARGS[@]}" >/dev/null
fi
make -j"$(nproc)" >/tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }

BUILT="$(find camlibs -name ptp2.so | head -1)"
[ -n "$BUILT" ] || { echo "[build] ptp2.so not produced"; exit 1; }
cp "$BUILT" /work/out/ptp2.so
echo "[build] ptp2.so built: $(stat -c %s /work/out/ptp2.so) bytes"

# Harvest the usb1 iolib (from libgphoto2_port). Non-fatal here — patch.sh
# requires it only when the usb1 swap is enabled, and verifies it fully.
USB1_BUILT="$(find libgphoto2_port -name usb1.so | head -1)"
if [ -n "$USB1_BUILT" ]; then
  cp "$USB1_BUILT" /work/out/usb1.so
  # libtool over-links libgphoto2_port.la's dependency chain, adding a spurious
  # libltdl.so.7 to usb1.so's DT_NEEDED that the STOCK usb1.so does not carry.
  # usb1.so references no lt_dl* symbol, so drop it to match stock's NEEDED set
  # exactly (patch.sh's DT_NEEDED ⊆ stock check would otherwise abort).
  if "$XT-readelf" -d /work/out/usb1.so 2>/dev/null | grep -q 'libltdl\.so\.7'; then
    if "$XT-nm" -D --undefined-only /work/out/usb1.so | grep -qE '\blt_dl'; then
      echo "[build] WARNING: usb1.so references lt_dl* — NOT stripping libltdl"
    else
      patchelf --remove-needed libltdl.so.7 /work/out/usb1.so
      echo "[build] usb1.so: dropped over-linked libltdl.so.7 (unreferenced)"
    fi
  fi
  echo "[build] usb1.so built: $(stat -c %s /work/out/usb1.so) bytes"
else
  echo "[build] NOTE: usb1.so not produced (libusb not detected) — usb1 swap unavailable"
fi
