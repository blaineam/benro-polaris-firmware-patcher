#!/bin/bash
# ============================================================================
#  Benro Polaris libgphoto2 patcher — in-container pipeline
#
#  Rebuilds the ptp2 camera driver from a chosen libgphoto2 release, minimally
#  patches the stock `pgphoto` binary so the firmware actually loads that driver
#  (instead of its compiled-in 2.5.27 copy), and repacks a flashable appfs.
#
#  Runs entirely inside the debian:9 image (see docker/Dockerfile).
#  Inputs/outputs are bind mounts:
#     /in   read-only stock FwPkt (folder containing camera/appfs.ubifs, ...)
#     /out  destination for the custom FwPkt/ and FwPkt.zip
#
#  Env:
#     LIBGPHOTO2_VERSION   libgphoto2 release tag to build (default 2.5.34)
#     FIX_R5M2_TYPO        1 = correct the upstream "EOS 5Rm2" model-name typo
#     SELFTEST             1 = qemu-emulate the driver load (needs qemu-arm-static)
#
#  SEE README.md AND docs/TESTED.md.  Use at your own risk.  Tested ONLY against
#  Benro Polaris FwVer 4.0.0.32 with a Canon EOS R5 Mark II.
# ============================================================================
set -euo pipefail

log(){ printf '\033[1;36m[patcher]\033[0m %s\n' "$*"; }
warn(){ printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }
die(){ printf '\033[1;31m[abort]\033[0m %s\n' "$*" >&2; exit 1; }

LIBGPHOTO2_VERSION="${LIBGPHOTO2_VERSION:-2.5.34}"
FIX_R5M2_TYPO="${FIX_R5M2_TYPO:-1}"
SELFTEST="${SELFTEST:-0}"
# Also swap the usb1 port/iolib (USB transport) alongside the ptp2 camlib.
# 1 = swap usb1.so too (default); 0 = ptp2-only (legacy behaviour).
SWAP_USB1="${SWAP_USB1:-1}"
XT=arm-linux-gnueabi
W=/work
mkdir -p "$W"

# Known-good reference for the ONLY firmware this tool has been tested against.
TESTED_FWVER="4.0.0.32"
TESTED_APPFS_MD5="47f2ae680be3a5f5d69aa20e20a2397b"
TESTED_PGPHOTO_MD5="a0"  # informational only; verified structurally below

# ---------------------------------------------------------------------------
# 0. Validate input FwPkt
# ---------------------------------------------------------------------------
[ -f /in/firmwareInfo ] || die "/in/firmwareInfo not found — mount your stock FwPkt at /in"
[ -f /in/camera/appfs.ubifs ] || die "/in/camera/appfs.ubifs not found"
[ -f /in/camera/config ] || die "/in/camera/config not found"

STOCK_APPFS=/in/camera/appfs.ubifs
log "libgphoto2 target : $LIBGPHOTO2_VERSION"
log "stock appfs.ubifs : $(stat -c %s "$STOCK_APPFS") bytes  md5=$(md5sum "$STOCK_APPFS"|cut -d' ' -f1)"

FWVER="unknown"
[ -f /in/FwVer ] && FWVER="$(cat /in/FwVer 2>/dev/null || true)"
if ! grep -q "$TESTED_FWVER" <<<"$FWVER"; then
    warn "This FwPkt reports FwVer='$FWVER'."
    warn "This tool has ONLY been tested against FwVer $TESTED_FWVER."
    warn "Continuing, but the patch-site checks below will ABORT on any mismatch."
fi

# ---------------------------------------------------------------------------
# 1. Faithful extraction of the stock appfs (preserves uid/gid/mode/symlinks)
# ---------------------------------------------------------------------------
log "extracting stock appfs (permission-preserving)…"
rm -rf "$W/app_ext"
ubireader_extract_files -k -o "$W/app_ext" "$STOCK_APPFS" >/dev/null 2>&1
APP="$(find "$W/app_ext" -maxdepth 3 -name ubifs -type d | head -1)"
[ -n "$APP" ] || die "extraction failed"
PG="$APP/bin/pgphoto"
[ -f "$PG" ] || die "pgphoto not found inside appfs (unexpected firmware layout)"

