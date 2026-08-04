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
for l in libexif.so.12 libltdl.so.7; do
  s="$(find "$APP/lib" -name "${l}*" ! -name '*.la' | sort | tail -1)"
  [ -n "$s" ] && { cp -a "$s" "$DEV/$l"; ln -sf "$l" "$DEV/${l%.so.*}.so"; }
done
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
# 7. Swap the two files into the tree (preserve original owner/mode)
# ---------------------------------------------------------------------------
O_UID="$(stat -c %u "$STOCK_PTP2")"; O_GID="$(stat -c %g "$STOCK_PTP2")"; O_MODE="$(stat -c %a "$STOCK_PTP2")"
install -m "$O_MODE" -o "$O_UID" -g "$O_GID" "$NEW_PTP2"            "$STOCK_PTP2"
P_UID="$(stat -c %u "$PG")"; P_GID="$(stat -c %g "$PG")"; P_MODE="$(stat -c %a "$PG")"
install -m "$P_MODE" -o "$P_UID" -g "$P_GID" "$W/pgphoto.patched"  "$PG"

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
