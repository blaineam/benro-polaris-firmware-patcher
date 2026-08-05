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

# FULLSTACK=1 : full-libgphoto2 mode (the DEFAULT of the patcher).  Builds the
# whole 2.5.34 stack -- core (libgphoto2.so.6) + port (libgphoto2_port.so.12) +
# ptp2 camlib + usb1 iolib -- so the on-disk trampoline loader can dlopen a fresh
# core/port instead of pgphoto's compiled-in 2.5.27 core.  Two fullstack-only
# build deltas vs the legacy ptp2-only path:
#   * NO trampoline shim: the ptp2-only path shims gp_filesystem_set_info_dirty
#     because the device's on-disk 2.5.27 core predates that symbol; the fullstack
#     NEW core exports it natively, so the shim is neither needed nor wanted.
#   * `_Camera` pad: pad struct _Camera to 4140 bytes so the new core's
#     gp_camera_new() calloc's the SAME size Benro's binary expects (Benro appended
#     a 4120-byte tail to _Camera; upstream is 20).  4120 is an interop SIZE, not
#     Benro code -- nothing proprietary is introduced.  ABI-inert for the library
#     (upstream never reads the tail).  See docs/HOW-IT-WORKS.md.
FULLSTACK="${FULLSTACK:-0}"

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

# --- DIAGNOSTIC ONLY: POLARIS_TRACE instrumentation (TRACE=1) ----------------
#  THROWAWAY tracing build — NOT for shipping. Injects unbuffered
#  fprintf(stderr,"POLARIS_TRACE: ...") across the Canon EOS post-capture event
#  path (camera_wait_for_event / ptp_check_eos_events / gp_filesystem_append) to
#  pin exactly where GP_EVENT_FILE_ADDED is lost after CAPTURE_COMPLETE. Adds NO
#  exported symbols (fprintf/fflush/stderr are libc) so the trampoline boundary
#  set + CAMLIBS loading resolve identically to the normal build. This gate is
#  OFF by default; never enable it for a shippable recipe.
if [ "${TRACE:-0}" = "1" ]; then
  echo "[build] TRACE=1: applying POLARIS_TRACE diagnostic instrumentation (throwaway)"
  python3 /opt/patcher/trace_patch.py .
fi

# --- POLARIS_DBG: EOS-init NON-FATAL patch (SHIPPABLE, no tracing) -----------
#  The full-libgphoto2 default runs ptp2 against a FRESH 2.5.34 core, so Canon's
#  EOS event/keep-alive drains during camera_init are exercised as upstream wrote
#  them. On the R5 Mark II a couple of those `C_PTP(ptp_check_eos_events(...))` /
#  `CR(camera_keep_device_on(...))` drains return a transient non-OK the FIRST
#  time and abort init — the camera then falls back to the generic "USB PTP Class
#  Camera" driver (no settings, no live view). dbg_patch.py swallows exactly those
#  drains (config.c x13 + library.c keep_device_on x3 + check_eos_events x1) so
#  camera_init COMPLETES as the real Canon driver. It is the behaviour-only half
#  of trace_patch.py — NO fprintf tracing, NO new exported symbols / DT_NEEDED, a
#  no-op when the camera returns OK. This is a documented LGPL source modification
#  (see NOTICE). Idempotent; asserts exact anchor counts and aborts on drift.
#  build_fullstack.sh forces POLARIS_DBG=1; the ptp2-only fallback leaves it off.
if [ "${POLARIS_DBG:-0}" = "1" ]; then
  echo "[build] POLARIS_DBG=1: applying EOS-init non-fatal patch (dbg_patch.py, no tracing)"
  python3 /opt/patcher/dbg_patch.py .
fi

# --- inject the trampoline shim so gp_filesystem_set_info_dirty resolves to
#     pgphoto's own implementation (see docs/HOW-IT-WORKS.md) ---
#     FULLSTACK skips this: the NEW 2.5.34 core exports the symbol natively.
if [ "$FULLSTACK" != "1" ]; then
  if ! grep -q "polaris-patcher trampoline shim" camlibs/ptp2/library.c; then
    sed "s|@TRAMP@|$TRAMP|g" /opt/patcher/polaris_shim.c.in >> camlibs/ptp2/library.c
  fi