# Locate the stock camlib (its directory name encodes the stock libgphoto2 rev).
STOCK_PTP2="$(find "$APP/lib/libgphoto2" -name ptp2.so | head -1)"
[ -n "$STOCK_PTP2" ] || die "stock ptp2.so not found inside appfs"
CAMLIB_DIR="$(dirname "$STOCK_PTP2")"
log "camlib dir        : /app/lib/libgphoto2/$(basename "$CAMLIB_DIR")"

# Locate the stock usb1 iolib (port/USB transport). Unlike ptp2, pgphoto's port
# loader dlopen's this file at runtime (verified: gp_port_set_info →
# lt_dlopenext + lt_dlsym("gp_port_library_operations"), no static short-circuit),
# so replacing it actually takes effect. Its dir name encodes the port ABI rev.
STOCK_USB1=""
if [ "$SWAP_USB1" = "1" ]; then
  STOCK_USB1="$(find "$APP/lib/libgphoto2_port" -name usb1.so | head -1)"
  if [ -n "$STOCK_USB1" ]; then
    log "iolib dir         : /app/lib/libgphoto2_port/$(basename "$(dirname "$STOCK_USB1")")"
  else
    warn "usb1.so not found inside appfs — disabling usb1 swap (ptp2-only)"
    SWAP_USB1=0
  fi
fi

# ---------------------------------------------------------------------------
# 2. Analyse pgphoto: trampoline target + the three static-dispatch gates
# ---------------------------------------------------------------------------
python3 /opt/patcher/analyze_pgphoto.py "$PG" > "$W/pgphoto.plan" || die "pgphoto analysis failed"
cat "$W/pgphoto.plan"
TRAMP_ADDR="$(grep '^TRAMPOLINE_ADDR=' "$W/pgphoto.plan" | cut -d= -f2)"
GATES="$(grep '^GATE=' "$W/pgphoto.plan" | cut -d= -f2 | tr '\n' ' ')"
RESETUSB="$(grep '^RESETUSB_ADDR=' "$W/pgphoto.plan" | cut -d= -f2)"
LISTFILES="$(grep '^LISTFILES_BL=' "$W/pgphoto.plan" | cut -d= -f2 | tr '\n' ' ')"
[ -n "$TRAMP_ADDR" ] || die "could not locate gp_filesystem_set_info_dirty in pgphoto"
[ "$(wc -w <<<"$GATES")" = "3" ] || die "expected exactly 3 dispatch gates, found: $GATES — refusing to patch unknown firmware"
[ -n "$RESETUSB" ] || die "could not locate resetUsb in pgphoto — refusing to patch unknown firmware"
[ "$(wc -w <<<"$LISTFILES")" = "1" ] || die "expected exactly 1 ARG_LIST_FILES dispatch, found: $LISTFILES — refusing to patch"
log "trampoline target : $TRAMP_ADDR (pgphoto's own gp_filesystem_set_info_dirty)"
log "dispatch gates    : $GATES"
log "resetUsb          : $RESETUSB (USBDEVFS_RESET → return 0; stops cold re-enumeration storm)"
log "list-files gate   : $LISTFILES (skip full-card PTP scan at connect → ready in seconds)"

# ---------------------------------------------------------------------------
# 3. Stage the device's own link libraries (exact soname / ABI match)
# ---------------------------------------------------------------------------
DEV=/work/devlibs; rm -rf "$DEV"; mkdir -p "$DEV"
# libexif/libltdl: link targets for ptp2.  libusb-1.0: link target for usb1
# (device's OWN soname/ABI, so the rebuilt usb1.so binds the exact libusb the
# device ships).  Each staged with a plain `.so` symlink for -l<name>.
STAGE_LIBS="libexif.so.12 libltdl.so.7"
[ "$SWAP_USB1" = "1" ] && STAGE_LIBS="$STAGE_LIBS libusb-1.0.so.0"
for l in $STAGE_LIBS; do
  s="$(find "$APP/lib" -name "${l}*" ! -name '*.la' | sort | tail -1)"
  [ -n "$s" ] && { cp -a "$s" "$DEV/$l"; ln -sf "$l" "$DEV/${l%.so.*}.so"; }
done
if [ "$SWAP_USB1" = "1" ] && [ ! -f "$DEV/libusb-1.0.so.0" ]; then
  warn "device libusb-1.0.so.0 not found in appfs — disabling usb1 swap (ptp2-only)"
  SWAP_USB1=0
fi
cp -a "$(find "$APP/lib" -name 'libgphoto2.so.6*'      ! -name '*.la'|sort|tail -1)" "$DEV/dev_libgphoto2.so.6"
cp -a "$(find "$APP/lib" -name 'libgphoto2_port.so.12*' ! -name '*.la'|sort|tail -1)" "$DEV/dev_libgphoto2_port.so.12"

# ---------------------------------------------------------------------------
# 4. Cross-build the chosen libgphoto2 ptp2 driver
#    Reliability hardening baked in for the R5 Mark II (see docs/HOW-IT-WORKS.md):
#      * drop the EOS keep-device-on heartbeat (clobbers the viewfinder settle timer)
#      * drop the SetRemoteMode toggle 2.5.34 added to camera_exit (extra re-enum)
#    (COLD_START_TIMEOUT_MS is intentionally left at the stock 1.5s — a longer
#    timeout makes camera_init exceed polestar_app's ~5s watchdog and crash-loops.)
# ---------------------------------------------------------------------------
export REMOVE_KEEP_DEVICE_ON=1
export REMOVE_EXIT_REMOTEMODE=1
/opt/patcher/build_ptp2.sh "$LIBGPHOTO2_VERSION" "$TRAMP_ADDR" "$FIX_R5M2_TYPO"
NEW_PTP2="$W/out/ptp2.so"
[ -f "$NEW_PTP2" ] || die "ptp2.so build failed"

# ---------------------------------------------------------------------------
# 5. Verify the rebuilt driver against the DEVICE'S OWN 2.5.27 core
# ---------------------------------------------------------------------------
log "verifying rebuilt ptp2.so…"
FLAGS="$($XT-readelf -h "$NEW_PTP2" | awk -F: '/Flags/{print $2}')"
grep -q 'soft-float' <<<"$FLAGS" || die "ABI mismatch: expected soft-float EABI, got:$FLAGS"
MAXGLIBC="$($XT-readelf --dyn-syms "$NEW_PTP2" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)"
log "  ABI=$FLAGS  glibc_ceiling=$MAXGLIBC"
case "$MAXGLIBC" in GLIBC_2.4|GLIBC_2.5|GLIBC_2.6|GLIBC_2.7|GLIBC_2.8|GLIBC_2.9|GLIBC_2.1[0-9]|GLIBC_2.2[0-4]) : ;;
  *) die "glibc ceiling $MAXGLIBC exceeds device glibc 2.24 — driver would not load";; esac
# every core symbol the driver imports must be provided by the device core / pgphoto
$XT-nm -D --defined-only "$DEV/dev_libgphoto2.so.6"        | awk '{print $3}'  >"$W/prov.txt"
$XT-nm -D --defined-only "$DEV/dev_libgphoto2_port.so.12"  | awk '{print $3}' >>"$W/prov.txt"
$XT-nm -D --defined-only "$PG"                             | awk '{print $3}' >>"$W/prov.txt"
sort -u "$W/prov.txt" -o "$W/prov.txt"
$XT-nm -D --undefined-only "$NEW_PTP2" | awk '{print $2}' | grep -E '^(gp_|gpi_)' | sort -u >"$W/need.txt"
MISSING="$(comm -23 "$W/need.txt" "$W/prov.txt" || true)"
[ -z "$MISSING" ] || die "rebuilt driver needs core symbols the device lacks:\n$MISSING"
log "  all core symbols resolve against the device's stock core ✓"