else
  echo "[build] FULLSTACK: trampoline shim NOT injected (new core exports gp_filesystem_set_info_dirty)"
fi

# --- FULLSTACK: pad struct _Camera to 4140 B so the new core allocates the size
#     Benro's binary expects (interop constant; ABI-inert; nothing proprietary) ---
if [ "$FULLSTACK" = "1" ]; then
  CAMHDR=gphoto2/gphoto2-camera.h
  [ -f "$CAMHDR" ] || { echo "[build] ERROR: $CAMHDR not found"; exit 1; }
  if ! grep -q "_reserved_tail" "$CAMHDR"; then
    # insert the pad as the last member of `struct _Camera { ... };`
    awk '
      /struct[ \t]+_Camera[ \t]*\{/ { instruct=1 }
      instruct==1 && /^\};/ {
        print "\tchar _reserved_tail[4120]; /* polaris fullstack: pad _Camera to 4140 (Benro-tail ABI parity) */";
        instruct=0
      }
      { print }
    ' "$CAMHDR" > "$CAMHDR.pad" && mv "$CAMHDR.pad" "$CAMHDR"
  fi
  grep -q "_reserved_tail" "$CAMHDR" || { echo "[build] ERROR: _Camera pad did not apply"; exit 1; }
  echo "[build] FULLSTACK: struct _Camera padded with _reserved_tail[4120] (-> sizeof 4140)"
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

# --- FULLSTACK: harvest the freshly-built core + port shared libraries. --------
#  These are the ordinary LGPL libgphoto2 shared libs `make` produces; the on-disk
#  trampoline loader dlopens them by absolute path.  Copy the REAL ELF (deref the
#  soname symlink), not the `.so.6`/`.so.12` link.
if [ "$FULLSTACK" = "1" ]; then
  CORE_BUILT="$(find libgphoto2 -path '*/.libs/libgphoto2.so.6.*' ! -name '*.so.6' | sort | tail -1)"
  PORT_BUILT="$(find libgphoto2_port -path '*/.libs/libgphoto2_port.so.12.*' ! -name '*.so.12' | sort | tail -1)"
  [ -n "$CORE_BUILT" ] || { echo "[build] FULLSTACK ERROR: libgphoto2.so.6 not produced"; exit 1; }
  [ -n "$PORT_BUILT" ] || { echo "[build] FULLSTACK ERROR: libgphoto2_port.so.12 not produced"; exit 1; }
  cp -L "$CORE_BUILT" /work/out/libgphoto2.so.6
  cp -L "$PORT_BUILT" /work/out/libgphoto2_port.so.12
  # Strip the whole fullstack (core+port+ptp2+usb1): shrinks the appfs footprint
  # (the on-disk `.symtab` is not needed at runtime — dlopen resolves via `.dynsym`
  # which strip keeps). The result is within a handful of bytes of the
  # hardware-validated libs (which were also stripped).
  #
  # Prove the _Camera pad took BEFORE stripping (needs .symtab): the new core's
  # gp_camera_new must calloc 0x102c (=4140) bytes.  Fail closed if not — a wrong
  # size would put Benro's _Camera-tail reads out of bounds.
  if "$XT-objdump" -d /work/out/libgphoto2.so.6 2>/dev/null \
       | sed -n '/<gp_camera_new>:/,/^$/p' | grep -qiE '\.word[[:space:]]+0x0*102c|#[[:space:]]*4140'; then
    echo "[build] FULLSTACK: _Camera pad confirmed (gp_camera_new allocates 4140 B)"
  else
    echo "[build] FULLSTACK ERROR: gp_camera_new does not allocate 4140 B — _Camera pad failed"; exit 1
  fi
  "$XT-strip" /work/out/libgphoto2.so.6 /work/out/libgphoto2_port.so.12 \
              /work/out/ptp2.so /work/out/usb1.so
  echo "[build] FULLSTACK: core libgphoto2.so.6 built (stripped): $(stat -c %s /work/out/libgphoto2.so.6) bytes"
  echo "[build] FULLSTACK: port libgphoto2_port.so.12 built (stripped): $(stat -c %s /work/out/libgphoto2_port.so.12) bytes"
fi