# ---------------------------------------------------------------------------
# 5b. Verify the rebuilt usb1 iolib (fail-safe, same rigour as ptp2).
#     Aborts on ANY mismatch so a bad usb1.so can never reach the firmware.
# ---------------------------------------------------------------------------
NEW_USB1="$W/out/usb1.so"
if [ "$SWAP_USB1" = "1" ]; then
  [ -f "$NEW_USB1" ] || die "usb1 swap requested but usb1.so was not built (libusb detection failed) — aborting"
  log "verifying rebuilt usb1.so…"
  UFLAGS="$($XT-readelf -h "$NEW_USB1" | awk -F: '/Flags/{print $2}')"
  grep -q 'soft-float' <<<"$UFLAGS" || die "usb1 ABI mismatch: expected soft-float EABI, got:$UFLAGS"
  UMAXGLIBC="$($XT-readelf --dyn-syms "$NEW_USB1" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)"
  log "  ABI=$UFLAGS  glibc_ceiling=$UMAXGLIBC"
  case "$UMAXGLIBC" in GLIBC_2.4|GLIBC_2.5|GLIBC_2.6|GLIBC_2.7|GLIBC_2.8|GLIBC_2.9|GLIBC_2.1[0-9]|GLIBC_2.2[0-4]) : ;;
    *) die "usb1 glibc ceiling $UMAXGLIBC exceeds device glibc 2.24 — iolib would not load";; esac
  # it must export the three iolib entry points the port loader lt_dlsym's.
  for e in gp_port_library_type gp_port_library_list gp_port_library_operations; do
    $XT-nm -D --defined-only "$NEW_USB1" | awk '{print $3}' | grep -qx "$e" \
      || die "rebuilt usb1.so is missing iolib entry point '$e' — aborting"
  done
  # DT_NEEDED ⊆ the STOCK usb1.so's NEEDED: never introduce a shared library the
  # stock iolib did not already depend on (and the device therefore provably has).
  $XT-readelf -d "$STOCK_USB1" | awk -F'[][]' '/\(NEEDED\)/{print $2}' | sort -u >"$W/usb1_stock_needed.txt"
  $XT-readelf -d "$NEW_USB1"   | awk -F'[][]' '/\(NEEDED\)/{print $2}' | sort -u >"$W/usb1_new_needed.txt"
  EXTRA="$(comm -23 "$W/usb1_new_needed.txt" "$W/usb1_stock_needed.txt" || true)"
  [ -z "$EXTRA" ] || die "rebuilt usb1.so pulls in libs the stock usb1.so did not:\n$EXTRA"
  log "  DT_NEEDED ⊆ stock usb1.so ($(tr '\n' ' ' <"$W/usb1_new_needed.txt")) ✓"
  # every gp_/gpi_ (core/port) symbol it imports must be provided by the device
  # port core; every libusb_* import must be provided by the device's libusb.
  $XT-nm -D --undefined-only "$NEW_USB1" | awk '{print $2}' | grep -E '^(gp_|gpi_)' | sort -u >"$W/usb1_need.txt"
  MISSING_U="$(comm -23 "$W/usb1_need.txt" "$W/prov.txt" || true)"
  [ -z "$MISSING_U" ] || die "rebuilt usb1.so needs core/port symbols the device lacks:\n$MISSING_U"
  $XT-nm -D --undefined-only "$NEW_USB1" | awk '{print $2}' | grep -iE '^libusb_' | sort -u >"$W/usb1_needusb.txt"
  $XT-nm -D --defined-only "$DEV/libusb-1.0.so.0" | awk '{print $3}' | sort -u >"$W/usb1_provusb.txt"
  MISSING_LU="$(comm -23 "$W/usb1_needusb.txt" "$W/usb1_provusb.txt" || true)"
  [ -z "$MISSING_LU" ] || die "rebuilt usb1.so needs libusb symbols the device's libusb-1.0 lacks:\n$MISSING_LU"
  log "  all core/port + libusb symbols resolve on-device ✓ ($(wc -l <"$W/usb1_needusb.txt"|tr -d ' ') libusb imports)"
fi

if [ "$SELFTEST" = "1" ] && command -v qemu-arm-static >/dev/null 2>&1; then
  /opt/patcher/selftest.sh "$APP" "$NEW_PTP2" "$DEV" || warn "selftest reported an issue (non-fatal)"
fi

# ---------------------------------------------------------------------------
# 6. Patch pgphoto (three 1-byte edits: mov r3,r0 -> mov r3,#0)
# ---------------------------------------------------------------------------
log "patching pgphoto (3 gates + resetUsb return-0 + list-files skip)…"
python3 /opt/patcher/analyze_pgphoto.py "$PG" --apply "$W/pgphoto.patched" >/dev/null
DIFFB="$( { cmp -l "$PG" "$W/pgphoto.patched" || true; } | wc -l | tr -d ' ')"
# 14 = 3 (gates, 1 byte each) + 7 (resetUsb: mov r0,#0 + bx lr) + 4 (list-files bl → nop)
[ "$DIFFB" = "14" ] || die "pgphoto patch changed $DIFFB bytes (expected 14) — aborting"
log "  pgphoto patched: 14 bytes (gates + resetUsb + list-files skip) ✓"

# ---------------------------------------------------------------------------
# 7. Swap the rebuilt files into the tree (preserve original owner/mode)
#    ptp2.so (camlib) + pgphoto (14-byte patch) always; usb1.so (iolib) when enabled.
# ---------------------------------------------------------------------------
O_UID="$(stat -c %u "$STOCK_PTP2")"; O_GID="$(stat -c %g "$STOCK_PTP2")"; O_MODE="$(stat -c %a "$STOCK_PTP2")"
install -m "$O_MODE" -o "$O_UID" -g "$O_GID" "$NEW_PTP2"            "$STOCK_PTP2"
P_UID="$(stat -c %u "$PG")"; P_GID="$(stat -c %g "$PG")"; P_MODE="$(stat -c %a "$PG")"
install -m "$P_MODE" -o "$P_UID" -g "$P_GID" "$W/pgphoto.patched"  "$PG"
if [ "$SWAP_USB1" = "1" ]; then
  U_UID="$(stat -c %u "$STOCK_USB1")"; U_GID="$(stat -c %g "$STOCK_USB1")"; U_MODE="$(stat -c %a "$STOCK_USB1")"
  install -m "$U_MODE" -o "$U_UID" -g "$U_GID" "$NEW_USB1"          "$STOCK_USB1"
  log "swapped usb1 iolib: /app/lib/libgphoto2_port/$(basename "$(dirname "$STOCK_USB1")")/usb1.so"
fi

# ---------------------------------------------------------------------------
# 8. Repack appfs (geometry read from the stock image) + regenerate firmwareInfo
# ---------------------------------------------------------------------------
/opt/patcher/repack_appfs.sh "$STOCK_APPFS" "$APP" "$W/out/appfs.ubifs"

log "assembling custom FwPkt in /out…"
rm -rf /out/FwPkt; mkdir -p /out/FwPkt/camera /out/FwPkt/gimbal
cp -p /in/camera/config /in/camera/uImage /in/camera/rootfs.ubifs /out/FwPkt/camera/
cp -p /in/gimbal/*.bin /out/FwPkt/gimbal/ 2>/dev/null || true
cp "$W/out/appfs.ubifs" /out/FwPkt/camera/appfs.ubifs
python3 /opt/patcher/gen_firmwareinfo.py /in/firmwareInfo /out/FwPkt > /out/FwPkt/firmwareInfo
( cd /out && rm -f FwPkt.zip && (command -v zip >/dev/null && zip -rqX FwPkt.zip FwPkt || python3 -c "import shutil;shutil.make_archive('FwPkt','zip','.','FwPkt')") )

log "----------------------------------------------------------------------"
log "DONE.  Custom firmware written to /out :"
( cd /out && find FwPkt -type f | sort | sed 's/^/    /' )
log "    FwPkt.zip  md5=$(md5sum /out/FwPkt.zip 2>/dev/null | cut -d' ' -f1)"
log "custom appfs.ubifs md5=$(md5sum /out/FwPkt/camera/appfs.ubifs | cut -d' ' -f1)"
log "----------------------------------------------------------------------"
warn "FLASH AT YOUR OWN RISK. Verify on your own device. Keep your stock FwPkt"
warn "as the factory-restore image. See README.md."
